#include "Codegen.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>

#include <functional>
#include <unordered_map>

namespace cplus {

using K = Type::Kind;

struct Codegen::Impl {
    const SourceFile& file;
    CodegenOptions opts;
    llvm::LLVMContext ctx;
    std::unique_ptr<llvm::Module> mod;
    llvm::IRBuilder<> b;
    std::unique_ptr<llvm::TargetMachine> tm;

    llvm::StructType* stringTy = nullptr;  // { ptr data, i64 length }
    llvm::FunctionCallee rtPrintStr, rtPrintQuoted, rtPrintI32, rtPrintI64, rtPrintBool, rtPrintNewline, rtTrap;
    llvm::FunctionCallee rtAlloc, rtFree, rtRetain, rtRelease, rtPrintEnter, rtPrintLeave, rtStrEq;
    llvm::FunctionCallee rtTrapIndex, rtVecNew, rtVecRelease, rtVecReserve;
    llvm::Constant* fileName = nullptr;
    std::unordered_map<std::string, llvm::Constant*> cstrings;

    // What is generated for each struct and class.
    struct TypeInfo {
        llvm::StructType* layout = nullptr;  // a struct's value, or a class object: { i64 refcount, fields... }
        llvm::Function* print = nullptr;
        llvm::Function* equals = nullptr;   // structs: compares two values field by field
        llvm::Function* retain = nullptr;   // structs that hold class references
        llvm::Function* release = nullptr;  // classes, and structs that hold class references
        llvm::Function* destroy = nullptr;  // classes
        int managed = -1;                   // structs: whether they hold class references (-1: not computed)
    };
    std::unordered_map<const StructDecl*, TypeInfo> types;

    // A vector is a pointer to a buffer laid out as the runtime's cplus_vec: a header of
    // count, size and capacity, then the elements. Null is an empty vector.
    static constexpr uint64_t kVecSizeOffset = 8, kVecCapacityOffset = 16, kVecHeaderSize = 24;
    struct VecInfo {  // generated for each vector type, when first needed
        llvm::Type* elemTy = nullptr;
        uint64_t elemSize = 0;
        llvm::Function* print = nullptr;
        llvm::Function* equals = nullptr;
        llvm::Constant* retainElem = nullptr;   // void(ptr element), or null when elements hold no references
        llvm::Constant* releaseElem = nullptr;
    };
    std::unordered_map<const Type*, VecInfo> vecs;
    llvm::Function* vecSizeFn = nullptr;
    llvm::Function* vecReserveFn = nullptr;

    std::unordered_map<const FuncDecl*, llvm::Function*> funcs;
    std::unordered_map<const VarDecl*, llvm::AllocaInst*> locals;
    llvm::Function* fn = nullptr;
    const FuncDecl* fnDecl = nullptr;
    llvm::Value* thisPtr = nullptr;  // in a method: the object, or the address of the struct

    // Reference counting. A local that holds class references owns them, and releases
    // them when its scope ends. A value made or read during a statement and not stored
    // anywhere is a temporary, released when the statement ends.
    std::vector<std::vector<const VarDecl*>> scopes;  // locals to release, per open scope
    struct Temp {
        llvm::Value* value;
        const Type* type;
        bool inMemory;  // `value` is the address of the value
    };
    std::vector<Temp> temps;

    struct LoopTargets {
        llvm::BasicBlock* breakTo;
        llvm::BasicBlock* continueTo;
        size_t scopeDepth;  // scopes opened inside the loop are released when leaving it
    };
    std::vector<LoopTargets> loops;

    Impl(const SourceFile& f, CodegenOptions o) : file(f), opts(o), b(ctx) {}

    // ---- Setup -------------------------------------------------------------

    bool setupTarget(std::string& error) {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        std::string triple = llvm::sys::getDefaultTargetTriple();
        const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, error);
        if (!target) return false;
        llvm::TargetOptions options;
        tm.reset(target->createTargetMachine(triple, "generic", "", options, llvm::Reloc::PIC_, std::nullopt,
                                             opts.optimize ? llvm::CodeGenOptLevel::Default
                                                           : llvm::CodeGenOptLevel::None));
        if (!tm) {
            error = "could not create a target machine for " + triple;
            return false;
        }
        mod->setTargetTriple(triple);
        mod->setDataLayout(tm->createDataLayout());
        return true;
    }

    void declareRuntime() {
        auto* voidTy = b.getVoidTy();
        auto* ptrTy = b.getPtrTy();
        auto* i32 = b.getInt32Ty();
        auto* i64 = b.getInt64Ty();
        auto decl = [&](const char* name, llvm::Type* ret, std::vector<llvm::Type*> params) {
            return mod->getOrInsertFunction(name, llvm::FunctionType::get(ret, params, false));
        };
        rtPrintStr = decl("cplus_print_str", voidTy, {ptrTy, i64});
        rtPrintQuoted = decl("cplus_print_quoted", voidTy, {ptrTy, i64});
        rtPrintI32 = decl("cplus_print_i32", voidTy, {i32});
        rtPrintI64 = decl("cplus_print_i64", voidTy, {i64});
        rtPrintBool = decl("cplus_print_bool", voidTy, {i32});
        rtPrintNewline = decl("cplus_print_newline", voidTy, {});
        rtTrap = decl("cplus_trap", voidTy, {ptrTy, ptrTy, i32, i32});
        if (auto* f = llvm::dyn_cast<llvm::Function>(rtTrap.getCallee())) {
            f->addFnAttr(llvm::Attribute::NoReturn);
            f->addFnAttr(llvm::Attribute::Cold);
            f->addFnAttr(llvm::Attribute::NoUnwind);
        }
        rtAlloc = decl("cplus_alloc", ptrTy, {i64});
        rtFree = decl("cplus_free", voidTy, {ptrTy});
        rtRetain = decl("cplus_retain", voidTy, {ptrTy});
        rtRelease = decl("cplus_release", i32, {ptrTy});
        rtPrintEnter = decl("cplus_print_enter", i32, {ptrTy});
        rtPrintLeave = decl("cplus_print_leave", voidTy, {});
        rtStrEq = decl("cplus_str_eq", i32, {ptrTy, i64, ptrTy, i64});
        rtTrapIndex = decl("cplus_trap_index", voidTy, {i64, i64, ptrTy, i32, i32});
        if (auto* f = llvm::dyn_cast<llvm::Function>(rtTrapIndex.getCallee())) {
            f->addFnAttr(llvm::Attribute::NoReturn);
            f->addFnAttr(llvm::Attribute::Cold);
            f->addFnAttr(llvm::Attribute::NoUnwind);
        }
        rtVecNew = decl("cplus_vec_new", ptrTy, {i64, i64});
        rtVecRelease = decl("cplus_vec_release", voidTy, {ptrTy, i64, ptrTy});
        rtVecReserve = decl("cplus_vec_reserve", ptrTy, {ptrTy, i64, i64, ptrTy, ptrTy});
    }

    llvm::Type* llvmType(const Type* t) {
        switch (t->kind) {
            case K::Void: return b.getVoidTy();
            case K::Bool: return b.getInt1Ty();
            case K::Int: return b.getInt32Ty();
            case K::Long: return b.getInt64Ty();
            case K::String: return stringTy;
            case K::Struct: return types.at(t->decl).layout;
            case K::Class: case K::Vector: return b.getPtrTy();
            default: llvm_unreachable("unresolved type reached codegen");
        }
    }

    // Whether values of this type hold class references, which are retained when the
    // value is copied and released when it goes away.
    bool isManaged(const Type* t) {
        if (t->isClass() || t->isVector()) return true;
        if (!t->isStruct()) return false;
        TypeInfo& info = types.at(t->decl);
        if (info.managed < 0) {
            info.managed = 0;
            for (auto& f : t->decl->fields)
                if (isManaged(f.type)) info.managed = 1;
        }
        return info.managed == 1;
    }

    llvm::Constant* cstring(const std::string& s) {
        auto [it, inserted] = cstrings.emplace(s, nullptr);
        if (inserted) it->second = b.CreateGlobalString(s, ".str", 0, mod.get());
        return it->second;
    }

    llvm::Value* stringValue(const std::string& s) {
        llvm::Constant* data = cstring(s);
        return llvm::ConstantStruct::get(stringTy, {data, b.getInt64(s.size())});
    }

    llvm::AllocaInst* entryAlloca(llvm::Type* t, const std::string& name) {
        llvm::IRBuilder<> entry(&fn->getEntryBlock(), fn->getEntryBlock().begin());
        return entry.CreateAlloca(t, nullptr, name);
    }

    // ---- Traps -------------------------------------------------------------

    void trapIf(llvm::Value* cond, const std::string& message, Loc loc) {
        auto* trapBB = llvm::BasicBlock::Create(ctx, "trap", fn);
        auto* okBB = llvm::BasicBlock::Create(ctx, "ok", fn);
        b.CreateCondBr(cond, trapBB, okBB, llvm::MDBuilder(ctx).createBranchWeights(1, 1u << 20));
        b.SetInsertPoint(trapBB);
        auto* call = b.CreateCall(rtTrap, {cstring(message), fileName, b.getInt32(loc.line), b.getInt32(loc.col)});
        call->setDoesNotReturn();
        b.CreateUnreachable();
        b.SetInsertPoint(okBB);
    }

