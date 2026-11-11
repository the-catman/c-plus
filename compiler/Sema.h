// Semantic analysis: resolves names, assigns types, and enforces the rules that make
// C+ safer than C++: definite assignment (down to struct fields), no implicit narrowing,
// every path of a non-void function returns, and checked print format strings.
#pragma once

#include <deque>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "AST.h"
#include "ConstEval.h"

namespace cplus {

class Parser;

class Sema {
public:
    // `warnNewDelete` is false with -Wno-new-delete. The parser makes what generics are used for.
    Sema(Diagnostics& diags, Parser& parser, bool warnNewDelete = true)
        : diags_(diags), parser_(parser), warnNewDelete_(warnNewDelete) {}
    void check(Program& program);

private:
    // A place that holds a value: a variable (empty path) or a field inside one ("pos.x").
    using Place = std::pair<const VarDecl*, std::string>;
    // What is known at a point in the program: which places definitely hold a value,
    // and whether the point can be reached at all.
    struct Flow {
        std::set<Place> assigned;
        bool reachable = true;
        // Pointer variables that may hold the address of a local of this function, and the local, so
        // that returning one is caught. A path that may have made it so is enough.
        std::map<const VarDecl*, const VarDecl*> pointsToLocal;
    };
    struct LoopContext {
        std::vector<Flow> breaks;     // state at each `break`
        std::vector<Flow> continues;  // state at each `continue`
        bool isSwitch = false;        // a switch, which a `break` leaves and a `continue` passes through
    };

    // Definite assignment
    Flow merge(const Flow& a, const Flow& b) const;
    bool isAssigned(const Flow& flow, const VarDecl* var, const Type* type, const std::string& path) const;
    bool anyPartAssigned(const Flow& flow, const VarDecl* var, const std::string& path) const;
    std::string firstUnassigned(const Flow& flow, const VarDecl* var, const Type* type, const std::string& path) const;
    void markAssigned(Flow& flow, const VarDecl* var, const std::string& path);
    void markDefaults(Flow& flow, const VarDecl* var, const Type* type, const std::string& path);
    void markConstructed(Flow& flow, const VarDecl* var, const Type* type, const std::string& path);
    void markFieldConstructed(Flow& flow, const VarDecl* var, const FieldDecl& f, const std::string& path);
    void requireFullyBuilt(Flow& flow, Loc loc);  // at the end of a constructor
    void requireAssigned(Flow& flow, const VarDecl* var, const std::string& path, const Type* type, Loc loc);

