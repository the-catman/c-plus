#include "Codegen.h"

#include "../runtime/reflect.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <optional>
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
#include <llvm/Transforms/Instrumentation/AddressSanitizer.h>
#include <llvm/Transforms/Instrumentation/ThreadSanitizer.h>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <map>
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
    llvm::FunctionCallee rtThreadStart, rtThreadsFinish, rtPrintLock, rtPrintUnlock;
    llvm::FunctionCallee rtAlloc, rtFree, rtRetain, rtRelease, rtPrintEnter, rtPrintLeave, rtStrEq, rtHashBytes, rtTrapText;
    llvm::GlobalVariable* hashSeed = nullptr;
    llvm::FunctionCallee rtTrapIndex, rtTrapArrayIndex, rtVecNew, rtVecRelease, rtVecReserve;
    llvm::FunctionCallee rtStrRelease, rtStrCompare, rtStrConcat, rtStrAppend, rtStrSubstr, rtStrFind;
    llvm::FunctionCallee rtStrStartsWith, rtStrEndsWith, rtCString, rtCFree, rtStrFromC, rtStrFromChars;
    llvm::FunctionCallee rtPrintF32, rtPrintF64, rtPrintFloatSpec, rtTrapNarrow, rtTrapFloatCast;
    llvm::FunctionCallee rtPrintCharQuoted, rtTrapChar, rtTrapStringIndex, rtStrMutable, rtStrRepeat, rtPrintPtr;
    llvm::FunctionCallee rtNew, rtDeleteCheck, rtDeleteFree, rtStart, rtDynamicCast, rtRetainObject, rtObjectStart;
    llvm::FunctionCallee rtRetainClass, rtReleaseClass, rtFreeClass, rtDestructorHold, rtReferences;
    llvm::FunctionCallee rtWeakRetain, rtWeakRelease, rtWeakLock, rtWeakAlive, rtUncaught;
    llvm::GlobalVariable* charTable = nullptr;  // the 256 bytes, so a char can be a one-byte string
    llvm::Constant* fileName = nullptr;
    std::unordered_map<std::string, llvm::Constant*> cstrings;

    // In a debug build, the line table that lets a stack trace name files and lines.
    // `diFunction` is the C+ function being generated; generated helpers have none.
    std::unique_ptr<llvm::DIBuilder> di;
    llvm::DIFile* diFile = nullptr;
    llvm::DISubprogram* diFunction = nullptr;

    // How one value travels to a C function. A struct is passed in the values `parts`,
    // read from its bytes; on the stack, where the callee finds a copy; or as a pointer to
    // a copy. Anything else is passed as itself.
    struct CPass {
        enum Kind { Value, Parts, OnStack, ByPointer } kind = Value;
        std::vector<llvm::Type*> parts;
    };
    struct CSignature {
        std::vector<CPass> params;
        bool memoryResult = false;           // a struct returned through a hidden first pointer
        std::vector<llvm::Type*> resultParts;  // a struct returned in registers
    };
    std::unordered_map<const FuncDecl*, CSignature> cSignatures;
    std::string abiError;  // this platform's rules for passing structs are not known

    // What is generated for each struct and class.
    struct TypeInfo {
        // A struct's value, or a class object. An object starts with its reference count and a
        // pointer to its vtable, which is how code that only knows a base class finds what the
        // object really is. A class with base classes starts with the layout of each of them:
        // { i64 count, ptr vtable, fields... } or { base, base, fields... }.
        llvm::StructType* layout = nullptr;
        std::vector<BasePath> vtablePlaces;        // classes: see StructDecl::vtablePlaces
        std::vector<llvm::GlobalVariable*> vtables;  // classes: one for each place. The first also names the class
        llvm::Function* print = nullptr;
        llvm::Function* equals = nullptr;   // structs: compares two values field by field
        llvm::Function* retain = nullptr;   // structs that hold class references
        llvm::Function* release = nullptr;  // classes, and structs that hold class references
        llvm::Function* destroy = nullptr;  // classes
        int managed = -1;                   // structs: whether they hold class references (-1: not computed)
        int inMemory = -1;                  // structs: whether they hold arrays (-1: not computed)
    };
    std::unordered_map<const StructDecl*, TypeInfo> types;
    // A vtable is a table of addresses, for one way of seeing an object: the list of the parts of
    // the whole object, which dynamic_cast searches; how far the part with this vtable is from
    // the object's start; the destroy and print helpers; then the virtual methods by slot.
    static constexpr uint64_t kObjectVtableOffset = 8;
    static constexpr uint64_t kVtableType = 4;  // see CPLUS_VTABLE_TYPE in runtime/internal.h
    static constexpr uint64_t kVtableParts = 0, kVtableStart = 1, kVtableDestroy = 2, kVtablePrint = 3, kVtableMethods = 5;
    // Whether any class has more than one base class. Only then can a reference point into
    // the middle of an object, and only then does a reference count need to be looked for.
    bool hasInnerParts = false;
    llvm::Function* objectRelease = nullptr;  // void(ptr object), for every class
    std::map<std::pair<const FuncDecl*, uint64_t>, llvm::Function*> thunks;  // see vtableMethod
    std::unordered_map<const VarDecl*, llvm::Function*> constantFunctions;
    std::unordered_map<const Type*, llvm::Function*> hashFunctions;
    std::unordered_map<const Type*, llvm::GlobalVariable*> descriptors;
    std::map<std::pair<const Type*, bool>, llvm::Function*> countFunctions;
    bool usesDescriptors = false;  // a program that imports json or reflect: a class then says what it really is
    bool buildsFromDescriptors = false;  // one that imports json, which makes values from them too
    bool usesThreads = false;      // a program with a `spawn`
    bool usesAsync = false;        // a program with `async` or `await`, which has an event loop

    // An async function is two: the one that is called, which makes the Task and gives it, and a coroutine that runs the
    // body, which LLVM's coroutine passes cut into pieces at each `await`. What the body keeps across one goes in the
    // coroutine's frame. See defineAsync.
    struct AsyncState {
        llvm::Value* task;               // its Task, which the coroutine holds a reference to
        llvm::Value* id;                 // what llvm.coro.id gave
        llvm::Value* handle;             // the frame, from llvm.coro.begin
        llvm::BasicBlock* failed;        // for a function that can fail: the error is in errorOut
        llvm::BasicBlock* done;          // the task is finished: what the coroutine holds is let go of
        llvm::BasicBlock* freeFrame;     // and its frame
        llvm::BasicBlock* leave;         // back to whoever started or resumed it
    };
    std::optional<AsyncState> async;  // while the body of an async function is generated
    std::unordered_map<const FuncDecl*, llvm::Function*> asyncBodies;
    llvm::Function* resumeFn = nullptr;  // void(ptr frame): goes on with a coroutine, for the event loop
    llvm::FunctionCallee rtLoopFinish;
    llvm::FunctionCallee rtLoopWaitsForStart;

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
    struct ArrayInfo {  // generated for each array type, when first needed; all take addresses
        llvm::Function* print = nullptr;
        llvm::Function* equals = nullptr;
        llvm::Function* retain = nullptr;   // when elements hold references
        llvm::Function* release = nullptr;
    };
    std::unordered_map<const Type*, ArrayInfo> arrays;
    llvm::Function* vecSizeFn = nullptr;
    llvm::Function* vecReserveFn = nullptr;

    // Inside the rest of a `?.` chain, the value found in the optional.
    std::unordered_map<const OptValueExpr*, llvm::Value*> optValues;

    std::unordered_map<const FuncDecl*, llvm::Function*> funcs;
    std::unordered_map<const VarDecl*, llvm::Value*> locals;  // where each variable is stored
    llvm::Function* fn = nullptr;
    const FuncDecl* fnDecl = nullptr;
    llvm::Value* thisPtr = nullptr;  // in a method: the object, or the address of the struct

    // Reference counting. A local that holds class references owns them, and releases
    // them when its scope ends. A value made or read during a statement and not stored
    // anywhere is a temporary, released when the statement ends.
    std::vector<std::vector<const VarDecl*>> scopes;  // locals to release, per open scope
    std::deque<VarDecl> heldForScope;  // what a reference variable's place lies in, kept alive until its scope ends
    struct Temp {
        llvm::Value* value;
        const Type* type;
        bool inMemory;  // `value` is the address of the value
    };
    std::vector<Temp> temps;
    // Values of an expression still being worked out, owned and waiting to be used, such as the
    // arguments before a later one is evaluated. Only an error leaves them behind.
    std::vector<Temp> pending;

    // Errors. A function that can fail takes one more argument, the address of where to put an error,
    // and returns. Its caller looks there right after the call. That place holds null until then.
    const Type* errorTy = nullptr;           // `Error`
    llvm::Value* errorOut = nullptr;         // in a function that can fail: the caller's place
    llvm::AllocaInst* localError = nullptr;  // where calls inside a try block put their errors
    struct Handler {
        llvm::BasicBlock* dispatch;  // where the catches are chosen
        size_t scopeDepth;           // scopes opened inside the try block are released when leaving it
    };
    std::vector<Handler> handlers;  // the try blocks around the code being generated

    struct LoopTargets {
        llvm::BasicBlock* breakTo;
        llvm::BasicBlock* continueTo;
        size_t scopeDepth;  // scopes opened inside the loop are released when leaving it
    };
    std::vector<LoopTargets> loops;
    std::unordered_map<const Expr*, llvm::Value*> frozen;  // see freezePlace
    // While a changing struct method's `this` is found: the vectors it lies in, each as the
    // place holding the vector and the buffer held.
    std::vector<std::pair<llvm::Value*, llvm::Value*>>* heldBuffers = nullptr;

    Impl(const SourceFile& f, CodegenOptions o) : file(f), opts(o), b(ctx) {}

    // ---- Setup -------------------------------------------------------------

    bool setupTarget(std::string& error) {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        const char* tripleForTests = std::getenv("CPLUS_TARGET_FOR_TESTS");  // so the tests can reach what a machine with no target reports
        std::string triple = tripleForTests ? tripleForTests : llvm::sys::getDefaultTargetTriple();
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

    void setupLineTable() {
        mod->addModuleFlag(llvm::Module::Warning, "Debug Info Version", llvm::DEBUG_METADATA_VERSION);
        mod->addModuleFlag(llvm::Module::Warning, "Dwarf Version", 5);
        di = std::make_unique<llvm::DIBuilder>(*mod);
        diFile = di->createFile(file.path, "");  // as given, so a trace shows the path the user typed
        di->createCompileUnit(llvm::dwarf::DW_LANG_C_plus_plus, diFile, "cplus", opts.optimize, "", 0, "",
                              llvm::DICompileUnit::LineTablesOnly);
    }

    // Code generated from here on belongs to `loc`, in a stack trace.
    void setLine(Loc loc) {
        // Line 0 is what LLVM calls no line: code written in C+ that comes with the language. See Builtins.h.
        if (diFunction) b.SetCurrentDebugLocation(llvm::DILocation::get(ctx, loc.line, loc.col, diFunction));
    }

    // The same, until the end of a scope: an expression's operands have their own places,
    // and the expression's own code comes back to its place after them.
    struct AtLine {
        Impl& g;
        llvm::DebugLoc saved;
        AtLine(Impl& impl, Loc loc) : g(impl), saved(impl.b.getCurrentDebugLocation()) { g.setLine(loc); }
        ~AtLine() { g.b.SetCurrentDebugLocation(saved); }
    };

    void startLineTable(Loc loc) {
        if (!di) return;
        auto* type = di->createSubroutineType(di->getOrCreateTypeArray({}));
        diFunction = di->createFunction(diFile, fn->getName(), "", diFile, loc.line, type, loc.line,
                                        llvm::DINode::FlagZero, llvm::DISubprogram::SPFlagDefinition);
        fn->setSubprogram(diFunction);
        setLine(loc);
    }

    void endLineTable() {
        diFunction = nullptr;
        b.SetCurrentDebugLocation(llvm::DebugLoc());
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
        rtThreadStart = decl("cplus_thread_start", voidTy, {ptrTy, ptrTy});
        rtThreadsFinish = decl("cplus_threads_finish", voidTy, {});
        rtLoopFinish = decl("cplus_loop_finish", voidTy, {});
        rtLoopWaitsForStart = decl("cplus_loop_waits_for_start", voidTy, {b.getInt32Ty()});
        rtPrintLock = decl("cplus_print_lock", voidTy, {});
        rtPrintUnlock = decl("cplus_print_unlock", voidTy, {});
        rtAlloc = decl("cplus_alloc", ptrTy, {i64});
        rtFree = decl("cplus_free", voidTy, {ptrTy});
        rtRetain = decl("cplus_retain", voidTy, {ptrTy});
        rtRelease = decl("cplus_release", i32, {ptrTy});
        rtPrintEnter = decl("cplus_print_enter", i32, {ptrTy});
        rtPrintLeave = decl("cplus_print_leave", voidTy, {});
        rtStrEq = decl("cplus_str_eq", i32, {ptrTy, i64, ptrTy, i64});
        rtHashBytes = decl("cplus_hash_bytes", i64, {ptrTy, i64});
        rtTrapText = decl("cplus_trap_text", voidTy, {ptrTy, i64, ptrTy, i32, i32});
        hashSeed = new llvm::GlobalVariable(*mod, i64, false, llvm::GlobalValue::ExternalLinkage, nullptr, "cplus_hash_seed");
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
        rtStrFromChars = decl("cplus_str_from_chars", voidTy, {ptrTy, ptrTy, i64});
        rtTrapIndex = decl("cplus_trap_index", voidTy, {i64, i64, ptrTy, i32, i32});
        rtTrapArrayIndex = decl("cplus_trap_array_index", voidTy, {i64, i64, ptrTy, i32, i32});
        rtTrapStringIndex = decl("cplus_trap_string_index", voidTy, {i64, i64, ptrTy, i32, i32});
        rtTrapChar = decl("cplus_trap_char", voidTy, {i64, ptrTy, ptrTy, i32, i32});
        for (llvm::FunctionCallee trap : {rtTrapIndex, rtTrapArrayIndex, rtTrapStringIndex, rtTrapChar})
            if (auto* f = llvm::dyn_cast<llvm::Function>(trap.getCallee())) {
                f->addFnAttr(llvm::Attribute::NoReturn);
                f->addFnAttr(llvm::Attribute::Cold);
                f->addFnAttr(llvm::Attribute::NoUnwind);
            }
        rtPrintCharQuoted = decl("cplus_print_char_quoted", voidTy, {ptrTy, i32});
        rtPrintPtr = decl("cplus_print_ptr", voidTy, {ptrTy, ptrTy});
        rtNew = decl("cplus_new", ptrTy, {i64, i64, i32, ptrTy, i32, i32});
        rtDeleteCheck = decl("cplus_delete_check", i64, {ptrTy, i32, ptrTy, i32, i32});
        rtDeleteFree = decl("cplus_delete_free", voidTy, {ptrTy});
        rtStart = decl("cplus_start", voidTy, {});
        rtDynamicCast = decl("cplus_dynamic_cast", ptrTy, {ptrTy, ptrTy});
        rtRetainObject = decl("cplus_retain_object", voidTy, {ptrTy});
        rtRetainClass = decl("cplus_retain_class", voidTy, {ptrTy});
        rtReleaseClass = decl("cplus_release_class", i32, {ptrTy});
        rtFreeClass = decl("cplus_free_class", voidTy, {ptrTy});
        rtDestructorHold = decl("cplus_destructor_hold", voidTy, {ptrTy});
        rtReferences = decl("cplus_references", i64, {ptrTy});
        rtWeakRetain = decl("cplus_weak_retain", voidTy, {ptrTy});
        rtWeakRelease = decl("cplus_weak_release", voidTy, {ptrTy});
        rtWeakLock = decl("cplus_weak_lock", ptrTy, {ptrTy});
        rtWeakAlive = decl("cplus_weak_alive", ptrTy, {ptrTy});
        rtUncaught = decl("cplus_uncaught", voidTy, {ptrTy});
        rtObjectStart = decl("cplus_object_start", ptrTy, {ptrTy});
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
            case K::Array: return llvm::ArrayType::get(llvmType(t->elem), (uint64_t)t->length);
            case K::Class: case K::Vector: case K::Null: case K::Pointer: case K::Weak: case K::Unique: case K::Local: return b.getPtrTy();
            // An optional class reference is a pointer that may be null. Any other
            // optional is { i1 has, T value }, and its value is all zero bytes when it
            // has none, so retaining or releasing it is harmless either way.
            case K::Optional:
                if (t->elem->isObjectRef()) return b.getPtrTy();
                return llvm::StructType::get(ctx, {b.getInt1Ty(), llvmType(t->elem)});
            default: llvm_unreachable("unresolved type reached codegen");
        }
    }

    // Whether values of this type are handled through their address: an array, or what
    // holds one by value. LLVM is very slow at loading and storing a large aggregate as one
    // value, as C and Rust compilers also avoid, so here a value in the generated code is
    // the address of memory holding it, and copying one is a memory copy. That memory is
    // the value's own: nothing else changes it, so it can be used like any other value,
    // and a variable can take it over. See loadValue and storeValue.
    bool inMemory(const Type* t) {
        if (t->isArray()) return true;
        if (t->isOptional()) return inMemory(t->elem);
        if (!t->isStruct()) return false;
        TypeInfo& info = types.at(t->decl);
        if (info.inMemory < 0) {
            info.inMemory = 0;
            for (auto& f : t->decl->fields)
                if (inMemory(f.type)) info.inMemory = 1;
        }
        return info.inMemory == 1;
    }

    // The LLVM type of a value of this type in the generated code; llvmType is how it is
    // stored.
    llvm::Type* valueType(const Type* t) { return inMemory(t) ? b.getPtrTy() : llvmType(t); }

    llvm::Align alignOf(const Type* t) { return mod->getDataLayout().getABITypeAlign(llvmType(t)); }
    uint64_t sizeOf(const Type* t) { return mod->getDataLayout().getTypeAllocSize(llvmType(t)); }

    // A copy of the value stored at `addr`, for the value to outlive changes to the place.
    llvm::Value* loadValue(const Type* t, llvm::Value* addr, const char* name = "") {
        if (!inMemory(t)) return b.CreateLoad(llvmType(t), addr, name);
        llvm::AllocaInst* copy = entryAlloca(llvmType(t), "copy");
        b.CreateMemCpy(copy, alignOf(t), addr, alignOf(t), sizeOf(t));
        return copy;
    }

    // The value stored at `addr`, for a use that ends before the place can change: it is
    // released, printed, compared or copied right away.
    llvm::Value* peekValue(const Type* t, llvm::Value* addr, const char* name = "") {
        return inMemory(t) ? addr : b.CreateLoad(llvmType(t), addr, name);
    }

    void storeValue(llvm::Value* v, const Type* t, llvm::Value* addr) {
        if (inMemory(t)) b.CreateMemMove(addr, alignOf(t), v, alignOf(t), sizeOf(t));  // `a = a` is allowed
        else b.CreateStore(v, addr);
    }

    // The value of type `t` that is all zero bytes: an empty optional.
    llvm::Value* zeroValue(const Type* t) {
        if (!inMemory(t)) return llvm::Constant::getNullValue(llvmType(t));
        llvm::AllocaInst* zero = entryAlloca(llvmType(t), "zero");
        b.CreateMemSet(zero, b.getInt8(0), sizeOf(t), alignOf(t));
        return zero;
    }

    // Whether values of this type hold references to heap objects (class objects,
    // vector buffers, string buffers), which are retained when the value is copied and
    // released when it goes away.
    bool isManaged(const Type* t) {
        if (t->isObjectRef() || t->isVector() || t->is(K::String) || t->isWeak()) return true;
        if (t->isOptional() || t->isArray()) return isManaged(t->elem);
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

    void trapHere(const std::string& message, Loc loc) {
        auto* call = b.CreateCall(rtTrap, {cstring(message), fileName, b.getInt32(loc.line), b.getInt32(loc.col)});
        call->setDoesNotReturn();
        b.CreateUnreachable();
    }

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
    void checkIndex(llvm::Value* index, llvm::Value* size, Loc loc, llvm::FunctionCallee trap) {
        auto* trapBB = llvm::BasicBlock::Create(ctx, "bad.index", fn);
        auto* okBB = llvm::BasicBlock::Create(ctx, "ok", fn);
        // An unsigned compare also catches negative indexes.
        b.CreateCondBr(b.CreateICmpUGE(index, size), trapBB, okBB, llvm::MDBuilder(ctx).createBranchWeights(1, 1u << 20));
        b.SetInsertPoint(trapBB);
        auto* call = b.CreateCall(trap, {index, size, fileName, b.getInt32(loc.line), b.getInt32(loc.col)});
        call->setDoesNotReturn();
        b.CreateUnreachable();
        b.SetInsertPoint(okBB);
    }

    // Stops the program when the pointer `p` is null. `op` is how the pointer is followed:
    // `*`, `->` or `[ ]`.
    void checkNotNull(llvm::Value* p, const std::string& op, Loc loc) {
        trapIf(b.CreateIsNull(p), "null pointer: '" + op + "' was used on a pointer that is null", loc);
    }

    // In a debug build, stops the program when `p` is not a multiple of what `elem` needs,
    // which LLVM takes for granted and a cast from a char* can break.
    void checkAligned(llvm::Value* p, const Type* elem, const std::string& op, Loc loc) {
        if (!opts.debug) return;
        uint64_t align = mod->getDataLayout().getABITypeAlign(llvmType(elem)).value();
        if (align <= 1) return;
        llvm::Value* offset = b.CreateAnd(b.CreatePtrToInt(p, b.getInt64Ty()), b.getInt64(align - 1));
        trapIf(b.CreateIsNotNull(offset),
               "misaligned pointer: '" + op + "' was used on a pointer to '" + elem->name +
                   "' whose address is not a multiple of " + std::to_string(align),
               loc);
    }

    // Reads a place that a pointer may lead to. In a debug build, a bool read from a byte
    // other than 0 or 1 stops the program, since LLVM takes those two for granted.
    llvm::Value* loadPlace(const Expr& e, const char* name) {
        llvm::Value* place = addressOf(e);
        if (opts.debug && e.type->is(K::Bool))
            trapIf(b.CreateICmpUGT(b.CreateLoad(b.getInt8Ty(), place, "bool.byte"), b.getInt8(1)),
                   "invalid bool: a 'bool' was read from a byte that is neither 0 nor 1", e.loc);
        return loadValue(e.type, place, name);
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
        if (t->elem->isObjectRef()) return b.CreateIsNotNull(opt, "has");
        if (inMemory(t)) return b.CreateLoad(b.getInt1Ty(), b.CreateStructGEP(llvmType(t), opt, 0), "has");
        return b.CreateExtractValue(opt, 0, "has");
    }

    llvm::Value* valueOf(llvm::Value* opt, const Type* t) {
        if (t->elem->isObjectRef()) return opt;
        if (inMemory(t)) return b.CreateStructGEP(llvmType(t), opt, 1, "value");
        return b.CreateExtractValue(opt, 1, "value");
    }

    llvm::Value* wrapOptional(llvm::Value* v, const Type* t) {
        if (t->elem->isObjectRef()) return v;
        if (inMemory(t)) {
            llvm::AllocaInst* opt = entryAlloca(llvmType(t), "opt");
            b.CreateStore(b.getInt1(true), b.CreateStructGEP(llvmType(t), opt, 0));
            storeValue(v, t->elem, b.CreateStructGEP(llvmType(t), opt, 1));
            return opt;
        }
        llvm::Value* opt = b.CreateInsertValue(llvm::PoisonValue::get(llvmType(t)), b.getInt1(true), {0});
        return b.CreateInsertValue(opt, v, {1});
    }

    // Count work for a local reference, without the atomic steps that are what cost. The word holds more than the count, as
    // runtime.c explains. What is not the usual case goes to the runtime: the part of an object that does not start it, the
    // last reference, and anything else it must look at.
    static constexpr int64_t kStrongMask = 0xffffffff;
    static constexpr int64_t kStrongMax = 0x7fffffff;
    llvm::Function* localRetain = nullptr;
    llvm::Function* localRelease = nullptr;

    llvm::Function* localRetainFunction() {
        if (localRetain) return localRetain;
        localRetain = internalFunction("cplus.retain_local", b.getVoidTy(), {b.getPtrTy()});
        SavedPoint saved(*this);
        startHelper(localRetain);
        llvm::Value* object = localRetain->getArg(0);
        auto* countBB = llvm::BasicBlock::Create(ctx, "count", fn);
        auto* fastBB = llvm::BasicBlock::Create(ctx, "fast", fn);
        auto* slowBB = llvm::BasicBlock::Create(ctx, "slow", fn);
        auto* doneBB = llvm::BasicBlock::Create(ctx, "done", fn);
        b.CreateCondBr(b.CreateIsNull(object), doneBB, countBB);
        b.SetInsertPoint(countBB);
        llvm::Value* word = b.CreateAlignedLoad(b.getInt64Ty(), object, llvm::MaybeAlign(8), "word");
        llvm::Value* strong = b.CreateAnd(word, b.getInt64(kStrongMask));
        llvm::Value* usual = b.CreateAnd(b.CreateICmpSGT(word, b.getInt64(0)),
                                         b.CreateAnd(b.CreateICmpNE(strong, b.getInt64(0)), b.CreateICmpSLT(strong, b.getInt64(kStrongMax))));
        b.CreateCondBr(usual, fastBB, slowBB);
        b.SetInsertPoint(fastBB);
        b.CreateAlignedStore(b.CreateAdd(word, b.getInt64(1)), object, llvm::MaybeAlign(8));
        b.CreateRetVoid();
        b.SetInsertPoint(slowBB);
        b.CreateCall(hasInnerParts ? rtRetainObject : rtRetainClass, {object});
        b.CreateRetVoid();
        b.SetInsertPoint(doneBB);
        b.CreateRetVoid();
        return localRetain;
    }

    llvm::Function* localReleaseFunction() {
        if (localRelease) return localRelease;
        localRelease = internalFunction("cplus.release_local", b.getVoidTy(), {b.getPtrTy()});
        SavedPoint saved(*this);
        startHelper(localRelease);
        llvm::Value* object = localRelease->getArg(0);
        auto* countBB = llvm::BasicBlock::Create(ctx, "count", fn);
        auto* fastBB = llvm::BasicBlock::Create(ctx, "fast", fn);
        auto* slowBB = llvm::BasicBlock::Create(ctx, "slow", fn);
        auto* doneBB = llvm::BasicBlock::Create(ctx, "done", fn);
        b.CreateCondBr(b.CreateIsNull(object), doneBB, countBB);
        b.SetInsertPoint(countBB);
        llvm::Value* word = b.CreateAlignedLoad(b.getInt64Ty(), object, llvm::MaybeAlign(8), "word");
        llvm::Value* strong = b.CreateAnd(word, b.getInt64(kStrongMask));
        b.CreateCondBr(b.CreateAnd(b.CreateICmpSGT(word, b.getInt64(0)), b.CreateICmpSGT(strong, b.getInt64(1))), fastBB, slowBB);
        b.SetInsertPoint(fastBB);
        b.CreateAlignedStore(b.CreateSub(word, b.getInt64(1)), object, llvm::MaybeAlign(8));
        b.CreateRetVoid();
        b.SetInsertPoint(slowBB);
        b.CreateCall(objectRelease, {object});
        b.CreateRetVoid();
        b.SetInsertPoint(doneBB);
        b.CreateRetVoid();
        return localRelease;
    }

    void retainValue(llvm::Value* v, const Type* t) {
        if (t->isOptional()) return retainValue(valueOf(v, t), t->elem);
        if (t->isLocal()) {
            b.CreateCall(localRetainFunction(), {v});
            return;
        }
        if (t->isObjectRef()) b.CreateCall(hasInnerParts ? rtRetainObject : rtRetainClass, {v});
        else if (t->isWeak()) b.CreateCall(rtWeakRetain, {v});
        else if (t->isVector()) b.CreateCall(rtRetain, {v});
        else if (t->is(K::String)) b.CreateCall(rtRetain, {b.CreateExtractValue(v, 2, "owner")});
        else if (!isManaged(t)) return;
        else if (t->isArray()) b.CreateCall(arrayInfo(t).retain, {v});
        else b.CreateCall(types.at(t->decl).retain, {v});
    }

    void releaseValue(llvm::Value* v, const Type* t) {
        if (t->isOptional()) return releaseValue(valueOf(v, t), t->elem);
        if (t->isLocal()) {
            b.CreateCall(localReleaseFunction(), {v});
            return;
        }
        if (t->isWeak()) {
            b.CreateCall(rtWeakRelease, {v});
        } else if (t->is(K::String)) {
            b.CreateCall(rtStrRelease, {b.CreateExtractValue(v, 2, "owner")});
        } else if (t->isVector()) {
            VecInfo& info = vecInfo(t);
            b.CreateCall(rtVecRelease, {v, b.getInt64(info.elemSize), info.releaseElem});
        } else if (!isManaged(t)) {
            return;
        } else if (t->isArray()) {
            b.CreateCall(arrayInfo(t).release, {v});
        } else {
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

    // A function of runtime/strings.c, declared where it is first used, so a program that uses none of them links none.
    llvm::FunctionCallee stringRuntime(const char* name, llvm::Type* result, std::vector<llvm::Type*> params) {
        return mod->getOrInsertFunction(name, llvm::FunctionType::get(result, params, false));
    }

    // A string value, in memory, for a runtime function that takes its address.
    llvm::Value* stringInMemory(llvm::Value* s, const char* name) {
        llvm::AllocaInst* slot = entryAlloca(stringTy, name);
        b.CreateStore(s, slot);
        return slot;
    }

    static bool isStringEdit(StrMethod m) {
        switch (m) {
            case StrMethod::Append: case StrMethod::Insert: case StrMethod::Erase: case StrMethod::Replace: case StrMethod::Rfind:
            case StrMethod::Compare: case StrMethod::Split: case StrMethod::Trim: case StrMethod::TrimStart: case StrMethod::TrimEnd:
            case StrMethod::ReplaceAll: return true;
            default: return false;
        }
    }

    // The string methods that Sema's kStringEdits lists.
    llvm::Value* genStringEdit(const MethodCallExpr& call) {
        const Expr& recv = *call.receiver;
        auto* ptrTy = b.getPtrTy();
        auto* i64 = b.getInt64Ty();
        auto* i32 = b.getInt32Ty();
        llvm::Value* line = b.getInt32(call.loc.line);
        llvm::Value* column = b.getInt32(call.loc.col);
        const StrMethod m = call.strMethod;
        if (m == StrMethod::Append || m == StrMethod::Insert || m == StrMethod::Erase || m == StrMethod::Replace) {
            // The string is found after the arguments, so code they run cannot move it from under the change.
            const size_t textAt = m == StrMethod::Append ? 0 : m == StrMethod::Insert ? 1 : m == StrMethod::Replace ? 2 : 3;
            std::vector<llvm::Value*> args;
            llvm::Value* slot = placeAfter(recv, [&] {
                for (size_t i = 0; i < call.args.size(); ++i)
                    args.push_back(i == textAt ? textValue(gen(*call.args[i]), call.args[i]->type) : genConverted(*call.args[i], Type::longTy()));
            });
            if (m == StrMethod::Append) {  // as `s += text`
                b.CreateCall(rtStrAppend, {slot, strData(args[0]), strLength(args[0])});
                return nullptr;
            }
            llvm::Value* pos = args.size() > 0 && textAt != 0 ? args[0] : b.getInt64(0);
            llvm::Value* count = m == StrMethod::Insert ? b.getInt64(0) : args.size() > 1 ? args[1] : b.getInt64(-1);  // string::npos: the rest
            llvm::Value* text = textAt < args.size() ? stringInMemory(args[textAt], "text") : llvm::ConstantPointerNull::get(ptrTy);
            b.CreateCall(stringRuntime("cplus_str_splice", b.getVoidTy(), {ptrTy, i64, i64, ptrTy, ptrTy, ptrTy, i32, i32}),
                         {slot, pos, count, text, cstring(call.name), fileName, line, column});
            return nullptr;
        }
        // The receiver is evaluated before the arguments, and read in place unless an argument runs code that could change it.
        bool argsRunCode = false;
        for (auto& a : call.args) argsRunCode = argsRunCode || mayRunCode(*a);
        llvm::Value* s = argsRunCode ? gen(recv) : stringForRead(recv);
        std::vector<llvm::Value*> args;
        for (size_t i = 0; i < call.args.size(); ++i) {
            const Expr& a = *call.args[i];
            if (m == StrMethod::Rfind && i == 1) args.push_back(genConverted(a, Type::longTy()));
            else if (a.type->is(K::Char)) args.push_back(charString(gen(a)));  // `s.split(',')`
            else args.push_back(argsRunCode ? gen(a) : stringForRead(a));
        }
        switch (m) {
            case StrMethod::Rfind: {
                llvm::Value* from = args.size() > 1 ? args[1] : b.getInt64(-1);
                if (args.size() > 1) trapIf(b.CreateICmpSLT(from, b.getInt64(-1)), "rfind() was given a negative position", call.loc);
                return b.CreateCall(stringRuntime("cplus_str_rfind", i64, {ptrTy, i64, ptrTy, i64, i64}),
                                    {strData(s), strLength(s), strData(args[0]), strLength(args[0]), from}, "pos");
            }
            case StrMethod::Compare: return b.CreateCall(rtStrCompare, {strData(s), strLength(s), strData(args[0]), strLength(args[0])}, "order");
            case StrMethod::Split:
                return b.CreateCall(stringRuntime("cplus_str_split", ptrTy, {ptrTy, ptrTy, ptrTy, i32, i32}),
                                    {stringInMemory(s, "str"), stringInMemory(args[0], "sep"), fileName, line, column}, "parts");
            case StrMethod::Trim:
            case StrMethod::TrimStart:
            case StrMethod::TrimEnd: {
                llvm::AllocaInst* out = entryAlloca(stringTy, "trimmed");
                const int sides = m == StrMethod::TrimStart ? 1 : m == StrMethod::TrimEnd ? 2 : 3;
                b.CreateCall(stringRuntime("cplus_str_trim", b.getVoidTy(), {ptrTy, ptrTy, i32}), {out, stringInMemory(s, "str"), b.getInt32(sides)});
                return b.CreateLoad(stringTy, out, "trimmed");  // shares the bytes of `s`, with its own reference
            }
            case StrMethod::ReplaceAll: {
                llvm::AllocaInst* out = entryAlloca(stringTy, "replaced");
                b.CreateCall(stringRuntime("cplus_str_replace_all", b.getVoidTy(), {ptrTy, ptrTy, ptrTy, ptrTy, ptrTy, i32, i32}),
                             {out, stringInMemory(s, "str"), stringInMemory(args[0], "from"), stringInMemory(args[1], "to"), fileName, line, column});
                return b.CreateLoad(stringTy, out, "replaced");
            }
            default: break;
        }
        llvm_unreachable("unknown string method");
    }

    llvm::Value* genStringMethod(const MethodCallExpr& call) {
        if (isStringEdit(call.strMethod)) return genStringEdit(call);
        const Expr& recv = *call.receiver;
        if (call.strMethod == StrMethod::Clear) {
            llvm::Value* slot = addressOf(recv, false, true);
            llvm::Value* old = b.CreateLoad(stringTy, slot, "old");
            b.CreateStore(llvm::Constant::getNullValue(stringTy), slot);
            releaseValue(old, Type::stringTy());
            return nullptr;
        }
        if (call.strMethod == StrMethod::PushBack) {  // `s += c`
            llvm::Value* c = nullptr;
            llvm::Value* slot = placeAfter(recv, [&] { c = gen(*call.args[0]); });
            b.CreateCall(rtStrAppend, {slot, charBytes(c), b.getInt64(1)});
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
            default: break;  // genStringEdit
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
        llvm::DebugLoc line;
        llvm::DISubprogram* diFunction;
        std::optional<AsyncState> async;
        explicit SavedPoint(Impl& impl)
            : g(impl), ip(impl.b.saveIP()), fn(impl.fn), line(impl.b.getCurrentDebugLocation()),
              diFunction(impl.diFunction), async(impl.async) {
            g.endLineTable();
            g.async.reset();
        }
        ~SavedPoint() {
            g.b.restoreIP(ip);
            g.async = async;
            g.fn = fn;
            g.b.SetCurrentDebugLocation(line);
            g.diFunction = diFunction;
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
            printValue(out, peekValue(elem, elementAt(v, i, vt)), elem, true);
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
            llvm::Value* x = peekValue(elem, elementAt(l, i, vt));
            llvm::Value* y = peekValue(elem, elementAt(r, i, vt));
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
            llvm::Value* x = peekValue(elem, f->getArg(0));
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
        pending.push_back({buf, vt, false});  // what it holds so far, the rest being zero bytes, which release safely
        for (size_t i = 0; i < list.elements.size(); ++i)
            storeValue(genOwned(*list.elements[i].value, vt->elem), vt->elem, elementAt(buf, b.getInt64(i), vt));
        pending.pop_back();
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
        // New elements are zero bytes, which is the default of most types.
        if (value || !defaultIsZero(elem)) {
            countedLoop(count, [&](llvm::Value* i) {
                if (!value) return fillElement(elementAt(buf, i, vt), elem);
                retainValue(value, elem);  // each element holds its own reference
                storeValue(value, elem, elementAt(buf, i, vt));
            });
            if (value) releaseValue(value, elem);
        }
        return buf;
    }

    llvm::Value* genVectorMethod(const MethodCallExpr& call) {
        const Expr& recv = *call.receiver;
        const Type* vt = recv.type;
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
                llvm::Value* v = loadValue(vt->elem, elementAt(buf, index, vt), call.name.c_str());
                retainValue(v, vt->elem);  // a method call's result is owned
                return v;
            }
            case VecMethod::PushBack: {
                // The vector is found after the value, so code it runs cannot move the
                // vector away from under the store.
                llvm::Value* value = nullptr;
                llvm::Value* slot = placeAfter(recv, [&] { value = genOwned(*call.args[0], vt->elem); });
                llvm::Value* buf = b.CreateLoad(b.getPtrTy(), slot, "vec");
                llvm::Value* size = vecSize(buf);
                buf = reserve(slot, buf, vt, b.CreateAdd(size, b.getInt64(1)));
                storeValue(value, vt->elem, elementAt(buf, size, vt));
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
                if (isManaged(vt->elem)) releaseValue(peekValue(vt->elem, elementAt(buf, last, vt)), vt->elem);
                return nullptr;
            }
            case VecMethod::Clear: {
                llvm::Value* slot = addressOf(recv, false, true);
                llvm::Value* old = b.CreateLoad(b.getPtrTy(), slot, "old");
                b.CreateStore(llvm::ConstantPointerNull::get(b.getPtrTy()), slot);
                releaseValue(old, vt);
                return nullptr;
            }
            case VecMethod::Join: {  // `parts.join(sep)`
                const Expr& sep = *call.args[0];
                llvm::Value* buf = mayRunCode(sep) ? gen(recv) : vectorForRead(recv);
                llvm::Value* text = textValue(gen(sep), sep.type);
                llvm::AllocaInst* out = entryAlloca(stringTy, "joined");
                b.CreateCall(stringRuntime("cplus_str_join", b.getVoidTy(), {b.getPtrTy(), b.getPtrTy(), b.getPtrTy()}),
                             {out, buf, stringInMemory(text, "sep")});
                return b.CreateLoad(stringTy, out, "joined");
            }
            case VecMethod::Fill: case VecMethod::Data:
            case VecMethod::None: break;
        }
        llvm_unreachable("unknown vector method");
    }

    // ---- Arrays ------------------------------------------------------------

    llvm::Value* arrayElement(llvm::Value* array, llvm::Value* index, const Type* at) {
        return b.CreateInBoundsGEP(llvmType(at->elem), array, index, "elem.addr");
    }

    ArrayInfo& arrayInfo(const Type* at) {
        auto [it, inserted] = arrays.try_emplace(at);
        ArrayInfo& info = it->second;
        if (!inserted) return info;
        const Type* elem = at->elem;
        auto* ptrTy = b.getPtrTy();
        std::string prefix = std::string("cplus.") + at->name + "#";
        llvm::Value* length = b.getInt64(at->length);
        // Everything is declared before any body is generated, so element types that lead
        // back to this array find its helpers.
        info.print = internalFunction(prefix + "print", b.getVoidTy(), {ptrTy, ptrTy});  // the array, the output
        info.equals = internalFunction(prefix + "equals", b.getInt1Ty(), {ptrTy, ptrTy});
        if (isManaged(elem)) {
            info.retain = internalFunction(prefix + "retain", b.getVoidTy(), {ptrTy});
            info.release = internalFunction(prefix + "release", b.getVoidTy(), {ptrTy});
        }
        SavedPoint saved(*this);

        // Printing: `[1, 2, 3]`, as a vector prints.
        startHelper(info.print);
        llvm::Value* a = info.print->getArg(0);
        llvm::Value* out = info.print->getArg(1);
        printText(out, "[");
        countedLoop(length, [&](llvm::Value* i) {
            auto* sepBB = llvm::BasicBlock::Create(ctx, "sep", fn);
            auto* elemBB = llvm::BasicBlock::Create(ctx, "elem", fn);
            b.CreateCondBr(b.CreateICmpNE(i, b.getInt64(0)), sepBB, elemBB);
            b.SetInsertPoint(sepBB);
            printText(out, ", ");
            b.CreateBr(elemBB);
            b.SetInsertPoint(elemBB);
            printValue(out, peekValue(elem, arrayElement(a, i, at)), elem, true);
        });
        printText(out, "]");
        b.CreateRetVoid();

        // Equality: each pair of elements equal.
        startHelper(info.equals);
        llvm::Value* l = info.equals->getArg(0);
        llvm::Value* r = info.equals->getArg(1);
        auto* differBB = llvm::BasicBlock::Create(ctx, "differ", fn);
        countedLoop(length, [&](llvm::Value* i) {
            llvm::Value* same = genEquals(peekValue(elem, arrayElement(l, i, at)), peekValue(elem, arrayElement(r, i, at)), elem);
            auto* nextBB = llvm::BasicBlock::Create(ctx, "next", fn);
            b.CreateCondBr(same, nextBB, differBB);
            b.SetInsertPoint(nextBB);
        });
        b.CreateRet(b.getInt1(true));
        b.SetInsertPoint(differBB);
        b.CreateRet(b.getInt1(false));

        // Retaining and releasing every element, the last one first, as C++ destroys them.
        if (info.retain) {
            startHelper(info.retain);
            countedLoop(length, [&](llvm::Value* i) {
                retainValue(peekValue(elem, arrayElement(info.retain->getArg(0), i, at)), elem);
            });
            b.CreateRetVoid();
            startHelper(info.release);
            countedLoop(length, [&](llvm::Value* i) {
                llvm::Value* index = b.CreateSub(b.CreateSub(length, b.getInt64(1)), i);
                releaseValue(peekValue(elem, arrayElement(info.release->getArg(0), index, at)), elem);
            });
            b.CreateRetVoid();
        }
        return info;
    }

    // `{1, 2, 3}` for an array: the elements are evaluated in order, and those not listed
    // get their default.
    llvm::Value* genArrayList(const InitListExpr& list, const Type* at) {
        const Type* elem = at->elem;
        llvm::AllocaInst* out = entryAlloca(llvmType(at), "array");
        uint64_t given = list.elements.size();
        size_t mark = pending.size();
        for (uint64_t i = 0; i < given; ++i) {
            llvm::Value* place = arrayElement(out, b.getInt64(i), at);
            storeValue(genOwned(*list.elements[i].value, elem), elem, place);
            if (isManaged(elem)) pending.push_back({place, elem, true});
        }
        pending.resize(mark);
        uint64_t rest = (uint64_t)at->length - given;
        if (defaultIsZero(elem)) {
            if (rest) b.CreateMemSet(arrayElement(out, b.getInt64(given), at), b.getInt8(0), rest * sizeOf(elem), alignOf(elem));
        } else {
            countedLoop(b.getInt64(rest), [&](llvm::Value* i) {
                fillElement(arrayElement(out, b.CreateAdd(i, b.getInt64(given)), at), elem);
            });
        }
        return out;
    }

    // The value of an array that is only read right away, in place if it is stored somewhere.
    llvm::Value* arrayForRead(const Expr& e) { return isPlace(e) ? addressOf(e) : gen(e); }

    llvm::Value* genArrayMethod(const MethodCallExpr& call) {
        const Expr& recv = *call.receiver;
        const Type* at = recv.type;
        const Type* elem = at->elem;
        switch (call.vecMethod) {
            case VecMethod::Size:
            case VecMethod::Empty:
                arrayForRead(recv);  // only for what it does: the size is known, but indexes are still checked
                if (call.vecMethod == VecMethod::Size) return b.getInt64(at->length);
                return b.getInt1(at->length == 0);
            case VecMethod::Front:
            case VecMethod::Back: {
                bool front = call.vecMethod == VecMethod::Front;
                llvm::Value* a = arrayForRead(recv);
                if (at->length == 0)
                    trapIf(b.getInt1(true), std::string(front ? "front()" : "back()") + " was called on an array with no elements", call.loc);
                llvm::Value* index = b.getInt64(front ? 0 : at->length - 1);
                llvm::Value* v = loadValue(elem, arrayElement(a, index, at), call.name.c_str());
                retainValue(v, elem);  // a method call's result is owned
                return v;
            }
            case VecMethod::Fill: {
                // The array is found after the value, and each element gets its own copy.
                llvm::Value* value = nullptr;
                llvm::Value* a = placeAfter(recv, [&] { value = genOwned(*call.args[0], elem); });
                countedLoop(b.getInt64(at->length), [&](llvm::Value* i) {
                    llvm::Value* place = arrayElement(a, i, at);
                    llvm::Value* old = isManaged(elem) ? loadValue(elem, place, "old") : nullptr;
                    retainValue(value, elem);
                    storeValue(value, elem, place);
                    if (old) releaseValue(old, elem);
                });
                releaseValue(value, elem);
                return nullptr;
            }
            case VecMethod::Data: return addressOf(recv);
            default: break;
        }
        llvm_unreachable("unknown array method");
    }

    // Releases the temporaries made since `mark`, newest first.
    void flushTemps(size_t mark = 0) {
        while (temps.size() > mark) {
            Temp t = temps.back();
            temps.pop_back();
            releaseValue(t.inMemory ? peekValue(t.type, t.value) : t.value, t.type);
        }
    }

    // Releases the locals of every scope from `depth` on, innermost first.
    void releaseScopes(size_t depth) {
        for (size_t i = scopes.size(); i-- > depth;)
            for (auto it = scopes[i].rbegin(); it != scopes[i].rend(); ++it)
                releaseValue(peekValue((*it)->type, locals.at(*it), (*it)->name.c_str()), (*it)->type);
    }

    void popScope() {
        releaseScopes(scopes.size() - 1);
        scopes.pop_back();
    }

    // ---- Expressions -------------------------------------------------------

    llvm::Value* convert(llvm::Value* v, const Type* from, const Type* to) {
        if (from == to) return v;
        if (from->isNull()) return zeroValue(to);
        if (to->isWeak() || (from->isOptional() && to->isOptional())) {  // `Dog?` to `Animal?`, or an object to `weak<Animal>`
            const Type* fromClass = from->isObjectRef() ? from : from->elem;
            uint64_t offset = pathOffset(fromClass->decl->pathTo(to->elem->decl));
            if (!offset) return v;
            return b.CreateSelect(b.CreateIsNull(v), v, atOffset(v, offset));
        }
        if (to->isOptional()) return wrapOptional(convert(v, from, to->elem), to);
        if (from->isObjectRef() && to->isObjectRef()) return atOffset(v, pathOffset(from->decl->pathTo(to->decl)));  // to a base class
        if (from->is(K::Int) && to->is(K::Long)) return b.CreateSExt(v, b.getInt64Ty());
        if (from->is(K::Char) && to->isInteger()) return b.CreateZExt(v, llvmType(to));  // its code, 0 to 255
        if (from->is(K::Char) && to->isFloating()) return b.CreateUIToFP(v, llvmType(to));
        if (from->isNumber() && to->is(K::Bool)) return condValue(v, from);
        if (from->isInteger() && to->isFloating()) return b.CreateSIToFP(v, llvmType(to));
        if (from->is(K::Float) && to->is(K::Double)) return b.CreateFPExt(v, b.getDoubleTy());
        if (from->is(K::Double) && to->is(K::Float)) return b.CreateFPTrunc(v, b.getFloatTy());  // a constant
        if (from->isPointer() && to->isPointer()) return v;  // to `const T*` or `void*`: the same address
        llvm_unreachable("Sema allowed a conversion Codegen does not know");
    }

    // Evaluates `e` for use during the current statement. A value holding class
    // references stays valid until the statement ends: a new one is released then,
    // and one read from a variable or field is retained first, so reassigning that
    // variable later in the statement cannot free it.
    llvm::Value* gen(const Expr& e) {
        AtLine line(*this, e.loc);
        bool owned = false;
        llvm::Value* v = genRaw(e, owned);
        bool isThis = e.kind == Expr::Kind::This && e.type->isClass();  // kept alive by the caller
        if (v && !isThis && isManaged(e.type)) {
            // What a local reference leads to is held the way that reference counts.
            const Type* counted = e.type;
            if (e.kind == Expr::Kind::Borrow && static_cast<const BorrowExpr&>(e).operand->type->isLocal())
                counted = static_cast<const BorrowExpr&>(e).operand->type;
            if (!owned) retainValue(v, counted);
            temps.push_back({v, counted, false});
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
        llvm::Value* raw = genRaw(e, owned);
        llvm::Value* v = convert(raw, e.type, to);
        if (to->isWeak() && !e.type->isWeak()) {  // a strong reference it was made from goes away
            retainValue(v, to);
            if (owned) releaseValue(raw, e.type);
            return v;
        }
        if (!owned) retainValue(v, to);
        return v;
    }

    // A reference is passed as the address of a place. A class object is already a reference, so a `const C&` is the object.
    static bool passedByRef(const VarDecl& p) { return p.isRef && (!p.isConst || !p.type->isClass()); }

    // Whether a place stays where it is while the variable it lies in lives: it is reached from the variable, or from `this`,
    // through fields of structs and elements of arrays only.
    static bool stablePlace(const Expr& e) {
        switch (e.kind) {
            case Expr::Kind::Name: case Expr::Kind::This: return true;
            case Expr::Kind::Member: {
                const Expr& base = *static_cast<const MemberExpr&>(e).base;
                return !base.type->isClass() && stablePlace(base);
            }
            case Expr::Kind::Index: {
                const Expr& base = *static_cast<const IndexExpr&>(e).base;
                return base.type->isArray() && stablePlace(base);
            }
            default: return false;
        }
    }

    // Whether a place lies in an element of a vector, a string or a hashmap, before any class object on the way to its variable.
    static bool inContainerElement(const Expr& e) {
        switch (e.kind) {
            case Expr::Kind::Member: {
                const Expr& base = *static_cast<const MemberExpr&>(e).base;
                return !base.type->isClass() && inContainerElement(base);
            }
            case Expr::Kind::Index: {
                const Expr& base = *static_cast<const IndexExpr&>(e).base;
                if (base.type->isArray()) return inContainerElement(base);
                return !base.type->isPointer();
            }
            default: return false;
        }
    }

    // What a reference parameter is given: the address of the argument, if it is a place of the parameter's type, and otherwise of
    // a temporary that lives until the statement ends, as in C++. What holds the place is kept alive meanwhile.
    llvm::Value* refArgument(const Expr& e, const Type* to, bool write = false) {
        if (isPlace(e) && e.type == to) return addressOf(e, /*hold=*/true, write);
        llvm::Value* v = genOwned(e, to);
        llvm::Value* slot = v;  // a value handled through its address already has its place
        if (!inMemory(to)) {
            slot = entryAlloca(llvmType(to), "ref.tmp");
            b.CreateStore(v, slot);
        }
        if (isManaged(to)) temps.push_back({slot, to, true});
        return slot;
    }

    llvm::Value* argument(const Expr& e, const VarDecl& param) {
        return passedByRef(param) ? refArgument(e, param.type, !param.isConst) : holdOwned(e, param.type);
    }

    // Keeps the temporaries made since `mark` until the current scope ends, instead of the statement.
    void keepTempsForScope(size_t mark) {
        for (size_t i = mark; i < temps.size(); ++i) {
            const Temp& t = temps[i];
            VarDecl& held = heldForScope.emplace_back(Loc{}, "held", t.type, Loc{});
            held.type = t.type;
            if (t.inMemory) {
                locals[&held] = t.value;
            } else {
                llvm::AllocaInst* slot = entryAlloca(llvmType(t.type), "held");
                b.CreateStore(t.value, slot);
                locals[&held] = slot;
            }
            scopes.back().push_back(&held);
        }
        temps.resize(mark);
    }

    // The same, for a value that waits while more of the expression is worked out: an error there
    // releases it. The caller removes it from `pending` once it is used.
    llvm::Value* holdOwned(const Expr& e, const Type* to) {
        llvm::Value* v = genOwned(e, to);
        if (isManaged(to)) pending.push_back({v, to, false});
        return v;
    }

    // Evaluates an expression whose value is not used.
    void genDiscarded(const Expr& e) {
        if (e.kind == Expr::Kind::Assign && inMemory(e.type)) {  // no copy of the value is made to return it
            genMemoryAssign(static_cast<const AssignExpr&>(e));
            flushTemps();
            return;
        }
        bool owned = false;
        llvm::Value* v = genRaw(e, owned);
        if (v && owned) releaseValue(v, e.type);
        flushTemps();
    }

    // The value a declaration without an initializer starts with: numbers 0, bools
    // false, strings empty, struct fields their defaults, and class references none.
    // Definite assignment makes sure a missing class reference is never read.
    llvm::Value* genDefault(const Type* t) {
        if (inMemory(t)) {
            llvm::AllocaInst* out = entryAlloca(llvmType(t), "default");
            fillDefault(out, t);
            return out;
        }
        switch (t->kind) {
            case K::Bool: return b.getInt1(false);
            case K::Int: case K::Long: case K::Char: return llvm::ConstantInt::get(llvmType(t), 0);
            case K::Float: case K::Double: return llvm::ConstantFP::get(llvmType(t), 0.0);
            case K::String: return llvm::Constant::getNullValue(stringTy);
            case K::Struct:
                if (const FuncDecl* ctor = t->decl->constructor; ctor && ctor->params.empty()) return construct(*t->decl, {});
                return buildAggregate(*t->decl, {});
            case K::Class: case K::Vector: case K::Pointer: case K::Weak: case K::Unique: case K::Local:
                return llvm::ConstantPointerNull::get(b.getPtrTy());
            case K::Optional: return zeroValue(t);  // null
            default: llvm_unreachable("no default value for this type");
        }
    }

    // Whether the default value of `t` is all zero bytes, so that memory set to zero
    // already holds it. So it is for a type whose constructor needs arguments, which has no
    // default: Sema makes sure the zero bytes are replaced before they are read.
    bool defaultIsZero(const Type* t) {
        if (t->isArray()) return t->length == 0 || defaultIsZero(t->elem);
        if (!t->isStruct()) return true;
        if (const FuncDecl* ctor = t->decl->constructor) return !ctor->params.empty();
        for (auto& f : t->decl->fields)
            if (f.defaultValue || !defaultIsZero(f.type)) return false;
        return true;
    }

    // Puts the value genDefault gives into the memory at `addr`.
    void fillDefault(llvm::Value* addr, const Type* t) {
        if (!inMemory(t)) {
            b.CreateStore(genDefault(t), addr);
        } else if (defaultIsZero(t) || t->isOptional()) {
            b.CreateMemSet(addr, b.getInt8(0), sizeOf(t), alignOf(t));
        } else if (t->isArray()) {
            countedLoop(b.getInt64(t->length), [&](llvm::Value* i) { fillElement(arrayElement(addr, i, t), t->elem); });
        } else {
            fillFields(addr, *t->decl, std::vector<llvm::Value*>(t->decl->fields.size(), nullptr));
        }
    }

    // Puts a new element into memory that belongs to a vector, an array or `new`. Unlike a
    // variable, which can be filled field by field, an element runs a constructor without
    // parameters.
    void fillElement(llvm::Value* addr, const Type* t) {
        size_t mark = temps.size();
        if (t->isStruct() && t->decl->constructor) {
            if (t->decl->constructor->params.empty()) storeValue(construct(*t->decl, {}), t, addr);
        } else {
            fillDefault(addr, t);
        }
        flushTemps(mark);
    }

    llvm::Value* allocObject(const StructDecl& sd) {
        uint64_t size = mod->getDataLayout().getTypeAllocSize(types.at(&sd).layout);
        return b.CreateCall(rtAlloc, {b.getInt64(size)}, sd.name);
    }

    llvm::Value* fieldAddress(const Type* owner, llvm::Value* object, int index, const std::string& name) {
        // A class object starts with its base classes, or with its count and vtable.
        size_t before = !owner->isClass() ? 0 : owner->decl->bases.empty() ? 2 : owner->decl->bases.size();
        return b.CreateStructGEP(types.at(owner->decl).layout, object, index + before, name);
    }

    // How far the base class `base` of `sd` is from the start of an `sd`.
    uint64_t baseOffset(const StructDecl& sd, const StructDecl& base) {
        const llvm::StructLayout* layout = mod->getDataLayout().getStructLayout(types.at(&sd).layout);
        for (size_t i = 0; i < sd.bases.size(); ++i)
            if (sd.bases[i] == &base) return layout->getElementOffset(i);
        llvm_unreachable("not a base class");
    }

    uint64_t pathOffset(const BasePath& path) {
        uint64_t offset = 0;
        for (size_t i = 0; i + 1 < path.size(); ++i) offset += baseOffset(*path[i], *path[i + 1]);
        return offset;
    }

    llvm::Value* atOffset(llvm::Value* object, uint64_t offset) {
        return offset ? b.CreateConstInBoundsGEP1_64(b.getInt8Ty(), object, offset) : object;
    }

    // Makes `object`, and any part of it with a vtable of its own, count as an object of class
    // `sd`, which is what virtual methods then go by.
    void storeVtables(llvm::Value* object, const StructDecl& sd) {
        TypeInfo& info = types.at(&sd);
        for (size_t i = 0; i < info.vtables.size(); ++i)
            b.CreateStore(info.vtables[i], atOffset(object, pathOffset(info.vtablePlaces[i]) + kObjectVtableOffset));
    }

    llvm::Value* loadVtable(llvm::Value* object) {
        return b.CreateLoad(b.getPtrTy(), atOffset(object, kObjectVtableOffset), "vtable");
    }

    llvm::Value* vtableSlot(llvm::Value* vtable, uint64_t slot, const char* name) {
        return b.CreateLoad(b.getPtrTy(), b.CreateConstInBoundsGEP1_64(b.getPtrTy(), vtable, slot), name);
    }

    // Builds the base classes in the memory of an object, in the order they are declared,
    // from what a constructor's initializer list gives each one.
    void buildBases(llvm::Value* object, const StructDecl& sd, const FuncDecl* ctor) {
        for (size_t i = 0; i < sd.bases.size(); ++i) {
            const StructDecl& base = *sd.bases[i];
            uint64_t offset = baseOffset(sd, base);
            llvm::Value* part = atOffset(object, offset);
            // Where a count would be, a part other than the first holds how far back to look for
            // it, as a negative number. It is there before anything can use the part.
            if (i > 0) b.CreateStore(b.getInt64(-(int64_t)offset), part);
            const std::vector<ExprPtr>* args = nullptr;
            if (ctor)
                for (auto& init : ctor->baseInits)
                    if (init.name == base.name) args = &init.args;
            buildBase(part, base, args);
        }
    }

    // Builds one base class, from its constructor's arguments, or else its fields, in order.
    void buildBase(llvm::Value* part, const StructDecl& base, const std::vector<ExprPtr>* args) {
        if (const FuncDecl* ctor = base.constructor) {
            std::vector<llvm::Value*> values{part};
            for (size_t i = 0; args && i < args->size(); ++i) values.push_back(genOwned(*(*args)[i], ctor->params[i]->type));
            b.CreateCall(funcs.at(ctor), values);
            return;
        }
        std::vector<llvm::Value*> given(base.fields.size(), nullptr);
        for (size_t i = 0; args && i < args->size(); ++i) given[i] = genOwned(*(*args)[i], base.fields[i].type);
        buildObject(part, base, given);
    }

    // Fills a class object that has no constructor: its base classes, then its own fields.
    void buildObject(llvm::Value* object, const StructDecl& sd, const std::vector<llvm::Value*>& given) {
        buildBases(object, sd, nullptr);
        storeVtables(object, sd);
        fillFields(object, sd, given);
    }

    // Builds a struct value, or a new class object, without a constructor. `given[i]`,
    // when present, is the owned value of field i; the others get their default.
    llvm::Value* buildAggregate(const StructDecl& sd, std::vector<llvm::Value*> given) {
        given.resize(sd.fields.size(), nullptr);
        if (sd.isClass) {
            llvm::Value* object = allocObject(sd);
            buildObject(object, sd, given);
            return object;
        }
        if (inMemory(&sd.type)) {
            llvm::AllocaInst* out = entryAlloca(types.at(&sd).layout, sd.name);
            fillFields(out, sd, given);
            return out;
        }
        auto valueOf = [&](size_t i) {
            const FieldDecl& f = sd.fields[i];
            if (given[i]) return given[i];
            return f.defaultValue ? genOwned(*f.defaultValue, f.type) : genDefault(f.type);
        };
        llvm::Value* agg = llvm::PoisonValue::get(types.at(&sd).layout);
        for (size_t i = 0; i < sd.fields.size(); ++i) agg = b.CreateInsertValue(agg, valueOf(i), {(unsigned)i});
        return agg;
    }

    // Fills the fields of the struct or class object at `object`, as buildAggregate does.
    void fillFields(llvm::Value* object, const StructDecl& sd, const std::vector<llvm::Value*>& given) {
        for (size_t i = 0; i < sd.fields.size(); ++i) {
            const FieldDecl& f = sd.fields[i];
            llvm::Value* place = fieldAddress(&sd.type, object, (int)i, f.name);
            if (given[i]) storeValue(given[i], f.type, place);
            else if (f.defaultValue) storeValue(genOwned(*f.defaultValue, f.type), f.type, place);
            else fillDefault(place, f.type);
        }
    }

    // Calls the constructor of `sd`, returning the new owned value.
    llvm::Value* construct(const StructDecl& sd, const std::vector<const Expr*>& args) {
        const FuncDecl& ctor = *sd.constructor;
        std::vector<llvm::Value*> values{nullptr};  // `this`, set below
        size_t mark = pending.size();
        for (size_t i = 0; i < args.size(); ++i) values.push_back(argument(*args[i], *ctor.params[i]));
        pending.resize(mark);
        if (sd.isClass) {
            values[0] = allocObject(sd);
            b.CreateCall(funcs.at(&ctor), values);
            return values[0];
        }
        llvm::AllocaInst* slot = entryAlloca(types.at(&sd).layout, sd.name);
        values[0] = slot;
        b.CreateCall(funcs.at(&ctor), values);
        return peekValue(&sd.type, slot, sd.name.c_str());
    }

    llvm::Value* genInitList(const InitListExpr& list, const Type* to) {
        const Type* type = list.explicitType ? list.explicitType : to;
        if (list.mapFill) {  // a new map, and then the entries go in, so that the first of two with one key stays
            llvm::Value* entries = genVectorList(list, list.mapFill->params[0]->type);
            llvm::Value* map = entryAlloca(llvmType(type), "map");
            fillDefault(map, type);
            callFunction(*list.mapFill, {map, entries});
            return inMemory(type) ? map : b.CreateLoad(llvmType(type), map, "map");
        }
        if (type->isVector()) return genVectorList(list, type);
        if (type->isArray()) return genArrayList(list, type);
        const StructDecl& sd = *type->decl;
        if (sd.constructor) {  // the values are the constructor's arguments
            std::vector<const Expr*> args;
            for (auto& el : list.elements) args.push_back(el.value.get());
            return construct(sd, args);
        }
        std::vector<llvm::Value*> given(sd.fields.size(), nullptr);
        size_t mark = pending.size();
        for (auto& el : list.elements)  // evaluated in the order written
            given[el.fieldIndex] = holdOwned(*el.value, sd.fields[el.fieldIndex].type);
        pending.resize(mark);
        return buildAggregate(sd, std::move(given));
    }

    // A condition is a bool, a number that counts as true when it is not zero, or an
    // optional that counts as true when it holds a value.
    llvm::Value* genCond(const Expr& e) { return condValue(gen(e), e.type); }

    llvm::Value* condValue(llvm::Value* v, const Type* t) {
        if (t->isInteger()) return b.CreateICmpNE(v, llvm::ConstantInt::get(v->getType(), 0));
        if (t->isFloating()) return b.CreateFCmpUNE(v, llvm::ConstantFP::get(v->getType(), 0.0));  // NaN is true
        if (t->isOptional()) return hasValue(v, t);
        if (t->isPointer()) return b.CreateIsNotNull(v);
        return v;
    }

    // Whether `e` is storage that can be read in place or assigned: a variable, a field
    // of one, `this` in a struct method, any field of a class object, or an element of
    // a vector or an array stored in one of those.
    static bool isPlace(const Expr& e) {
        switch (e.kind) {
            case Expr::Kind::Name: {  // a constant is not stored anywhere: see genRaw
                const VarDecl* decl = static_cast<const NameExpr&>(e).decl;
                return !(decl && decl->isTopLevel);
            }
            case Expr::Kind::This: return !e.type->isClass();
            case Expr::Kind::Member: {
                auto& m = static_cast<const MemberExpr&>(e);
                return m.base->type->isClass() || isPlace(*m.base);
            }
            case Expr::Kind::Index: {
                auto& ix = static_cast<const IndexExpr&>(e);
                return ix.base->type->isPointer() || isPlace(*ix.base);  // `p[i]` is memory, wherever p came from
            }
            case Expr::Kind::Unary: return isDeref(e);
            default: return false;
        }
    }

    // Whether evaluating `e` may run code that changes or frees other values: a call,
    // an assignment, which releases the value it replaces, or `++` and `--`.
    static bool mayRunCode(const Expr& e) {
        switch (e.kind) {
            case Expr::Kind::IntLit: case Expr::Kind::BoolLit: case Expr::Kind::StringLit: case Expr::Kind::NullLit:
            case Expr::Kind::FloatLit: case Expr::Kind::CharLit: case Expr::Kind::Name: case Expr::Kind::This:
            case Expr::Kind::OptValue: case Expr::Kind::Sizeof: case Expr::Kind::Constraint:
                return false;
            case Expr::Kind::Spawn: case Expr::Kind::Await: return true;
            case Expr::Kind::Cast: {
                auto& c = static_cast<const CastExpr&>(e);
                return c.operand && mayRunCode(*c.operand);
            }
            case Expr::Kind::Member: return mayRunCode(*static_cast<const MemberExpr&>(e).base);
            case Expr::Kind::Index: {
                auto& ix = static_cast<const IndexExpr&>(e);
                return ix.mapSlot || mayRunCode(*ix.base) || mayRunCode(*ix.index);  // a map may have to add the key
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
            default: return true;
        }
    }

    // Works out now, in order, the indexes and pointers that lead to the place `e`, leaving
    // its address to be found later. In `v[i].add(f())`, i comes before f(), as in C++, but
    // the element is found after it, since f() may change or move the vector.
    void freezePlace(const Expr& e, std::vector<const Expr*>& made) {
        auto freeze = [&](const Expr& part, llvm::Value* v) {
            frozen[&part] = v;
            made.push_back(&part);
        };
        switch (e.kind) {
            case Expr::Kind::Member: {
                auto& m = static_cast<const MemberExpr&>(e);
                if (m.base->type->isClass()) freeze(*m.base, gen(*m.base));
                else freezePlace(*m.base, made);
                return;
            }
            case Expr::Kind::Unary: {
                const Expr& pointer = *static_cast<const UnaryExpr&>(e).operand;
                freeze(pointer, gen(pointer));
                return;
            }
            case Expr::Kind::Index: {
                auto& ix = static_cast<const IndexExpr&>(e);
                if (ix.base->type->isPointer() || !isPlace(*ix.base)) freeze(*ix.base, gen(*ix.base));
                else freezePlace(*ix.base, made);
                freeze(*ix.index, ix.mapSlot ? heldKey(ix) : genIndex(*ix.index));
                return;
            }
            default: return;  // a variable or `this`, which stay where they are
        }
    }

    // The key of `m[key]`, kept until the statement ends.
    llvm::Value* heldKey(const IndexExpr& ix) {
        const Type* keyType = ix.mapSlot->params[0]->type;
        llvm::Value* key = genOwned(*ix.index, keyType);
        if (isManaged(keyType)) temps.push_back({key, keyType, inMemory(keyType)});
        return key;
    }

    // The address of the value of `m[key]`. What leads to the map comes first and then the key, as in C++, but the
    // map is found after the key, whose code may move it. A method of the map finds the entry, and from then
    // on it is an element of a vector, the map's entries, in all that that takes.
    llvm::Value* mapValueAddress(const IndexExpr& ix, bool hold, bool write) {
        const Type* mapType = ix.base->type;
        const FuncDecl& find = *ix.mapSlot;
        const Type* keyType = find.params[0]->type;
        bool adds = !find.isConst;
        llvm::Value* held = nullptr;  // a key to report if it is missing, which the method's own copy cannot be
        llvm::Value* key = nullptr;
        auto genKey = [&] {
            held = frozenValue(*ix.index);
            if (!held && adds) {
                key = genOwned(*ix.index, keyType);
                return;
            }
            if (!held) held = heldKey(ix);
            retainValue(held, keyType);
            key = held;
        };
        llvm::Value* self;
        if (isPlace(*ix.base)) {
            self = placeAfter(*ix.base, genKey, hold, write || adds);
        } else {  // a temporary, kept until the statement ends
            llvm::Value* value = baseOnce(*ix.base);
            genKey();
            self = value;
            if (!inMemory(mapType)) {
                self = entryAlloca(llvmType(mapType), "map");
                b.CreateStore(value, self);
            }
        }
        std::vector<llvm::Value*> args{self, key};
        if (find.params.size() == 2) args.push_back(zeroValue(find.params[1]->type));  // see Builtins.h
        llvm::Value* position = callFunction(find, std::move(args));
        if (find.isConst) {
            auto* missingBB = llvm::BasicBlock::Create(ctx, "missing", fn);
            auto* foundBB = llvm::BasicBlock::Create(ctx, "found", fn);
            b.CreateCondBr(b.CreateICmpSLT(position, b.getInt64(0)), missingBB, foundBB,
                           llvm::MDBuilder(ctx).createBranchWeights(1, 1u << 20));
            b.SetInsertPoint(missingBB);
            llvm::Value* text = entryAlloca(stringTy, "text");
            b.CreateStore(llvm::Constant::getNullValue(stringTy), text);
            printText(text, "the key ");
            printValue(text, held, keyType, true);
            printText(text, " is not in this hashmap; check with `contains`, or use `get`, which gives null");
            llvm::Value* message = b.CreateLoad(stringTy, text, "message");
            auto* stop = b.CreateCall(rtTrapText, {strData(message), strLength(message), fileName, b.getInt32(ix.loc.line), b.getInt32(ix.loc.col)});
            stop->setDoesNotReturn();
            b.CreateUnreachable();
            b.SetInsertPoint(foundBB);
        }
        int entriesField = hashmapEntriesField(*mapType->decl);
        const Type* entries = mapType->decl->fields[entriesField].type;
        llvm::Value* slot = fieldAddress(mapType, self, entriesField, "entries");
        llvm::Value* buf = b.CreateLoad(b.getPtrTy(), slot, "entries");
        if (write) buf = reserve(slot, buf, entries, b.getInt64(0));
        if (hold) {
            b.CreateCall(rtRetain, {buf});
            temps.push_back({buf, entries, false});
            if (write && heldBuffers) heldBuffers->push_back({slot, buf});
        }
        return fieldAddress(entries->elem, elementAt(buf, position, entries), 1, "value");
    }

    // The address of the place `recv`, about to change, found after `args` run: what leads
    // to it comes first, as in C++, but `args` may move it.
    llvm::Value* placeAfter(const Expr& recv, const std::function<void()>& args, bool hold = false, bool write = true) {
        std::vector<const Expr*> made;
        freezePlace(recv, made);
        args();
        llvm::Value* place = addressOf(recv, hold, write);
        for (const Expr* part : made) frozen.erase(part);
        return place;
    }

    llvm::Value* frozenValue(const Expr& e) {
        auto it = frozen.find(&e);
        return it == frozen.end() ? nullptr : it->second;
    }
    llvm::Value* baseOnce(const Expr& e) {
        llvm::Value* v = frozenValue(e);
        return v ? v : gen(e);
    }
    llvm::Value* indexOnce(const Expr& e) {
        llvm::Value* v = frozenValue(e);
        return v ? v : genIndex(e);
    }

    // The address of a place. The object a field lies in is normally only kept alive
    // by its owner, which is fine while the address is used right away. With `hold`,
    // the object is retained until the statement ends, for an address that is used
    // while other code runs (the receiver of a struct method). With `write`, the place
    // is about to change, so each vector it lies in first gets a buffer of its own.
    llvm::Value* addressOf(const Expr& e, bool hold = false, bool write = false) {
        AtLine line(*this, e.loc);
        switch (e.kind) {
            case Expr::Kind::Name: {
                auto& n = static_cast<const NameExpr&>(e);
                if (n.implicitField >= 0)
                    return fieldAddress(&n.path.back()->type, atOffset(thisPtr, pathOffset(n.path)), n.implicitField, n.name);
                return locals.at(n.decl);
            }
            case Expr::Kind::This: return thisPtr;
            case Expr::Kind::Member: {
                auto& m = static_cast<const MemberExpr&>(e);
                const Type* baseType = m.base->type;
                llvm::Value* object;
                if (!baseType->isClass()) object = addressOf(*m.base, hold, write);
                else if (llvm::Value* f = frozenValue(*m.base)) object = f;
                else if (!hold && m.base->kind == Expr::Kind::Borrow && isPlace(*static_cast<const BorrowExpr&>(*m.base).operand)) {
                    // Reading a field of what a variable refers to needs no count, as for a class reference. Only what runs
                    // code in between could lose the object, and that asks to hold it.
                    bool owned = false;
                    object = genRaw(*m.base, owned);
                } else if (hold || !isPlace(*m.base)) object = gen(*m.base);
                else object = b.CreateLoad(b.getPtrTy(), addressOf(*m.base), "obj");
                return fieldAddress(&m.fieldOwner->type, atOffset(object, pathOffset(m.path)), m.fieldIndex, m.field);
            }
            case Expr::Kind::Unary: {  // `*p`
                auto& u = static_cast<const UnaryExpr&>(e);
                llvm::Value* p = baseOnce(*u.operand);
                checkNotNull(p, u.isArrow() ? "->" : "*", u.loc);
                checkAligned(p, e.type, u.isArrow() ? "->" : "*", u.loc);
                return p;
            }
            case Expr::Kind::Index: {
                auto& ix = static_cast<const IndexExpr&>(e);
                const Type* vt = ix.base->type;
                if (ix.mapSlot) return mapValueAddress(ix, hold, write);
                if (vt->is(K::String)) return charAddress(ix, hold, write);
                if (vt->isPointer()) {  // `p[i]`: not bounds-checked, as in C++
                    llvm::Value* p = baseOnce(*ix.base);
                    llvm::Value* index = indexOnce(*ix.index);
                    checkNotNull(p, "[ ]", ix.loc);
                    checkAligned(p, vt->elem, "[ ]", ix.loc);
                    return b.CreateGEP(llvmType(vt->elem), p, index, "elem.addr");
                }
                if (vt->isArray()) {
                    // As for a vector below, the array's place stays alive while the index is computed.
                    llvm::Value* array = isPlace(*ix.base) ? addressOf(*ix.base, hold || mayRunCode(*ix.index), write) : baseOnce(*ix.base);
                    llvm::Value* index = indexOnce(*ix.index);
                    checkIndex(index, b.getInt64(vt->length), ix.loc, rtTrapArrayIndex);
                    return arrayElement(array, index, vt);
                }
                llvm::Value* slot = nullptr;
                llvm::Value* buf;
                llvm::Value* index;
                if (isPlace(*ix.base)) {
                    // The index is computed before the vector is read, so code it runs
                    // cannot leave `buf` stale. Meanwhile the vector's place is kept
                    // alive if that code could free it.
                    slot = addressOf(*ix.base, hold || mayRunCode(*ix.index), write);
                    index = indexOnce(*ix.index);
                    buf = b.CreateLoad(b.getPtrTy(), slot, "vec");
                } else {
                    buf = baseOnce(*ix.base);  // a temporary, kept until the statement ends
                    index = indexOnce(*ix.index);
                }
                checkIndex(index, vecSize(buf), ix.loc, rtTrapIndex);
                if (write) {
                    if (!slot) llvm_unreachable("Sema allowed changing an element of a temporary vector");
                    buf = reserve(slot, buf, vt, b.getInt64(0));
                }
                if (hold) {
                    b.CreateCall(rtRetain, {buf});
                    temps.push_back({buf, vt, false});
                    if (write && heldBuffers) heldBuffers->push_back({slot, buf});
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
        checkIndex(index, strLength(s), ix.loc, rtTrapStringIndex);
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
            case Expr::Kind::Try: return genRaw(*static_cast<const TryExpr&>(e).operand, owned);
            case Expr::Kind::Constraint: return b.getInt1(static_cast<const ConstraintExpr&>(e).value);
            case Expr::Kind::Borrow: {  // the object a unique reference owns
                auto& borrow = static_cast<const BorrowExpr&>(e);
                llvm::Value* object = isPlace(*borrow.operand) ? loadPlace(*borrow.operand, "object") : genRaw(*borrow.operand, owned);
                if (borrow.checkEmpty)
                    trapIf(b.CreateIsNull(object), "this '" + std::string(borrow.operand->type->name) + "' has been moved away", e.loc);
                return object;
            }
            case Expr::Kind::CharLit: return b.getInt8(static_cast<const CharLitExpr&>(e).value);
            case Expr::Kind::BoolLit: return b.getInt1(static_cast<const BoolLitExpr&>(e).value);
            case Expr::Kind::StringLit: return stringValue(static_cast<const StringLitExpr&>(e).value);
            case Expr::Kind::NullLit: return llvm::ConstantPointerNull::get(b.getPtrTy());
            case Expr::Kind::OptValue: return optValues.at(static_cast<const OptValueExpr*>(&e));
            case Expr::Kind::OptChain: return genOptChain(static_cast<const OptChainExpr&>(e), owned);
            case Expr::Kind::Coalesce: return genCoalesce(static_cast<const CoalesceExpr&>(e), owned);
            case Expr::Kind::Name: {
                // A constant is a literal by now, or a function of its own for what must happen
                // when it is used, such as a pointer's value or stopping at `1 / 0`.
                const VarDecl* decl = static_cast<const NameExpr&>(e).decl;
                if (decl && decl->isTopLevel) {
                    owned = true;
                    if (auto it = constantFunctions.find(decl); it != constantFunctions.end())
                        return b.CreateCall(it->second, {}, decl->name);
                    return genOwned(*decl->init, decl->type);
                }
                return loadValue(e.type, addressOf(e), static_cast<const NameExpr&>(e).name.c_str());
            }
            case Expr::Kind::This:
                if (e.type->isClass()) return thisPtr;
                return loadValue(e.type, thisPtr, "this");
            case Expr::Kind::Member: {
                auto& m = static_cast<const MemberExpr&>(e);
                if (isPlace(e)) return loadPlace(e, m.field.c_str());
                // A field of a struct value that is not stored anywhere, e.g. makePoint().x
                return loadField(*m.base->type->decl, gen(*m.base), m.fieldIndex);
            }
            case Expr::Kind::Index: return loadPlace(e, "elem");
            case Expr::Kind::InitList:
                owned = true;
                return genInitList(static_cast<const InitListExpr&>(e), e.type);
            case Expr::Kind::New: return genNew(static_cast<const NewExpr&>(e));
            case Expr::Kind::Delete: genDelete(static_cast<const DeleteExpr&>(e)); return nullptr;
            case Expr::Kind::Sizeof: {
                auto& z = static_cast<const SizeofExpr&>(e);
                const Type* t = z.target;
                if (z.what == SizeofExpr::What::Zero) {
                    owned = true;
                    return zeroValue(t);
                }
                if (z.what == SizeofExpr::What::Descriptor) return typeDescriptor(t);
                return b.getInt64(mod->getDataLayout().getTypeAllocSize(llvmType(t)));
            }
            case Expr::Kind::Unary: {
                auto& u = static_cast<const UnaryExpr&>(e);
                if (u.op == UnOp::Deref) return loadPlace(e, "deref");
                if (u.op == UnOp::AddrOf) return addressOf(*u.operand);
                if (u.op == UnOp::Not) return b.CreateNot(genCond(*u.operand));
                llvm::Value* v = genConverted(*u.operand, e.type);  // a char becomes an int first
                switch (u.op) {
                    case UnOp::Plus: return v;
                    case UnOp::Deref:
                    case UnOp::AddrOf:
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
                if (bin.operandType->isPointer()) return genPointerBinary(bin);
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
                    llvm::Value* l = genComparedSide(*bin.lhs, bin.operandType, mayRunCode(*bin.rhs));
                    llvm::Value* same = genEquals(l, genComparedSide(*bin.rhs, bin.operandType, false), bin.operandType);
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
                if (a.op && ty->isPointer()) {  // `p += n` or `p -= n`
                    llvm::Value* n = genConverted(*a.value, Type::longTy());
                    if (*a.op == BinOp::Sub) n = b.CreateNeg(n);
                    llvm::Value* slot = addressOf(*a.target, false, true);
                    llvm::Value* v = b.CreateGEP(llvmType(ty->elem), b.CreateLoad(b.getPtrTy(), slot), n, "ptr");
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
                if (inMemory(ty)) {
                    llvm::Value* place = genMemoryAssign(a);
                    return loadValue(ty, place);
                }
                size_t mark = pending.size();
                llvm::Value* v = holdOwned(*a.value, ty);
                llvm::Value* slot = addressOf(*a.target, false, true);
                pending.resize(mark);
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
                llvm::PHINode* phi = b.CreatePHI(valueType(e.type), 2);
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
                if (e.type->isPointer()) {  // moves to the next or previous element
                    llvm::Value* updated =
                        b.CreateGEP(llvmType(e.type->elem), old, b.getInt64(i.increment ? 1 : -1), "ptr");
                    b.CreateStore(updated, slot);
                    return i.prefix ? updated : old;
                }
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
            case Expr::Kind::Spawn: owned = true; return genSpawn(static_cast<const SpawnExpr&>(e));
            case Expr::Kind::Await: owned = true; return genAwait(static_cast<const AwaitExpr&>(e));
            case Expr::Kind::Call: owned = true; return genCall(static_cast<const CallExpr&>(e));
            case Expr::Kind::MethodCall: {
                auto& call = static_cast<const MethodCallExpr&>(e);
                if (call.lowered) return genRaw(*call.lowered, owned);
                owned = true;
                return genMethodCall(call);
            }
        }
        llvm_unreachable("unknown expression kind");
    }

    // `a = b` for a value handled through its address, returning the place assigned to.
    // A value read from a place is copied straight there, when nothing runs in between.
    llvm::Value* genMemoryAssign(const AssignExpr& a) {
        const Type* ty = a.target->type;
        llvm::Value* v;
        if (a.value->type == ty && isPlace(*a.value) && !mayRunCode(*a.target)) {
            v = addressOf(*a.value);
            retainValue(v, ty);
        } else {
            v = genOwned(*a.value, ty);
        }
        size_t mark = pending.size();
        if (isManaged(ty)) pending.push_back({v, ty, false});
        llvm::Value* slot = addressOf(*a.target, false, true);
        pending.resize(mark);
        llvm::Value* old = isManaged(ty) ? loadValue(ty, slot, "old") : nullptr;
        storeValue(v, ty, slot);
        if (old) releaseValue(old, ty);  // after the store, in case a destructor looks at the place
        return slot;
    }

    // One side of `==`. A value handled through its address is compared in place, unless
    // the code of the other side, which runs later, could change it.
    llvm::Value* genComparedSide(const Expr& e, const Type* type, bool codeRunsLater) {
        if (e.type == type && inMemory(type) && isPlace(e) && !codeRunsLater) return addressOf(e);
        return genConverted(e, type);
    }

    // `new T...`: memory from the runtime, which records before it how many elements
    // there are, filled with the value given or the default of T.
    llvm::Value* genNew(const NewExpr& n) {
        const Type* t = n.target;
        llvm::Type* lt = llvmType(t);
        uint64_t size = sizeOf(t);
        llvm::Value* count = n.count ? genConverted(*n.count, Type::longTy()) : b.getInt64(1);
        llvm::Value* init = n.init ? genOwned(*n.init, t) : nullptr;  // first, since it can fail, leaving no memory behind
        llvm::Value* p = b.CreateCall(rtNew, {b.getInt64(size), count, b.getInt32(n.count ? 1 : 0), fileName,
                                              b.getInt32(n.loc.line), b.getInt32(n.loc.col)},
                                      "new");
        if (init) {
            storeValue(init, t, p);
        } else if (!defaultIsZero(t)) {  // the memory starts as zero bytes, the default of most types
            countedLoop(count, [&](llvm::Value* i) { fillElement(b.CreateInBoundsGEP(lt, p, i), t); });
        }
        return p;
    }

    // `delete p` and `delete[] p`: the values are released, the last one first, and the
    // memory is freed. Deleting null does nothing, as in C++.
    void genDelete(const DeleteExpr& d) {
        llvm::Value* p = gen(*d.operand);
        const Type* t = d.operand->type->elem;
        auto* freeBB = llvm::BasicBlock::Create(ctx, "delete", fn);
        auto* doneBB = llvm::BasicBlock::Create(ctx, "delete.done", fn);
        b.CreateCondBr(b.CreateIsNull(p), doneBB, freeBB);
        b.SetInsertPoint(freeBB);
        llvm::Value* count = b.CreateCall(rtDeleteCheck, {p, b.getInt32(d.array ? 1 : 0), fileName,
                                                          b.getInt32(d.loc.line), b.getInt32(d.loc.col)},
                                          "count");
        if (isManaged(t)) {
            llvm::Type* lt = llvmType(t);
            countedLoop(count, [&](llvm::Value* i) {
                llvm::Value* index = b.CreateSub(b.CreateSub(count, b.getInt64(1)), i);
                releaseValue(peekValue(t, b.CreateInBoundsGEP(lt, p, index)), t);
            });
        }
        b.CreateCall(rtDeleteFree, {p});
        b.CreateBr(doneBB);
        b.SetInsertPoint(doneBB);
    }

    // `p == q`, `p < q`, `p + n`, `n + p`, `p - n` and `p - q`. Pointers compare as
    // addresses, and arithmetic moves by whole elements, as in C++.
    llvm::Value* genPointerBinary(const BinaryExpr& bin) {
        const Type* pt = bin.operandType;
        if (bin.op >= BinOp::Eq && bin.op <= BinOp::Ge) {
            llvm::Value* l = genConverted(*bin.lhs, pt);
            llvm::Value* r = genConverted(*bin.rhs, pt);
            switch (bin.op) {
                case BinOp::Eq: return b.CreateICmpEQ(l, r);
                case BinOp::Ne: return b.CreateICmpNE(l, r);
                case BinOp::Lt: return b.CreateICmpULT(l, r);
                case BinOp::Le: return b.CreateICmpULE(l, r);
                case BinOp::Gt: return b.CreateICmpUGT(l, r);
                default: return b.CreateICmpUGE(l, r);
            }
        }
        llvm::Type* elem = llvmType(pt->elem);
        bool leftPtr = bin.lhs->type->isPointer();
        if (leftPtr && bin.rhs->type->isPointer()) {  // `p - q`: how many elements apart
            llvm::Value* l = gen(*bin.lhs);
            llvm::Value* r = gen(*bin.rhs);
            // Elements that take no room, such as `array<int, 0>`, are all in one place.
            if (mod->getDataLayout().getTypeAllocSize(elem) == 0) return b.getInt64(0);
            return b.CreatePtrDiff(elem, l, r, "distance");
        }
        llvm::Value* l = leftPtr ? gen(*bin.lhs) : genConverted(*bin.lhs, Type::longTy());
        llvm::Value* r = leftPtr ? genConverted(*bin.rhs, Type::longTy()) : gen(*bin.rhs);
        llvm::Value* n = leftPtr ? r : l;
        if (bin.op == BinOp::Sub) n = b.CreateNeg(n);
        return b.CreateGEP(elem, leftPtr ? l : r, n, "ptr");
    }

    llvm::Value* hashWord(llvm::Value* word) {
        llvm::Value* x = b.CreateXor(word, b.CreateLoad(b.getInt64Ty(), hashSeed, "seed"));
        x = b.CreateXor(x, b.CreateLShr(x, 33));
        x = b.CreateMul(x, b.getInt64(0xff51afd7ed558ccdULL));
        x = b.CreateXor(x, b.CreateLShr(x, 33));
        x = b.CreateMul(x, b.getInt64(0xc4ceb9fe1a85ec53ULL));
        return b.CreateXor(x, b.CreateLShr(x, 33));
    }

    llvm::Value* hashCombine(llvm::Value* h, llvm::Value* x) {
        llvm::Value* turned = b.CreateOr(b.CreateShl(h, 5), b.CreateLShr(h, 59));
        return b.CreateMul(b.CreateXor(turned, x), b.getInt64(0x9e3779b97f4a7c15ULL));
    }

    // A number that is the same for two values that are `==`: see genEquals, which this follows. A struct, a
    // vector and an array get a function each, as a struct can lead back to itself through a vector.
    llvm::Value* hashValue(llvm::Value* v, const Type* t) {
        switch (t->kind) {
            case K::Bool: case K::Char: return hashWord(b.CreateZExt(v, b.getInt64Ty()));
            case K::Int: return hashWord(b.CreateSExt(v, b.getInt64Ty()));
            case K::Long: return hashWord(v);
            case K::Class: case K::Pointer: return hashWord(b.CreatePtrToInt(v, b.getInt64Ty()));
            case K::Float: v = b.CreateFPExt(v, b.getDoubleTy()); [[fallthrough]];
            case K::Double:  // -0.0 == 0.0, and adding 0.0 makes them one value
                return hashWord(b.CreateBitCast(b.CreateFAdd(v, llvm::ConstantFP::get(b.getDoubleTy(), 0.0)), b.getInt64Ty()));
            case K::String: return b.CreateCall(rtHashBytes, {strData(v), strLength(v)});
            case K::Struct: case K::Vector: case K::Array: return b.CreateCall(hashFunction(t), {v});
            case K::Optional: {
                if (t->elem->isObjectRef()) return hashWord(b.CreatePtrToInt(v, b.getInt64Ty()));
                // An empty optional holds zero bytes, which are safe to look at, and have the one hash.
                return b.CreateSelect(hasValue(v, t), hashValue(valueOf(v, t), t->elem), b.getInt64(0));
            }
            default: llvm_unreachable("Sema allowed a key that Codegen cannot hash");
        }
    }

    llvm::Function* hashFunction(const Type* t) {
        auto [it, inserted] = hashFunctions.try_emplace(t, nullptr);
        if (!inserted) return it->second;
        auto* f = internalFunction(std::string("cplus.") + t->name + "#hash", b.getInt64Ty(), {inMemory(t) ? b.getPtrTy() : llvmType(t)});
        it->second = f;
        SavedPoint saved(*this);
        startHelper(f);
        llvm::Value* v = f->getArg(0);
        llvm::Value* total = entryAlloca(b.getInt64Ty(), "total");
        b.CreateStore(b.getInt64(0x243f6a8885a308d3ULL), total);
        auto add = [&](llvm::Value* part) { b.CreateStore(hashCombine(b.CreateLoad(b.getInt64Ty(), total), part), total); };
        if (t->isStruct()) {
            for (size_t i = 0; i < t->decl->fields.size(); ++i) add(hashValue(loadField(*t->decl, v, i), t->decl->fields[i].type));
        } else if (t->isVector()) {
            countedLoop(vecSize(v), [&](llvm::Value* i) { add(hashValue(peekValue(t->elem, elementAt(v, i, t)), t->elem)); });
        } else {
            countedLoop(b.getInt64(t->length), [&](llvm::Value* i) { add(hashValue(peekValue(t->elem, arrayElement(v, i, t)), t->elem)); });
        }
        b.CreateRet(b.CreateLoad(b.getInt64Ty(), total));
        return f;
    }

    // Whether two values are equal: strings by contents, structs field by field, and
    // class references by identity.
    // A number for a word, which depends on the seed each run of the program starts with: see cplus_start.
    llvm::Value* genEquals(llvm::Value* l, llvm::Value* r, const Type* t) {
        switch (t->kind) {
            case K::Bool: case K::Int: case K::Long: case K::Char: case K::Class: case K::Pointer: case K::Unique: case K::Local:
                return b.CreateICmpEQ(l, r);
            case K::Float: case K::Double: return b.CreateFCmpOEQ(l, r);
            case K::String: {
                llvm::Value* eq = b.CreateCall(rtStrEq, {b.CreateExtractValue(l, 0), b.CreateExtractValue(l, 1),
                                                         b.CreateExtractValue(r, 0), b.CreateExtractValue(r, 1)});
                return b.CreateICmpNE(eq, b.getInt32(0));
            }
            // The same object while it is there; once gone, it is the same as an empty one.
            case K::Weak: return b.CreateICmpEQ(b.CreateCall(rtWeakAlive, {l}), b.CreateCall(rtWeakAlive, {r}));
            case K::Struct: return b.CreateCall(types.at(t->decl).equals, {l, r});
            case K::Vector: return b.CreateCall(vecInfo(t).equals, {l, r});
            case K::Array: return b.CreateCall(arrayInfo(t).equals, {l, r});
            case K::Null: return b.getInt1(true);
            case K::Optional: {
                // Both null, or both holding equal values. An empty optional holds zero
                // bytes, which compare safely.
                if (t->elem->isObjectRef()) return b.CreateICmpEQ(l, r);
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
            case K::Struct: b.CreateCall(types.at(t->decl).print, {v, out}); break;
            case K::Class: case K::Unique: case K::Local:  // whatever the object really is, with all of its fields
            {
                llvm::BasicBlock* endBB = nullptr;
                if (t->isUnique()) {  // what was moved out of a field or an element shows as that
                    auto* thereBB = llvm::BasicBlock::Create(ctx, "print.object", fn);
                    auto* movedBB = llvm::BasicBlock::Create(ctx, "print.moved", fn);
                    endBB = llvm::BasicBlock::Create(ctx, "print.end", fn);
                    b.CreateCondBr(b.CreateIsNotNull(v), thereBB, movedBB);
                    b.SetInsertPoint(movedBB);
                    printText(out, "moved");
                    b.CreateBr(endBB);
                    b.SetInsertPoint(thereBB);
                }
                llvm::Value* vtable = loadVtable(v);
                llvm::Value* print = vtableSlot(vtable, kVtablePrint, "print");
                if (hasInnerParts) {  // print takes the object from its start
                    llvm::Value* start = b.CreatePtrToInt(vtableSlot(vtable, kVtableStart, "start"), b.getInt64Ty());
                    v = b.CreateInBoundsGEP(b.getInt8Ty(), v, b.CreateNeg(start));
                }
                b.CreateCall(types.at(t->decl).print->getFunctionType(), print, {v, out});
                if (endBB) {
                    b.CreateBr(endBB);
                    b.SetInsertPoint(endBB);
                }
                break;
            }
            case K::Vector: b.CreateCall(vecInfo(t).print, {v, out}); break;
            case K::Array: b.CreateCall(arrayInfo(t).print, {v, out}); break;
            case K::Null: printText(out, "null"); break;
            case K::Weak: {  // not what it refers to, which is often what is printing it
                auto* thereBB = llvm::BasicBlock::Create(ctx, "print.there", fn);
                auto* goneBB = llvm::BasicBlock::Create(ctx, "print.gone", fn);
                auto* endBB = llvm::BasicBlock::Create(ctx, "print.end", fn);
                b.CreateCondBr(b.CreateIsNotNull(b.CreateCall(rtWeakAlive, {v})), thereBB, goneBB);
                b.SetInsertPoint(thereBB);
                printText(out, t->name);
                b.CreateBr(endBB);
                b.SetInsertPoint(goneBB);
                printText(out, "null");
                b.CreateBr(endBB);
                b.SetInsertPoint(endBB);
                break;
            }
            case K::Pointer: b.CreateCall(rtPrintPtr, {out, v}); break;  // the address, as std::format prints a void*
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
        // A value handled through its address is printed in place, unless a later value's
        // code could change it.
        std::vector<bool> codeRunsLater(call.args.size());
        bool later = false;
        for (size_t i = call.args.size(); i-- > first;) {
            codeRunsLater[i] = later;
            later = later || mayRunCode(*call.args[i]);
        }
        std::vector<llvm::Value*> values;
        for (size_t i = first; i < call.args.size(); ++i) {
            const Expr& arg = *call.args[i];
            values.push_back(inMemory(arg.type) && isPlace(arg) && !codeRunsLater[i] ? addressOf(arg) : gen(arg));
        }
        bool toString = call.builtin == Builtin::Format || call.builtin == Builtin::ToString;
        llvm::Value* out = llvm::ConstantPointerNull::get(b.getPtrTy());
        if (toString) {
            out = entryAlloca(stringTy, "text");
            b.CreateStore(llvm::Constant::getNullValue(stringTy), out);
        }
        // All the values are there by now, so what they run cannot be holding up the other threads' printing.
        if (usesThreads && !toString) b.CreateCall(rtPrintLock, {});
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
        if (usesThreads && !toString) b.CreateCall(rtPrintUnlock, {});
        return toString ? b.CreateLoad(stringTy, out, "text") : nullptr;  // a new string, owned
    }

    // `int(x)` and the other conversions, as in C++, except that a value the new type
    // cannot hold stops the program instead of wrapping around or being undefined.
    llvm::Value* genCast(const CastExpr& c) {
        if (!c.operand) return genDefault(c.target);  // `T()`
        const Type* from = c.operand->type;
        const Type* to = c.target;
        llvm::Value* v = gen(*c.operand);
        if (c.dynamic) {
            const Type* toClass = to->isOptional() ? to->elem : to;
            llvm::Value* part = b.CreateCall(rtDynamicCast, {v, types.at(toClass->decl).vtables[0]}, "part");
            if (to->isOptional()) return part;
            trapIf(b.CreateIsNull(part),
                   "the object is not a '" + std::string(toClass->name) + "'; dynamic_cast<" + toClass->name +
                       "?> gives null instead of stopping the program",
                   c.loc);
            return part;
        }
        if (from == to) return v;
        if (to->isClass() || to->isOptional() || to->isWeak()) return convert(v, from, to);  // to a base class, or a weak reference
        Loc loc = c.loc;
        if (to->isPointer()) {
            if (from->isInteger()) return b.CreateIntToPtr(b.CreateSExt(v, b.getInt64Ty()), b.getPtrTy());  // reinterpret_cast
            return v;  // from another pointer, or null: the same address
        }
        if (from->isPointer()) {
            if (to->is(K::Long)) return b.CreatePtrToInt(v, b.getInt64Ty());  // reinterpret_cast
            if (to->is(K::Bool)) return b.CreateIsNotNull(v);
            // `string(p)`: a copy of the text p points to, up to its zero byte. It is a new
            // value, released when the statement ends unless it is stored.
            trapIf(b.CreateIsNull(v), "null pointer: string(p) was given a pointer that is null", loc);
            llvm::AllocaInst* slot = entryAlloca(stringTy, "text");
            b.CreateCall(rtStrFromC, {slot, v});
            llvm::Value* text = b.CreateLoad(stringTy, slot, "text");
            temps.push_back({text, Type::stringTy(), false});
            return text;
        }
        if (from->isArray()) {  // `string(a)`: a copy of the text in an array of chars
            llvm::AllocaInst* slot = entryAlloca(stringTy, "text");
            b.CreateCall(rtStrFromChars, {slot, v, b.getInt64(from->length)});
            llvm::Value* text = b.CreateLoad(stringTy, slot, "text");
            temps.push_back({text, Type::stringTy(), false});
            return text;
        }
        llvm::Type* target = llvmType(to);
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
            case CharFn::IsPrint: return in(' ', '~');
            case CharFn::IsGraph: return in('!', '~');
            case CharFn::IsCntrl: return b.CreateOr(in('\0', '\x1f'), b.CreateICmpEQ(c, b.getInt8(127)));
            case CharFn::IsBlank: return b.CreateOr(b.CreateICmpEQ(c, b.getInt8(' ')), b.CreateICmpEQ(c, b.getInt8('\t')));
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

    // `move(x)`: x's value, owned by whoever takes it. A variable is left empty, so that when it goes
    // away there is nothing to release, and Sema stops anything from reading it. In a field or an element, what
    // can be copied just is, and what cannot is left empty, which reading it through a Borrow finds.
    llvm::Value* genMove(const CallExpr& call) {
        const Expr& x = *call.args[0];
        const Type* t = x.type;
        bool variable = x.kind == Expr::Kind::Name && static_cast<const NameExpr&>(x).implicitField < 0;
        if (!isPlace(x) || (!variable && !call.clears && !holdsUnique(t))) return genOwned(x, t);
        llvm::Value* place = addressOf(x, false, true);
        llvm::Value* value = loadValue(t, place, "moved");
        // A variable is known to hold it, and the rest could have been moved from already.
        if (t->isUnique() && !variable) trapIf(b.CreateIsNull(value), "this '" + std::string(t->name) + "' has been moved away", call.loc);
        if (inMemory(t)) b.CreateMemSet(place, b.getInt8(0), sizeOf(t), alignOf(t));
        else b.CreateStore(llvm::Constant::getNullValue(llvmType(t)), place);
        return value;
    }

    llvm::Value* genCall(const CallExpr& call) {
        if (call.builtin == Builtin::Move) return genMove(call);
        if (call.builtin == Builtin::Hash) {  // never negative: see Builtins.h
            const Expr& x = *call.args[0];
            return b.CreateAnd(hashValue(gen(x), x.type), b.getInt64(INT64_MAX));
        }
        if (call.copies) return genOwned(*call.args[0], call.vectorType ? call.vectorType : &call.constructs->type);  // `Point(p)`
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
            size_t mark = pending.size();
            for (size_t i = 0; i < call.args.size(); ++i) given.push_back(holdOwned(*call.args[i], sd.fields[i].type));
            pending.resize(mark);
            return buildAggregate(sd, std::move(given));
        }
        // A function, or a method called by name inside another method of the same type.
        const FuncDecl* target = call.target ? call.target : call.method;
        if (target->isExtern) return genCCall(call, *target);
        std::vector<llvm::Value*> args;
        if (call.method) args.push_back(atOffset(thisPtr, pathOffset(call.path)));
        size_t mark = pending.size();
        for (size_t i = 0; i < call.args.size(); ++i) args.push_back(argument(*call.args[i], *target->params[i]));
        pending.resize(mark);
        return callFunction(*target, std::move(args), call.method != nullptr);
    }

    // `spawn f(a, b)`: the arguments, and what a method is called on, are worked out here as for any call, then go in memory of
    // their own, for a function made for this call, which runs on the new thread and gets them from there. A function owns its
    // arguments, so that is all it has to do with those. A struct's method runs on a copy of the struct.
    llvm::Value* genSpawn(const SpawnExpr& s) {
        const FuncDecl& target = *s.target;
        const Type* taskType = &s.task->type;
        const auto* method = s.call->kind == Expr::Kind::MethodCall ? static_cast<const MethodCallExpr*>(s.call.get()) : nullptr;
        const auto* plain = s.call->kind == Expr::Kind::Call ? static_cast<const CallExpr*>(s.call.get()) : nullptr;
        size_t mark = pending.size();
        const Type* selfType = nullptr;
        llvm::Value* self = nullptr;
        BasePath path;
        bool dispatch = plain ? plain->method != nullptr : method->qualifier.empty();
        if (method) {
            selfType = method->receiver->type;
            path = method->path;
            self = holdOwned(*method->receiver, selfType);
        } else if (plain->method) {
            selfType = &fnDecl->owner->type;
            path = plain->path;
            if (selfType->isClass()) {
                self = thisPtr;
                retainValue(self, selfType);
            } else {
                self = loadValue(selfType, thisPtr, "this");
                retainValue(self, selfType);
            }
            if (isManaged(selfType)) pending.push_back({self, selfType, false});
        }
        const auto& args = plain ? plain->args : method->args;
        std::vector<llvm::Value*> values;
        for (size_t i = 0; i < args.size(); ++i) values.push_back(holdOwned(*args[i], target.params[i]->type));

        // A place for each of them, after what memory from cplus_alloc starts with, which is a count and an empty vtable, and the task.
        std::vector<llvm::Type*> slots{b.getInt64Ty(), b.getPtrTy(), b.getPtrTy()};
        if (selfType) slots.push_back(llvmType(selfType));
        for (auto& p : target.params) slots.push_back(llvmType(p->type));
        auto* closureTy = llvm::StructType::get(ctx, slots);

        auto* run = internalFunction("cplus.spawn", b.getVoidTy(), {b.getPtrTy()});
        {
            SavedPoint saved(*this);
            startHelper(run);
            startLineTable(s.loc);  // what is inlined here would otherwise lose its lines
            llvm::Value* closure = run->getArg(0);
            unsigned next = 3;
            llvm::Value* task = b.CreateLoad(b.getPtrTy(), b.CreateStructGEP(closureTy, closure, 2), "task");
            std::vector<llvm::Value*> callArgs;
            llvm::Value* selfSlot = nullptr;
            if (selfType) {
                selfSlot = b.CreateStructGEP(closureTy, closure, next++);
                // A class is called through the reference, and a struct in its place.
                callArgs.push_back(selfType->isClass() ? atOffset(b.CreateLoad(b.getPtrTy(), selfSlot, "self"), pathOffset(path)) : selfSlot);
            }
            for (auto& p : target.params) {
                llvm::Value* slot = b.CreateStructGEP(closureTy, closure, next++);
                callArgs.push_back(inMemory(p->type) ? slot : b.CreateLoad(llvmType(p->type), slot, p->name));
            }
            llvm::Value* errorSlot = nullptr;
            if (target.isThrows) {
                errorSlot = entryAlloca(b.getPtrTy(), "error");
                b.CreateStore(llvm::ConstantPointerNull::get(b.getPtrTy()), errorSlot);
            }
            llvm::Value* result = emitCall(target, std::move(callArgs), selfType && dispatch, target.isAsync ? nullptr : errorSlot);
            if (target.isAsync) {  // the thread waits for its task, running its own event loop
                llvm::Value* inner = result;
                b.CreateCall(rtLoopWaitsForStart, {b.getInt32(1)});
                result = emitCall(*target.taskWait, {inner}, false, target.isThrows ? errorSlot : nullptr);
                releaseValue(inner, target.retType);
            }
            if (selfType) releaseValue(selfType->isClass() ? b.CreateLoad(b.getPtrTy(), selfSlot, "self") : peekValue(selfType, selfSlot), selfType);
            b.CreateCall(rtFree, {closure});
            auto finish = [&] {
                if (!result) result = llvm::Constant::getNullValue(llvmType(s.finish->params[0]->type));  // the empty struct of `Task<void>`
                callFunction(*s.finish, {task, result});
            };
            if (target.isThrows) {
                llvm::Value* error = b.CreateLoad(b.getPtrTy(), errorSlot, "error");
                auto* failedBB = llvm::BasicBlock::Create(ctx, "failed", fn);
                auto* doneBB = llvm::BasicBlock::Create(ctx, "done", fn);
                auto* endBB = llvm::BasicBlock::Create(ctx, "end", fn);
                b.CreateCondBr(b.CreateIsNotNull(error), failedBB, doneBB);
                b.SetInsertPoint(failedBB);
                callFunction(*s.fail, {task, error});
                b.CreateBr(endBB);
                b.SetInsertPoint(doneBB);
                finish();
                b.CreateBr(endBB);
                b.SetInsertPoint(endBB);
            } else {
                finish();
            }
            releaseValue(task, taskType);
            b.CreateRetVoid();
            endLineTable();
        }

        llvm::Value* task = construct(*s.task, {});
        retainValue(task, taskType);  // one for the thread, and one for this expression
        llvm::Value* closure = b.CreateCall(rtAlloc, {b.getInt64(mod->getDataLayout().getTypeAllocSize(closureTy))}, "closure");
        b.CreateStore(task, b.CreateStructGEP(closureTy, closure, 2));
        unsigned next = 3;
        auto put = [&](llvm::Value* v, const Type* t) {
            llvm::Value* slot = b.CreateStructGEP(closureTy, closure, next++);
            if (inMemory(t)) b.CreateMemCpy(slot, llvm::MaybeAlign(), v, llvm::MaybeAlign(), mod->getDataLayout().getTypeAllocSize(llvmType(t)));
            else b.CreateStore(v, slot);
        };
        if (selfType) put(self, selfType);
        for (size_t i = 0; i < values.size(); ++i) put(values[i], target.params[i]->type);
        pending.resize(mark);
        b.CreateCall(rtThreadStart, {run, closure});
        return task;
    }

    // Calls a function of the program. One that returns a value handled through its address
    // writes it to memory the caller gives it, as a hidden first argument.
    // With `dispatch`, a virtual method is found through the vtable of `this`, the first of `args`.
    llvm::Value* callFunction(const FuncDecl& f, std::vector<llvm::Value*> args, bool dispatch = false) {
        llvm::Value* result = emitCall(f, std::move(args), dispatch, f.callFails() ? errorPlace() : nullptr);
        if (f.callFails()) leaveOnError();
        return result;
    }

    // The call itself, with `errorPlace` where an error goes, for a function that can fail.
    llvm::Value* emitCall(const FuncDecl& f, std::vector<llvm::Value*> args, bool dispatch, llvm::Value* errorPlace) {
        llvm::FunctionCallee callee = funcs.at(&f);
        if (dispatch && f.vtableSlot >= 0)
            callee = llvm::FunctionCallee(funcs.at(&f)->getFunctionType(),
                                          vtableSlot(loadVtable(args[0]), kVtableMethods + f.vtableSlot, f.name.c_str()));
        if (f.callFails()) args.push_back(errorPlace);
        if (inMemory(f.retType)) {
            llvm::AllocaInst* result = entryAlloca(llvmType(f.retType), "result");
            args.insert(args.begin(), result);
            b.CreateCall(callee, args);
            return result;
        }
        llvm::CallInst* inst = b.CreateCall(callee, args);
        return f.retType->is(K::Void) ? nullptr : inst;
    }

    llvm::Value* genMethodCall(const MethodCallExpr& call) {
        const Expr& recv = *call.receiver;
        if (recv.type->isOptional()) return genOptionalMethod(call);
        if (recv.type->isWeak()) return genWeakMethod(call);
        if (recv.type->isVector()) return genVectorMethod(call);
        if (recv.type->isArray()) return genArrayMethod(call);
        if (recv.type->is(K::String)) return genStringMethod(call);
        if (call.uniqueGet) {  // lends the object, as a reference of its own
            llvm::Value* object = gen(recv);
            trapIf(b.CreateIsNull(object), "this '" + std::string(recv.type->name) + "' has been moved away", call.loc);
            retainValue(object, recv.type->elem);
            return object;
        }
        std::vector<llvm::Value*> args{nullptr};  // `this` comes first, and is set below
        auto genArgs = [&] {
            size_t mark = pending.size();
            for (size_t i = 0; i < call.args.size(); ++i)
                args.push_back(argument(*call.args[i], *call.method->params[i]));
            pending.resize(mark);
        };
        // A struct method works on the struct in place, so its place is found after the arguments.
        // An argument could write to a vector it lies in, or hold a copy of that vector, and in both
        // cases the vector must then get a buffer of its own, with `this` in it.
        bool argsFirst = false;
        for (auto& a : call.args) argsFirst = argsFirst || mayRunCode(*a) || isManaged(a->type);
        bool argsDone = false;
        std::vector<std::pair<llvm::Value*, llvm::Value*>> held;
        llvm::Value* self;
        if (recv.type->isClass()) {
            self = atOffset(gen(recv), pathOffset(call.path));
        } else if (isPlace(recv)) {
            auto* outerHeld = heldBuffers;
            auto* thisHeld = call.method->isConst ? nullptr : &held;
            if (argsFirst) {
                self = placeAfter(recv, [&] {
                    genArgs();
                    heldBuffers = thisHeld;
                }, /*hold=*/true, /*write=*/!call.method->isConst);
                argsDone = true;
            } else {
                heldBuffers = thisHeld;
                self = addressOf(recv, /*hold=*/true, /*write=*/!call.method->isConst);
            }
            heldBuffers = outerHeld;
        } else {
            // A struct value that is not stored anywhere, e.g. makePoint().move(1): the
            // method works on a temporary, which is released when the statement ends.
            llvm::Value* v = genOwned(recv, recv.type);
            llvm::Value* slot = v;  // a value handled through its address already has its place
            if (!inMemory(recv.type)) {
                slot = entryAlloca(llvmType(recv.type), "tmp");
                b.CreateStore(v, slot);
            }
            if (isManaged(recv.type)) temps.push_back({slot, recv.type, true});
            self = slot;
        }
        if (!argsDone) genArgs();
        args[0] = self;
        llvm::Value* result = callFunction(*call.method, std::move(args), call.qualifier.empty());
        // While the method ran, only the vector and this call held the buffer `this` lies in. If that
        // changed, the method's body changed, copied or freed the vector. What it did to `this` would then
        // be lost, or seen by a copy, without a word, so the program stops. The buffer is held, so
        // it can be looked at, while what holds the vector may be gone.
        for (auto& [slot, buf] : held) {
            auto* count = b.CreateAlignedLoad(b.getInt64Ty(), buf, llvm::MaybeAlign(8), "count");
            count->setAtomic(llvm::AtomicOrdering::Monotonic);
            trapIf(b.CreateICmpNE(count, b.getInt64(2)),
                   "a vector was changed, copied or freed while a method of one of its elements was running, which would lose "
                   "what the method did to the element; call the method on a copy of the element, then store it back",
                   call.loc);
        }
        return result;
    }

    // `base?.rest`: when `base` holds a value, the rest of the chain runs on it and its
    // result becomes an optional; otherwise the result is null.
    llvm::Value* genOptChain(const OptChainExpr& c, bool& owned) {
        const Type* optTy = c.base->type;
        const Type* resultTy = c.type;
        llvm::Value* opt = gen(*c.base);  // kept until the statement ends
        auto* someBB = llvm::BasicBlock::Create(ctx, "chain.value", fn);
        auto* endBB = llvm::BasicBlock::Create(ctx, "chain.end", fn);
        llvm::Value* none = resultTy->is(K::Void) ? nullptr : zeroValue(resultTy);
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
        llvm::PHINode* phi = b.CreatePHI(valueType(resultTy), 2);
        phi->addIncoming(v, someEnd);
        phi->addIncoming(none, noneFrom);
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
        llvm::PHINode* phi = b.CreatePHI(valueType(resultTy), 2);
        phi->addIncoming(x, someEnd);
        phi->addIncoming(y, noneEnd);
        owned = managed;
        return phi;
    }

    llvm::Value* genWeakMethod(const MethodCallExpr& call) {
        switch (call.weakMethod) {
            case WeakMethod::Lock: return b.CreateCall(rtWeakLock, {gen(*call.receiver)}, "locked");  // owned, or null
            case WeakMethod::Expired: return b.CreateIsNull(b.CreateCall(rtWeakAlive, {gen(*call.receiver)}), "expired");
            case WeakMethod::Reset: {
                llvm::Value* place = addressOf(*call.receiver, /*hold=*/false, /*write=*/true);
                llvm::Value* old = b.CreateLoad(b.getPtrTy(), place, "old");
                b.CreateStore(llvm::ConstantPointerNull::get(b.getPtrTy()), place);
                b.CreateCall(rtWeakRelease, {old});
                return nullptr;
            }
            case WeakMethod::None: break;
        }
        llvm_unreachable("unknown method of a weak reference");
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
        storeValue(value, var.type, allocaFor(var));
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
    llvm::Value* errorPlace() {
        if (handlers.empty()) return errorOut;
        if (!localError) {
            llvm::IRBuilder<> entry(&fn->getEntryBlock(), fn->getEntryBlock().begin());
            localError = entry.CreateAlloca(b.getPtrTy(), nullptr, "error");
            entry.CreateStore(llvm::ConstantPointerNull::get(b.getPtrTy()), localError);
        }
        return localError;
    }

    void returnAfterError() {
        if (async) {  // the task gets the error
            if (async->failed) b.CreateBr(async->failed);
            else b.CreateUnreachable();  // Sema proved that nothing can fail here
            return;
        }
        if (fn->getReturnType()->isVoidTy()) b.CreateRetVoid();
        else b.CreateRet(llvm::PoisonValue::get(fn->getReturnType()));
    }

    // Goes where an error goes from here, the error being in its place already.
    void leaveWithError() {
        if (handlers.empty()) {
            releaseScopes(0);
            returnAfterError();
        } else {
            releaseScopes(handlers.back().scopeDepth);
            b.CreateBr(handlers.back().dispatch);
        }
    }

    // After a call that can fail: on error, what has been built so far is released, and the
    // error goes on. The rest of the expression is generated for when there was none.
    void leaveOnError() {
        auto* failedBB = llvm::BasicBlock::Create(ctx, "failed", fn);
        auto* okBB = llvm::BasicBlock::Create(ctx, "ok", fn);
        llvm::Value* error = b.CreateLoad(b.getPtrTy(), errorPlace(), "error");
        b.CreateCondBr(b.CreateIsNotNull(error), failedBB, okBB, llvm::MDBuilder(ctx).createBranchWeights(1, 1u << 20));
        b.SetInsertPoint(failedBB);
        auto release = [&](const Temp& t) { releaseValue(t.inMemory ? peekValue(t.type, t.value) : t.value, t.type); };
        for (size_t i = pending.size(); i-- > 0;) release(pending[i]);
        for (size_t i = temps.size(); i-- > 0;) release(temps[i]);
        leaveWithError();
        b.SetInsertPoint(okBB);
    }

    // `throw`, with an owned reference to the error.
    void throwValue(llvm::Value* error) {
        b.CreateStore(error, errorPlace());
        leaveWithError();
    }

    void genTry(const TryStmt& st) {
        auto* dispatchBB = llvm::BasicBlock::Create(ctx, "catch.dispatch", fn);
        auto* endBB = llvm::BasicBlock::Create(ctx, "try.end", fn);
        handlers.push_back({dispatchBB, scopes.size()});
        llvm::Value* place = errorPlace();
        genStmt(*st.body);
        handlers.pop_back();
        b.CreateBr(endBB);

        // Each catch in turn asks whether the error is what it takes.
        b.SetInsertPoint(dispatchBB);
        llvm::Value* error = b.CreateLoad(b.getPtrTy(), place, "error");
        b.CreateStore(llvm::ConstantPointerNull::get(b.getPtrTy()), place);
        bool taken = false;
        for (const CatchClause& c : st.catches) {
            auto* bodyBB = llvm::BasicBlock::Create(ctx, "catch", fn);
            llvm::BasicBlock* afterTest = nullptr;
            llvm::Value* part = error;
            if (c.takesEverything) {
                b.CreateBr(bodyBB);
                taken = true;
            } else {
                auto* nextBB = llvm::BasicBlock::Create(ctx, "catch.next", fn);
                part = b.CreateCall(rtDynamicCast, {error, types.at(c.type->decl).vtables[0]}, "part");
                b.CreateCondBr(b.CreateIsNotNull(part), bodyBB, nextBB);
                afterTest = nextBB;
            }
            b.SetInsertPoint(bodyBB);
            scopes.emplace_back();
            // The catch holds the reference that was thrown, for `throw;`, and its variable has its own.
            b.CreateStore(error, allocaFor(*c.held));
            scopes.back().push_back(c.held.get());
            if (c.var) {
                retainValue(part, c.var->type);
                b.CreateStore(part, allocaFor(*c.var));
                scopes.back().push_back(c.var.get());
            }
            for (auto& s : c.body->stmts) genStmt(*s);
            popScope();
            b.CreateBr(endBB);
            if (afterTest) b.SetInsertPoint(afterTest);
            if (taken) break;
        }
        // No catch takes it, so it goes further out. Sema made sure there is somewhere to go, unless the
        // block cannot fail, and then this never runs.
        if (taken) {
        } else if (!handlers.empty() || errorOut) {
            throwValue(error);
        } else {
            b.CreateUnreachable();
        }
        b.SetInsertPoint(endBB);
    }

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
        setLine(s.loc);
        switch (s.kind) {
            case Stmt::Kind::VarDecl: {
                auto& v = static_cast<const VarDecl&>(s);
                // `const T& r = place;` names the place, and keeps what holds it alive until the scope ends. An element of a
                // vector, a string or a hashmap is copied for now, since holding its buffer would make every change to the
                // container copy it. Any other value is a temporary that lives as long as the reference, as in C++.
                if (passedByRef(v) && isPlace(*v.init) && v.init->type == v.type && !inContainerElement(*v.init)) {
                    size_t mark = temps.size();
                    locals[&v] = addressOf(*v.init, /*hold=*/true, /*write=*/!v.isConst);
                    keepTempsForScope(mark);
                    flushTemps();
                    return;
                }
                llvm::Value* init = v.init ? genOwned(*v.init, v.type)
                                    : v.type->is(K::String) || v.type->hasFields() || v.type->isVector() ||
                                              v.type->isOptional() || v.type->isArray() || v.type->isWeak() || v.type->isUnique() || v.type->isLocal()
                                        ? genDefault(v.type)
                                        : nullptr;
                flushTemps();
                if (inMemory(v.type)) locals[&v] = init;  // the value's own memory becomes the variable
                else if (init) b.CreateStore(init, allocaFor(v));
                else allocaFor(v);
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
                if (st.isConstexpr) {  // only the branch that runs is still there
                    if (st.thenStmt) genScoped(*st.thenStmt);
                    else if (st.elseStmt) genScoped(*st.elseStmt);
                    return;
                }
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
                if (async) {  // the task gets what it returns
                    llvm::Value* v = st.value ? genOwned(*st.value, fnDecl->bodyRet()) : nullptr;
                    flushTemps();
                    releaseScopes(0);
                    finishAsync(v);
                    startDeadBlock();
                    return;
                }
                const Type* rt = fnDecl->retType;
                if (st.value && inMemory(rt)) {  // written to the memory the caller gave, the hidden first argument
                    llvm::Value* v;
                    if (st.value->type == rt && isPlace(*st.value)) {
                        v = addressOf(*st.value);
                        retainValue(v, rt);
                    } else {
                        v = genOwned(*st.value, rt);
                    }
                    storeValue(v, rt, fn->getArg(0));
                    flushTemps();
                    releaseScopes(0);
                    b.CreateRetVoid();
                    startDeadBlock();
                    return;
                }
                llvm::Value* v = st.value ? genOwned(*st.value, rt) : nullptr;
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
            case Stmt::Kind::Try: genTry(static_cast<const TryStmt&>(s)); return;
            case Stmt::Kind::Throw: {
                auto& st = static_cast<const ThrowStmt&>(s);
                llvm::Value* error;
                if (st.value) {
                    error = genOwned(*st.value, errorTy);
                } else {
                    error = b.CreateLoad(b.getPtrTy(), locals.at(st.rethrows), "error");
                    retainValue(error, errorTy);
                }
                flushTemps();
                throwValue(error);
                startDeadBlock();
                return;
            }
            case Stmt::Kind::Error: llvm_unreachable("a program with parse errors reached codegen");
        }
    }

    // The loop goes over the vector as it was when the loop started: it holds its own
    // reference, so a change made in the body copies the vector instead of moving the
    // elements under the loop. For an array, it holds its own copy.
    void genForEach(const ForEachStmt& st) {
        const Type* vt = st.rangeVar->type;
        const VarDecl& var = *st.var;
        bool str = vt->is(K::String);  // a string goes over its bytes
        bool arr = vt->isArray();
        scopes.emplace_back();  // holds the vector, array or string
        const Type* elemType = str ? Type::charTy() : vt->elem;
        bool byRef = passedByRef(var) && var.type == elemType;
        // `for (const auto& x : a)` over an array in a variable goes over the array itself, which stays where it is.
        bool inPlace = arr && byRef && isPlace(*st.range) && stablePlace(*st.range);
        llvm::Value* range = inPlace ? addressOf(*st.range) : genOwned(*st.range, vt);
        flushTemps();
        if (arr) locals[st.rangeVar.get()] = range;
        else b.CreateStore(range, allocaFor(*st.rangeVar));
        if (!inPlace) scopes.back().push_back(st.rangeVar.get());
        llvm::Value* size = arr ? b.getInt64(vt->length) : str ? strLength(range) : vecSize(range);
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
        if (byRef) {  // the element itself, in what the loop holds
            locals[&var] = arr ? arrayElement(range, i, vt)
                           : str ? b.CreateInBoundsGEP(b.getInt8Ty(), strData(range), i, "c.addr")
                                 : elementAt(range, i, vt);
            genScoped(*st.body);
            popScope();
            loops.pop_back();
            b.CreateBr(stepBB);
            b.SetInsertPoint(stepBB);
            b.CreateStore(b.CreateAdd(i, b.getInt64(1)), counter);
            b.CreateBr(condBB);
            b.SetInsertPoint(endBB);
            popScope();
            return;
        }
        llvm::Value* elem = arr ? peekValue(vt->elem, arrayElement(range, i, vt), "elem")
                            : str ? b.CreateLoad(b.getInt8Ty(), b.CreateInBoundsGEP(b.getInt8Ty(), strData(range), i), "c")
                                  : peekValue(vt->elem, elementAt(range, i, vt), "elem");
        llvm::Value* value = convert(elem, str ? Type::charTy() : vt->elem, var.type);
        retainValue(value, var.type);
        storeValue(value, var.type, allocaFor(var));
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
        bool result = inMemory(f.retType);
        if (result) params.push_back(b.getPtrTy());  // where the result goes; see callFunction
        if (f.owner) params.push_back(b.getPtrTy());  // `this`
        for (auto& p : f.params) params.push_back(passedByRef(*p) ? b.getPtrTy() : valueType(p->type));
        if (f.callFails()) params.push_back(b.getPtrTy());  // where an error goes; see errorPlace
        auto* type = llvm::FunctionType::get(result ? b.getVoidTy() : llvmType(f.retType), params, false);
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
        unsigned first = (result ? 1 : 0) + (f.owner ? 1 : 0);
        if (result) func->getArg(0)->setName("result");
        if (f.owner) func->getArg(first - 1)->setName("this");
        for (size_t i = 0; i < f.params.size(); ++i) func->getArg(first + i)->setName(f.params[i]->name);
        if (f.callFails()) func->getArg(func->arg_size() - 1)->setName("error");
        funcs[&f] = func;
        if (f.isAsync && !f.isPure) {  // the coroutine, which takes the task, then what the function takes
            std::vector<llvm::Type*> bodyParams{b.getPtrTy()};
            bodyParams.insert(bodyParams.end(), params.begin(), params.end());
            auto* body = llvm::Function::Create(llvm::FunctionType::get(b.getVoidTy(), bodyParams, false), llvm::Function::InternalLinkage,
                                                name + "#body", mod.get());
            body->setPresplitCoroutine();
            body->getArg(0)->setName("task");
            for (unsigned i = 0; i < func->arg_size(); ++i) body->getArg(i + 1)->setName(func->getArg(i)->getName());
            asyncBodies[&f] = body;
        }
    }

    // Whether C's `char` is signed on the target: it is on x86 and on Apple and Windows
    // ARM, and unsigned on other ARM, PowerPC, RISC-V and s390x systems.
    bool cCharIsSigned() {
        llvm::Triple t(mod->getTargetTriple());
        if (t.isOSDarwin() || t.isOSWindows()) return true;
        return !(t.isARM() || t.isAArch64() || t.isPPC() || t.isRISCV() || t.isSystemZ());
    }

    // ---- Structs and the C calling convention ------------------------------
    //
    // LLVM leaves it to the compiler to say how a struct travels to and from a C function,
    // so this follows each platform's rules as a C compiler does. The fields of a C struct
    // are only numbers, pointers and arrays of those, with no unions or bit fields, which
    // keeps the rules short. See the System V x86-64 psABI, section 3.2.3, and AAPCS64, section 6.8.

    // The scalars inside a struct, with their byte offsets.
    void flattenStruct(llvm::Type* t, uint64_t offset, std::vector<std::pair<llvm::Type*, uint64_t>>& leaves) {
        if (auto* at = llvm::dyn_cast<llvm::ArrayType>(t)) {
            uint64_t size = mod->getDataLayout().getTypeAllocSize(at->getElementType());
            for (uint64_t i = 0; i < at->getNumElements(); ++i) flattenStruct(at->getElementType(), offset + i * size, leaves);
            return;
        }
        auto* st = llvm::dyn_cast<llvm::StructType>(t);
        if (!st) {
            leaves.push_back({t, offset});
            return;
        }
        const llvm::StructLayout* layout = mod->getDataLayout().getStructLayout(st);
        for (unsigned i = 0; i < st->getNumElements(); ++i)
            flattenStruct(st->getElementType(i), offset + layout->getElementOffset(i), leaves);
    }

    // The values a struct is passed in when it goes in registers, each read from the
    // struct's bytes in turn, or none when it goes in memory.
    std::vector<llvm::Type*> cStructParts(llvm::StructType* st) {
        uint64_t size = mod->getDataLayout().getTypeAllocSize(st);
        if (size > 32) return {};  // larger than any struct that goes in registers
        std::vector<std::pair<llvm::Type*, uint64_t>> leaves;
        flattenStruct(st, 0, leaves);
        if (llvm::Triple(mod->getTargetTriple()).isAArch64()) {
            // Up to four floats, or up to four doubles, go in floating-point registers.
            bool sameFloat = leaves.size() <= 4;
            for (auto& leaf : leaves) sameFloat = sameFloat && leaf.first->isFloatingPointTy() && leaf.first == leaves[0].first;
            if (sameFloat) return {llvm::ArrayType::get(leaves[0].first, leaves.size())};
            if (size > 16) return {};
            if (size <= 8) return {b.getInt64Ty()};
            return {llvm::ArrayType::get(b.getInt64Ty(), 2)};
        }
        // x86-64: each eight bytes go in a floating-point register if they hold only
        // floating-point fields, and in an integer register otherwise.
        if (size > 16) return {};
        bool onlyFloats[2] = {true, true};
        for (auto& leaf : leaves)
            if (!leaf.first->isFloatingPointTy()) onlyFloats[leaf.second / 8] = false;
        std::vector<llvm::Type*> parts;
        for (uint64_t i = 0; i * 8 < size; ++i)
            parts.push_back(onlyFloats[i] ? (llvm::Type*)b.getDoubleTy() : b.getInt64Ty());
        return parts;
    }

    // Plans how each value of a C function travels.
    CSignature planCSignature(const FuncDecl& f) {
        CSignature sig;
        bool x86 = llvm::Triple(mod->getTargetTriple()).getArch() == llvm::Triple::x86_64;
        int intRegs = 6, floatRegs = 8;  // x86-64: a struct goes in registers only if all of it fits
        const Type* rt = f.retType;
        if (rt->isStruct()) {
            std::vector<llvm::Type*> parts = cStructParts(llvm::cast<llvm::StructType>(llvmType(rt)));
            sig.memoryResult = parts.empty();
            sig.resultParts = parts;
            if (sig.memoryResult) --intRegs;  // it holds the address of the result
        }
        for (auto& p : f.params) {
            const Type* t = p->type;
            CPass pass;
            if (!t->isStruct()) {
                if (t->isFloating()) floatRegs = std::max(0, floatRegs - 1);
                else intRegs = std::max(0, intRegs - 1);
                sig.params.push_back(pass);
                continue;
            }
            pass.parts = cStructParts(llvm::cast<llvm::StructType>(llvmType(t)));
            if (x86) {
                int ints = 0, floats = 0;
                for (llvm::Type* part : pass.parts) (part->isDoubleTy() ? floats : ints)++;
                bool fits = !pass.parts.empty() && ints <= intRegs && floats <= floatRegs;
                if (fits) intRegs -= ints, floatRegs -= floats;
                else pass.parts.clear(), pass.kind = CPass::OnStack;
            } else if (pass.parts.empty()) {
                pass.kind = CPass::ByPointer;  // to a copy, which the function may change
            }
            if (!pass.parts.empty()) pass.kind = CPass::Parts;
            sig.params.push_back(std::move(pass));
        }
        return sig;
    }

    // A buffer with room for a struct and for all the registers it is passed in, so that
    // whole registers can be read from and written to it.
    llvm::Value* cStructBuffer(llvm::Type* st) {
        uint64_t size = std::max<uint64_t>(mod->getDataLayout().getTypeAllocSize(st), 16);
        llvm::AllocaInst* buffer = entryAlloca(llvm::ArrayType::get(b.getInt8Ty(), size), "c.struct");
        buffer->setAlignment(llvm::Align(8));
        b.CreateMemSet(buffer, b.getInt8(0), size, llvm::Align(8));  // padding reaches C as zeros
        return buffer;
    }

    // A C function, under its own name. Text crosses as a `const char*`, a bool as a C
    // `bool`, which is zero-extended to a byte, and a char as a C `char`, extended as the
    // platform's C compiler does. The char's bits are the same either way.
    void declareCFunction(const FuncDecl& f) {
        llvm::Triple triple(mod->getTargetTriple());
        bool byValue = f.retType->isStruct();
        for (auto& p : f.params) byValue = byValue || p->type->isStruct();
        if (byValue && triple.getArch() != llvm::Triple::x86_64 && !triple.isAArch64() && abiError.empty())
            abiError = "this compiler does not support passing structs by value to C functions such as '" + f.name +
                       "' on " + triple.str() + " yet; pass a pointer to the struct instead";
        CSignature sig = abiError.empty() ? planCSignature(f) : CSignature{};
        sig.params.resize(f.params.size());
        auto cType = [&](const Type* t) -> llvm::Type* {
            return t->is(K::String) || t->isOptional() ? b.getPtrTy() : llvmType(t);
        };
        std::vector<llvm::Type*> params;
        if (sig.memoryResult) params.push_back(b.getPtrTy());
        for (size_t i = 0; i < f.params.size(); ++i) {
            switch (sig.params[i].kind) {
                case CPass::Value: params.push_back(cType(f.params[i]->type)); break;
                case CPass::Parts: params.insert(params.end(), sig.params[i].parts.begin(), sig.params[i].parts.end()); break;
                case CPass::OnStack: case CPass::ByPointer: params.push_back(b.getPtrTy()); break;
            }
        }
        llvm::Type* result = cType(f.retType);
        if (sig.memoryResult) result = b.getVoidTy();
        else if (sig.resultParts.size() == 1) result = sig.resultParts[0];
        else if (sig.resultParts.size() == 2) result = llvm::StructType::get(ctx, sig.resultParts);
        auto* type = llvm::FunctionType::get(result, params, false);
        auto* func = llvm::Function::Create(type, llvm::Function::ExternalLinkage, f.name, mod.get());
        auto extension = [&](const Type* t) {
            if (t->is(K::Bool)) return llvm::Attribute::ZExt;
            if (t->is(K::Char)) return cCharIsSigned() ? llvm::Attribute::SExt : llvm::Attribute::ZExt;
            return llvm::Attribute::None;
        };
        unsigned index = 0;
        if (sig.memoryResult) {
            llvm::Type* st = llvmType(f.retType);
            func->addParamAttr(0, llvm::Attribute::getWithStructRetType(ctx, st));
            func->addParamAttr(0, llvm::Attribute::getWithAlignment(ctx, mod->getDataLayout().getABITypeAlign(st)));
            index = 1;
        }
        for (size_t i = 0; i < f.params.size(); ++i) {
            const CPass& pass = sig.params[i];
            if (pass.kind == CPass::Parts) {
                // AArch64: floats that run out of registers still take 8 bytes each on the stack.
                if (auto* floats = llvm::dyn_cast<llvm::ArrayType>(pass.parts[0]))
                    if (floats->getElementType()->isFloatingPointTy())
                        func->addParamAttr(index, llvm::Attribute::getWithStackAlignment(ctx, llvm::Align(8)));
                index += pass.parts.size();
                continue;
            }
            if (pass.kind == CPass::OnStack) {
                func->addParamAttr(index, llvm::Attribute::getWithByValType(ctx, llvmType(f.params[i]->type)));
                func->addParamAttr(index, llvm::Attribute::getWithAlignment(ctx, llvm::Align(8)));
            } else if (auto ext = extension(f.params[i]->type); ext != llvm::Attribute::None) {
                func->addParamAttr(index, ext);
            }
            ++index;
        }
        if (auto ext = extension(f.retType); ext != llvm::Attribute::None) func->addRetAttr(ext);
        funcs[&f] = func;
        cSignatures[&f] = std::move(sig);
    }

    // A call to a C function. Each string is copied, with a zero byte at the end, right
    // after it is evaluated, and the copies are freed when the call returns. Text the
    // function returns is copied into a string, and a struct is put back together from
    // what the function returned it in.
    llvm::Value* genCCall(const CallExpr& call, const FuncDecl& f) {
        const CSignature& sig = cSignatures.at(&f);
        std::vector<llvm::Value*> args, copies;
        llvm::Value* resultSlot = nullptr;
        if (sig.memoryResult) args.push_back(resultSlot = entryAlloca(llvmType(f.retType), "c.result"));
        for (size_t i = 0; i < call.args.size(); ++i) {
            const Type* t = f.params[i]->type;
            llvm::Value* v = genConverted(*call.args[i], t);
            const CPass& pass = sig.params[i];
            if (pass.kind == CPass::Parts) {
                llvm::Value* buffer = cStructBuffer(llvmType(t));
                storeValue(v, t, buffer);
                uint64_t offset = 0;
                for (llvm::Type* part : pass.parts) {
                    args.push_back(b.CreateLoad(part, byteOffset(buffer, offset, "c.part")));
                    offset += 8;
                }
                continue;
            }
            if (pass.kind == CPass::OnStack || pass.kind == CPass::ByPointer) {
                llvm::AllocaInst* copy = entryAlloca(llvmType(t), "c.arg");
                storeValue(v, t, copy);
                args.push_back(copy);
                continue;
            }
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
        if (sig.memoryResult) return peekValue(rt, resultSlot, "c.result");
        if (rt->isStruct()) {
            llvm::Value* buffer = cStructBuffer(llvmType(rt));
            if (sig.resultParts.size() == 1) {
                b.CreateStore(result, buffer);
            } else {
                b.CreateStore(b.CreateExtractValue(result, {0}), buffer);
                b.CreateStore(b.CreateExtractValue(result, {1}), byteOffset(buffer, 8, "c.part"));
            }
            return peekValue(rt, buffer, "c.result");
        }
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
            llvm::Value* place = fieldAddress(&sd.type, thisPtr, (int)i, f.name);
            if (!inits[i] && !f.defaultValue) {
                fillDefault(place, f.type);
                flushTemps();
                continue;
            }
            llvm::Value* v = genOwned(inits[i] ? *inits[i] : *f.defaultValue, f.type);
            flushTemps();
            storeValue(v, f.type, place);
        }
    }

    void defineFunction(const FuncDecl& f) {
        if (f.isExtern) return;  // defined in C
        if (f.isAsync && !f.isPure) return defineAsync(f);
        fn = funcs.at(&f);
        fnDecl = &f;
        locals.clear();
        errorOut = f.isThrows ? fn->getArg(fn->arg_size() - 1) : nullptr;
        localError = nullptr;
        b.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", fn));
        startLineTable(f.nameLoc);
        if (f.isPure) {
            // Calling this takes a constructor or destructor of the class, when the object is
            // only that class, since any other object has a version with a body.
            trapHere("the method '" + f.owner->name + "." + f.name + "' has no body, and was called while an object was "
                     "only a '" + f.owner->name + "', because a constructor or destructor was running",
                     f.nameLoc);
            endLineTable();
            return;
        }
        unsigned first = (inMemory(f.retType) ? 1 : 0) + (f.owner ? 1 : 0);
        thisPtr = f.owner ? fn->getArg(first - 1) : nullptr;
        // Parameters own their values, like locals, and are released when the function ends.
        scopes.emplace_back();
        for (size_t i = 0; i < f.params.size(); ++i) {
            if (passedByRef(*f.params[i])) {  // the caller's place, which the caller keeps
                locals[f.params[i].get()] = fn->getArg(first + i);
                continue;
            }
            if (inMemory(f.params[i]->type)) locals[f.params[i].get()] = fn->getArg(first + i);  // the caller's copy
            else b.CreateStore(fn->getArg(first + i), allocaFor(*f.params[i]));
            if (isManaged(f.params[i]->type)) scopes.back().push_back(f.params[i].get());
        }
        if (f.role == FuncDecl::Role::Constructor) {
            if (f.owner->isClass) {
                // The base classes come first. While the constructor of one runs, the object is
                // only that class, and a virtual call finds its version, as in C++.
                buildBases(thisPtr, *f.owner, &f);
                flushTemps();
                storeVtables(thisPtr, *f.owner);
            }
            genFieldInits(f);
        }
        genStmt(*f.body);
        if (!b.GetInsertBlock()->getTerminator()) {
            releaseScopes(0);
            if (f.retType->is(K::Void)) b.CreateRetVoid();
            else if (f.name == "main" && f.role == FuncDecl::Role::Free)
                b.CreateRet(b.getInt32(0));  // falling off main returns 0, as in C++
            else b.CreateUnreachable();       // Sema proved this is never reached
        }
        scopes.clear();
        endLineTable();
    }

    llvm::Function* intrinsic(llvm::Intrinsic::ID id, llvm::ArrayRef<llvm::Type*> types = {}) {
        return llvm::Intrinsic::getDeclaration(mod.get(), id, types);
    }

    void defineAsync(const FuncDecl& f) {
        const Type* taskType = f.retType;
        llvm::Function* body = asyncBodies.at(&f);
        // The call makes the task, bound to this thread's event loop, with one reference for the caller and one for the
        // coroutine, which runs until it first has to wait.
        fn = funcs.at(&f);
        fnDecl = &f;
        locals.clear();
        errorOut = nullptr;
        localError = nullptr;
        thisPtr = nullptr;
        b.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", fn));
        startLineTable(f.nameLoc);
        llvm::Value* task = construct(*taskType->decl, {});
        retainValue(task, taskType);
        callFunction(*f.taskBind, {task});
        std::vector<llvm::Value*> args{task};
        for (llvm::Argument& a : fn->args()) args.push_back(&a);
        b.CreateCall(body, args);
        b.CreateRet(task);
        endLineTable();

        fn = body;
        b.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", fn));
        startLineTable(f.nameLoc);
        llvm::Value* null = llvm::ConstantPointerNull::get(b.getPtrTy());
        // C++'s promise, which C+ keeps nothing in. LLVM 18 reads it whenever the line table is C++'s, as ours says it is.
        llvm::Value* promise = entryAlloca(b.getInt8Ty(), "promise");
        llvm::Value* id = b.CreateCall(intrinsic(llvm::Intrinsic::coro_id), {b.getInt32(16), promise, null, null}, "id");
        llvm::Value* size = b.CreateCall(intrinsic(llvm::Intrinsic::coro_size, {b.getInt64Ty()}), {}, "size");
        llvm::Value* memory = b.CreateCall(rtAlloc, {size}, "memory");
        llvm::Value* handle = b.CreateCall(intrinsic(llvm::Intrinsic::coro_begin), {id, memory}, "frame");
        AsyncState state{fn->getArg(0), id, handle,
                         f.isThrows ? llvm::BasicBlock::Create(ctx, "failed", fn) : nullptr,
                         llvm::BasicBlock::Create(ctx, "done", fn), llvm::BasicBlock::Create(ctx, "free", fn),
                         llvm::BasicBlock::Create(ctx, "leave", fn)};
        async = state;
        // What it is a method of is kept until it ends: a class object by a reference, and a struct as a copy, as with `spawn`.
        unsigned first = 1 + (f.owner ? 1 : 0);
        const Type* ownerType = f.owner ? &f.owner->type : nullptr;
        if (ownerType && ownerType->isClass()) {
            thisPtr = fn->getArg(1);
            retainValue(thisPtr, ownerType);
        } else if (ownerType) {
            llvm::AllocaInst* copy = entryAlloca(types.at(f.owner).layout, "self");
            llvm::Value* v = loadValue(ownerType, fn->getArg(1), "self");
            retainValue(v, ownerType);
            storeValue(v, ownerType, copy);
            thisPtr = copy;
        }
        // The arguments are the coroutine's own. One given by its address is moved into the frame, since the caller's
        // place for it goes away once the coroutine first waits.
        scopes.emplace_back();
        for (size_t i = 0; i < f.params.size(); ++i) {
            const Type* t = f.params[i]->type;
            llvm::Value* arg = fn->getArg(first + i);
            llvm::AllocaInst* slot = allocaFor(*f.params[i]);
            if (inMemory(t)) b.CreateMemCpy(slot, alignOf(t), arg, alignOf(t), sizeOf(t));
            else b.CreateStore(arg, slot);
            if (isManaged(t)) scopes.back().push_back(f.params[i].get());
        }
        if (f.isThrows) {
            errorOut = entryAlloca(b.getPtrTy(), "error");
            b.CreateStore(null, errorOut);
        }
        genStmt(*f.body);
        if (!b.GetInsertBlock()->getTerminator()) {
            releaseScopes(0);
            if (f.bodyRet()->is(K::Void)) finishAsync(nullptr);
            else if (f.name == "main" && f.role == FuncDecl::Role::Free) finishAsync(b.getInt32(0));  // as in C++
            else b.CreateUnreachable();  // Sema proved this is never reached
        }
        if (state.failed) {
            b.SetInsertPoint(state.failed);
            llvm::Value* error = b.CreateLoad(b.getPtrTy(), errorOut, "error");
            callFunction(*f.taskFail, {state.task, error});
            b.CreateBr(state.done);
        }
        b.SetInsertPoint(state.done);
        if (ownerType) releaseValue(ownerType->isClass() ? thisPtr : peekValue(ownerType, thisPtr), ownerType);
        releaseValue(state.task, taskType);
        b.CreateBr(state.freeFrame);
        b.SetInsertPoint(state.freeFrame);
        llvm::Value* used = b.CreateCall(intrinsic(llvm::Intrinsic::coro_free), {id, handle}, "used");
        auto* freeBB = llvm::BasicBlock::Create(ctx, "free.memory", fn);
        b.CreateCondBr(b.CreateIsNotNull(used), freeBB, state.leave);
        b.SetInsertPoint(freeBB);
        b.CreateCall(rtFree, {used});
        b.CreateBr(state.leave);
        b.SetInsertPoint(state.leave);
        b.CreateCall(intrinsic(llvm::Intrinsic::coro_end), {handle, b.getFalse(), llvm::ConstantTokenNone::get(ctx)});
        b.CreateRetVoid();
        scopes.clear();
        async.reset();
        endLineTable();
    }

    // The body of an async function is done, with what it returns, which is owned: the task gets it.
    void finishAsync(llvm::Value* v) {
        const FuncDecl& finish = *fnDecl->taskFinish;
        if (!v) v = llvm::Constant::getNullValue(llvmType(finish.params[0]->type));  // the empty struct of `Task<void>`
        callFunction(finish, {async->task, v});
        b.CreateBr(async->done);
    }

    // `await t`: if the task is not done, the coroutine stops here, and the event loop goes on with it once it is. Then
    // what the task gives is taken as `t.wait()` would give it, which can no longer have to wait.
    llvm::Value* genAwait(const AwaitExpr& a) {
        const Type* taskType = &a.taskDecl->type;
        llvm::Value* task = genOwned(*a.task, taskType);
        pending.push_back({task, taskType, false});
        llvm::Value* mustWait = callFunction(*a.then, {task, resumeFn, async->handle});
        auto* waitBB = llvm::BasicBlock::Create(ctx, "await.wait", fn);
        auto* readyBB = llvm::BasicBlock::Create(ctx, "await.ready", fn);
        b.CreateCondBr(condValue(mustWait, a.then->retType), waitBB, readyBB);
        b.SetInsertPoint(waitBB);
        llvm::Value* step = b.CreateCall(intrinsic(llvm::Intrinsic::coro_suspend), {llvm::ConstantTokenNone::get(ctx), b.getFalse()}, "step");
        llvm::SwitchInst* next = b.CreateSwitch(step, async->leave, 2);
        next->addCase(b.getInt8(0), readyBB);
        next->addCase(b.getInt8(1), async->freeFrame);  // destroyed while waiting, which nothing does
        b.SetInsertPoint(readyBB);
        llvm::Value* result = callFunction(*a.wait, {task});
        pending.pop_back();
        releaseValue(task, taskType);
        return result;
    }

    void defineEntryPoint(const FuncDecl& userMain) {
        auto* type = llvm::FunctionType::get(b.getInt32Ty(), {b.getInt32Ty(), b.getPtrTy()}, false);
        fn = llvm::Function::Create(type, llvm::Function::ExternalLinkage, "main", mod.get());
        b.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", fn));
        // With -O, the program's main is inlined here, and without a line table of its own
        // this function would lose that main's lines.
        startLineTable(userMain.nameLoc);
        b.CreateCall(rtStart, {});
        std::vector<llvm::Value*> args;
        if (!userMain.params.empty()) args = {fn->getArg(0), fn->getArg(1)};  // argc and argv
        // The program does not end while anything is pending on the event loop, or a thread is running.
        auto finish = [&](llvm::Value* code) {
            if (usesAsync) b.CreateCall(rtLoopFinish, {});
            if (usesThreads) b.CreateCall(rtThreadsFinish, {});
            b.CreateRet(code);
        };
        llvm::AllocaInst* place = nullptr;
        if (userMain.isThrows) {
            place = entryAlloca(b.getPtrTy(), "error");
            b.CreateStore(llvm::ConstantPointerNull::get(b.getPtrTy()), place);
        }
        llvm::Value* code;
        if (userMain.isAsync) {  // `async int main()`: its task is waited for, which runs the loop until it is done
            llvm::Value* task = b.CreateCall(funcs.at(&userMain), args);
            b.CreateCall(rtLoopWaitsForStart, {b.getInt32(0)});
            code = emitCall(*userMain.taskWait, {task}, false, place);
            releaseValue(task, userMain.retType);
        } else {
            if (place) args.push_back(place);
            code = b.CreateCall(funcs.at(&userMain), args);
        }
        if (!place) {
            finish(code);
            endLineTable();
            return;
        }
        // `int main() throws`: an error that gets this far is reported, and the program fails.
        llvm::Value* error = b.CreateLoad(b.getPtrTy(), place, "error");
        auto* failedBB = llvm::BasicBlock::Create(ctx, "uncaught", fn);
        auto* doneBB = llvm::BasicBlock::Create(ctx, "done", fn);
        b.CreateCondBr(b.CreateIsNotNull(error), failedBB, doneBB);
        b.SetInsertPoint(failedBB);
        llvm::AllocaInst* text = entryAlloca(stringTy, "text");
        b.CreateStore(llvm::Constant::getNullValue(stringTy), text);
        printValue(text, error, errorTy);
        b.CreateCall(rtUncaught, {text});
        releaseValue(error, errorTy);
        finish(b.getInt32(1));
        b.SetInsertPoint(doneBB);
        finish(code);
        endLineTable();
    }

    // ---- Generated functions for structs and classes -----------------------

    llvm::Function* helper(const StructDecl& sd, const char* what, llvm::Type* ret, std::vector<llvm::Type*> params) {
        return llvm::Function::Create(llvm::FunctionType::get(ret, params, false), llvm::Function::InternalLinkage,
                                      "cplus." + sd.name + "#" + what, mod.get());
    }

    void declareHelpers(const StructDecl& sd) {
        TypeInfo& info = types.at(&sd);
        llvm::Type* self = valueType(&sd.type);
        info.print = helper(sd, "print", b.getVoidTy(), {self, b.getPtrTy()});  // the value, the output
        if (!sd.isClass) info.equals = helper(sd, "equals", b.getInt1Ty(), {self, self});
        if (sd.isClass) {
            if (!objectRelease)
                objectRelease = llvm::Function::Create(llvm::FunctionType::get(b.getVoidTy(), {self}, false),
                                                       llvm::Function::InternalLinkage, "cplus.release_object", mod.get());
            info.release = objectRelease;
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

    // Reads field i of a struct value or class object, for a use that comes right away.
    llvm::Value* loadField(const StructDecl& sd, llvm::Value* self, size_t i) {
        if (!sd.isClass && !inMemory(&sd.type)) return b.CreateExtractValue(self, {(unsigned)i}, sd.fields[i].name);
        return peekValue(sd.fields[i].type, fieldAddress(&sd.type, self, (int)i, sd.fields[i].name),
                         sd.fields[i].name.c_str());
    }

    // The fields of a type, those of its base classes first.
    void printFields(llvm::Value* out, const StructDecl& sd, llvm::Value* self, bool& first) {
        for (const StructDecl* base : sd.bases) printFields(out, *base, atOffset(self, baseOffset(sd, *base)), first);
        for (size_t i = 0; i < sd.fields.size(); ++i) {
            printText(out, (first ? "" : ", ") + sd.fields[i].name + ": ");
            printValue(out, loadField(sd, self, i), sd.fields[i].type, true);
            first = false;
        }
    }

    // A class and its base classes, in the order they are destroyed, each with its distance
    // from the start of the object.
    struct ObjectPart {
        const StructDecl* cls;
        uint64_t offset;
    };
    void collectParts(const StructDecl& cls, uint64_t offset, std::vector<ObjectPart>& out) {
        out.push_back({&cls, offset});
        for (size_t i = cls.bases.size(); i-- > 0;) collectParts(*cls.bases[i], offset + baseOffset(cls, *cls.bases[i]), out);
    }

    // Reflection: constant data that tells the running program about a type. See runtime/reflect.h. It lays each out for
    // the types that ask for it, and what they lead to, so the rest of the program pays for nothing.
    llvm::StructType* descriptorType() {
        auto* ptr = b.getPtrTy();
        auto* i64 = b.getInt64Ty();
        return llvm::StructType::get(ctx, {i64, ptr, i64, ptr, i64, ptr, i64, ptr, ptr, ptr, ptr});
    }

    // A function that retains or releases the value at an address, for the runtime to call.
    llvm::Constant* countFunction(const Type* t, bool retain) {
        if (!isManaged(t)) return llvm::ConstantPointerNull::get(b.getPtrTy());
        auto [it, inserted] = countFunctions.try_emplace(std::pair{t, retain}, nullptr);
        if (!inserted) return it->second;
        auto* f = internalFunction(std::string("cplus.") + t->name + (retain ? "#retain_at" : "#release_at"), b.getVoidTy(), {b.getPtrTy()});
        it->second = f;
        SavedPoint saved(*this);
        startHelper(f);
        llvm::Value* value = peekValue(t, f->getArg(0));
        retain ? retainValue(value, t) : releaseValue(value, t);
        b.CreateRetVoid();
        return f;
    }

    // What makes a value in zero bytes, or null if they already are one: see cplus_type.
    llvm::Constant* makeFunction(const Type* t) {
        llvm::Constant* none = llvm::ConstantPointerNull::get(b.getPtrTy());
        bool object = t->isClass();
        if (object && (t->decl->isInterface || t->decl->firstPure() || (t->decl->constructor && !t->decl->constructor->params.empty()))) return none;
        if (!object && !t->isStruct() && !t->isArray()) return none;
        auto* f = internalFunction(std::string("cplus.") + t->name + "#make", b.getVoidTy(), {b.getPtrTy()});
        SavedPoint saved(*this);
        startHelper(f);
        if (object) {
            size_t mark = temps.size();
            const StructDecl& sd = *t->decl;
            llvm::Value* made = sd.constructor ? construct(sd, {}) : buildAggregate(sd, {});
            b.CreateStore(made, f->getArg(0));
            flushTemps(mark);
        } else {
            fillElement(f->getArg(0), t);
        }
        b.CreateRetVoid();
        return f;
    }

    // Whether a JSON object must have this field: what has no value of its own until it is given one.
    static bool mustBeGiven(const Type* t) {
        if (t->isOptional() || t->isVector() || t->isWeak() || isHashmap(t) || isJsonValue(t)) return false;
        if (t->isArray()) return t->length > 0 && mustBeGiven(t->elem);
        if (!t->isStruct()) return true;
        if (t->decl->constructor) return false;
        for (auto& f : t->decl->fields)
            if (!f.defaultValue && mustBeGiven(f.type)) return true;
        return false;
    }

    // The fields of a struct or class, those of its base classes first, as each is measured from `offset`.
    // A field that the compiler or the runtime needs of what comes with the language, wherever Builtins.h has it.
    static int builtinField(const StructDecl& sd, const char* name) {
        int index = sd.fieldIndex(name);
        if (index < 0) llvm::report_fatal_error(llvm::Twine("Builtins.h has no field ") + name + " in " + sd.name);
        return index;
    }

    void addFieldsOf(const StructDecl& sd, uint64_t offset, std::vector<llvm::Constant*>& out, bool built = false) {
        built = built || sd.constructor;  // this one, or one that derives from it, can give the fields their values
        for (const StructDecl* base : sd.bases) addFieldsOf(*base, offset + baseOffset(sd, *base), out, built);
        const llvm::StructLayout* layout = mod->getDataLayout().getStructLayout(types.at(&sd).layout);
        size_t before = !sd.isClass ? 0 : sd.bases.empty() ? 2 : sd.bases.size();
        auto* fieldTy = llvm::StructType::get(ctx, {b.getPtrTy(), b.getInt64Ty(), b.getPtrTy(), b.getInt64Ty()});
        // json.c knows a json::Value's fields by their places in the descriptor, whatever their order in Builtins.h.
        static const char* const kJsonValueFields[] = {"kind_", "flag_", "number_", "isWhole_", "whole_", "text_", "items_", "fields_"};
        std::vector<size_t> order;
        if (isJsonValue(&sd.type)) {
            for (const char* name : kJsonValueFields) order.push_back((size_t)builtinField(sd, name));
        } else {
            for (size_t i = 0; i < sd.fields.size(); ++i) order.push_back(i);
        }
        for (size_t i : order) {
            const FieldDecl& f = sd.fields[i];
            bool isPublic = f.access == Access::Public;
            int64_t flags = (isPublic ? CPLUS_FIELD_PUBLIC : 0) |
                            (isPublic && !f.defaultValue && !built && mustBeGiven(f.type) ? CPLUS_FIELD_REQUIRED : 0);
            out.push_back(llvm::ConstantStruct::get(fieldTy, {cstring(f.name), b.getInt64(offset + layout->getElementOffset(i + before)),
                                                              typeDescriptor(f.type), b.getInt64(flags)}));
        }
    }

    llvm::Constant* typeDescriptor(const Type* t) {
        auto [it, inserted] = descriptors.try_emplace(t, nullptr);
        if (!inserted) return it->second;
        auto* g = new llvm::GlobalVariable(*mod, descriptorType(), true, llvm::GlobalValue::InternalLinkage, nullptr, std::string("cplus.type.") + t->name);
        it->second = g;  // a type can lead back to itself
        llvm::Constant* none = llvm::ConstantPointerNull::get(b.getPtrTy());
        int64_t kind = CPLUS_KIND_OTHER, length = 0;
        llvm::Constant* elem = none;
        llvm::Constant* fields = none;
        llvm::Constant* put = none;
        int64_t fieldCount = 0;
        switch (t->kind) {
            case K::Bool: kind = CPLUS_KIND_BOOL; break;
            case K::Int: kind = CPLUS_KIND_INT; break;
            case K::Long: kind = CPLUS_KIND_LONG; break;
            case K::Float: kind = CPLUS_KIND_FLOAT; break;
            case K::Double: kind = CPLUS_KIND_DOUBLE; break;
            case K::Char: kind = CPLUS_KIND_CHAR; break;
            case K::String: kind = CPLUS_KIND_STRING; break;
            case K::Weak: kind = CPLUS_KIND_WEAK; break;
            case K::Vector: kind = CPLUS_KIND_VECTOR; elem = typeDescriptor(t->elem); break;
            case K::Unique: kind = CPLUS_KIND_UNIQUE; elem = typeDescriptor(t->elem); break;
            case K::Local: kind = CPLUS_KIND_LOCAL; elem = typeDescriptor(t->elem); break;
            case K::Array: kind = CPLUS_KIND_ARRAY; length = t->length; elem = typeDescriptor(t->elem); break;
            case K::Optional:
                kind = CPLUS_KIND_OPTIONAL;
                elem = typeDescriptor(t->elem);
                length = t->elem->isObjectRef() ? -1 : (int64_t)mod->getDataLayout().getStructLayout(llvm::cast<llvm::StructType>(llvmType(t)))->getElementOffset(1);
                break;
            case K::Struct: case K::Class: {
                const StructDecl& sd = *t->decl;
                if (isHashmap(t)) {
                    kind = CPLUS_KIND_MAP;
                    // json.c finds these by where they are, and `first` and `second` are what a program sees.
                    const StructDecl& entry = *sd.fields[builtinField(sd, "entries_")].type->elem->decl;
                    if (builtinField(sd, "entries_") != 0 || builtinField(entry, "first") != 0 || builtinField(entry, "second") != 1)
                        llvm::report_fatal_error("Builtins.h has moved a field of hashmap that json.c finds by its place");
                    elem = typeDescriptor(sd.fields[builtinField(sd, "entries_")].type->elem);
                    length = (int64_t)mod->getDataLayout().getStructLayout(types.at(&sd).layout)->getElementOffset(builtinField(sd, "hashes_"));
                    if (!buildsFromDescriptors) break;
                    auto* f = internalFunction("cplus." + sd.name + "#put", b.getVoidTy(), {b.getPtrTy(), b.getPtrTy()});
                    {
                        SavedPoint saved(*this);
                        startHelper(f);
                        const FuncDecl& method = *sd.ownMethod("__put");
                        callFunction(method, {f->getArg(0), peekValue(method.params[0]->type, f->getArg(1))});
                        b.CreateRetVoid();
                    }
                    put = f;
                    break;
                }
                kind = t->isClass() ? CPLUS_KIND_CLASS : isJsonValue(t) ? CPLUS_KIND_JSON_VALUE : CPLUS_KIND_STRUCT;
                std::vector<llvm::Constant*> list;
                addFieldsOf(sd, 0, list);
                fieldCount = (int64_t)list.size();
                if (!list.empty()) {
                    auto* arrayTy = llvm::ArrayType::get(list[0]->getType(), list.size());
                    fields = new llvm::GlobalVariable(*mod, arrayTy, true, llvm::GlobalValue::InternalLinkage, llvm::ConstantArray::get(arrayTy, list),
                                                      "cplus.fields." + sd.name);
                }
                break;
            }
            default: break;
        }
        uint64_t size = t->is(K::Void) || t->isOpaque() || t->isNull() ? 0 : mod->getDataLayout().getTypeAllocSize(llvmType(t));
        bool canHold = buildsFromDescriptors && !t->is(K::Void) && !t->isOpaque() && !t->isNull();  // for JSON, which makes values
        g->setInitializer(llvm::ConstantStruct::get(
            descriptorType(), {b.getInt64(kind), cstring(t->name), b.getInt64(size), elem, b.getInt64(length), fields, b.getInt64(fieldCount),
                               canHold ? makeFunction(t) : none, canHold ? countFunction(t, true) : none, canHold ? countFunction(t, false) : none, put}));
        return g;
    }

    // A hashmap prints as `{"a": 1, "b": 2}`, in order, and is equal to one with the same entries in any order.
    // See Builtins.h for what it holds. The insertion point is at the start of the print helper.
    void defineMapHelpers(const StructDecl& sd, TypeInfo& info) {
        int entriesField = builtinField(sd, "entries_");
        int hashesField = builtinField(sd, "hashes_");
        const Type* entries = sd.fields[entriesField].type;
        const Type* hashes = sd.fields[hashesField].type;
        const Type* entry = entries->elem;
        const Type* keyType = sd.templateArgs[0].type;
        const Type* valueType = sd.templateArgs[1].type;
        llvm::Value* self = info.print->getArg(0);
        llvm::Value* out = info.print->getArg(1);
        llvm::Value* buf = loadField(sd, self, entriesField);
        llvm::Value* hashBuf = loadField(sd, self, hashesField);
        llvm::Value* any = entryAlloca(b.getInt1Ty(), "any");
        b.CreateStore(b.getInt1(false), any);
        printText(out, "{");
        countedLoop(vecSize(buf), [&](llvm::Value* i) {
            auto* entryBB = llvm::BasicBlock::Create(ctx, "entry", fn);
            auto* sepBB = llvm::BasicBlock::Create(ctx, "sep", fn);
            auto* showBB = llvm::BasicBlock::Create(ctx, "show", fn);
            auto* nextBB = llvm::BasicBlock::Create(ctx, "next", fn);
            llvm::Value* hash = b.CreateLoad(b.getInt64Ty(), elementAt(hashBuf, i, hashes), "hash");
            b.CreateCondBr(b.CreateICmpSLT(hash, b.getInt64(0)), nextBB, entryBB);
            b.SetInsertPoint(entryBB);
            b.CreateCondBr(b.CreateLoad(b.getInt1Ty(), any), sepBB, showBB);
            b.SetInsertPoint(sepBB);
            printText(out, ", ");
            b.CreateBr(showBB);
            b.SetInsertPoint(showBB);
            b.CreateStore(b.getInt1(true), any);
            llvm::Value* at = elementAt(buf, i, entries);
            printValue(out, peekValue(keyType, fieldAddress(entry, at, 0, "key")), keyType, true);
            printText(out, ": ");
            printValue(out, peekValue(valueType, fieldAddress(entry, at, 1, "value")), valueType, true);
            b.CreateBr(nextBB);
            b.SetInsertPoint(nextBB);
        });
        printText(out, "}");
        b.CreateRetVoid();

        startHelper(info.equals);
        auto place = [&](llvm::Value* value) -> llvm::Value* {
            if (inMemory(&sd.type)) return value;
            llvm::Value* slot = entryAlloca(llvmType(&sd.type), "map");
            b.CreateStore(value, slot);
            return slot;
        };
        llvm::Value* left = place(info.equals->getArg(0));
        llvm::Value* right = place(info.equals->getArg(1));
        b.CreateRet(callFunction(*sd.ownMethod("__same"), {left, right}));
        defineFieldCounting(sd, info);
    }

    // Retaining and releasing a struct, field by field, the last declared first.
    void defineFieldCounting(const StructDecl& sd, TypeInfo& info) {
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
    }

    void defineHelpers(const StructDecl& sd) {
        TypeInfo& info = types.at(&sd);

        // Printing: `{x: 1, y: 2}` for a struct, `User {name: "Ada", age: 36}` for a class.
        startHelper(info.print);
        llvm::Value* self = info.print->getArg(0);
        llvm::Value* out = info.print->getArg(1);
        if (isHashmap(&sd.type)) {
            defineMapHelpers(sd, info);
            return;
        }
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
        if (isJsonValue(&sd.type)) {  // as the JSON it stands for
            llvm::Value* place = entryAlloca(llvmType(&sd.type), "value");
            storeValue(self, &sd.type, place);
            llvm::Value* text = entryAlloca(stringTy, "text");
            b.CreateStore(llvm::Constant::getNullValue(stringTy), text);
            // Declared by the module json, which is where this type comes from.
            b.CreateCall(mod->getFunction("cplus_json_write"), {place, typeDescriptor(&sd.type), b.getInt32(0), text});
            llvm::Value* written = b.CreateLoad(stringTy, text, "written");
            printValue(out, written, Type::stringTy());
            releaseValue(written, Type::stringTy());
        } else if (sd.role == StructDecl::Role::Opaque) {
            if (!sd.isClass) printText(out, sd.name + " ");
            printText(out, "{...}");
        } else {
            printText(out, "{");
            bool first = true;
            printFields(out, sd, self, first);
            printText(out, "}");
        }
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
            defineFieldCounting(sd, info);
            return;
        }

        // Destroy: run the destructor, then release the fields, last declared first as in C++,
        // and the same for each base class in turn, the last one first, with the object then
        // counting as only that class. The fields that come after the last destructor are read
        // first, the memory freed, and then they are released. The last release is a tail call,
        // so freeing a long chain of objects, like a linked list, needs no more stack than
        // freeing one.
        startHelper(info.destroy);
        self = info.destroy->getArg(0);
        std::vector<ObjectPart> parts;
        collectParts(sd, 0, parts);
        size_t lastDestructor = 0;
        for (size_t c = 0; c < parts.size(); ++c)
            if (parts[c].cls->destructor) lastDestructor = c;
        struct HeldField {
            const ObjectPart* part;
            size_t index;
            const Type* type() const { return part->cls->fields[index].type; }
        };
        std::vector<HeldField> order;  // the managed fields to release after freeing, in that order
        bool held = false;
        for (size_t c = 0; c < parts.size(); ++c) {
            const StructDecl& cls = *parts[c].cls;
            llvm::Value* part = atOffset(self, parts[c].offset);
            if (cls.destructor) {
                // While the destructors run, the object counts as having one reference, so
                // passing `this` to a function cannot destroy it a second time.
                if (!held) b.CreateCall(rtDestructorHold, {self});
                held = true;
                if (c > 0) storeVtables(part, cls);
                b.CreateCall(funcs.at(cls.destructor), {part});
                trapIf(b.CreateICmpNE(b.CreateCall(rtReferences, {self}), b.getInt64(1)),
                       "the destructor of '" + cls.name + "' stored 'this' somewhere, so the object would be used after it is freed",
                       cls.destructor->nameLoc);
            }
            for (size_t i = cls.fields.size(); i-- > 0;) {
                if (!isManaged(cls.fields[i].type)) continue;
                if (c >= lastDestructor) order.push_back({&parts[c], i});
                else releaseValue(loadField(cls, part, i), cls.fields[i].type);
            }
        }
        // The chain usually goes through the first class-typed field. It can be released
        // last only if no field declared before it could run a destructor.
        for (size_t k = order.size(); k-- > 0;) {
            const Type* ft = order[k].type();
            if (classOf(ft)) {
                std::rotate(order.begin() + k, order.begin() + k + 1, order.end());
                break;
            }
            std::set<const StructDecl*> seen;
            if (mayRunDestructor(ft, seen)) break;
        }
        std::vector<llvm::Value*> values;
        for (const HeldField& f : order) {  // a value in the object's memory is copied out of it first
            const StructDecl& cls = *f.part->cls;
            const FieldDecl& field = cls.fields[f.index];
            llvm::Value* part = atOffset(self, f.part->offset);
            values.push_back(inMemory(field.type) ? loadValue(field.type, fieldAddress(&cls.type, part, (int)f.index, field.name))
                                                  : loadField(cls, part, f.index));
        }
        b.CreateCall(rtFreeClass, {self});
        for (size_t k = 0; k < order.size(); ++k) {
            const Type* ft = order[k].type();
            if (k + 1 == order.size() && classOf(ft)) {
                b.CreateCall(objectRelease, {values[k]})->setTailCallKind(llvm::CallInst::TCK_MustTail);
                break;
            }
            releaseValue(values[k], ft);
        }
        b.CreateRetVoid();
    }

    // Releasing any object: when that was the last reference, destroy what the object really is.
    void defineObjectRelease() {
        startHelper(objectRelease);
        llvm::Value* self = objectRelease->getArg(0);
        if (hasInnerParts) self = b.CreateCall(rtObjectStart, {self}, "start");
        auto* destroyBB = llvm::BasicBlock::Create(ctx, "destroy", fn);
        auto* doneBB = llvm::BasicBlock::Create(ctx, "done", fn);
        b.CreateCondBr(b.CreateICmpNE(b.CreateCall(rtReleaseClass, {self}), b.getInt32(0)), destroyBB, doneBB);
        b.SetInsertPoint(destroyBB);
        b.CreateCall(objectRelease->getFunctionType(), vtableSlot(loadVtable(self), kVtableDestroy, "destroy"), {self})
            ->setTailCallKind(llvm::CallInst::TCK_MustTail);
        b.CreateRetVoid();
        b.SetInsertPoint(doneBB);
        b.CreateRetVoid();
    }

    // What a vtable at a part of an object calls for method `f`. `f` wants `this` to be the part
    // that declares it, so when that is not where this vtable lies, a small function moves it.
    llvm::Constant* vtableMethod(const FuncDecl& f, uint64_t distance) {
        llvm::Function* target = funcs.at(&f);
        if (!distance) return target;
        auto key = std::pair{&f, distance};
        auto [it, inserted] = thunks.emplace(key, nullptr);
        if (!inserted) return it->second;
        auto* thunk = llvm::Function::Create(target->getFunctionType(), llvm::Function::InternalLinkage,
                                             target->getName() + "#from" + std::to_string(distance), mod.get());
        it->second = thunk;
        startHelper(thunk);
        std::vector<llvm::Value*> args;
        for (llvm::Argument& a : thunk->args()) args.push_back(&a);
        unsigned self = inMemory(f.retType) ? 1 : 0;
        args[self] = b.CreateInBoundsGEP(b.getInt8Ty(), args[self], b.getInt64(-(int64_t)distance));
        llvm::CallInst* call = b.CreateCall(target, args);
        call->setTailCallKind(llvm::CallInst::TCK_MustTail);
        if (call->getType()->isVoidTy()) b.CreateRetVoid();
        else b.CreateRet(call);
        return thunk;
    }

    // Each vtable of a class, and the list of its parts they share. A part is named by the
    // first vtable of its class, which makes its address the class's identity.
    void defineVtables(const StructDecl& sd) {
        TypeInfo& info = types.at(&sd);
        std::vector<ObjectPart> parts;
        collectParts(sd, 0, parts);
        auto* partTy = llvm::StructType::get(ctx, {b.getPtrTy(), b.getInt64Ty()});
        std::vector<llvm::Constant*> partValues;
        for (const ObjectPart& p : parts)
            partValues.push_back(llvm::ConstantStruct::get(partTy, {types.at(p.cls).vtables[0], b.getInt64(p.offset)}));
        auto* listTy = llvm::ArrayType::get(partTy, parts.size());
        llvm::Constant* partList = llvm::ConstantStruct::getAnon({b.getInt64(parts.size()), llvm::ConstantArray::get(listTy, partValues)});
        auto* partsGlobal = new llvm::GlobalVariable(*mod, partList->getType(), true, llvm::GlobalValue::InternalLinkage, partList,
                                                     "cplus.parts." + sd.name);
        for (size_t k = 0; k < info.vtables.size(); ++k) {
            const BasePath& place = info.vtablePlaces[k];
            uint64_t placeOffset = pathOffset(place);
            std::vector<llvm::Constant*> entries{
                partsGlobal, llvm::ConstantExpr::getIntToPtr(b.getInt64(placeOffset), b.getPtrTy()), info.destroy, info.print,
                usesDescriptors ? typeDescriptor(&sd.type) : llvm::ConstantPointerNull::get(b.getPtrTy())};
            for (const FuncDecl* m : place.back()->vtable) {
                const FuncDecl* f = StructDecl::finalOverrider(place, m);
                // A method declared on the way to this part is that far back. Any other came with
                // the part, from its first base class, and lies at the same address.
                uint64_t distance = 0;
                for (size_t i = 0; i < place.size(); ++i)
                    if (place[i] == f->owner) {
                        BasePath prefix(place.begin(), place.begin() + i + 1);
                        distance = placeOffset - pathOffset(prefix);
                        break;
                    }
                entries.push_back(vtableMethod(*f, distance));
            }
            info.vtables[k]->setInitializer(
                llvm::ConstantArray::get(llvm::cast<llvm::ArrayType>(info.vtables[k]->getValueType()), entries));
        }
    }

    // The class of a field that refers to an object, or may: `User` or `User?`.
    static const Type* classOf(const Type* t) {
        if (t->isOptional()) t = t->elem;
        return t->isObjectRef() ? t : nullptr;
    }

    // Whether releasing a value of this type could free an object, and so run a
    // destructor.
    static bool mayRunDestructor(const Type* t, std::set<const StructDecl*>& seen) {
        if (t->isOptional() || t->isVector() || t->isArray()) return mayRunDestructor(t->elem, seen);
        if (t->isObjectRef()) return true;
        if (!t->isStruct() || !seen.insert(t->decl).second) return false;
        for (auto& f : t->decl->fields)
            if (mayRunDestructor(f.type, seen)) return true;
        return false;
    }

    bool generate(const Program& program, std::string& error) {
        mod = std::make_unique<llvm::Module>(file.path, ctx);
        if (!setupTarget(error)) return false;
        if (opts.debug || opts.race) setupLineTable();
        usesDescriptors = program.usesDescriptors() || opts.debug;  // a leak report says what an object is
        buildsFromDescriptors = program.buildsFromDescriptors();
        usesThreads = program.usesThreads;
        usesAsync = program.usesAsync;
        stringTy = llvm::StructType::create(ctx, {b.getPtrTy(), b.getInt64Ty(), b.getPtrTy()}, "cplus.string");
        // Named types are created first and given bodies after, so they can refer to
        // each other in any order.
        // A C type known only by name has no layout; C+ only points to it.
        std::vector<const StructDecl*> structs;
        for (auto& sd : program.structs)
            if (!sd->opaque) structs.push_back(sd.get());
        for (const StructDecl* sd : structs)
            types[sd].layout = llvm::StructType::create(ctx, (sd->isClass ? "cplus.class." : "cplus.struct.") + sd->name);
        for (const StructDecl* sd : structs) {
            std::vector<llvm::Type*> fields;
            for (const StructDecl* base : sd->bases) fields.push_back(types.at(base).layout);
            hasInnerParts = hasInnerParts || sd->bases.size() > 1;
            if (sd->isClass && sd->bases.empty()) {
                fields.push_back(b.getInt64Ty());  // the reference count
                fields.push_back(b.getPtrTy());    // the vtable
            }
            for (auto& f : sd->fields) fields.push_back(llvmType(f.type));
            // As in C++, an empty struct takes one byte, so `new E[3]` makes three of them at
            // different addresses. Inside `extern "C"` it follows C, where it takes none.
            if (fields.empty() && !sd->externC) fields.push_back(b.getInt8Ty());
            types[sd].layout->setBody(fields);
        }
        declareRuntime();
        fileName = cstring(file.path);
        if (const StructDecl* error = program.findStruct("Error"); error && error->isClass) errorTy = &error->type;

        const FuncDecl* userMain = nullptr;
        for (auto& f : program.funcs) {
            declareFunction(*f);
            if (f->name == "main") userMain = f.get();
        }
        if (!abiError.empty()) {
            error = abiError;
            return false;
        }
        for (const StructDecl* sd : structs) {
            for (auto& m : sd->methods)
                if (m->requested) declareFunction(*m);  // a generic class's other methods were never used
            declareHelpers(*sd);
        }
        for (const StructDecl* sd : structs) {
            if (!sd->isClass) continue;
            TypeInfo& info = types.at(sd);
            info.vtablePlaces = sd->vtablePlaces();
            for (const BasePath& place : info.vtablePlaces) {
                auto* type = llvm::ArrayType::get(b.getPtrTy(), kVtableMethods + place.back()->vtable.size());
                info.vtables.push_back(new llvm::GlobalVariable(*mod, type, true, llvm::GlobalValue::InternalLinkage, nullptr,
                                                                "cplus.vtable." + sd->name));
            }
        }
        for (const StructDecl* sd : structs)
            if (sd->isClass) defineVtables(*sd);
        if (objectRelease) defineObjectRelease();
        for (const StructDecl* sd : structs) defineHelpers(*sd);
        // One function for each constant that is not a literal, so that a chain of them, each using the one
        // before twice, stays as small as it is written.
        auto isLiteral = [](const Expr& e) {
            return e.kind == Expr::Kind::IntLit || e.kind == Expr::Kind::FloatLit || e.kind == Expr::Kind::CharLit ||
                   e.kind == Expr::Kind::BoolLit || e.kind == Expr::Kind::StringLit;
        };
        for (auto& c : program.constants)
            if (!isLiteral(*c->init))
                constantFunctions[c.get()] = llvm::Function::Create(llvm::FunctionType::get(llvmType(c->type), {}, false),
                                                                    llvm::Function::InternalLinkage, "cplus.const." + c->name, mod.get());
        for (auto& [c, f] : constantFunctions) {
            fn = f;
            fnDecl = nullptr;
            locals.clear();
            errorOut = nullptr;
            localError = nullptr;
            thisPtr = nullptr;
            b.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", fn));
            startLineTable(c->loc);
            llvm::Value* v = genOwned(*c->init, c->type);
            flushTemps();
            b.CreateRet(v);
            endLineTable();
        }
        if (usesAsync) {
            resumeFn = llvm::Function::Create(llvm::FunctionType::get(b.getVoidTy(), {b.getPtrTy()}, false), llvm::Function::InternalLinkage,
                                              "cplus.resume", mod.get());
            llvm::IRBuilder<> rb(llvm::BasicBlock::Create(ctx, "entry", resumeFn));
            rb.CreateCall(intrinsic(llvm::Intrinsic::coro_resume), {resumeFn->getArg(0)});
            rb.CreateRetVoid();
        }
        for (auto& f : program.funcs) defineFunction(*f);
        for (const StructDecl* sd : structs)
            for (auto& m : sd->methods)
                if (m->requested) defineFunction(*m);
        if (userMain) defineEntryPoint(*userMain);

        // A large array on the stack must not step over the guard page below it into other
        // memory: each new page is touched in turn, so running out of stack always stops
        // the program.
        for (llvm::Function& f : *mod) {
            if (f.isDeclaration()) continue;
            f.addFnAttr("probe-stack", "inline-asm");
            if (opts.debug) {  // checked by AddressSanitizer, with frames a stack trace can walk
                f.addFnAttr(llvm::Attribute::SanitizeAddress);
                f.addFnAttr("frame-pointer", "all");
                f.setUWTableKind(llvm::UWTableKind::Async);
            }
            if (opts.race) {  // the same, by ThreadSanitizer
                f.addFnAttr(llvm::Attribute::SanitizeThread);
                f.addFnAttr("frame-pointer", "all");
                f.setUWTableKind(llvm::UWTableKind::Async);
            }
        }
        if (di) di->finalize();

        std::string verifyErrors;
        llvm::raw_string_ostream os(verifyErrors);
        if (std::getenv("CPLUS_INVALID_IR_FOR_TESTS")) {  // so the tests can reach what a mistake of the compiler reports
            llvm::Function* broken = llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), false), llvm::Function::InternalLinkage, "__broken", *mod);
            llvm::BasicBlock::Create(ctx, "entry", broken);  // a block with no end
        }
        if (llvm::verifyModule(*mod, &os)) {
            const std::string first = os.str().substr(0, os.str().find('\n'));  // what LLVM says first; the rest is the code it was about
            error = "internal compiler error: generated invalid LLVM IR (" + first + ")";
            return false;
        }
        if (opts.optimize || opts.debug || opts.race || usesAsync) runPasses();  // coroutines are only cut up by the passes
        return true;
    }

    void runPasses() {
        llvm::LoopAnalysisManager lam;
        llvm::FunctionAnalysisManager fam;
        llvm::CGSCCAnalysisManager cgam;
        llvm::ModuleAnalysisManager mam;
        llvm::PassBuilder pb(tm.get());
        // After the optimizer, so its checks are not optimized and it checks what will run.
        if (opts.debug)
            pb.registerOptimizerLastEPCallback([](llvm::ModulePassManager& mpm, llvm::OptimizationLevel) {
                mpm.addPass(llvm::AddressSanitizerPass(llvm::AddressSanitizerOptions{}));
            });
        if (opts.race)
            pb.registerOptimizerLastEPCallback([](llvm::ModulePassManager& mpm, llvm::OptimizationLevel) {
                mpm.addPass(llvm::ModuleThreadSanitizerPass());
                mpm.addPass(llvm::createModuleToFunctionPassAdaptor(llvm::ThreadSanitizerPass()));
            });
        pb.registerModuleAnalyses(mam);
        pb.registerCGSCCAnalyses(cgam);
        pb.registerFunctionAnalyses(fam);
        pb.registerLoopAnalyses(lam);
        pb.crossRegisterProxies(lam, fam, cgam, mam);
        llvm::ModulePassManager mpm = opts.optimize ? pb.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2)
                                                    : pb.buildO0DefaultPipeline(llvm::OptimizationLevel::O0);
        mpm.run(*mod, mam);
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
    if (std::getenv("CPLUS_NO_OBJECT_FILES_FOR_TESTS") || impl_->tm->addPassesToEmitFile(pm, out, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        error = "this target cannot emit object files";
        return false;
    }
    pm.run(*impl_->mod);
    out.close();
    if (out.has_error()) {  // a full disk, say. The stream would stop the compiler if this were left unasked
        error = "cannot write " + path + ": " + out.error().message();
        out.clear_error();
        return false;
    }
    return true;
}

}  // namespace cplus