    // Stops the program unless 0 <= index < size; `index` and `size` are i64.
    void checkIndex(llvm::Value* index, llvm::Value* size, Loc loc) {
        auto* trapBB = llvm::BasicBlock::Create(ctx, "bad.index", fn);
        auto* okBB = llvm::BasicBlock::Create(ctx, "ok", fn);
        // An unsigned compare also catches negative indexes.
        b.CreateCondBr(b.CreateICmpUGE(index, size), trapBB, okBB, llvm::MDBuilder(ctx).createBranchWeights(1, 1u << 20));
        b.SetInsertPoint(trapBB);
        auto* call = b.CreateCall(rtTrapIndex, {index, size, fileName, b.getInt32(loc.line), b.getInt32(loc.col)});
        call->setDoesNotReturn();
        b.CreateUnreachable();
        b.SetInsertPoint(okBB);
    }

    llvm::Value* checkedArith(llvm::Intrinsic::ID id, llvm::Value* l, llvm::Value* r, const char* sym,
                              const Type* ty, Loc loc) {
        llvm::Function* f = llvm::Intrinsic::getDeclaration(mod.get(), id, {l->getType()});
        llvm::Value* pair = b.CreateCall(f, {l, r});
        trapIf(b.CreateExtractValue(pair, 1),
               std::string("integer overflow: the result of '") + sym + "' does not fit in '" + ty->name + "'", loc);
        return b.CreateExtractValue(pair, 0);
    }

    llvm::Value* arith(BinOp op, llvm::Value* l, llvm::Value* r, const Type* ty, Loc loc) {
        const char* sym = binOpSpelling(op);
        switch (op) {
            case BinOp::Add: return checkedArith(llvm::Intrinsic::sadd_with_overflow, l, r, sym, ty, loc);
            case BinOp::Sub: return checkedArith(llvm::Intrinsic::ssub_with_overflow, l, r, sym, ty, loc);
            case BinOp::Mul: return checkedArith(llvm::Intrinsic::smul_with_overflow, l, r, sym, ty, loc);
            case BinOp::Div:
            case BinOp::Rem: {
                llvm::Type* t = l->getType();
                trapIf(b.CreateICmpEQ(r, llvm::ConstantInt::get(t, 0)),
                       std::string("division by zero in '") + sym + "'", loc);
                llvm::Value* minusOne = b.CreateICmpEQ(r, llvm::ConstantInt::getSigned(t, -1));
                if (op == BinOp::Div) {
                    llvm::Value* isMin = b.CreateICmpEQ(l, llvm::ConstantInt::get(t, llvm::APInt::getSignedMinValue(t->getIntegerBitWidth())));
                    trapIf(b.CreateAnd(isMin, minusOne),
                           std::string("integer overflow: the result of '/' does not fit in '") + ty->name + "'", loc);
                    return b.CreateSDiv(l, r);
                }
                // x % -1 is always 0; LLVM treats MIN % -1 as undefined, so avoid it.
                llvm::Value* safeR = b.CreateSelect(minusOne, llvm::ConstantInt::get(t, 1), r);
                return b.CreateSelect(minusOne, llvm::ConstantInt::get(t, 0), b.CreateSRem(l, safeR));
            }
            case BinOp::Shl:
            case BinOp::Shr: {
                unsigned width = ty->bits();
                llvm::Value* limit = llvm::ConstantInt::get(r->getType(), width);
                trapIf(b.CreateICmpUGE(r, limit),  // unsigned compare also catches negative amounts
                       std::string("shift amount out of range: shifting '") + ty->name + "' needs an amount from 0 to " +
                           std::to_string(width - 1),
                       loc);
                llvm::Value* amount = b.CreateSExtOrTrunc(r, l->getType());
                return op == BinOp::Shl ? b.CreateShl(l, amount) : b.CreateAShr(l, amount);
            }
            case BinOp::BitAnd: return b.CreateAnd(l, r);
            case BinOp::BitOr: return b.CreateOr(l, r);
            case BinOp::BitXor: return b.CreateXor(l, r);
            case BinOp::Eq: return b.CreateICmpEQ(l, r);
            case BinOp::Ne: return b.CreateICmpNE(l, r);
            case BinOp::Lt: return b.CreateICmpSLT(l, r);
            case BinOp::Le: return b.CreateICmpSLE(l, r);
            case BinOp::Gt: return b.CreateICmpSGT(l, r);
            case BinOp::Ge: return b.CreateICmpSGE(l, r);
            case BinOp::LogAnd:
            case BinOp::LogOr: break;
        }
        llvm_unreachable("logical operators are lowered separately");
    }

    // ---- Reference counting ------------------------------------------------

    void retainValue(llvm::Value* v, const Type* t) {
        if (t->isClass() || t->isVector()) b.CreateCall(rtRetain, {v});
        else if (isManaged(t)) b.CreateCall(types.at(t->decl).retain, {v});
    }

    void releaseValue(llvm::Value* v, const Type* t) {
        if (t->isVector()) {
            VecInfo& info = vecInfo(t);
            b.CreateCall(rtVecRelease, {v, b.getInt64(info.elemSize), info.releaseElem});
        } else if (isManaged(t)) {
            b.CreateCall(types.at(t->decl).release, {v});
        }
    }

    // ---- Vectors -----------------------------------------------------------

    // Helpers are generated when first needed, which can be in the middle of another
    // function; this puts the builder back where it was.
    struct SavedPoint {
        Impl& g;
        llvm::IRBuilderBase::InsertPoint ip;
        llvm::Function* fn;
        explicit SavedPoint(Impl& impl) : g(impl), ip(impl.b.saveIP()), fn(impl.fn) {}
        ~SavedPoint() {
            g.b.restoreIP(ip);
            g.fn = fn;
        }
    };

    llvm::Function* internalFunction(const std::string& name, llvm::Type* ret, std::vector<llvm::Type*> params) {
        return llvm::Function::Create(llvm::FunctionType::get(ret, params, false), llvm::Function::InternalLinkage, name,
                                      mod.get());
    }

    llvm::Value* byteOffset(llvm::Value* buf, uint64_t offset, const char* name) {
        return b.CreateConstInBoundsGEP1_64(b.getInt8Ty(), buf, offset, name);
    }

    // The number of elements; 0 for an empty vector, whose pointer is null.
    llvm::Value* vecSize(llvm::Value* buf) {
        if (!vecSizeFn) {
            SavedPoint saved(*this);
            vecSizeFn = internalFunction("cplus.vector#size", b.getInt64Ty(), {b.getPtrTy()});
            startHelper(vecSizeFn);
            llvm::Value* v = vecSizeFn->getArg(0);
            auto* emptyBB = llvm::BasicBlock::Create(ctx, "empty", fn);
            auto* loadBB = llvm::BasicBlock::Create(ctx, "load", fn);
            b.CreateCondBr(b.CreateIsNull(v), emptyBB, loadBB);
            b.SetInsertPoint(emptyBB);
            b.CreateRet(b.getInt64(0));
            b.SetInsertPoint(loadBB);
            b.CreateRet(b.CreateLoad(b.getInt64Ty(), byteOffset(v, kVecSizeOffset, "size.addr"), "size"));
        }
        return b.CreateCall(vecSizeFn, {buf}, "size");
    }

    llvm::Value* sizeAddress(llvm::Value* buf) { return byteOffset(buf, kVecSizeOffset, "size.addr"); }

    llvm::Value* elementAt(llvm::Value* buf, llvm::Value* index, const Type* vecType) {
        return b.CreateInBoundsGEP(vecInfo(vecType).elemTy, byteOffset(buf, kVecHeaderSize, "elems"), index, "elem.addr");
    }

    // Makes the vector stored at `slot` one that only this place holds, with room for
    // `minCapacity` elements, and returns its buffer. A buffer shared with other copies
    // is copied first (copy-on-write).
    llvm::Value* reserve(llvm::Value* slot, llvm::Value* buf, const Type* vecType, llvm::Value* minCapacity) {
        if (!vecReserveFn) {
            SavedPoint saved(*this);
            auto* ptrTy = b.getPtrTy();
            auto* i64 = b.getInt64Ty();
            vecReserveFn = internalFunction("cplus.vector#reserve", ptrTy, {ptrTy, i64, i64, ptrTy, ptrTy});
            startHelper(vecReserveFn);
            llvm::Value* v = vecReserveFn->getArg(0);
            auto* checkBB = llvm::BasicBlock::Create(ctx, "check", fn);
            auto* fastBB = llvm::BasicBlock::Create(ctx, "fast", fn);
            auto* slowBB = llvm::BasicBlock::Create(ctx, "slow", fn);
            b.CreateCondBr(b.CreateIsNull(v), slowBB, checkBB);
            b.SetInsertPoint(checkBB);
            llvm::LoadInst* count = b.CreateLoad(i64, v, "count");
            count->setAtomic(llvm::AtomicOrdering::Acquire);
            count->setAlignment(llvm::Align(8));
            llvm::Value* capacity = b.CreateLoad(i64, byteOffset(v, kVecCapacityOffset, "capacity.addr"), "capacity");
            llvm::Value* fits = b.CreateAnd(b.CreateICmpEQ(count, b.getInt64(1)),
                                            b.CreateICmpSGE(capacity, vecReserveFn->getArg(2)));
            b.CreateCondBr(fits, fastBB, slowBB);
            b.SetInsertPoint(fastBB);
            b.CreateRet(v);
            b.SetInsertPoint(slowBB);
            std::vector<llvm::Value*> args;
            for (auto& a : vecReserveFn->args()) args.push_back(&a);
            b.CreateRet(b.CreateCall(rtVecReserve, args));
        }
        VecInfo& info = vecInfo(vecType);
        llvm::Value* own =
            b.CreateCall(vecReserveFn, {buf, b.getInt64(info.elemSize), minCapacity, info.retainElem, info.releaseElem}, "vec");
        b.CreateStore(own, slot);
        return own;
    }