    // New types can appear while checking, whenever a generic is used. adoptTypes makes them known
    // at once; checkNewStructs checks them when no function is being checked.
    void adoptTypes();
    void checkNewStructs();
    void requestMethod(FuncDecl* m);  // a method of a generic class is checked once it is used
    void checkTemplateNames(Program& program);
    const Type* checkTemplateCall(CallExpr& call, TemplateDecl& t, Flow& flow);
    bool checkConstraints(TemplateDecl& t, const std::vector<TemplateArg>& args, Loc usedAt);
    // Why `t` does not fit a constraint, as a help text; empty if it does. `where` gets a
    // requirement that is not met.
    std::string constraintProblem(const std::string& constraint, const Type* t, Loc& where);
    void checkBases(size_t first);
    void checkOverrides(StructDecl& sd);
    // Reports what stops the base class `base` of `sd` from being built with no values.
    void checkBaseWithoutValues(const StructDecl& sd, const StructDecl& base, Loc loc);
    // The path to the class named before `::` in `x.Animal::speak()`, when `sd` is it or
    // derives from it. Empty after reporting an error.
    BasePath findQualifier(const std::string& name, const StructDecl& sd, Loc loc);
    // The path to the member called `name` in `from`, or empty if there is none. Reports the
    // name when it does not say which member is meant, and then gives the first.
    BasePath findMember(const StructDecl& from, const std::string& name, Loc loc);
    const Type* checkDynamicCast(CastExpr& c, const Type* from, const Type* to);
    void checkStructs(size_t first);
    void checkConstants(Program& program);
    void checkConstant(VarDecl& constant);
    const Type* checkReinterpretCast(CastExpr& c, const Type* from, const Type* to);
    std::set<const VarDecl*> constantsChecked_, constantsBeingChecked_;
    void checkMembers(StructDecl& sd);
    void checkFunction(FuncDecl& fn);
    void checkExternFunction(FuncDecl& fn);
    void checkStmt(Stmt& s, Flow& flow);
    void checkVarDecl(VarDecl& v, Flow& flow);
    void checkLoopBody(Stmt& body, Flow& flow);
    void checkForEach(ForEachStmt& st, Flow& flow);
    void checkSwitch(SwitchStmt& st, Flow& flow);
    const Type* checkEnumValue(EnumValueExpr& e);
    const Type* checkEnumCast(CastExpr& c, const Type* from, const Type* to);
    void reportEnumOperator(const BinaryExpr& b, const Type* l, const Type* r);
    void checkIfConstexpr(IfStmt& st, Flow& flow);
    // `move(x)`, which leaves a variable with no value.
    const Type* checkMove(CallExpr& call, Flow& flow);
    void markMoved(Flow& flow, const VarDecl* var, Loc loc);
    void dropMovedSince(Flow& flow, size_t logStart);  // for code that may or may not have run
    // A variable moved in a loop has no value the next time round, unless it gets a new one.
    void checkMovedInLoop(const Flow& head, size_t logStart, const Flow& end);
    void rejectCopy(const Expr& e);  // reports reading what has one owner as if it could be copied
    // Working out values while compiling. `what` starts the message of an error.
    ConstHooks constHooks();
    bool evaluate(const Expr& e, ConstValue& out, const std::string& what);
    // For `constexpr`: checks the value can be worked out, and puts it in place of the expression.
    void foldConstexpr(VarDecl& v);
    // For a plain `const`: the same when it can be done. It is an error only if the value calls a function.
    void foldConstant(VarDecl& v);
    // Whether a member of the type being checked hides a constant of the file with this name.
    bool fieldHides(const std::string& name) const;
    FuncDecl* mapMethod(const Type* map, const std::string& name);
    const Type* checkMapIndex(IndexExpr& ix, Flow& flow, const Type* map);
    const Expr* constMapReported_ = nullptr;  // a `m[key]` that was reported, so that it is said once
    void putLiteral(VarDecl& v, const ConstValue& value);
    bool constraintHolds(const ConstraintExpr& c);
    void checkNow(FuncDecl& fn);  // for a function that runs while compiling, whose body must be checked first
    // While something else is checked in the middle of what is being checked: nothing around it counts.
    struct FreshContext {
        Sema& s;
        FuncDecl* fn;
        const FieldDecl* initField;
        const StructDecl* buildingBase;
        const StructDecl* defaultsOf;
        std::vector<std::unordered_map<std::string, VarDecl*>> scopes;
        std::vector<LoopContext> loops;
        int tryBlocks, tryPrefixes, failPoints;
        std::vector<const CatchClause*> catches;
        explicit FreshContext(Sema& sema);
        ~FreshContext();
    };
    void checkTry(TryStmt& st, Flow& flow);
    void checkThrow(ThrowStmt& st, Flow& flow);
    const StructDecl* errorClass();  // `Error`, what every thrown object derives from
    // A call to a function that can fail, or a `throw`, must have somewhere for the error to go.
    void checkCanFail(const FuncDecl& callee, Loc loc, const Expr* receiver = nullptr);
    std::string whereAnErrorCanGo() const;
    std::string copyHelp(const Expr& target, const std::string& change, bool givesValue = false, const std::string& before = "");
    std::string freshName(const std::string& avoid, std::initializer_list<const char*> names = {"v", "inner", "item", "held", "unwrapped"});
    std::string constHolderHelp(const Expr& e, bool canCopy = true);
    std::string loopCopyNote(const Expr& e);
    std::string changeForm(const Expr& base, const std::string& change, const std::string& opening, const std::string& how, const std::string& before = "");
    std::string storeItBack(const Expr& recv, const std::string& method, bool hasArguments, const std::string& before = "", const std::string& keepAs = "");
    const Type* checkSpawn(SpawnExpr& s, Flow& flow);
    const Type* checkAwait(AwaitExpr& a, Flow& flow);
    void prepareAsync(FuncDecl& fn);  // an async function's call gives its Task: see FuncDecl::declaredRet
    void checkTaskKept(const FuncDecl& f, const Expr& call, Loc loc);  // `f();`, which throws away the task of an async call
    const Expr* spawned_ = nullptr;  // the call `spawn` is on, whose error goes in the task
    std::vector<const Expr*> discarded_;  // what the statement being checked throws away, whose values go nowhere
    const Type* checkExpr(Expr& e, Flow& flow);
    // Checks `e` where a value of type `expected` is needed. A `{ }` list takes that type.
    void checkValue(Expr& e, Flow& flow, const Type* expected, const std::string& context);
    void checkInitList(InitListExpr& list, Flow& flow, const Type* type);
    void checkLooseValue(Expr& e, Flow& flow);  // checks an expression whose target type is unknown
    const Type* checkCall(CallExpr& call, Flow& flow);
    const Type* checkMethodCall(MethodCallExpr& call, Flow& flow);
    const Type* checkVectorMethod(MethodCallExpr& call, Flow& flow, const Type* vecType);
    const Type* checkArrayMethod(MethodCallExpr& call, Flow& flow, const Type* arrayType);
    const Type* checkVectorConstruction(CallExpr& call, Flow& flow);
    const Type* checkNew(NewExpr& n, Flow& flow);
    const Type* checkDelete(DeleteExpr& d, Flow& flow);
    // The warning for `new` and `delete`, unless [[allow(new_delete)]] or -Wno-new-delete
    // turns it off.
    void warnManualMemory(Loc loc, const std::string& message, const std::string& alternative);
    const Type* checkStringMethod(MethodCallExpr& call, Flow& flow);
    const Type* checkStringEdit(MethodCallExpr& call, Flow& flow);
    const Type* checkStringHabit(MethodCallExpr& call, Flow& flow);
    const Type* checkJoin(MethodCallExpr& call, Flow& flow, const Type* vecType);
    const Type* checkVectorInsertErase(MethodCallExpr& call, Flow& flow, const Type* vecType);
    const Type* checkToString(CallExpr& call, Flow& flow);
    const Type* checkCast(CastExpr& c, Flow& flow);
    const Type* checkPointerCast(CastExpr& c, const Type* from, const Type* to);
    const Type* checkAddressOf(UnaryExpr& u, Flow& flow);
    const Type* checkPointerBinary(BinaryExpr& b, const Type* l, const Type* r);
    void reportNotPointer(const UnaryExpr& u, const Type* t);
    // `op` cannot be used on `ptr` because what it points to has no known size.
    void reportUnknownPointee(Loc loc, const Type* ptr, const std::string& op);
    // Whether a place can only be read: `&x` of it is a pointer to const.
    bool isReadOnlyPlace(const Expr& target);
    // Reports a C type known only by name used as a value, not through a pointer.
    bool checkStorable(const Type* t, Loc loc);
    bool checkSize(const Type* t, Loc loc);
    const Type* checkMath(CallExpr& call, Flow& flow);
    const Type* checkCharFn(CallExpr& call, Flow& flow);
    const Type* checkStringFill(CallExpr& call, Flow& flow);
    const Type* checkOptionalMethod(MethodCallExpr& call, Flow& flow, const Type* opt);
    const Type* checkWeakMethod(MethodCallExpr& call, Flow& flow, const Type* weak);
    const Type* checkOptChain(OptChainExpr& c, Flow& flow);
    const Type* checkCoalesce(CoalesceExpr& c, Flow& flow);
    void checkCondDecl(CondDecl& d, Flow& flow, const char* stmt);
    void checkChangingReceiver(const Expr& recv, const char* method, const char* what, const char* anchor);
    // Checks arguments against parameters; `what` names the callee in messages.
    void checkArgs(std::vector<ExprPtr>& args, const FuncDecl& fn, Flow& flow, Loc loc, const std::string& what);
    // Overloading: the function a call means, of those with its name, and its arguments checked against it.
    FuncDecl* chooseOverload(const std::string& name, const std::string& calledAs, const StructDecl* owner, const std::vector<FuncDecl*>& candidates, const std::vector<Expr*>& args, Flow& flow, Loc loc);
    void checkChosenArgs(const std::vector<Expr*>& args, const FuncDecl& fn, Flow& flow);
    void checkOverloadPair(const FuncDecl& first, const FuncDecl& second, bool& sameParams);
    // One value given when building a struct or class: `Type(args)` or a `{ }` list.
    struct ConstructArg {
        std::string designator;  // field name for `.x = value`, empty for positional
        Loc loc;
        Expr* value;
        int* fieldIndex;  // where to record the field it fills, or null
        bool checked = false;  // its type is already known
    };
    // `asBase` when a constructor is building its base class, not a new object.
    // Checks the values that build a `sd`, and gives the constructor that takes them, if it has constructors.
    const FuncDecl* checkConstruction(const StructDecl& sd, std::vector<ConstructArg> args, Flow& flow, Loc loc, bool asBase = false);
    // Whether the code being checked can use a member `sd` declares, reached through a value
    // of type `through`. As in C++, a deriving class sees protected members of its own objects.
    bool canAccess(const StructDecl& sd, Access access, const StructDecl* through) const;
    void reportAccess(const StructDecl& sd, const std::string& member, bool isMethod, Access access, Loc loc);
    const Type* checkPrint(CallExpr& call, Flow& flow);
    void checkCondition(Expr& e, Flow& flow, const char* what);
    void checkShiftAmount(const Type* lhsType, const Expr* lhs, const Expr& amount);
    void requireConvertible(const Expr& e, const Type* to, const std::string& context);
    // Resolves a variable or field access. When the expression is rooted at a variable,
    // `var` and `path` name the place; otherwise var is null (e.g. `makePoint().x`).
    const Type* resolvePlace(Expr& e, Flow& flow, const VarDecl*& var, std::string& path);
    bool checkAssignable(Expr& target, Flow& flow, const char* action, const VarDecl*& var, std::string& path, const std::string& change = " = ...", const std::string& before = "");
    // Reports an error when the resolved place `target` cannot be changed: it is const,
    // a field of `this` in a const method, or part of a temporary value.
    bool checkChangeable(const Expr& target, const char* action, const VarDecl* var, const std::string& path, const std::string& change = " = ...", const std::string& before = "");
    const VarDecl* rootVariable(const Expr& root);
    const Expr* boundByRef_ = nullptr;  // the argument being given to a reference parameter, which is not copied
    void checkRefBinding(const Expr& e, const Type* type, const std::string& what);
    // The local variable or parameter of this function that the pointer `e` may point into, or null.
    const VarDecl* localPointee(const Expr& e, const Flow& flow);
    // Keeps track of what the pointer variable `var` holds once it is given `value`.
    void notePointer(Flow& flow, const VarDecl* var, const Expr& value);
    void reportUnknownName(const NameExpr& n);
    // A program of more than one file: see Modules.h.
    bool hiddenFrom(const std::string& name, Loc declared, Loc used) const;  // the main file's own, used in another file
    std::string elsewhere(const std::string& name, Loc used) const;  // where a name not found here is, as a help, or ""
    bool reportElsewhere(const std::string& name, Loc used, const char* what);  // says so, if it is
    std::string resolved(const std::string& name, Loc used) const;

