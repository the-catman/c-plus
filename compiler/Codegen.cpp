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
#include <llvm/TargetParser/Triple.h>

#include <algorithm>
#include <functional>
#include <set>
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

    // A string is { ptr data, i64 length, ptr owner }, as the runtime's cplus_string.
    // `owner` is the reference-counted buffer holding the bytes, or null for a literal.
    llvm::StructType* stringTy = nullptr;
    llvm::FunctionCallee rtPrintStr, rtPrintQuoted, rtPrintI32, rtPrintI64, rtPrintBool, rtPrintNewline, rtTrap;
    llvm::FunctionCallee rtAlloc, rtFree, rtRetain, rtRelease, rtPrintEnter, rtPrintLeave, rtStrEq;
    llvm::FunctionCallee rtTrapIndex, rtVecNew, rtVecRelease, rtVecReserve;
    llvm::FunctionCallee rtStrRelease, rtStrCompare, rtStrConcat, rtStrAppend, rtStrSubstr, rtStrFind;
    llvm::FunctionCallee rtStrStartsWith, rtStrEndsWith, rtCString, rtCFree, rtStrFromC;
    llvm::FunctionCallee rtPrintF32, rtPrintF64, rtPrintFloatSpec, rtTrapNarrow, rtTrapFloatCast;
    llvm::FunctionCallee rtPrintCharQuoted, rtTrapChar, rtTrapStringIndex, rtStrMutable, rtStrRepeat;
    llvm::GlobalVariable* charTable = nullptr;  // the 256 bytes, so a char can be a one-byte string
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

    // Inside the rest of a `?.` chain, the value found in the optional.
    std::unordered_map<const OptValueExpr*, llvm::Value*> optValues;

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
        // The print functions write to the string their first argument points to, or
        // to standard output when it is null.
        rtPrintStr = decl("cplus_print_str", voidTy, {ptrTy, ptrTy, i64});
        rtPrintQuoted = decl("cplus_print_quoted", voidTy, {ptrTy, ptrTy, i64});
        rtPrintI32 = decl("cplus_print_i32", voidTy, {ptrTy, i32});
        rtPrintI64 = decl("cplus_print_i64", voidTy, {ptrTy, i64});
        rtPrintBool = decl("cplus_print_bool", voidTy, {ptrTy, i32});
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
        rtStrRelease = decl("cplus_str_release", voidTy, {ptrTy});
        rtStrCompare = decl("cplus_str_compare", i32, {ptrTy, i64, ptrTy, i64});
        rtStrConcat = decl("cplus_str_concat", voidTy, {ptrTy, ptrTy, i64});
        rtStrAppend = decl("cplus_str_append", voidTy, {ptrTy, ptrTy, i64});
        rtStrSubstr = decl("cplus_str_substr", voidTy, {ptrTy, ptrTy, i64, i64, ptrTy, i32, i32});
        rtStrFind = decl("cplus_str_find", i64, {ptrTy, i64, ptrTy, i64, i64});
        rtStrStartsWith = decl("cplus_str_starts_with", i32, {ptrTy, i64, ptrTy, i64});
        rtStrEndsWith = decl("cplus_str_ends_with", i32, {ptrTy, i64, ptrTy, i64});
        rtPrintF32 = decl("cplus_print_f32", voidTy, {ptrTy, b.getFloatTy()});
        rtPrintF64 = decl("cplus_print_f64", voidTy, {ptrTy, b.getDoubleTy()});
        rtPrintFloatSpec = decl("cplus_print_float_spec", voidTy, {ptrTy, b.getDoubleTy(), i32, i32});
        rtTrapNarrow = decl("cplus_trap_narrow", voidTy, {i64, ptrTy, i32, i32});
        rtTrapFloatCast = decl("cplus_trap_float_cast", voidTy, {b.getDoubleTy(), ptrTy, ptrTy, i32, i32});
        rtCString = decl("cplus_c_string", ptrTy, {ptrTy, i64, i32, ptrTy, ptrTy, i32, i32});
        rtCFree = decl("cplus_c_free", voidTy, {ptrTy});
        rtStrFromC = decl("cplus_str_from_c", voidTy, {ptrTy, ptrTy});
        rtTrapIndex = decl("cplus_trap_index", voidTy, {i64, i64, ptrTy, i32, i32});
        rtTrapStringIndex = decl("cplus_trap_string_index", voidTy, {i64, i64, ptrTy, i32, i32});
        rtTrapChar = decl("cplus_trap_char", voidTy, {i64, ptrTy, ptrTy, i32, i32});
        for (llvm::FunctionCallee trap : {rtTrapIndex, rtTrapStringIndex, rtTrapChar})
            if (auto* f = llvm::dyn_cast<llvm::Function>(trap.getCallee())) {
                f->addFnAttr(llvm::Attribute::NoReturn);
                f->addFnAttr(llvm::Attribute::Cold);
                f->addFnAttr(llvm::Attribute::NoUnwind);
            }
        rtPrintCharQuoted = decl("cplus_print_char_quoted", voidTy, {ptrTy, i32});
        rtStrMutable = decl("cplus_str_mutable", ptrTy, {ptrTy});
        rtStrRepeat = decl("cplus_str_repeat", voidTy, {ptrTy, i64, i32, ptrTy, i32, i32});
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
            case K::Float: return b.getFloatTy();
            case K::Double: return b.getDoubleTy();
            case K::Char: return b.getInt8Ty();
            case K::String: return stringTy;
            case K::Struct: return types.at(t->decl).layout;
            case K::Class: case K::Vector: case K::Null: return b.getPtrTy();
            // An optional class reference is a pointer that may be null. Any other
            // optional is { i1 has, T value }, and its value is all zero bytes when it
            // has none, so retaining or releasing it is harmless either way.
            case K::Optional:
                if (t->elem->isClass()) return b.getPtrTy();
                return llvm::StructType::get(ctx, {b.getInt1Ty(), llvmType(t->elem)});
            default: llvm_unreachable("unresolved type reached codegen");
        }
    }

    // Whether values of this type hold references to heap objects (class objects,
    // vector buffers, string buffers), which are retained when the value is copied and
    // released when it goes away.
    bool isManaged(const Type* t) {
        if (t->isClass() || t->isVector() || t->is(K::String)) return true;
        if (t->isOptional()) return isManaged(t->elem);
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
        return llvm::ConstantStruct::get(stringTy, {data, b.getInt64(s.size()), llvm::ConstantPointerNull::get(b.getPtrTy())});
    }

    llvm::Value* strData(llvm::Value* s) { return b.CreateExtractValue(s, 0, "data"); }

    // Where the byte `c` lies in a table of all 256, so that a char can be passed where
    // text is expected, like a one-byte string literal.
    llvm::Value* charBytes(llvm::Value* c) {
        if (!charTable) {
            std::vector<uint8_t> bytes(256);
            for (int i = 0; i < 256; ++i) bytes[i] = (uint8_t)i;
            auto* init = llvm::ConstantDataArray::get(ctx, bytes);
            charTable = new llvm::GlobalVariable(*mod, init->getType(), true, llvm::GlobalValue::PrivateLinkage, init,
                                                 "cplus.chars");
            charTable->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        }
        return b.CreateInBoundsGEP(b.getInt8Ty(), charTable, b.CreateZExt(c, b.getInt64Ty()), "char");
    }

    // A char as a string of one byte, which needs no buffer of its own.
    llvm::Value* charString(llvm::Value* c) {
        llvm::Value* s = b.CreateInsertValue(llvm::Constant::getNullValue(stringTy), charBytes(c), {0});
        return b.CreateInsertValue(s, b.getInt64(1), {1});
    }

    // A string, or a char made into one: what the text arguments of string methods take.
    llvm::Value* textValue(llvm::Value* v, const Type* t) { return t->is(K::Char) ? charString(v) : v; }
    llvm::Value* strLength(llvm::Value* s) { return b.CreateExtractValue(s, 1, "length"); }

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
    void checkIndex(llvm::Value* index, llvm::Value* size, Loc loc, bool inString = false) {
        auto* trapBB = llvm::BasicBlock::Create(ctx, "bad.index", fn);
        auto* okBB = llvm::BasicBlock::Create(ctx, "ok", fn);
        // An unsigned compare also catches negative indexes.
        b.CreateCondBr(b.CreateICmpUGE(index, size), trapBB, okBB, llvm::MDBuilder(ctx).createBranchWeights(1, 1u << 20));
        b.SetInsertPoint(trapBB);
        auto* call = b.CreateCall(inString ? rtTrapStringIndex : rtTrapIndex,
                                  {index, size, fileName, b.getInt32(loc.line), b.getInt32(loc.col)});
        call->setDoesNotReturn();
        b.CreateUnreachable();
        b.SetInsertPoint(okBB);
    }

    // The char a computed code stands for. `wide` is an i64; a value that is not from 0
    // to 255 stops the program, saying `what` gave it.
    llvm::Value* checkedChar(llvm::Value* wide, const std::string& what, Loc loc) {
        auto* trapBB = llvm::BasicBlock::Create(ctx, "char.trap", fn);
        auto* okBB = llvm::BasicBlock::Create(ctx, "char.ok", fn);
        b.CreateCondBr(b.CreateICmpUGT(wide, b.getInt64(255)), trapBB, okBB,  // also catches negative values
                       llvm::MDBuilder(ctx).createBranchWeights(1, 1u << 20));
        b.SetInsertPoint(trapBB);
        b.CreateCall(rtTrapChar, {wide, cstring(what), fileName, b.getInt32(loc.line), b.getInt32(loc.col)});
        b.CreateUnreachable();
        b.SetInsertPoint(okBB);
        return b.CreateTrunc(wide, b.getInt8Ty(), "char");
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
        if (ty->isFloating()) return floatArith(op, l, r);
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

    // IEEE arithmetic, as in C++: dividing by zero gives an infinity and overflow gives
    // an infinity, so nothing traps. Comparisons with NaN are false, except !=.
    llvm::Value* floatArith(BinOp op, llvm::Value* l, llvm::Value* r) {
        switch (op) {
            case BinOp::Add: return b.CreateFAdd(l, r);
            case BinOp::Sub: return b.CreateFSub(l, r);
            case BinOp::Mul: return b.CreateFMul(l, r);
            case BinOp::Div: return b.CreateFDiv(l, r);
            case BinOp::Eq: return b.CreateFCmpOEQ(l, r);
            case BinOp::Ne: return b.CreateFCmpUNE(l, r);
            case BinOp::Lt: return b.CreateFCmpOLT(l, r);
            case BinOp::Le: return b.CreateFCmpOLE(l, r);
            case BinOp::Gt: return b.CreateFCmpOGT(l, r);
            case BinOp::Ge: return b.CreateFCmpOGE(l, r);
            default: llvm_unreachable("Sema allowed an integer operator on floating-point values");
        }
    }

    // ---- Reference counting ------------------------------------------------

    // ---- Optionals ---------------------------------------------------------

    llvm::Value* hasValue(llvm::Value* opt, const Type* t) {
        if (t->elem->isClass()) return b.CreateIsNotNull(opt, "has");
        return b.CreateExtractValue(opt, 0, "has");
    }

    llvm::Value* valueOf(llvm::Value* opt, const Type* t) {
        if (t->elem->isClass()) return opt;
        return b.CreateExtractValue(opt, 1, "value");
    }

    llvm::Value* wrapOptional(llvm::Value* v, const Type* t) {
        if (t->elem->isClass()) return v;
        llvm::Value* opt = b.CreateInsertValue(llvm::PoisonValue::get(llvmType(t)), b.getInt1(true), {0});
        return b.CreateInsertValue(opt, v, {1});
    }

    void retainValue(llvm::Value* v, const Type* t) {
        if (t->isOptional()) return retainValue(valueOf(v, t), t->elem);
        if (t->isClass() || t->isVector()) b.CreateCall(rtRetain, {v});
        else if (t->is(K::String)) b.CreateCall(rtRetain, {b.CreateExtractValue(v, 2, "owner")});
        else if (isManaged(t)) b.CreateCall(types.at(t->decl).retain, {v});
    }

    void releaseValue(llvm::Value* v, const Type* t) {
        if (t->isOptional()) return releaseValue(valueOf(v, t), t->elem);
        if (t->is(K::String)) {
            b.CreateCall(rtStrRelease, {b.CreateExtractValue(v, 2, "owner")});
        } else if (t->isVector()) {
            VecInfo& info = vecInfo(t);
            b.CreateCall(rtVecRelease, {v, b.getInt64(info.elemSize), info.releaseElem});
        } else if (isManaged(t)) {
            b.CreateCall(types.at(t->decl).release, {v});
        }
    }

    // ---- Strings -----------------------------------------------------------

    // The value of a string that is only read before any other code runs, so one
    // stored in a variable or field is used in place, without a reference of its own.
    llvm::Value* stringForRead(const Expr& e) {
        return isPlace(e) ? b.CreateLoad(stringTy, addressOf(e), "str") : gen(e);
    }

    // The operands of `a + b + c`, which are joined all at once.
    static void concatParts(const Expr& e, std::vector<const Expr*>& parts) {
        if (e.kind == Expr::Kind::Binary) {
            auto& bin = static_cast<const BinaryExpr&>(e);
            if (bin.op == BinOp::Add && bin.operandType && bin.operandType->is(K::String)) {
                concatParts(*bin.lhs, parts);
                concatParts(*bin.rhs, parts);
                return;
            }
        }
        parts.push_back(&e);
    }

    // `a + b + c`: a new string, made with one allocation.
    llvm::Value* genConcat(const BinaryExpr& bin) {
        std::vector<const Expr*> parts;
        concatParts(bin, parts);
        // A part is read in place unless a later part runs code that could change it.
        std::vector<bool> codeRunsLater(parts.size());
        bool later = false;
        for (size_t i = parts.size(); i-- > 0;) {
            codeRunsLater[i] = later;
            later = later || mayRunCode(*parts[i]);
        }
        auto* arrayTy = llvm::ArrayType::get(stringTy, parts.size());
        llvm::AllocaInst* array = entryAlloca(arrayTy, "parts");
        for (size_t i = 0; i < parts.size(); ++i) {
            llvm::Value* v = parts[i]->type->is(K::Char) ? charString(gen(*parts[i]))  // `s + c`
                             : codeRunsLater[i]          ? gen(*parts[i])
                                                         : stringForRead(*parts[i]);
            b.CreateStore(v, b.CreateConstInBoundsGEP2_32(arrayTy, array, 0, (unsigned)i));
        }
        llvm::AllocaInst* out = entryAlloca(stringTy, "joined");
        b.CreateCall(rtStrConcat, {out, array, b.getInt64(parts.size())});
        return b.CreateLoad(stringTy, out, "joined");
    }

    llvm::Value* genStringMethod(const MethodCallExpr& call) {
        const Expr& recv = *call.receiver;
        if (call.strMethod == StrMethod::Clear) {
            llvm::Value* slot = addressOf(recv, false, true);
            llvm::Value* old = b.CreateLoad(stringTy, slot, "old");
            b.CreateStore(llvm::Constant::getNullValue(stringTy), slot);
            releaseValue(old, Type::stringTy());
            return nullptr;
        }
        if (call.strMethod == StrMethod::PushBack) {  // `s += c`
            llvm::Value* c = gen(*call.args[0]);
            b.CreateCall(rtStrAppend, {addressOf(recv, false, true), charBytes(c), b.getInt64(1)});
            return nullptr;
        }
        if (call.strMethod == StrMethod::PopBack) {
            // The string keeps its bytes and sees one fewer, so nothing is copied.
            llvm::Value* slot = addressOf(recv, false, true);
            llvm::Value* length = b.CreateLoad(b.getInt64Ty(), b.CreateStructGEP(stringTy, slot, 1), "length");
            trapIf(b.CreateICmpEQ(length, b.getInt64(0)), "pop_back() was called on an empty string", call.loc);
            b.CreateStore(b.CreateSub(length, b.getInt64(1)), b.CreateStructGEP(stringTy, slot, 1));
            return nullptr;
        }
        // The receiver is evaluated before the arguments, as in C++17, and read in
        // place unless an argument runs code that could change it.
        bool argsRunCode = false;
        for (auto& a : call.args) argsRunCode = argsRunCode || mayRunCode(*a);
        llvm::Value* s = argsRunCode ? gen(recv) : stringForRead(recv);
        std::vector<llvm::Value*> args;
        for (auto& a : call.args) {
            if (a->type->is(K::Char)) args.push_back(charString(gen(*a)));  // `s.find('x')`
            else if (!a->type->is(K::String)) args.push_back(genConverted(*a, Type::longTy()));
            else args.push_back(argsRunCode ? gen(*a) : stringForRead(*a));
        }
        auto test = [&](llvm::FunctionCallee f, const char* name) {
            llvm::Value* r = b.CreateCall(f, {strData(s), strLength(s), strData(args[0]), strLength(args[0])});
            return b.CreateICmpNE(r, b.getInt32(0), name);
        };
        switch (call.strMethod) {
            case StrMethod::Size: return strLength(s);
            case StrMethod::Empty: return b.CreateICmpEQ(strLength(s), b.getInt64(0), "empty");
            case StrMethod::Substr: {
                llvm::Value* pos = args.size() > 0 ? args[0] : b.getInt64(0);
                llvm::Value* count = args.size() > 1 ? args[1] : b.getInt64(-1);  // string::npos: the rest
                llvm::AllocaInst* in = entryAlloca(stringTy, "str");
                b.CreateStore(s, in);
                llvm::AllocaInst* out = entryAlloca(stringTy, "sub");
                b.CreateCall(rtStrSubstr, {out, in, pos, count, fileName, b.getInt32(call.loc.line), b.getInt32(call.loc.col)});
                return b.CreateLoad(stringTy, out, "sub");  // shares the bytes of `s`, with its own reference
            }
            case StrMethod::Find:
            case StrMethod::Contains: {
                llvm::Value* from = args.size() > 1 ? args[1] : b.getInt64(0);
                if (args.size() > 1) trapIf(b.CreateICmpSLT(from, b.getInt64(0)), "find() was given a negative position", call.loc);
                llvm::Value* pos = b.CreateCall(rtStrFind, {strData(s), strLength(s), strData(args[0]), strLength(args[0]), from}, "pos");
                if (call.strMethod == StrMethod::Find) return pos;
                return b.CreateICmpSGE(pos, b.getInt64(0), "contains");
            }
            case StrMethod::StartsWith: return test(rtStrStartsWith, "starts");
            case StrMethod::EndsWith: return test(rtStrEndsWith, "ends");
            case StrMethod::Front:
            case StrMethod::Back: {
                bool front = call.strMethod == StrMethod::Front;
                llvm::Value* length = strLength(s);
                trapIf(b.CreateICmpEQ(length, b.getInt64(0)),
                       std::string(front ? "front()" : "back()") + " was called on an empty string", call.loc);
                llvm::Value* index = front ? b.getInt64(0) : b.CreateSub(length, b.getInt64(1));
                return b.CreateLoad(b.getInt8Ty(), b.CreateInBoundsGEP(b.getInt8Ty(), strData(s), index), call.name);
            }
            case StrMethod::Clear:
            case StrMethod::PushBack:
            case StrMethod::PopBack:
            case StrMethod::None: break;
        }
        llvm_unreachable("unknown string method");
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
        info.print = internalFunction(prefix + "print", b.getVoidTy(), {ptrTy, ptrTy});  // the vector, the output
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
        llvm::Value* out = info.print->getArg(1);
        printText(out, "[");
        countedLoop(vecSize(v), [&](llvm::Value* i) {
            auto* sepBB = llvm::BasicBlock::Create(ctx, "sep", fn);
            auto* elemBB = llvm::BasicBlock::Create(ctx, "elem", fn);
            b.CreateCondBr(b.CreateICmpNE(i, b.getInt64(0)), sepBB, elemBB);
            b.SetInsertPoint(sepBB);
            printText(out, ", ");
            b.CreateBr(elemBB);
            b.SetInsertPoint(elemBB);
            printValue(out, b.CreateLoad(info.elemTy, elementAt(v, i, vt)), elem, true);
        });
        printText(out, "]");
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
        if (from->isNull()) return llvm::Constant::getNullValue(llvmType(to));
        if (to->isOptional()) return wrapOptional(convert(v, from, to->elem), to);
        if (from->is(K::Int) && to->is(K::Long)) return b.CreateSExt(v, b.getInt64Ty());
        if (from->is(K::Char) && to->isInteger()) return b.CreateZExt(v, llvmType(to));  // its code, 0 to 255
        if (from->is(K::Char) && to->isFloating()) return b.CreateUIToFP(v, llvmType(to));
        if (from->isNumber() && to->is(K::Bool)) return condValue(v, from);
        if (from->isInteger() && to->isFloating()) return b.CreateSIToFP(v, llvmType(to));
        if (from->is(K::Float) && to->is(K::Double)) return b.CreateFPExt(v, b.getDoubleTy());
        if (from->is(K::Double) && to->is(K::Float)) return b.CreateFPTrunc(v, b.getFloatTy());  // a constant
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
    // passed to a function, or returned. A `{ }` list takes that type, or the type of the
    // value an optional holds.
    llvm::Value* genOwned(const Expr& e, const Type* to) {
        if (e.kind == Expr::Kind::InitList) {
            auto& list = static_cast<const InitListExpr&>(e);
            if (!to->isOptional()) return genInitList(list, to);
            return convert(genInitList(list, list.type), list.type, to);
        }
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
            case K::Int: case K::Long: case K::Char: return llvm::ConstantInt::get(llvmType(t), 0);
            case K::Float: case K::Double: return llvm::ConstantFP::get(llvmType(t), 0.0);
            case K::String: return llvm::Constant::getNullValue(stringTy);
            case K::Struct: return buildAggregate(*t->decl, {});
            case K::Class: case K::Vector: return llvm::ConstantPointerNull::get(b.getPtrTy());
            case K::Optional: return llvm::Constant::getNullValue(llvmType(t));  // null
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

    // A condition is a bool, a number that counts as true when it is not zero, or an
    // optional that counts as true when it holds a value.
    llvm::Value* genCond(const Expr& e) { return condValue(gen(e), e.type); }

    llvm::Value* condValue(llvm::Value* v, const Type* t) {
        if (t->isInteger()) return b.CreateICmpNE(v, llvm::ConstantInt::get(v->getType(), 0));
        if (t->isFloating()) return b.CreateFCmpUNE(v, llvm::ConstantFP::get(v->getType(), 0.0));  // NaN is true
        if (t->isOptional()) return hasValue(v, t);
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
            case Expr::Kind::IntLit: case Expr::Kind::BoolLit: case Expr::Kind::StringLit: case Expr::Kind::NullLit:
            case Expr::Kind::FloatLit: case Expr::Kind::CharLit: case Expr::Kind::Name: case Expr::Kind::This:
            case Expr::Kind::OptValue:
                return false;
            case Expr::Kind::Cast: return mayRunCode(*static_cast<const CastExpr&>(e).operand);
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
                if (vt->is(K::String)) return charAddress(ix, hold, write);
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

    // The address of `s[i]`, a byte of a string, found as for an element of a vector.
    // To change it, the string first gets bytes of its own (see cplus_str_mutable).
    llvm::Value* charAddress(const IndexExpr& ix, bool hold, bool write) {
        llvm::Value* slot = nullptr;
        llvm::Value* s;
        llvm::Value* index;
        if (isPlace(*ix.base)) {
            slot = addressOf(*ix.base, hold || mayRunCode(*ix.index), write);
            index = genIndex(*ix.index);
            s = b.CreateLoad(stringTy, slot, "str");
        } else {
            s = gen(*ix.base);  // a temporary, kept until the statement ends
            index = genIndex(*ix.index);
        }
        checkIndex(index, strLength(s), ix.loc, true);
        llvm::Value* data = strData(s);
        if (write) {
            if (!slot) llvm_unreachable("Sema allowed changing a char of a temporary string");
            data = b.CreateCall(rtStrMutable, {slot}, "data");
        }
        if (hold) {
            llvm::Value* held = slot ? b.CreateLoad(stringTy, slot, "str") : s;
            retainValue(held, Type::stringTy());
            temps.push_back({held, Type::stringTy(), false});
        }
        return b.CreateInBoundsGEP(b.getInt8Ty(), data, index, "char.addr");
    }

    // Lowers `e`. `owned` is set when the result is a new value the caller must
    // release; otherwise a value holding class references is borrowed from a place.
    llvm::Value* genRaw(const Expr& e, bool& owned) {
        switch (e.kind) {
            case Expr::Kind::IntLit:
                return llvm::ConstantInt::getSigned(llvmType(e.type), static_cast<const IntLitExpr&>(e).value);
            case Expr::Kind::FloatLit:
                return llvm::ConstantFP::get(llvmType(e.type), static_cast<const FloatLitExpr&>(e).value);
            case Expr::Kind::Cast: return genCast(static_cast<const CastExpr&>(e));
            case Expr::Kind::CharLit: return b.getInt8(static_cast<const CharLitExpr&>(e).value);
            case Expr::Kind::BoolLit: return b.getInt1(static_cast<const BoolLitExpr&>(e).value);
            case Expr::Kind::StringLit: return stringValue(static_cast<const StringLitExpr&>(e).value);
            case Expr::Kind::NullLit: return llvm::ConstantPointerNull::get(b.getPtrTy());
            case Expr::Kind::OptValue: return optValues.at(static_cast<const OptValueExpr*>(&e));
            case Expr::Kind::OptChain: return genOptChain(static_cast<const OptChainExpr&>(e), owned);
            case Expr::Kind::Coalesce: return genCoalesce(static_cast<const CoalesceExpr&>(e), owned);
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
                llvm::Value* v = genConverted(*u.operand, e.type);  // a char becomes an int first
                switch (u.op) {
                    case UnOp::Plus: return v;
                    case UnOp::Not:
                    case UnOp::BitNot: return b.CreateNot(v);
                    case UnOp::Neg:
                        if (e.type->isFloating()) return b.CreateFNeg(v);
                        return checkedArith(llvm::Intrinsic::ssub_with_overflow, llvm::ConstantInt::get(v->getType(), 0),
                                            v, "-", e.type, e.loc);
                }
                break;
            }
            case Expr::Kind::Binary: {
                auto& bin = static_cast<const BinaryExpr&>(e);
                if (bin.op == BinOp::LogAnd || bin.op == BinOp::LogOr) return genLogical(bin);
                bool equality = bin.op == BinOp::Eq || bin.op == BinOp::Ne;
                if (bin.operandType->is(K::String) && bin.op == BinOp::Add) {
                    owned = true;
                    return genConcat(bin);
                }
                if (bin.operandType->is(K::String) && !equality) {  // ordered by their bytes
                    llvm::Value* l = gen(*bin.lhs);
                    llvm::Value* r = gen(*bin.rhs);
                    llvm::Value* order = b.CreateCall(rtStrCompare, {strData(l), strLength(l), strData(r), strLength(r)}, "order");
                    return arith(bin.op, order, b.getInt32(0), Type::intTy(), bin.loc);
                }
                if (!bin.operandType->isNumber()) {  // == or != on bools, strings, structs, objects, optionals
                    llvm::Value* l = genConverted(*bin.lhs, bin.operandType);
                    llvm::Value* same = genEquals(l, genConverted(*bin.rhs, bin.operandType), bin.operandType);
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
                if (a.op && ty->is(K::String)) {
                    // `s += x` appends in place when nothing else holds the string's buffer.
                    llvm::Value* rhs = textValue(gen(*a.value), a.value->type);
                    llvm::Value* slot = addressOf(*a.target, false, true);
                    b.CreateCall(rtStrAppend, {slot, strData(rhs), strLength(rhs)});
                    return b.CreateLoad(stringTy, slot, "str");
                }
                if (a.op && ty->is(K::Char)) {  // `c += n` or `c -= n`, which must stay a char
                    llvm::Value* rhs = genConverted(*a.value, Type::longTy());
                    llvm::Value* slot = addressOf(*a.target, false, true);
                    llvm::Value* old = b.CreateZExt(b.CreateLoad(b.getInt8Ty(), slot), b.getInt64Ty());
                    const char* sym = *a.op == BinOp::Add ? "+=" : "-=";
                    llvm::Value* wide = checkedArith(*a.op == BinOp::Add ? llvm::Intrinsic::sadd_with_overflow
                                                                         : llvm::Intrinsic::ssub_with_overflow,
                                                     old, rhs, sym, Type::longTy(), a.loc);
                    llvm::Value* v = checkedChar(wide, std::string("the result of '") + sym + "' is", a.loc);
                    b.CreateStore(v, slot);
                    return v;
                }
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
                if (e.type->is(K::Char)) {  // stays from 0 to 255
                    llvm::Value* wide = b.CreateZExt(old, b.getInt64Ty());
                    wide = i.increment ? b.CreateAdd(wide, b.getInt64(1)) : b.CreateSub(wide, b.getInt64(1));
                    llvm::Value* updated =
                        checkedChar(wide, std::string("the result of '") + (i.increment ? "++" : "--") + "' is", e.loc);
                    b.CreateStore(updated, slot);
                    return i.prefix ? updated : old;
                }
                llvm::Value* updated =
                    e.type->isFloating()
                        ? floatArith(i.increment ? BinOp::Add : BinOp::Sub, old, llvm::ConstantFP::get(t, 1.0))
                        : checkedArith(i.increment ? llvm::Intrinsic::sadd_with_overflow
                                                   : llvm::Intrinsic::ssub_with_overflow,
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
            case K::Bool: case K::Int: case K::Long: case K::Char: case K::Class: return b.CreateICmpEQ(l, r);
            case K::Float: case K::Double: return b.CreateFCmpOEQ(l, r);
            case K::String: {
                llvm::Value* eq = b.CreateCall(rtStrEq, {b.CreateExtractValue(l, 0), b.CreateExtractValue(l, 1),
                                                         b.CreateExtractValue(r, 0), b.CreateExtractValue(r, 1)});
                return b.CreateICmpNE(eq, b.getInt32(0));
            }
            case K::Struct: return b.CreateCall(types.at(t->decl).equals, {l, r});
            case K::Vector: return b.CreateCall(vecInfo(t).equals, {l, r});
            case K::Null: return b.getInt1(true);
            case K::Optional: {
                // Both null, or both holding equal values. An empty optional holds zero
                // bytes, which compare safely.
                if (t->elem->isClass()) return b.CreateICmpEQ(l, r);
                llvm::Value* hl = hasValue(l, t);
                llvm::Value* same = genEquals(valueOf(l, t), valueOf(r, t), t->elem);
                return b.CreateAnd(b.CreateICmpEQ(hl, hasValue(r, t)), b.CreateOr(b.CreateNot(hl), same));
            }
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

    // Printing writes to `out`: the address of a string being built by format() or
    // to_string(), or null for standard output.
    void printText(llvm::Value* out, const std::string& text) {
        if (!text.empty()) b.CreateCall(rtPrintStr, {out, cstring(text), b.getInt64(text.size())});
    }

    // Strings print as-is at the top level and in quotes inside a struct or class, so
    // `{name: "Ada", age: 36}` stays readable.
    void printValue(llvm::Value* out, llvm::Value* v, const Type* t, bool nested = false) {
        switch (t->kind) {
            case K::Int: b.CreateCall(rtPrintI32, {out, v}); break;
            case K::Long: b.CreateCall(rtPrintI64, {out, v}); break;
            case K::Float: b.CreateCall(rtPrintF32, {out, v}); break;
            case K::Double: b.CreateCall(rtPrintF64, {out, v}); break;
            case K::Bool: b.CreateCall(rtPrintBool, {out, b.CreateZExt(v, b.getInt32Ty())}); break;
            case K::Char:  // the byte itself, as std::format prints a char; in quotes inside a struct or vector
                if (nested) b.CreateCall(rtPrintCharQuoted, {out, b.CreateZExt(v, b.getInt32Ty())});
                else b.CreateCall(rtPrintStr, {out, charBytes(v), b.getInt64(1)});
                break;
            case K::String: b.CreateCall(nested ? rtPrintQuoted : rtPrintStr, {out, strData(v), strLength(v)}); break;
            case K::Struct:
            case K::Class: b.CreateCall(types.at(t->decl).print, {v, out}); break;
            case K::Vector: b.CreateCall(vecInfo(t).print, {v, out}); break;
            case K::Null: printText(out, "null"); break;
            case K::Optional: {
                auto* someBB = llvm::BasicBlock::Create(ctx, "print.value", fn);
                auto* noneBB = llvm::BasicBlock::Create(ctx, "print.null", fn);
                auto* endBB = llvm::BasicBlock::Create(ctx, "print.end", fn);
                b.CreateCondBr(hasValue(v, t), someBB, noneBB);
                b.SetInsertPoint(someBB);
                printValue(out, valueOf(v, t), t->elem, nested);
                b.CreateBr(endBB);
                b.SetInsertPoint(noneBB);
                printText(out, "null");
                b.CreateBr(endBB);
                b.SetInsertPoint(endBB);
                break;
            }
            default: llvm_unreachable("unprintable type reached codegen");
        }
    }

    // print, println, format and to_string. The values are evaluated left to right
    // before anything is written.
    llvm::Value* genFormatted(const CallExpr& call) {
        size_t first = call.builtin == Builtin::ToString ? 0 : 1;  // the others start with the format string
        std::vector<llvm::Value*> values;
        for (size_t i = first; i < call.args.size(); ++i) values.push_back(gen(*call.args[i]));
        bool toString = call.builtin == Builtin::Format || call.builtin == Builtin::ToString;
        llvm::Value* out = llvm::ConstantPointerNull::get(b.getPtrTy());
        if (toString) {
            out = entryAlloca(stringTy, "text");
            b.CreateStore(llvm::Constant::getNullValue(stringTy), out);
        }
        for (size_t i = 0; i < call.formatPieces.size(); ++i) {
            printText(out, call.formatPieces[i]);
            if (i >= values.size()) continue;
            const Type* t = call.args[i + first]->type;
            FormatSpec spec = i < call.formatSpecs.size() ? call.formatSpecs[i] : FormatSpec{};
            if (spec.type || spec.precision >= 0)  // `{:.2f}`, for a float or a double
                b.CreateCall(rtPrintFloatSpec, {out, convert(values[i], t, Type::doubleTy()), b.getInt32(spec.type),
                                                b.getInt32(spec.precision)});
            else
                printValue(out, values[i], t);
        }
        if (call.builtin == Builtin::Println) b.CreateCall(rtPrintNewline, {});
        return toString ? b.CreateLoad(stringTy, out, "text") : nullptr;  // a new string, owned
    }

    // `int(x)` and the other conversions, as in C++, except that a value the new type
    // cannot hold stops the program instead of wrapping around or being undefined.
    llvm::Value* genCast(const CastExpr& c) {
        const Type* from = c.operand->type;
        const Type* to = c.target;
        llvm::Value* v = gen(*c.operand);
        if (from == to) return v;
        llvm::Type* target = llvmType(to);
        Loc loc = c.loc;
        if (from->is(K::Char)) {  // its code, from 0 to 255
            if (to->is(K::Bool)) return b.CreateICmpNE(v, b.getInt8(0));
            return to->isFloating() ? b.CreateUIToFP(v, target) : b.CreateZExt(v, target);
        }
        if (to->is(K::Char)) {  // `char(65)` is 'A'; a value outside 0 to 255 stops the program
            if (from->is(K::Bool)) return b.CreateZExt(v, target);
            if (from->isFloating()) {
                llvm::Value* x = convert(v, from, Type::doubleTy());
                llvm::Value* fits = b.CreateAnd(b.CreateFCmpOGT(x, llvm::ConstantFP::get(b.getDoubleTy(), -1.0)),
                                                b.CreateFCmpOLT(x, llvm::ConstantFP::get(b.getDoubleTy(), 256.0)));
                auto* trapBB = llvm::BasicBlock::Create(ctx, "cast.trap", fn);
                auto* okBB = llvm::BasicBlock::Create(ctx, "cast.ok", fn);
                b.CreateCondBr(fits, okBB, trapBB, llvm::MDBuilder(ctx).createBranchWeights(1u << 20, 1));
                b.SetInsertPoint(trapBB);
                b.CreateCall(rtTrapFloatCast, {x, cstring("char"), fileName, b.getInt32(loc.line), b.getInt32(loc.col)});
                b.CreateUnreachable();
                b.SetInsertPoint(okBB);
                return b.CreateFPToUI(v, target);
            }
            return checkedChar(b.CreateSExt(v, b.getInt64Ty()), "char(x) was given", loc);
        }
        if (to->is(K::Bool)) return condValue(v, from);
        if (from->is(K::Bool)) return to->isFloating() ? b.CreateUIToFP(v, target) : b.CreateZExt(v, target);
        if (from->isInteger() && to->isInteger()) {
            if (to->is(K::Long)) return b.CreateSExt(v, target);
            llvm::Value* narrow = b.CreateTrunc(v, target);
            auto* trapBB = llvm::BasicBlock::Create(ctx, "narrow.trap", fn);
            auto* okBB = llvm::BasicBlock::Create(ctx, "narrow.ok", fn);
            b.CreateCondBr(b.CreateICmpNE(b.CreateSExt(narrow, v->getType()), v), trapBB, okBB,
                           llvm::MDBuilder(ctx).createBranchWeights(1, 1u << 20));
            b.SetInsertPoint(trapBB);
            b.CreateCall(rtTrapNarrow, {v, fileName, b.getInt32(loc.line), b.getInt32(loc.col)});
            b.CreateUnreachable();
            b.SetInsertPoint(okBB);
            return narrow;
        }
        if (from->isInteger()) return b.CreateSIToFP(v, target);
        if (to->isFloating()) return from->is(K::Float) ? b.CreateFPExt(v, target) : b.CreateFPTrunc(v, target);
        // A floating-point value becomes an integer by dropping its fraction. It must be
        // a number, and the result must fit: from -2^31 to 2^31 - 1 for an int.
        llvm::Value* x = convert(v, from, Type::doubleTy());
        double lo = to->is(K::Int) ? -2147483649.0 : -9223372036854775808.0;
        double hi = to->is(K::Int) ? 2147483648.0 : 9223372036854775808.0;
        llvm::Value* above = to->is(K::Int) ? b.CreateFCmpOGT(x, llvm::ConstantFP::get(b.getDoubleTy(), lo))
                                            : b.CreateFCmpOGE(x, llvm::ConstantFP::get(b.getDoubleTy(), lo));
        llvm::Value* fits = b.CreateAnd(above, b.CreateFCmpOLT(x, llvm::ConstantFP::get(b.getDoubleTy(), hi)));
        auto* trapBB = llvm::BasicBlock::Create(ctx, "cast.trap", fn);
        auto* okBB = llvm::BasicBlock::Create(ctx, "cast.ok", fn);
        b.CreateCondBr(fits, okBB, trapBB, llvm::MDBuilder(ctx).createBranchWeights(1u << 20, 1));
        b.SetInsertPoint(trapBB);
        b.CreateCall(rtTrapFloatCast, {x, cstring(to->name), fileName, b.getInt32(loc.line), b.getInt32(loc.col)});
        b.CreateUnreachable();
        b.SetInsertPoint(okBB);
        return b.CreateFPToSI(v, target);
    }

    // isdigit(c), toupper(c), ...: ASCII only, as C's are in the default "C" locale.
    llvm::Value* genCharFn(const CallExpr& call) {
        llvm::Value* c = gen(*call.args[0]);
        auto in = [&](char lo, char hi) {  // lo <= c <= hi, as one unsigned compare
            return b.CreateICmpULE(b.CreateSub(c, b.getInt8((uint8_t)lo)), b.getInt8((uint8_t)(hi - lo)));
        };
        auto upper = [&] { return in('A', 'Z'); };
        auto lower = [&] { return in('a', 'z'); };
        auto digit = [&] { return in('0', '9'); };
        auto alnum = [&] { return b.CreateOr(b.CreateOr(upper(), lower()), digit()); };
        switch (call.charFn) {
            case CharFn::IsAlpha: return b.CreateOr(upper(), lower());
            case CharFn::IsDigit: return digit();
            case CharFn::IsAlnum: return alnum();
            case CharFn::IsSpace: return b.CreateOr(b.CreateICmpEQ(c, b.getInt8(' ')), in('\t', '\r'));
            case CharFn::IsUpper: return upper();
            case CharFn::IsLower: return lower();
            case CharFn::IsPunct: return b.CreateAnd(in('!', '~'), b.CreateNot(alnum()));
            case CharFn::IsXDigit: return b.CreateOr(digit(), b.CreateOr(in('a', 'f'), in('A', 'F')));
            case CharFn::ToUpper: return b.CreateSelect(lower(), b.CreateSub(c, b.getInt8(32)), c);
            case CharFn::ToLower: return b.CreateSelect(upper(), b.CreateAdd(c, b.getInt8(32)), c);
        }
        llvm_unreachable("unknown char function");
    }

    // The math functions. Most become LLVM intrinsics, which compile to an instruction
    // where the processor has one; the rest call the C library's math functions.
    llvm::Value* genMath(const CallExpr& call) {
        MathFn f = call.math;
        if (f == MathFn::IsNan || f == MathFn::IsInf || f == MathFn::IsFinite) {
            const Type* t = call.args[0]->type->is(K::Float) ? Type::floatTy() : Type::doubleTy();
            llvm::Value* v = genConverted(*call.args[0], t);
            if (f == MathFn::IsNan) return b.CreateFCmpUNO(v, v);
            llvm::Value* mag = b.CreateUnaryIntrinsic(llvm::Intrinsic::fabs, v);
            llvm::Value* inf = llvm::ConstantFP::getInfinity(llvmType(t));
            return f == MathFn::IsInf ? b.CreateFCmpOEQ(mag, inf) : b.CreateFCmpONE(mag, inf);
        }
        const Type* t = call.type;
        std::vector<llvm::Value*> args;
        for (auto& a : call.args) args.push_back(genConverted(*a, t));
        llvm::Value* x = args[0];
        if (t->isInteger() || t->is(K::Char)) {  // abs, min and max; chars compare by their codes
            if (f == MathFn::Abs) {
                llvm::Value* negated = checkedArith(llvm::Intrinsic::ssub_with_overflow,
                                                    llvm::ConstantInt::get(x->getType(), 0), x, "abs", t, call.loc);
                return b.CreateSelect(b.CreateICmpSLT(x, llvm::ConstantInt::get(x->getType(), 0)), negated, x);
            }
            llvm::Value* a0 = f == MathFn::Min ? args[1] : args[0];
            llvm::Value* a1 = f == MathFn::Min ? args[0] : args[1];
            llvm::Value* less = t->is(K::Char) ? b.CreateICmpULT(a0, a1) : b.CreateICmpSLT(a0, a1);
            return b.CreateSelect(less, args[1], args[0]);
        }
        auto unary = [&](llvm::Intrinsic::ID id) { return b.CreateUnaryIntrinsic(id, x); };
        auto binary = [&](llvm::Intrinsic::ID id) { return b.CreateBinaryIntrinsic(id, x, args[1]); };
        auto libm = [&](const char* name) {
            std::string n = std::string(name) + (t->is(K::Float) ? "f" : "");
            std::vector<llvm::Type*> params(args.size(), llvmType(t));
            return b.CreateCall(mod->getOrInsertFunction(n, llvm::FunctionType::get(llvmType(t), params, false)), args);
        };
        switch (f) {
            case MathFn::Sqrt: return unary(llvm::Intrinsic::sqrt);
            case MathFn::Exp: return unary(llvm::Intrinsic::exp);
            case MathFn::Exp2: return unary(llvm::Intrinsic::exp2);
            case MathFn::Log: return unary(llvm::Intrinsic::log);
            case MathFn::Log2: return unary(llvm::Intrinsic::log2);
            case MathFn::Log10: return unary(llvm::Intrinsic::log10);
            case MathFn::Sin: return unary(llvm::Intrinsic::sin);
            case MathFn::Cos: return unary(llvm::Intrinsic::cos);
            case MathFn::Floor: return unary(llvm::Intrinsic::floor);
            case MathFn::Ceil: return unary(llvm::Intrinsic::ceil);
            case MathFn::Round: return unary(llvm::Intrinsic::round);  // halves away from zero, as in C
            case MathFn::Trunc: return unary(llvm::Intrinsic::trunc);
            case MathFn::Fabs: case MathFn::Abs: return unary(llvm::Intrinsic::fabs);
            case MathFn::Pow: return binary(llvm::Intrinsic::pow);
            case MathFn::Fmod: return b.CreateFRem(x, args[1]);
            case MathFn::Cbrt: return libm("cbrt");
            case MathFn::Tan: return libm("tan");
            case MathFn::Asin: return libm("asin");
            case MathFn::Acos: return libm("acos");
            case MathFn::Atan: return libm("atan");
            case MathFn::Atan2: return libm("atan2");
            case MathFn::Sinh: return libm("sinh");
            case MathFn::Cosh: return libm("cosh");
            case MathFn::Tanh: return libm("tanh");
            case MathFn::Hypot: return libm("hypot");
            case MathFn::Min: case MathFn::Max: {
                // As std::min and std::max: `b < a ? b : a` and `a < b ? b : a`.
                llvm::Value* less = f == MathFn::Min ? b.CreateFCmpOLT(args[1], x) : b.CreateFCmpOLT(x, args[1]);
                return b.CreateSelect(less, args[1], x);
            }
            default: break;
        }
        llvm_unreachable("unknown math function");
    }

    llvm::Value* genCall(const CallExpr& call) {
        if (call.vectorType) return genVectorConstruction(call);
        if (call.builtin == Builtin::Math) return genMath(call);
        if (call.builtin == Builtin::CharFn) return genCharFn(call);
        if (call.builtin == Builtin::StringFill) {  // `string(count, c)`, or `string()`
            if (call.args.empty()) return llvm::Constant::getNullValue(stringTy);
            llvm::Value* count = genConverted(*call.args[0], Type::longTy());
            llvm::Value* c = b.CreateZExt(gen(*call.args[1]), b.getInt32Ty());
            llvm::AllocaInst* out = entryAlloca(stringTy, "filled");
            b.CreateCall(rtStrRepeat, {out, count, c, fileName, b.getInt32(call.loc.line), b.getInt32(call.loc.col)});
            return b.CreateLoad(stringTy, out, "filled");
        }
        if (call.builtin != Builtin::None) return genFormatted(call);
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
        if (target->isExtern) return genCCall(call, *target);
        std::vector<llvm::Value*> args;
        if (call.method) args.push_back(thisPtr);
        for (size_t i = 0; i < call.args.size(); ++i) args.push_back(genOwned(*call.args[i], target->params[i]->type));
        llvm::CallInst* inst = b.CreateCall(funcs.at(target), args);
        return target->retType->is(K::Void) ? nullptr : inst;
    }

    llvm::Value* genMethodCall(const MethodCallExpr& call) {
        const Expr& recv = *call.receiver;
        if (recv.type->isOptional()) return genOptionalMethod(call);
        if (recv.type->isVector()) return genVectorMethod(call);
        if (recv.type->is(K::String)) return genStringMethod(call);
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

    // `base?.rest`: when `base` holds a value, the rest of the chain runs on it and its
    // result becomes an optional; otherwise the result is null.
    llvm::Value* genOptChain(const OptChainExpr& c, bool& owned) {
        const Type* optTy = c.base->type;
        const Type* resultTy = c.type;
        llvm::Value* opt = gen(*c.base);  // kept until the statement ends
        auto* someBB = llvm::BasicBlock::Create(ctx, "chain.value", fn);
        auto* endBB = llvm::BasicBlock::Create(ctx, "chain.end", fn);
        llvm::BasicBlock* noneFrom = b.GetInsertBlock();
        b.CreateCondBr(hasValue(opt, optTy), someBB, endBB);
        b.SetInsertPoint(someBB);
        optValues[c.value] = valueOf(opt, optTy);
        // Only this branch runs the rest of the chain, so it cleans up after itself, and
        // a value holding references leaves it owned.
        size_t mark = temps.size();
        bool managed = isManaged(resultTy);
        llvm::Value* v = nullptr;
        if (resultTy->is(K::Void)) {
            bool bodyOwned = false;
            if (llvm::Value* x = genRaw(*c.body, bodyOwned); x && bodyOwned) releaseValue(x, c.body->type);
        } else {
            v = genOwned(*c.body, resultTy);
        }
        flushTemps(mark);
        llvm::BasicBlock* someEnd = b.GetInsertBlock();
        b.CreateBr(endBB);
        b.SetInsertPoint(endBB);
        if (!v) return nullptr;
        llvm::PHINode* phi = b.CreatePHI(llvmType(resultTy), 2);
        phi->addIncoming(v, someEnd);
        phi->addIncoming(llvm::Constant::getNullValue(llvmType(resultTy)), noneFrom);
        owned = managed;
        return phi;
    }

    // `opt ?? fallback`: the fallback is only evaluated when `opt` is null.
    llvm::Value* genCoalesce(const CoalesceExpr& c, bool& owned) {
        const Type* optTy = c.lhs->type;
        const Type* resultTy = c.type;
        bool managed = isManaged(resultTy);
        llvm::Value* opt = gen(*c.lhs);
        auto* someBB = llvm::BasicBlock::Create(ctx, "coalesce.value", fn);
        auto* noneBB = llvm::BasicBlock::Create(ctx, "coalesce.fallback", fn);
        auto* endBB = llvm::BasicBlock::Create(ctx, "coalesce.end", fn);
        b.CreateCondBr(hasValue(opt, optTy), someBB, noneBB);
        b.SetInsertPoint(someBB);
        llvm::Value* x = resultTy->isOptional() ? convert(opt, optTy, resultTy)
                                                : convert(valueOf(opt, optTy), optTy->elem, resultTy);
        if (managed) retainValue(x, resultTy);
        llvm::BasicBlock* someEnd = b.GetInsertBlock();
        b.CreateBr(endBB);
        b.SetInsertPoint(noneBB);
        size_t mark = temps.size();
        llvm::Value* y = genOwned(*c.rhs, resultTy);
        flushTemps(mark);
        llvm::BasicBlock* noneEnd = b.GetInsertBlock();
        b.CreateBr(endBB);
        b.SetInsertPoint(endBB);
        llvm::PHINode* phi = b.CreatePHI(llvmType(resultTy), 2);
        phi->addIncoming(x, someEnd);
        phi->addIncoming(y, noneEnd);
        owned = managed;
        return phi;
    }

    llvm::Value* genOptionalMethod(const MethodCallExpr& call) {
        const Type* optTy = call.receiver->type;
        const Type* inner = optTy->elem;
        llvm::Value* opt = gen(*call.receiver);
        llvm::Value* has = hasValue(opt, optTy);
        switch (call.optMethod) {
            case OptMethod::HasValue: return has;
            case OptMethod::Value: {
                trapIf(b.CreateNot(has), "value() was called on an empty optional", call.loc);
                llvm::Value* v = valueOf(opt, optTy);
                retainValue(v, inner);  // a method call's result is owned
                return v;
            }
            case OptMethod::ValueOr: {
                // As in C++, the fallback is evaluated either way.
                llvm::Value* fallback = genOwned(*call.args[0], inner);
                llvm::Value* v = b.CreateSelect(has, valueOf(opt, optTy), fallback, "value");
                retainValue(v, inner);
                releaseValue(fallback, inner);
                return v;
            }
            case OptMethod::None: break;
        }
        llvm_unreachable("unknown optional method");
    }

    // Stores the variable of `if (auto u = found)` or `while (...)`, and returns the
    // condition: whether the optional held a value, or the value itself as a condition.
    llvm::Value* genCondDecl(const CondDecl& d) {
        const VarDecl& var = *d.var;
        const Type* t = var.init->type;
        llvm::Value* init = genOwned(*var.init, t);
        llvm::Value* cond;
        llvm::Value* value;
        if (d.unwraps) {
            cond = hasValue(init, t);
            value = convert(valueOf(init, t), t->elem, var.type);  // zero bytes when there is none
        } else {
            value = convert(init, t, var.type);
            cond = condValue(value, var.type);
        }
        b.CreateStore(value, allocaFor(var));
        return cond;
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
                                    : v.type->is(K::String) || v.type->hasFields() || v.type->isVector() ||
                                              v.type->isOptional()
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
                llvm::Value* cond = st.decl ? genCondDecl(*st.decl) : genCond(*st.cond);
                flushTemps();
                b.CreateCondBr(cond, thenBB, elseBB);
                b.SetInsertPoint(thenBB);
                scopes.emplace_back();  // the variable declared in the condition, if any
                if (st.decl && isManaged(st.decl->var->type)) scopes.back().push_back(st.decl->var.get());
                genScoped(*st.thenStmt);
                popScope();
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
                llvm::Value* cond = st.decl ? genCondDecl(*st.decl) : genCond(*st.cond);
                flushTemps();
                b.CreateCondBr(cond, bodyBB, endBB);
                b.SetInsertPoint(bodyBB);
                loops.push_back({endBB, condBB, scopes.size()});
                scopes.emplace_back();  // the variable declared in the condition, a new one each time
                if (st.decl && isManaged(st.decl->var->type)) scopes.back().push_back(st.decl->var.get());
                genScoped(*st.body);
                popScope();
                loops.pop_back();
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
        bool str = vt->is(K::String);  // a string goes over its bytes
        scopes.emplace_back();  // holds the vector or string
        llvm::Value* range = genOwned(*st.range, vt);
        flushTemps();
        b.CreateStore(range, allocaFor(*st.rangeVar));
        scopes.back().push_back(st.rangeVar.get());
        llvm::Value* size = str ? strLength(range) : vecSize(range);
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
        llvm::Value* elem = str ? b.CreateLoad(b.getInt8Ty(), b.CreateInBoundsGEP(b.getInt8Ty(), strData(range), i), "c")
                                : b.CreateLoad(vecInfo(vt).elemTy, elementAt(range, i, vt), "elem");
        llvm::Value* value = convert(elem, str ? Type::charTy() : vt->elem, var.type);
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
        if (f.isExtern) return declareCFunction(f);
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

    // Whether C's `char` is signed on the target: it is on x86 and on Apple and Windows
    // ARM, and unsigned on other ARM, PowerPC, RISC-V and s390x systems.
    bool cCharIsSigned() {
        llvm::Triple t(mod->getTargetTriple());
        if (t.isOSDarwin() || t.isOSWindows()) return true;
        return !(t.isARM() || t.isAArch64() || t.isPPC() || t.isRISCV() || t.isSystemZ());
    }

    // A C function, under its own name. Text crosses as a `const char*`, a bool as a C
    // `bool`, which is zero-extended to a byte, and a char as a C `char`, extended as the
    // platform's C compiler does. The char's bits are the same either way.
    void declareCFunction(const FuncDecl& f) {
        auto cType = [&](const Type* t) -> llvm::Type* {
            return t->is(K::String) || t->isOptional() ? b.getPtrTy() : llvmType(t);
        };
        std::vector<llvm::Type*> params;
        for (auto& p : f.params) params.push_back(cType(p->type));
        auto* type = llvm::FunctionType::get(cType(f.retType), params, false);
        auto* func = llvm::Function::Create(type, llvm::Function::ExternalLinkage, f.name, mod.get());
        auto extension = [&](const Type* t) {
            if (t->is(K::Bool)) return llvm::Attribute::ZExt;
            if (t->is(K::Char)) return cCharIsSigned() ? llvm::Attribute::SExt : llvm::Attribute::ZExt;
            return llvm::Attribute::None;
        };
        for (size_t i = 0; i < f.params.size(); ++i)
            if (auto ext = extension(f.params[i]->type); ext != llvm::Attribute::None)
                func->addParamAttr((unsigned)i, ext);
        if (auto ext = extension(f.retType); ext != llvm::Attribute::None) func->addRetAttr(ext);
        funcs[&f] = func;
    }

    // A call to a C function. Each string is copied, with a zero byte at the end, right
    // after it is evaluated, and the copies are freed when the call returns. Text the
    // function returns is copied into a string.
    llvm::Value* genCCall(const CallExpr& call, const FuncDecl& f) {
        std::vector<llvm::Value*> args, copies;
        for (size_t i = 0; i < call.args.size(); ++i) {
            const Type* t = f.params[i]->type;
            llvm::Value* v = genConverted(*call.args[i], t);
            if (!t->is(K::String) && !t->isOptional()) {
                args.push_back(v);
                continue;
            }
            llvm::Value* present = b.getInt32(1);
            if (t->isOptional()) {
                present = b.CreateZExt(hasValue(v, t), b.getInt32Ty());
                v = valueOf(v, t);  // zero bytes when null
            }
            llvm::Value* copy = b.CreateCall(rtCString, {strData(v), strLength(v), present, cstring(f.name), fileName,
                                                         b.getInt32(call.args[i]->loc.line),
                                                         b.getInt32(call.args[i]->loc.col)});
            args.push_back(copy);
            copies.push_back(copy);
        }
        llvm::Function* callee = funcs.at(&f);
        llvm::CallInst* result = b.CreateCall(callee, args);
        result->setAttributes(callee->getAttributes());  // a bool is extended at the call, too
        for (llvm::Value* copy : copies) b.CreateCall(rtCFree, {copy});
        const Type* rt = f.retType;
        if (rt->is(K::Void)) return nullptr;
        if (!rt->is(K::String) && !rt->isOptional()) return result;
        if (rt->is(K::String))
            trapIf(b.CreateIsNull(result),
                   "the C function '" + f.name + "' returned NULL, but its declaration says it always returns a "
                   "string; declare it as returning `string?`",
                   call.loc);
        llvm::AllocaInst* slot = entryAlloca(stringTy, "text");
        b.CreateCall(rtStrFromC, {slot, result});
        llvm::Value* text = b.CreateLoad(stringTy, slot, "text");
        if (rt->is(K::String)) return text;
        llvm::Value* opt = b.CreateInsertValue(llvm::PoisonValue::get(llvmType(rt)), b.CreateIsNotNull(result), {0});
        return b.CreateInsertValue(opt, text, {1});
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
        if (f.isExtern) return;  // defined in C
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
        info.print = helper(sd, "print", b.getVoidTy(), {self, b.getPtrTy()});  // the value, the output
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
        llvm::Value* out = info.print->getArg(1);
        if (sd.isClass) {
            auto* bodyBB = llvm::BasicBlock::Create(ctx, "print", fn);
            auto* againBB = llvm::BasicBlock::Create(ctx, "seen", fn);
            b.CreateCondBr(b.CreateICmpNE(b.CreateCall(rtPrintEnter, {self}), b.getInt32(0)), bodyBB, againBB);
            b.SetInsertPoint(againBB);
            printText(out, sd.name + " {...}");  // already being printed: it refers back to itself
            b.CreateRetVoid();
            b.SetInsertPoint(bodyBB);
            printText(out, sd.name + " ");
        }
        printText(out, "{");
        for (size_t i = 0; i < sd.fields.size(); ++i) {
            printText(out, (i ? ", " : "") + sd.fields[i].name + ": ");
            printValue(out, loadField(sd, self, i), sd.fields[i].type, true);
        }
        printText(out, "}");
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
        b.CreateCall(info.destroy, {self})->setTailCallKind(llvm::CallInst::TCK_MustTail);
        b.CreateRetVoid();
        b.SetInsertPoint(doneBB);
        b.CreateRetVoid();

        // Destroy: run the destructor, read the fields, free the memory, and release the
        // fields, last declared first as in C++. The last release is a tail call, so
        // freeing a long chain of objects, like a linked list, needs no more stack than
        // freeing one.
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
        std::vector<size_t> order;  // the managed fields, in the order they are released
        for (size_t i = sd.fields.size(); i-- > 0;)
            if (isManaged(sd.fields[i].type)) order.push_back(i);
        // The chain usually goes through the first class-typed field. It can be released
        // last only if no field declared before it could run a destructor.
        for (size_t k = order.size(); k-- > 0;) {
            const Type* ft = sd.fields[order[k]].type;
            if (classOf(ft)) {
                std::rotate(order.begin() + k, order.begin() + k + 1, order.end());
                break;
            }
            std::set<const StructDecl*> seen;
            if (mayRunDestructor(ft, seen)) break;
        }
        std::vector<llvm::Value*> values;
        for (size_t i : order) values.push_back(loadField(sd, self, i));
        b.CreateCall(rtFree, {self});
        for (size_t k = 0; k < order.size(); ++k) {
            const Type* ft = sd.fields[order[k]].type;
            if (k + 1 == order.size() && classOf(ft)) {
                b.CreateCall(types.at(classOf(ft)->decl).release, {values[k]})
                    ->setTailCallKind(llvm::CallInst::TCK_MustTail);
                break;
            }
            releaseValue(values[k], ft);
        }
        b.CreateRetVoid();
    }

    // The class of a field that refers to an object, or may: `User` or `User?`.
    static const Type* classOf(const Type* t) {
        if (t->isOptional()) t = t->elem;
        return t->isClass() ? t : nullptr;
    }

    // Whether releasing a value of this type could free an object, and so run a
    // destructor.
    static bool mayRunDestructor(const Type* t, std::set<const StructDecl*>& seen) {
        if (t->isOptional() || t->isVector()) return mayRunDestructor(t->elem, seen);
        if (t->isClass()) return true;
        if (!t->isStruct() || !seen.insert(t->decl).second) return false;
        for (auto& f : t->decl->fields)
            if (mayRunDestructor(f.type, seen)) return true;
        return false;
    }

    bool generate(const Program& program, std::string& error) {
        mod = std::make_unique<llvm::Module>(file.path, ctx);
        if (!setupTarget(error)) return false;
        stringTy = llvm::StructType::create(ctx, {b.getPtrTy(), b.getInt64Ty(), b.getPtrTy()}, "cplus.string");
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