    // Emits `for (i = 0; i < count; i++) body(i)`, with `count` an i64.
    void countedLoop(llvm::Value* count, const std::function<void(llvm::Value*)>& body) {
        llvm::AllocaInst* counter = entryAlloca(b.getInt64Ty(), "i");
        b.CreateStore(b.getInt64(0), counter);
        auto* condBB = llvm::BasicBlock::Create(ctx, "loop.cond", fn);
        auto* bodyBB = llvm::BasicBlock::Create(ctx, "loop.body", fn);
        auto* endBB = llvm::BasicBlock::Create(ctx, "loop.end", fn);
        b.CreateBr(condBB);
        b.SetInsertPoint(condBB);
        llvm::Value* i = b.CreateLoad(b.getInt64Ty(), counter, "i");
        b.CreateCondBr(b.CreateICmpSLT(i, count), bodyBB, endBB);
        b.SetInsertPoint(bodyBB);
        body(i);
        b.CreateStore(b.CreateAdd(i, b.getInt64(1)), counter);
        b.CreateBr(condBB);
        b.SetInsertPoint(endBB);
    }

    VecInfo& vecInfo(const Type* vt) {
        auto [it, inserted] = vecs.try_emplace(vt);
        VecInfo& info = it->second;
        if (!inserted) return info;
        const Type* elem = vt->elem;
        auto* ptrTy = b.getPtrTy();
        info.elemTy = llvmType(elem);
        info.elemSize = mod->getDataLayout().getTypeAllocSize(info.elemTy);
        std::string prefix = std::string("cplus.") + vt->name + "#";
        // Everything is declared before any body is generated, so element types that
        // lead back to this vector find its helpers.
        info.print = internalFunction(prefix + "print", b.getVoidTy(), {ptrTy});
        info.equals = internalFunction(prefix + "equals", b.getInt1Ty(), {ptrTy, ptrTy});
        llvm::Function* retainElem = nullptr;
        llvm::Function* releaseElem = nullptr;
        if (isManaged(elem)) {
            info.retainElem = retainElem = internalFunction(prefix + "retain_elem", b.getVoidTy(), {ptrTy});
            info.releaseElem = releaseElem = internalFunction(prefix + "release_elem", b.getVoidTy(), {ptrTy});
        } else {
            info.retainElem = info.releaseElem = llvm::ConstantPointerNull::get(ptrTy);
        }
        SavedPoint saved(*this);

        // Printing: `[1, 2, 3]`, with strings in quotes.
        startHelper(info.print);
        llvm::Value* v = info.print->getArg(0);
        printText("[");
        countedLoop(vecSize(v), [&](llvm::Value* i) {
            auto* sepBB = llvm::BasicBlock::Create(ctx, "sep", fn);
            auto* elemBB = llvm::BasicBlock::Create(ctx, "elem", fn);
            b.CreateCondBr(b.CreateICmpNE(i, b.getInt64(0)), sepBB, elemBB);
            b.SetInsertPoint(sepBB);
            printText(", ");
            b.CreateBr(elemBB);
            b.SetInsertPoint(elemBB);
            printValue(b.CreateLoad(info.elemTy, elementAt(v, i, vt)), elem, true);
        });
        printText("]");
        b.CreateRetVoid();

        // Equality: the same size, and each pair of elements equal.
        startHelper(info.equals);
        llvm::Value* l = info.equals->getArg(0);
        llvm::Value* r = info.equals->getArg(1);
        auto* sameBB = llvm::BasicBlock::Create(ctx, "same", fn);
        auto* differBB = llvm::BasicBlock::Create(ctx, "differ", fn);
        auto* sizesBB = llvm::BasicBlock::Create(ctx, "sizes", fn);
        auto* elemsBB = llvm::BasicBlock::Create(ctx, "elements", fn);
        b.CreateCondBr(b.CreateICmpEQ(l, r), sameBB, sizesBB);  // one buffer, or both empty
        b.SetInsertPoint(sizesBB);
        llvm::Value* size = vecSize(l);
        b.CreateCondBr(b.CreateICmpEQ(size, vecSize(r)), elemsBB, differBB);
        b.SetInsertPoint(elemsBB);
        countedLoop(size, [&](llvm::Value* i) {
            llvm::Value* x = b.CreateLoad(info.elemTy, elementAt(l, i, vt));
            llvm::Value* y = b.CreateLoad(info.elemTy, elementAt(r, i, vt));
            auto* nextBB = llvm::BasicBlock::Create(ctx, "next", fn);
            b.CreateCondBr(genEquals(x, y, elem), nextBB, differBB);
            b.SetInsertPoint(nextBB);
        });
        b.CreateBr(sameBB);
        b.SetInsertPoint(sameBB);
        b.CreateRet(b.getInt1(true));
        b.SetInsertPoint(differBB);
        b.CreateRet(b.getInt1(false));

        // Retaining and releasing one element, for the runtime to call.
        for (auto [f, retain] : {std::pair{retainElem, true}, std::pair{releaseElem, false}}) {
            if (!f) continue;
            startHelper(f);
            llvm::Value* x = b.CreateLoad(info.elemTy, f->getArg(0));
            retain ? retainValue(x, elem) : releaseValue(x, elem);
            b.CreateRetVoid();
        }
        return info;
    }

    // `v[i]` takes an int or a long.
    llvm::Value* genIndex(const Expr& index) {
        llvm::Value* v = gen(index);
        return index.type->is(K::Int) ? b.CreateSExt(v, b.getInt64Ty()) : v;
    }

    // The buffer of a vector that is only read right away.
    llvm::Value* vectorForRead(const Expr& e) {
        return isPlace(e) ? b.CreateLoad(b.getPtrTy(), addressOf(e), "vec") : gen(e);
    }

    // `{1, 2, 3}` for a vector: the elements are evaluated in order.
    llvm::Value* genVectorList(const InitListExpr& list, const Type* vt) {
        if (list.elements.empty()) return llvm::ConstantPointerNull::get(b.getPtrTy());
        VecInfo& info = vecInfo(vt);
        llvm::Value* buf = b.CreateCall(rtVecNew, {b.getInt64(info.elemSize), b.getInt64(list.elements.size())}, "vec");
        for (size_t i = 0; i < list.elements.size(); ++i)
            b.CreateStore(genOwned(*list.elements[i].value, vt->elem), elementAt(buf, b.getInt64(i), vt));
        return buf;
    }

    // `vector<T>()`, `vector<T>(n)` or `vector<T>(n, value)`.
    llvm::Value* genVectorConstruction(const CallExpr& call) {
        const Type* vt = call.vectorType;
        const Type* elem = vt->elem;
        if (call.args.empty()) return llvm::ConstantPointerNull::get(b.getPtrTy());
        VecInfo& info = vecInfo(vt);
        llvm::Value* count = genConverted(*call.args[0], Type::longTy());
        llvm::Value* value = call.args.size() == 2 ? genOwned(*call.args[1], elem) : nullptr;
        trapIf(b.CreateICmpSLT(count, b.getInt64(0)), "a vector cannot have a negative size", call.loc);
        llvm::Value* buf = b.CreateCall(rtVecNew, {b.getInt64(info.elemSize), count}, "vec");
        // New elements are zero bytes, which is already the default of every type but a struct.
        if (value || elem->isStruct()) {
            countedLoop(count, [&](llvm::Value* i) {
                size_t mark = temps.size();
                llvm::Value* x = value;
                if (value) retainValue(value, elem);  // each element holds its own reference
                else if (elem->decl->constructor) x = construct(*elem->decl, {});
                else x = genDefault(elem);
                flushTemps(mark);
                b.CreateStore(x, elementAt(buf, i, vt));
            });
            if (value) releaseValue(value, elem);
        }
        return buf;
    }