    VarDecl* lookup(const std::string& name);
    // The variable of each `for (auto x : list)`, which is a copy of the element
    struct LoopCopy {
        std::string list;          // as written, or empty when it is not a place
        bool isMap = false;        // its entries, whose keys say where each one is
        bool overACopy = false;    // the list is itself the variable of another loop
        std::string constList;     // the const variable the list is in
    };
    std::map<const VarDecl*, LoopCopy> loopCopies_;
    void declare(VarDecl& v);
    std::string suggestName(const std::string& name);

    Diagnostics& diags_;
    Parser& parser_;
    bool warnNewDelete_;
    Program* program_ = nullptr;
    size_t typesAdopted_ = 0, structsChecked_ = 0;
    std::set<const StructDecl*> basesWalked_, overridesDone_;
    std::unordered_map<const StructDecl*, int> structState_;  // 1 = being visited, 2 = done
    std::deque<FuncDecl*> pendingMethods_;
    std::vector<std::pair<const VarDecl*, Loc>> moveLog_;  // every `move(x)` of a variable so far
    int tryBlocks_ = 0;     // the `try { }` blocks around the code being checked
    int tryPrefixes_ = 0;   // the same for `try f()`
    int failPoints_ = 0;    // how many places that can fail have been seen, to tell if a try block has any
    std::vector<const CatchClause*> catches_;  // the catches around the code being checked
    std::map<std::pair<std::string, const Type*>, std::pair<std::string, Loc>> constraintResults_;
    std::unordered_map<std::string, FuncDecl*> funcs_;  // by name: the first, if there are more (see overloads_)
    std::unordered_map<std::string, std::vector<FuncDecl*>> overloads_;  // the functions of a name that has more than one
    std::unordered_map<std::string, StructDecl*> structs_;
    std::vector<std::unordered_map<std::string, VarDecl*>> scopes_;
    std::vector<LoopContext> loops_;
    FuncDecl* fn_ = nullptr;
    const FieldDecl* initField_ = nullptr;  // the field a constructor's initializer list is setting
    const StructDecl* buildingBase_ = nullptr;  // the base class a constructor's initializer list is building
    const StructDecl* defaultsOf_ = nullptr;  // the type whose field defaults are being checked
    StructDecl* owner() const { return fn_ ? fn_->owner : nullptr; }
    VarDecl* thisVar() const { return fn_ ? fn_->thisDecl.get() : nullptr; }
};

}  // namespace cplus