    llvm::Value* genVectorMethod(const MethodCallExpr& call) {
        const Expr& recv = *call.receiver;
        const Type* vt = recv.type;
        VecInfo& info = vecInfo(vt);
        switch (call.vecMethod) {
            case VecMethod::Size: return vecSize(vectorForRead(recv));
            case VecMethod::Empty: return b.CreateICmpEQ(vecSize(vectorForRead(recv)), b.getInt64(0), "empty");
            case VecMethod::Front:
            case VecMethod::Back: {
                bool front = call.vecMethod == VecMethod::Front;
                llvm::Value* buf = vectorForRead(recv);
                llvm::Value* size = vecSize(buf);
                trapIf(b.CreateICmpEQ(size, b.getInt64(0)),
                       std::string(front ? "front()" : "back()") + " was called on an empty vector", call.loc);
                llvm::Value* index = front ? b.getInt64(0) : b.CreateSub(size, b.getInt64(1));
                llvm::Value* v = b.CreateLoad(info.elemTy, elementAt(buf, index, vt), call.name);
                retainValue(v, vt->elem);  // a method call's result is owned
                return v;
            }
            case VecMethod::PushBack: {
                // The value is evaluated before the vector is found, so code it runs
                // cannot move the vector away from under the store.
                llvm::Value* value = genOwned(*call.args[0], vt->elem);
                llvm::Value* slot = addressOf(recv, false, true);
                llvm::Value* buf = b.CreateLoad(b.getPtrTy(), slot, "vec");
                llvm::Value* size = vecSize(buf);
                buf = reserve(slot, buf, vt, b.CreateAdd(size, b.getInt64(1)));
                b.CreateStore(value, elementAt(buf, size, vt));
                b.CreateStore(b.CreateAdd(size, b.getInt64(1)), sizeAddress(buf));
                return nullptr;
            }
            case VecMethod::PopBack: {
                llvm::Value* slot = addressOf(recv, false, true);
                llvm::Value* buf = b.CreateLoad(b.getPtrTy(), slot, "vec");
                llvm::Value* size = vecSize(buf);
                trapIf(b.CreateICmpEQ(size, b.getInt64(0)), "pop_back() was called on an empty vector", call.loc);
                buf = reserve(slot, buf, vt, b.getInt64(0));
                llvm::Value* last = b.CreateSub(size, b.getInt64(1));
                b.CreateStore(last, sizeAddress(buf));
                // Released after the vector is updated, in case a destructor looks at it.
                if (isManaged(vt->elem)) releaseValue(b.CreateLoad(info.elemTy, elementAt(buf, last, vt)), vt->elem);
                return nullptr;
            }
            case VecMethod::Clear: {
                llvm::Value* slot = addressOf(recv, false, true);
                llvm::Value* old = b.CreateLoad(b.getPtrTy(), slot, "old");
                b.CreateStore(llvm::ConstantPointerNull::get(b.getPtrTy()), slot);
                releaseValue(old, vt);
                return nullptr;
            }
            case VecMethod::None: break;
        }
        llvm_unreachable("unknown vector method");
    }

    // Releases the temporaries made since `mark`, newest first.
    void flushTemps(size_t mark = 0) {
        while (temps.size() > mark) {
            Temp t = temps.back();
            temps.pop_back();
            releaseValue(t.inMemory ? b.CreateLoad(llvmType(t.type), t.value) : t.value, t.type);
        }
    }

    // Releases the locals of every scope from `depth` on, innermost first.
    void releaseScopes(size_t depth) {
        for (size_t i = scopes.size(); i-- > depth;)
            for (auto it = scopes[i].rbegin(); it != scopes[i].rend(); ++it)
                releaseValue(b.CreateLoad(llvmType((*it)->type), locals.at(*it), (*it)->name), (*it)->type);
    }

    void popScope() {
        releaseScopes(scopes.size() - 1);
        scopes.pop_back();
    }

    // ---- Expressions -------------------------------------------------------

    llvm::Value* convert(llvm::Value* v, const Type* from, const Type* to) {
        if (from == to) return v;
        if (from->is(K::Int) && to->is(K::Long)) return b.CreateSExt(v, b.getInt64Ty());
        if (from->isInteger() && to->is(K::Bool)) return b.CreateICmpNE(v, llvm::ConstantInt::get(v->getType(), 0));
        llvm_unreachable("Sema allowed a conversion Codegen does not know");
    }

    // Evaluates `e` for use during the current statement. A value holding class
    // references stays valid until the statement ends: a new one is released then,
    // and one read from a variable or field is retained first, so reassigning that
    // variable later in the statement cannot free it.
    llvm::Value* gen(const Expr& e) {
        bool owned = false;
        llvm::Value* v = genRaw(e, owned);
        bool isThis = e.kind == Expr::Kind::This && e.type->isClass();  // kept alive by the caller
        if (v && !isThis && isManaged(e.type)) {
            if (!owned) retainValue(v, e.type);
            temps.push_back({v, e.type, false});
        }
        return v;
    }

    llvm::Value* genConverted(const Expr& e, const Type* to) { return convert(gen(e), e.type, to); }

    // Evaluates `e` as a value of type `to` that the caller takes over: it is stored,
    // passed to a function, or returned. A `{ }` list takes that type.
    llvm::Value* genOwned(const Expr& e, const Type* to) {
        if (e.kind == Expr::Kind::InitList) return genInitList(static_cast<const InitListExpr&>(e), to);
        bool owned = false;
        llvm::Value* v = convert(genRaw(e, owned), e.type, to);
        if (!owned) retainValue(v, to);
        return v;
    }

    // Evaluates an expression whose value is not used.
    void genDiscarded(const Expr& e) {
        bool owned = false;
        llvm::Value* v = genRaw(e, owned);
        if (v && owned) releaseValue(v, e.type);
        flushTemps();
    }

    // The value a declaration without an initializer starts with: numbers 0, bools
    // false, strings empty, struct fields their defaults, and class references none.
    // Definite assignment makes sure a missing class reference is never read.
    llvm::Value* genDefault(const Type* t) {
        switch (t->kind) {
            case K::Bool: return b.getInt1(false);
            case K::Int: case K::Long: return llvm::ConstantInt::get(llvmType(t), 0);
            case K::String:
                return llvm::ConstantStruct::get(stringTy, {llvm::ConstantPointerNull::get(b.getPtrTy()), b.getInt64(0)});
            case K::Struct: return buildAggregate(*t->decl, {});
            case K::Class: case K::Vector: return llvm::ConstantPointerNull::get(b.getPtrTy());
            default: llvm_unreachable("no default value for this type");
        }
    }

    llvm::Value* allocObject(const StructDecl& sd) {
        uint64_t size = mod->getDataLayout().getTypeAllocSize(types.at(&sd).layout);
        return b.CreateCall(rtAlloc, {b.getInt64(size)}, sd.name);
    }

    llvm::Value* fieldAddress(const Type* owner, llvm::Value* object, int index, const std::string& name) {
        unsigned slot = owner->isClass() ? index + 1 : index;  // a class object starts with its count
        return b.CreateStructGEP(types.at(owner->decl).layout, object, slot, name);
    }

    // Builds a struct value, or a new class object, without a constructor. `given[i]`,
    // when present, is the owned value of field i; the others get their default.
    llvm::Value* buildAggregate(const StructDecl& sd, std::vector<llvm::Value*> given) {
        given.resize(sd.fields.size(), nullptr);
        auto valueOf = [&](size_t i) {
            const FieldDecl& f = sd.fields[i];
            if (given[i]) return given[i];
            return f.defaultValue ? genOwned(*f.defaultValue, f.type) : genDefault(f.type);
        };
        if (sd.isClass) {
            llvm::Value* object = allocObject(sd);
            for (size_t i = 0; i < sd.fields.size(); ++i)
                b.CreateStore(valueOf(i), fieldAddress(&sd.type, object, (int)i, sd.fields[i].name));
            return object;
        }
        llvm::Value* agg = llvm::PoisonValue::get(types.at(&sd).layout);
        for (size_t i = 0; i < sd.fields.size(); ++i) agg = b.CreateInsertValue(agg, valueOf(i), {(unsigned)i});
        return agg;
    }

    // Calls the constructor of `sd`, returning the new owned value.
    llvm::Value* construct(const StructDecl& sd, const std::vector<const Expr*>& args) {
        const FuncDecl& ctor = *sd.constructor;
        std::vector<llvm::Value*> values{nullptr};  // `this`, set below
        for (size_t i = 0; i < args.size(); ++i) values.push_back(genOwned(*args[i], ctor.params[i]->type));
        if (sd.isClass) {
            values[0] = allocObject(sd);
            b.CreateCall(funcs.at(&ctor), values);
            return values[0];
        }
        llvm::AllocaInst* slot = entryAlloca(types.at(&sd).layout, sd.name);
        values[0] = slot;
        b.CreateCall(funcs.at(&ctor), values);
        return b.CreateLoad(types.at(&sd).layout, slot, sd.name);
    }

    llvm::Value* genInitList(const InitListExpr& list, const Type* to) {
        const Type* type = list.explicitType ? list.explicitType : to;
        if (type->isVector()) return genVectorList(list, type);
        const StructDecl& sd = *type->decl;
        if (sd.constructor) {  // the values are the constructor's arguments
            std::vector<const Expr*> args;
            for (auto& el : list.elements) args.push_back(el.value.get());
            return construct(sd, args);
        }
        std::vector<llvm::Value*> given(sd.fields.size(), nullptr);
        for (auto& el : list.elements)  // evaluated in the order written
            given[el.fieldIndex] = genOwned(*el.value, sd.fields[el.fieldIndex].type);
        return buildAggregate(sd, std::move(given));
    }

    // A condition is a bool, or a number that counts as true when it is not zero.
    llvm::Value* genCond(const Expr& e) {
        llvm::Value* v = gen(e);
        if (e.type->isInteger()) return b.CreateICmpNE(v, llvm::ConstantInt::get(v->getType(), 0));
        return v;
    }

    // Whether `e` is storage that can be read in place or assigned: a variable, a field
    // of one, `this` in a struct method, any field of a class object, or an element of
    // a vector stored in one of those.
    static bool isPlace(const Expr& e) {
        switch (e.kind) {
            case Expr::Kind::Name: return true;
            case Expr::Kind::This: return !e.type->isClass();
            case Expr::Kind::Member: {
                auto& m = static_cast<const MemberExpr&>(e);
                return m.base->type->isClass() || isPlace(*m.base);
            }
            case Expr::Kind::Index: return isPlace(*static_cast<const IndexExpr&>(e).base);
            default: return false;
        }
    }

    // Whether evaluating `e` may run code that changes or frees other values: a call,
    // or an assignment, which releases the value it replaces.
    static bool mayRunCode(const Expr& e) {
        switch (e.kind) {
            case Expr::Kind::IntLit: case Expr::Kind::BoolLit: case Expr::Kind::StringLit:
            case Expr::Kind::Name: case Expr::Kind::This: return false;
            case Expr::Kind::Member: return mayRunCode(*static_cast<const MemberExpr&>(e).base);
            case Expr::Kind::Index: {
                auto& ix = static_cast<const IndexExpr&>(e);
                return mayRunCode(*ix.base) || mayRunCode(*ix.index);
            }
            case Expr::Kind::Unary: return mayRunCode(*static_cast<const UnaryExpr&>(e).operand);
            case Expr::Kind::Binary: {
                auto& bin = static_cast<const BinaryExpr&>(e);
                return mayRunCode(*bin.lhs) || mayRunCode(*bin.rhs);
            }
            case Expr::Kind::Ternary: {
                auto& t = static_cast<const TernaryExpr&>(e);
                return mayRunCode(*t.cond) || mayRunCode(*t.thenExpr) || mayRunCode(*t.elseExpr);
            }
            case Expr::Kind::IncDec: return mayRunCode(*static_cast<const IncDecExpr&>(e).target);
            default: return true;
        }
    }

    // The address of a place. The object a field lies in is normally only kept alive
    // by its owner, which is fine while the address is used right away. With `hold`,
    // the object is retained until the statement ends, for an address that is used
    // while other code runs (the receiver of a struct method). With `write`, the place
    // is about to change, so each vector it lies in first gets a buffer of its own.
    llvm::Value* addressOf(const Expr& e, bool hold = false, bool write = false) {
        switch (e.kind) {
            case Expr::Kind::Name: {
                auto& n = static_cast<const NameExpr&>(e);
                if (n.implicitField >= 0) return fieldAddress(&fnDecl->owner->type, thisPtr, n.implicitField, n.name);
                return locals.at(n.decl);
            }
            case Expr::Kind::This: return thisPtr;
            case Expr::Kind::Member: {
                auto& m = static_cast<const MemberExpr&>(e);
                const Type* baseType = m.base->type;
                llvm::Value* object;
                if (!baseType->isClass()) object = addressOf(*m.base, hold, write);
                else if (hold || !isPlace(*m.base)) object = gen(*m.base);
                else object = b.CreateLoad(b.getPtrTy(), addressOf(*m.base), "obj");
                return fieldAddress(baseType, object, m.fieldIndex, m.field);
            }
            case Expr::Kind::Index: {
                auto& ix = static_cast<const IndexExpr&>(e);
                const Type* vt = ix.base->type;
                llvm::Value* slot = nullptr;
                llvm::Value* buf;
                llvm::Value* index;
                if (isPlace(*ix.base)) {
                    // The index is computed before the vector is read, so code it runs
                    // cannot leave `buf` stale. Meanwhile the vector's place is kept
                    // alive if that code could free it.
                    slot = addressOf(*ix.base, hold || mayRunCode(*ix.index), write);
                    index = genIndex(*ix.index);
                    buf = b.CreateLoad(b.getPtrTy(), slot, "vec");
                } else {
                    buf = gen(*ix.base);  // a temporary, kept until the statement ends
                    index = genIndex(*ix.index);
                }
                checkIndex(index, vecSize(buf), ix.loc);
                if (write) {
                    if (!slot) llvm_unreachable("Sema allowed changing an element of a temporary vector");
                    buf = reserve(slot, buf, vt, b.getInt64(0));
                }
                if (hold) {
                    b.CreateCall(rtRetain, {buf});
                    temps.push_back({buf, vt, false});
                }
                return elementAt(buf, index, vt);
            }
            default: llvm_unreachable("not a place");
        }
    }

    // Lowers `e`. `owned` is set when the result is a new value the caller must
    // release; otherwise a value holding class references is borrowed from a place.
    llvm::Value* genRaw(const Expr& e, bool& owned) {
        switch (e.kind) {
            case Expr::Kind::IntLit:
                return llvm::ConstantInt::getSigned(llvmType(e.type), static_cast<const IntLitExpr&>(e).value);
            case Expr::Kind::BoolLit: return b.getInt1(static_cast<const BoolLitExpr&>(e).value);
            case Expr::Kind::StringLit: return stringValue(static_cast<const StringLitExpr&>(e).value);
            case Expr::Kind::Name:
                return b.CreateLoad(llvmType(e.type), addressOf(e), static_cast<const NameExpr&>(e).name);
            case Expr::Kind::This:
                if (e.type->isClass()) return thisPtr;
                return b.CreateLoad(llvmType(e.type), thisPtr, "this");
            case Expr::Kind::Member: {
                auto& m = static_cast<const MemberExpr&>(e);
                if (isPlace(e)) return b.CreateLoad(llvmType(e.type), addressOf(e), m.field);
                // A field of a struct value that is not stored anywhere, e.g. makePoint().x
                return b.CreateExtractValue(gen(*m.base), {(unsigned)m.fieldIndex}, m.field);
            }
            case Expr::Kind::Index: return b.CreateLoad(llvmType(e.type), addressOf(e), "elem");
            case Expr::Kind::InitList:
                owned = true;
                return genInitList(static_cast<const InitListExpr&>(e), e.type);
            case Expr::Kind::Unary: {
                auto& u = static_cast<const UnaryExpr&>(e);
                if (u.op == UnOp::Not) return b.CreateNot(genCond(*u.operand));
                llvm::Value* v = gen(*u.operand);
                switch (u.op) {
                    case UnOp::Plus: return v;
                    case UnOp::Not:
                    case UnOp::BitNot: return b.CreateNot(v);
                    case UnOp::Neg:
                        return checkedArith(llvm::Intrinsic::ssub_with_overflow, llvm::ConstantInt::get(v->getType(), 0),
                                            v, "-", e.type, e.loc);
                }
                break;
            }
            case Expr::Kind::Binary: {
                auto& bin = static_cast<const BinaryExpr&>(e);
                if (bin.op == BinOp::LogAnd || bin.op == BinOp::LogOr) return genLogical(bin);
                if (!bin.operandType->isInteger()) {  // == or != on bools, strings, structs, objects
                    llvm::Value* l = gen(*bin.lhs);
                    llvm::Value* same = genEquals(l, gen(*bin.rhs), bin.operandType);
                    return bin.op == BinOp::Eq ? same : b.CreateNot(same);
                }
                llvm::Value* l = genConverted(*bin.lhs, bin.operandType);
                bool shift = bin.op == BinOp::Shl || bin.op == BinOp::Shr;
                llvm::Value* r = shift ? gen(*bin.rhs) : genConverted(*bin.rhs, bin.operandType);
                return arith(bin.op, l, r, bin.operandType, bin.loc);
            }
            case Expr::Kind::Assign: {
                auto& a = static_cast<const AssignExpr&>(e);
                const Type* ty = a.target->type;
                // As in C++17, the right side is evaluated before the left.
                if (a.op) {
                    bool shift = *a.op == BinOp::Shl || *a.op == BinOp::Shr;
                    llvm::Value* rhs = shift ? gen(*a.value) : genConverted(*a.value, ty);
                    llvm::Value* slot = addressOf(*a.target, false, true);
                    llvm::Value* v = arith(*a.op, b.CreateLoad(llvmType(ty), slot), rhs, ty, a.loc);
                    b.CreateStore(v, slot);
                    return v;
                }
                llvm::Value* v = genOwned(*a.value, ty);
                llvm::Value* slot = addressOf(*a.target, false, true);
                llvm::Value* old = isManaged(ty) ? b.CreateLoad(llvmType(ty), slot, "old") : nullptr;
                b.CreateStore(v, slot);
                if (old) releaseValue(old, ty);  // after the store, in case a destructor looks at the place
                return v;
            }
            case Expr::Kind::Ternary: {
                auto& t = static_cast<const TernaryExpr&>(e);
                llvm::Value* cond = genCond(*t.cond);
                auto* thenBB = llvm::BasicBlock::Create(ctx, "tern.then", fn);
                auto* elseBB = llvm::BasicBlock::Create(ctx, "tern.else", fn);
                auto* endBB = llvm::BasicBlock::Create(ctx, "tern.end", fn);
                b.CreateCondBr(cond, thenBB, elseBB);
                // Only one branch runs, so each releases its own temporaries, and a
                // value holding class references leaves its branch owned.
                bool managed = isManaged(e.type);
                auto branch = [&](const Expr& x, llvm::BasicBlock* bb) {
                    b.SetInsertPoint(bb);
                    size_t mark = temps.size();
                    llvm::Value* v = managed ? genOwned(x, e.type) : genConverted(x, e.type);
                    flushTemps(mark);
                    b.CreateBr(endBB);
                    return std::make_pair(v, b.GetInsertBlock());
                };
                auto [x, thenEnd] = branch(*t.thenExpr, thenBB);
                auto [y, elseEnd] = branch(*t.elseExpr, elseBB);
                b.SetInsertPoint(endBB);
                if (e.type->is(K::Void)) return nullptr;
                llvm::PHINode* phi = b.CreatePHI(llvmType(e.type), 2);
                phi->addIncoming(x, thenEnd);
                phi->addIncoming(y, elseEnd);
                owned = managed;
                return phi;
            }
            case Expr::Kind::IncDec: {
                auto& i = static_cast<const IncDecExpr&>(e);
                llvm::Value* slot = addressOf(*i.target, false, true);
                llvm::Type* t = llvmType(e.type);
                llvm::Value* old = b.CreateLoad(t, slot);
                llvm::Value* updated =
                    checkedArith(i.increment ? llvm::Intrinsic::sadd_with_overflow : llvm::Intrinsic::ssub_with_overflow,
                                 old, llvm::ConstantInt::get(t, 1), i.increment ? "++" : "--", e.type, e.loc);
                b.CreateStore(updated, slot);
                return i.prefix ? updated : old;
            }
            case Expr::Kind::Call: owned = true; return genCall(static_cast<const CallExpr&>(e));
            case Expr::Kind::MethodCall: owned = true; return genMethodCall(static_cast<const MethodCallExpr&>(e));
        }
        llvm_unreachable("unknown expression kind");
    }

    // Whether two values are equal: strings by contents, structs field by field, and
    // class references by identity.
    llvm::Value* genEquals(llvm::Value* l, llvm::Value* r, const Type* t) {
        switch (t->kind) {
            case K::Bool: case K::Int: case K::Long: case K::Class: return b.CreateICmpEQ(l, r);
            case K::String: {
                llvm::Value* eq = b.CreateCall(rtStrEq, {b.CreateExtractValue(l, 0), b.CreateExtractValue(l, 1),
                                                         b.CreateExtractValue(r, 0), b.CreateExtractValue(r, 1)});
                return b.CreateICmpNE(eq, b.getInt32(0));
            }
            case K::Struct: return b.CreateCall(types.at(t->decl).equals, {l, r});
            case K::Vector: return b.CreateCall(vecInfo(t).equals, {l, r});
            default: llvm_unreachable("Sema allowed == on a type Codegen cannot compare");
        }
    }

    llvm::Value* genLogical(const BinaryExpr& bin) {
        bool isAnd = bin.op == BinOp::LogAnd;
        llvm::Value* l = genCond(*bin.lhs);
        llvm::BasicBlock* lhsEnd = b.GetInsertBlock();
        auto* rhsBB = llvm::BasicBlock::Create(ctx, isAnd ? "and.rhs" : "or.rhs", fn);
        auto* endBB = llvm::BasicBlock::Create(ctx, isAnd ? "and.end" : "or.end", fn);
        if (isAnd) b.CreateCondBr(l, rhsBB, endBB);
        else b.CreateCondBr(l, endBB, rhsBB);
        b.SetInsertPoint(rhsBB);
        size_t mark = temps.size();  // the right side may not run, so it cleans up after itself
        llvm::Value* r = genCond(*bin.rhs);
        flushTemps(mark);
        llvm::BasicBlock* rhsEnd = b.GetInsertBlock();
        b.CreateBr(endBB);
        b.SetInsertPoint(endBB);
        llvm::PHINode* phi = b.CreatePHI(b.getInt1Ty(), 2);
        phi->addIncoming(b.getInt1(!isAnd), lhsEnd);
        phi->addIncoming(r, rhsEnd);
        return phi;
    }

    void printText(const std::string& text) {
        if (!text.empty()) b.CreateCall(rtPrintStr, {cstring(text), b.getInt64(text.size())});
    }

    // Strings print as-is at the top level and in quotes inside a struct or class, so
    // `{name: "Ada", age: 36}` stays readable.
    void printValue(llvm::Value* v, const Type* t, bool nested = false) {
        switch (t->kind) {
            case K::Int: b.CreateCall(rtPrintI32, {v}); break;
            case K::Long: b.CreateCall(rtPrintI64, {v}); break;
            case K::Bool: b.CreateCall(rtPrintBool, {b.CreateZExt(v, b.getInt32Ty())}); break;
            case K::String:
                b.CreateCall(nested ? rtPrintQuoted : rtPrintStr, {b.CreateExtractValue(v, 0), b.CreateExtractValue(v, 1)});
                break;
            case K::Struct:
            case K::Class: b.CreateCall(types.at(t->decl).print, {v}); break;
            case K::Vector: b.CreateCall(vecInfo(t).print, {v}); break;
            default: llvm_unreachable("unprintable type reached codegen");
        }
    }

    llvm::Value* genCall(const CallExpr& call) {
        if (call.vectorType) return genVectorConstruction(call);
        if (call.builtin != Builtin::None) {
            // Arguments are evaluated left to right before anything is printed.
            std::vector<llvm::Value*> values;
            for (size_t i = 1; i < call.args.size(); ++i) values.push_back(gen(*call.args[i]));
            for (size_t i = 0; i < call.formatPieces.size(); ++i) {
                printText(call.formatPieces[i]);
                if (i < values.size()) printValue(values[i], call.args[i + 1]->type);
            }
            if (call.builtin == Builtin::Println) b.CreateCall(rtPrintNewline, {});
            return nullptr;
        }
        if (call.constructs) {  // `Point(1, 2)`
            const StructDecl& sd = *call.constructs;
            if (sd.constructor) {
                std::vector<const Expr*> args;
                for (auto& a : call.args) args.push_back(a.get());
                return construct(sd, args);
            }
            std::vector<llvm::Value*> given;
            for (size_t i = 0; i < call.args.size(); ++i) given.push_back(genOwned(*call.args[i], sd.fields[i].type));
            return buildAggregate(sd, std::move(given));
        }
        // A function, or a method called by name inside another method of the same type.
        const FuncDecl* target = call.target ? call.target : call.method;
        std::vector<llvm::Value*> args;
        if (call.method) args.push_back(thisPtr);
        for (size_t i = 0; i < call.args.size(); ++i) args.push_back(genOwned(*call.args[i], target->params[i]->type));
        llvm::CallInst* inst = b.CreateCall(funcs.at(target), args);
        return target->retType->is(K::Void) ? nullptr : inst;
    }

    llvm::Value* genMethodCall(const MethodCallExpr& call) {
        const Expr& recv = *call.receiver;
        if (recv.type->isVector()) return genVectorMethod(call);
        llvm::Value* self;
        if (recv.type->isClass()) {
            self = gen(recv);
        } else if (isPlace(recv)) {
            // A struct method may change the struct in place.
            self = addressOf(recv, /*hold=*/true, /*write=*/!call.method->isConst);
        } else {
            // A struct value that is not stored anywhere, e.g. makePoint().move(1): the
            // method works on a temporary, which is released when the statement ends.
            llvm::Value* v = genOwned(recv, recv.type);
            llvm::AllocaInst* slot = entryAlloca(llvmType(recv.type), "tmp");
            b.CreateStore(v, slot);
            if (isManaged(recv.type)) temps.push_back({slot, recv.type, true});
            self = slot;
        }
        std::vector<llvm::Value*> args{self};
        for (size_t i = 0; i < call.args.size(); ++i)
            args.push_back(genOwned(*call.args[i], call.method->params[i]->type));
        llvm::CallInst* inst = b.CreateCall(funcs.at(call.method), args);
        return call.method->retType->is(K::Void) ? nullptr : inst;
    }

    // ---- Statements --------------------------------------------------------

    // After return/break/continue, code that follows is unreachable but must still
    // go somewhere, so it gets a fresh block with no predecessors.
    void startDeadBlock() { b.SetInsertPoint(llvm::BasicBlock::Create(ctx, "dead", fn)); }

    llvm::AllocaInst* allocaFor(const VarDecl& v) {
        llvm::AllocaInst* slot = entryAlloca(llvmType(v.type), v.name);
        locals[&v] = slot;
        return slot;
    }

    // A statement in its own scope, so locals it declares are released when it ends,
    // even without braces: `if (c) User u = ...;`.
    void genScoped(const Stmt& s) {
        if (s.kind == Stmt::Kind::Block) return genStmt(s);
        scopes.emplace_back();
        genStmt(s);
        popScope();
    }

    void genBody(const Stmt& body, llvm::BasicBlock* breakTo, llvm::BasicBlock* continueTo) {
        loops.push_back({breakTo, continueTo, scopes.size()});
        genScoped(body);
        loops.pop_back();
    }

    void genStmt(const Stmt& s) {
        switch (s.kind) {
            case Stmt::Kind::VarDecl: {
                auto& v = static_cast<const VarDecl&>(s);
                llvm::Value* init = v.init ? genOwned(*v.init, v.type)
                                    : v.type->is(K::String) || v.type->hasFields() || v.type->isVector()
                                        ? genDefault(v.type)
                                        : nullptr;
                flushTemps();
                llvm::AllocaInst* slot = allocaFor(v);
                if (init) b.CreateStore(init, slot);
                if (isManaged(v.type)) scopes.back().push_back(&v);
                return;
            }
            case Stmt::Kind::DeclGroup:
                for (auto& v : static_cast<const DeclGroupStmt&>(s).decls) genStmt(*v);
                return;
            case Stmt::Kind::Expr: genDiscarded(*static_cast<const ExprStmt&>(s).expr); return;
            case Stmt::Kind::Block:
                scopes.emplace_back();
                for (auto& st : static_cast<const BlockStmt&>(s).stmts) genStmt(*st);
                popScope();
                return;
            case Stmt::Kind::If: {
                auto& st = static_cast<const IfStmt&>(s);
                auto* thenBB = llvm::BasicBlock::Create(ctx, "if.then", fn);
                auto* endBB = llvm::BasicBlock::Create(ctx, "if.end", fn);
                auto* elseBB = st.elseStmt ? llvm::BasicBlock::Create(ctx, "if.else", fn) : endBB;
                llvm::Value* cond = genCond(*st.cond);
                flushTemps();
                b.CreateCondBr(cond, thenBB, elseBB);
                b.SetInsertPoint(thenBB);
                genScoped(*st.thenStmt);
                b.CreateBr(endBB);
                if (st.elseStmt) {
                    b.SetInsertPoint(elseBB);
                    genScoped(*st.elseStmt);
                    b.CreateBr(endBB);
                }
                b.SetInsertPoint(endBB);
                return;
            }
            case Stmt::Kind::While: {
                auto& st = static_cast<const WhileStmt&>(s);
                auto* condBB = llvm::BasicBlock::Create(ctx, "while.cond", fn);
                auto* bodyBB = llvm::BasicBlock::Create(ctx, "while.body", fn);
                auto* endBB = llvm::BasicBlock::Create(ctx, "while.end", fn);
                b.CreateBr(condBB);
                b.SetInsertPoint(condBB);
                llvm::Value* cond = genCond(*st.cond);
                flushTemps();
                b.CreateCondBr(cond, bodyBB, endBB);
                b.SetInsertPoint(bodyBB);
                genBody(*st.body, endBB, condBB);
                b.CreateBr(condBB);
                b.SetInsertPoint(endBB);
                return;
            }
            case Stmt::Kind::DoWhile: {
                auto& st = static_cast<const DoWhileStmt&>(s);
                auto* bodyBB = llvm::BasicBlock::Create(ctx, "do.body", fn);
                auto* condBB = llvm::BasicBlock::Create(ctx, "do.cond", fn);
                auto* endBB = llvm::BasicBlock::Create(ctx, "do.end", fn);
                b.CreateBr(bodyBB);
                b.SetInsertPoint(bodyBB);
                genBody(*st.body, endBB, condBB);
                b.CreateBr(condBB);
                b.SetInsertPoint(condBB);
                llvm::Value* cond = genCond(*st.cond);
                flushTemps();
                b.CreateCondBr(cond, bodyBB, endBB);
                b.SetInsertPoint(endBB);
                return;
            }
            case Stmt::Kind::For: {
                auto& st = static_cast<const ForStmt&>(s);
                scopes.emplace_back();  // for the variables declared in the init part
                if (st.init) genStmt(*st.init);
                auto* condBB = llvm::BasicBlock::Create(ctx, "for.cond", fn);
                auto* bodyBB = llvm::BasicBlock::Create(ctx, "for.body", fn);
                auto* stepBB = llvm::BasicBlock::Create(ctx, "for.step", fn);
                auto* endBB = llvm::BasicBlock::Create(ctx, "for.end", fn);
                b.CreateBr(condBB);
                b.SetInsertPoint(condBB);
                if (st.cond) {
                    llvm::Value* cond = genCond(*st.cond);
                    flushTemps();
                    b.CreateCondBr(cond, bodyBB, endBB);
                } else {
                    b.CreateBr(bodyBB);
                }
                b.SetInsertPoint(bodyBB);
                genBody(*st.body, endBB, stepBB);
                b.CreateBr(stepBB);
                b.SetInsertPoint(stepBB);
                for (auto& step : st.steps) genDiscarded(*step);
                b.CreateBr(condBB);
                b.SetInsertPoint(endBB);
                popScope();
                return;
            }
            case Stmt::Kind::ForEach: genForEach(static_cast<const ForEachStmt&>(s)); return;
            case Stmt::Kind::Return: {
                auto& st = static_cast<const ReturnStmt&>(s);
                llvm::Value* v = st.value ? genOwned(*st.value, fnDecl->retType) : nullptr;
                flushTemps();
                releaseScopes(0);
                if (v) b.CreateRet(v);
                else b.CreateRetVoid();
                startDeadBlock();
                return;
            }
            case Stmt::Kind::Break:
                releaseScopes(loops.back().scopeDepth);
                b.CreateBr(loops.back().breakTo);
                startDeadBlock();
                return;
            case Stmt::Kind::Continue:
                releaseScopes(loops.back().scopeDepth);
                b.CreateBr(loops.back().continueTo);
                startDeadBlock();
                return;
            case Stmt::Kind::Error: llvm_unreachable("a program with parse errors reached codegen");
        }
    }

    // The loop goes over the vector as it was when the loop started: it holds its own
    // reference, so a change made in the body copies the vector instead of moving the
    // elements under the loop.
    void genForEach(const ForEachStmt& st) {
        const Type* vt = st.rangeVar->type;
        const VarDecl& var = *st.var;
        scopes.emplace_back();  // holds the vector
        llvm::Value* range = genOwned(*st.range, vt);
        flushTemps();
        b.CreateStore(range, allocaFor(*st.rangeVar));
        scopes.back().push_back(st.rangeVar.get());
        llvm::Value* size = vecSize(range);
        llvm::AllocaInst* counter = entryAlloca(b.getInt64Ty(), "index");
        b.CreateStore(b.getInt64(0), counter);
        auto* condBB = llvm::BasicBlock::Create(ctx, "foreach.cond", fn);
        auto* bodyBB = llvm::BasicBlock::Create(ctx, "foreach.body", fn);
        auto* stepBB = llvm::BasicBlock::Create(ctx, "foreach.step", fn);
        auto* endBB = llvm::BasicBlock::Create(ctx, "foreach.end", fn);
        b.CreateBr(condBB);
        b.SetInsertPoint(condBB);
        llvm::Value* i = b.CreateLoad(b.getInt64Ty(), counter, "index");
        b.CreateCondBr(b.CreateICmpSLT(i, size), bodyBB, endBB);

        b.SetInsertPoint(bodyBB);
        loops.push_back({endBB, stepBB, scopes.size()});
        scopes.emplace_back();  // the loop variable: a new one for each element
        llvm::Value* elem = b.CreateLoad(vecInfo(vt).elemTy, elementAt(range, i, vt), "elem");
        llvm::Value* value = convert(elem, vt->elem, var.type);
        retainValue(value, var.type);
        b.CreateStore(value, allocaFor(var));
        if (isManaged(var.type)) scopes.back().push_back(&var);
        genScoped(*st.body);
        popScope();
        loops.pop_back();
        b.CreateBr(stepBB);

        b.SetInsertPoint(stepBB);
        b.CreateStore(b.CreateAdd(i, b.getInt64(1)), counter);
        b.CreateBr(condBB);
        b.SetInsertPoint(endBB);
        popScope();
    }

    // ---- Functions ---------------------------------------------------------

    void declareFunction(const FuncDecl& f) {
        std::vector<llvm::Type*> params;
        if (f.owner) params.push_back(b.getPtrTy());  // `this`
        for (auto& p : f.params) params.push_back(llvmType(p->type));
        auto* type = llvm::FunctionType::get(llvmType(f.retType), params, false);
        // User functions get a prefixed, internal symbol so they never collide with C
        // library names; `main` is wrapped by a C-ABI entry point below. Generated
        // functions use '#', which no user name contains.
        std::string name = "cplus.";
        switch (f.role) {
            case FuncDecl::Role::Free: name += f.name; break;
            case FuncDecl::Role::Method: name += f.owner->name + "." + f.name; break;
            case FuncDecl::Role::Constructor: name += f.owner->name + "#init"; break;
            case FuncDecl::Role::Destructor: name += f.owner->name + "#deinit"; break;
        }
        auto* func = llvm::Function::Create(type, llvm::Function::InternalLinkage, name, mod.get());
        unsigned first = f.owner ? 1 : 0;
        if (f.owner) func->getArg(0)->setName("this");
        for (size_t i = 0; i < f.params.size(); ++i) func->getArg(first + i)->setName(f.params[i]->name);
        funcs[&f] = func;
    }

    // Before a constructor's body runs, each field gets its value from the initializer
    // list, its default, or zero, in the order the fields are declared.
    void genFieldInits(const FuncDecl& ctor) {
        const StructDecl& sd = *ctor.owner;
        std::vector<const Expr*> inits(sd.fields.size(), nullptr);
        for (auto& init : ctor.memberInits)
            if (init.fieldIndex >= 0) inits[init.fieldIndex] = init.value.get();
        for (size_t i = 0; i < sd.fields.size(); ++i) {
            const FieldDecl& f = sd.fields[i];
            llvm::Value* v = inits[i] ? genOwned(*inits[i], f.type)
                             : f.defaultValue ? genOwned(*f.defaultValue, f.type)
                                              : genDefault(f.type);
            flushTemps();
            b.CreateStore(v, fieldAddress(&sd.type, thisPtr, (int)i, f.name));
        }
    }

    void defineFunction(const FuncDecl& f) {
        fn = funcs.at(&f);
        fnDecl = &f;
        locals.clear();
        b.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", fn));
        unsigned first = f.owner ? 1 : 0;
        thisPtr = f.owner ? fn->getArg(0) : nullptr;
        // Parameters own their values, like locals, and are released when the function ends.
        scopes.emplace_back();
        for (size_t i = 0; i < f.params.size(); ++i) {
            b.CreateStore(fn->getArg(first + i), allocaFor(*f.params[i]));
            if (isManaged(f.params[i]->type)) scopes.back().push_back(f.params[i].get());
        }
        if (f.role == FuncDecl::Role::Constructor) genFieldInits(f);
        genStmt(*f.body);
        if (!b.GetInsertBlock()->getTerminator()) {
            releaseScopes(0);
            if (f.retType->is(K::Void)) b.CreateRetVoid();
            else if (f.name == "main" && f.role == FuncDecl::Role::Free)
                b.CreateRet(b.getInt32(0));  // falling off main returns 0, as in C++
            else b.CreateUnreachable();       // Sema proved this is never reached
        }
        scopes.clear();
    }

    void defineEntryPoint(const FuncDecl& userMain) {
        auto* type = llvm::FunctionType::get(b.getInt32Ty(), {b.getInt32Ty(), b.getPtrTy()}, false);
        auto* entry = llvm::Function::Create(type, llvm::Function::ExternalLinkage, "main", mod.get());
        b.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", entry));
        b.CreateRet(b.CreateCall(funcs.at(&userMain), {}));
    }

    // ---- Generated functions for structs and classes -----------------------

    llvm::Function* helper(const StructDecl& sd, const char* what, llvm::Type* ret, std::vector<llvm::Type*> params) {
        return llvm::Function::Create(llvm::FunctionType::get(ret, params, false), llvm::Function::InternalLinkage,
                                      "cplus." + sd.name + "#" + what, mod.get());
    }

    void declareHelpers(const StructDecl& sd) {
        TypeInfo& info = types.at(&sd);
        llvm::Type* self = llvmType(&sd.type);
        info.print = helper(sd, "print", b.getVoidTy(), {self});
        if (!sd.isClass) info.equals = helper(sd, "equals", b.getInt1Ty(), {self, self});
        if (sd.isClass) {
            info.release = helper(sd, "release", b.getVoidTy(), {self});
            info.destroy = helper(sd, "destroy", b.getVoidTy(), {self});
        } else if (isManaged(&sd.type)) {
            info.retain = helper(sd, "retain", b.getVoidTy(), {self});
            info.release = helper(sd, "release", b.getVoidTy(), {self});
        }
    }

    void startHelper(llvm::Function* f) {
        fn = f;
        b.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", f));
    }

    // Reads field i of a struct value or class object.
    llvm::Value* loadField(const StructDecl& sd, llvm::Value* self, size_t i) {
        if (!sd.isClass) return b.CreateExtractValue(self, {(unsigned)i}, sd.fields[i].name);
        return b.CreateLoad(llvmType(sd.fields[i].type), fieldAddress(&sd.type, self, (int)i, sd.fields[i].name),
                            sd.fields[i].name);
    }

    void defineHelpers(const StructDecl& sd) {
        TypeInfo& info = types.at(&sd);

        // Printing: `{x: 1, y: 2}` for a struct, `User {name: "Ada", age: 36}` for a class.
        startHelper(info.print);
        llvm::Value* self = info.print->getArg(0);
        if (sd.isClass) {
            auto* bodyBB = llvm::BasicBlock::Create(ctx, "print", fn);
            auto* againBB = llvm::BasicBlock::Create(ctx, "seen", fn);
            b.CreateCondBr(b.CreateICmpNE(b.CreateCall(rtPrintEnter, {self}), b.getInt32(0)), bodyBB, againBB);
            b.SetInsertPoint(againBB);
            printText(sd.name + " {...}");  // already being printed: it refers back to itself
            b.CreateRetVoid();
            b.SetInsertPoint(bodyBB);
            printText(sd.name + " ");
        }
        printText("{");
        for (size_t i = 0; i < sd.fields.size(); ++i) {
            printText((i ? ", " : "") + sd.fields[i].name + ": ");
            printValue(loadField(sd, self, i), sd.fields[i].type, true);
        }
        printText("}");
        if (sd.isClass) b.CreateCall(rtPrintLeave, {});
        b.CreateRetVoid();

        if (!sd.isClass) {
            // Equality: the first field that differs decides.
            startHelper(info.equals);
            auto* differBB = llvm::BasicBlock::Create(ctx, "differ", fn);
            for (size_t i = 0; i < sd.fields.size(); ++i) {
                llvm::Value* same = genEquals(loadField(sd, info.equals->getArg(0), i),
                                              loadField(sd, info.equals->getArg(1), i), sd.fields[i].type);
                auto* nextBB = llvm::BasicBlock::Create(ctx, "same", fn);
                b.CreateCondBr(same, nextBB, differBB);
                b.SetInsertPoint(nextBB);
            }
            b.CreateRet(b.getInt1(true));
            b.SetInsertPoint(differBB);
            b.CreateRet(b.getInt1(false));

            if (!info.retain) return;
            for (auto [f, which] : {std::pair{info.retain, true}, std::pair{info.release, false}}) {
                startHelper(f);
                for (size_t i = sd.fields.size(); i-- > 0;) {
                    if (!isManaged(sd.fields[i].type)) continue;
                    llvm::Value* v = loadField(sd, f->getArg(0), i);
                    which ? retainValue(v, sd.fields[i].type) : releaseValue(v, sd.fields[i].type);
                }
                b.CreateRetVoid();
            }
            return;
        }

        // Release: destroy the object when the last reference goes away.
        startHelper(info.release);
        self = info.release->getArg(0);
        auto* destroyBB = llvm::BasicBlock::Create(ctx, "destroy", fn);
        auto* doneBB = llvm::BasicBlock::Create(ctx, "done", fn);
        b.CreateCondBr(b.CreateICmpNE(b.CreateCall(rtRelease, {self}), b.getInt32(0)), destroyBB, doneBB);
        b.SetInsertPoint(destroyBB);
        b.CreateCall(info.destroy, {self});
        b.CreateBr(doneBB);
        b.SetInsertPoint(doneBB);
        b.CreateRetVoid();

        // Destroy: run the destructor, release the fields (last declared first, as in
        // C++), and free the memory.
        startHelper(info.destroy);
        self = info.destroy->getArg(0);
        if (sd.destructor) {
            // While the destructor runs, the object counts as having one reference, so
            // passing `this` to a function cannot destroy it a second time.
            llvm::Value* count = b.CreateStructGEP(info.layout, self, 0, "count");
            b.CreateStore(b.getInt64(1), count);
            b.CreateCall(funcs.at(sd.destructor), {self});
            trapIf(b.CreateICmpNE(b.CreateLoad(b.getInt64Ty(), count), b.getInt64(1)),
                   "the destructor of '" + sd.name + "' stored 'this' somewhere, so the object would be used after it is freed",
                   sd.destructor->nameLoc);
        }
        for (size_t i = sd.fields.size(); i-- > 0;)
            if (isManaged(sd.fields[i].type)) releaseValue(loadField(sd, self, i), sd.fields[i].type);
        b.CreateCall(rtFree, {self});
        b.CreateRetVoid();
    }

    bool generate(const Program& program, std::string& error) {
        mod = std::make_unique<llvm::Module>(file.path, ctx);
        if (!setupTarget(error)) return false;
        stringTy = llvm::StructType::create(ctx, {b.getPtrTy(), b.getInt64Ty()}, "cplus.string");
        // Named types are created first and given bodies after, so they can refer to
        // each other in any order.
        for (auto& sd : program.structs)
            types[sd.get()].layout = llvm::StructType::create(ctx, (sd->isClass ? "cplus.class." : "cplus.struct.") + sd->name);
        for (auto& sd : program.structs) {
            std::vector<llvm::Type*> fields;
            if (sd->isClass) fields.push_back(b.getInt64Ty());  // the reference count
            for (auto& f : sd->fields) fields.push_back(llvmType(f.type));
            types[sd.get()].layout->setBody(fields);
        }
        declareRuntime();
        fileName = cstring(file.path);

        const FuncDecl* userMain = nullptr;
        for (auto& f : program.funcs) {
            declareFunction(*f);
            if (f->name == "main") userMain = f.get();
        }
        for (auto& sd : program.structs) {
            for (auto& m : sd->methods) declareFunction(*m);
            declareHelpers(*sd);
        }
        for (auto& sd : program.structs) defineHelpers(*sd);
        for (auto& f : program.funcs) defineFunction(*f);
        for (auto& sd : program.structs)
            for (auto& m : sd->methods) defineFunction(*m);
        if (userMain) defineEntryPoint(*userMain);

        std::string verifyErrors;
        llvm::raw_string_ostream os(verifyErrors);
        if (llvm::verifyModule(*mod, &os)) {
            error = "internal compiler error: generated invalid LLVM IR\n" + os.str();
            return false;
        }
        if (opts.optimize) optimize();
        return true;
    }

    void optimize() {
        llvm::LoopAnalysisManager lam;
        llvm::FunctionAnalysisManager fam;
        llvm::CGSCCAnalysisManager cgam;
        llvm::ModuleAnalysisManager mam;
        llvm::PassBuilder pb(tm.get());
        pb.registerModuleAnalyses(mam);
        pb.registerCGSCCAnalyses(cgam);
        pb.registerFunctionAnalyses(fam);
        pb.registerLoopAnalyses(lam);
        pb.crossRegisterProxies(lam, fam, cgam, mam);
        pb.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2).run(*mod, mam);
    }
};

Codegen::Codegen(const SourceFile& file, CodegenOptions options) : impl_(std::make_unique<Impl>(file, options)) {}
Codegen::~Codegen() = default;

bool Codegen::generate(const Program& program, std::string& error) { return impl_->generate(program, error); }

std::string Codegen::llvmIR() const {
    std::string out;
    llvm::raw_string_ostream os(out);
    impl_->mod->print(os, nullptr);
    return os.str();
}

bool Codegen::writeObjectFile(const std::string& path, std::string& error) {
    std::error_code ec;
    llvm::raw_fd_ostream out(path, ec, llvm::sys::fs::OF_None);
    if (ec) {
        error = "cannot write " + path + ": " + ec.message();
        return false;
    }
    llvm::legacy::PassManager pm;
    if (impl_->tm->addPassesToEmitFile(pm, out, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        error = "this target cannot emit object files";
        return false;
    }
    pm.run(*impl_->mod);
    out.flush();
    return true;
}

}  // namespace cplus
