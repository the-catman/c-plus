// Semantic analysis: resolves names, assigns types, and enforces the rules that make
// C+ safer than C++: definite assignment (down to struct fields), no implicit narrowing,
// every path of a non-void function returns, and checked print format strings.
#pragma once

#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "AST.h"

namespace cplus {

class Sema {
public:
    // `warnNewDelete` is false with -Wno-new-delete.
    explicit Sema(Diagnostics& diags, bool warnNewDelete = true) : diags_(diags), warnNewDelete_(warnNewDelete) {}
    void check(Program& program);

private:
    // A place that holds a value: a variable (empty path) or a field inside one ("pos.x").
    using Place = std::pair<const VarDecl*, std::string>;
    // What is known at a point in the program: which places definitely hold a value,
    // and whether the point can be reached at all.
    struct Flow {
        std::set<Place> assigned;
        bool reachable = true;
    };
    struct LoopContext {
        std::vector<Flow> breaks;     // state at each `break`
        std::vector<Flow> continues;  // state at each `continue`
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

    void checkStructs(Program& program);
    void checkConstants(Program& program);
    void checkConstant(VarDecl& constant);
    std::set<const VarDecl*> constantsChecked_, constantsBeingChecked_;
    void checkMembers(StructDecl& sd);
    void checkFunction(FuncDecl& fn);
    void checkExternFunction(FuncDecl& fn);
    void checkStmt(Stmt& s, Flow& flow);
    void checkVarDecl(VarDecl& v, Flow& flow);
    void checkLoopBody(Stmt& body, Flow& flow);
    void checkForEach(ForEachStmt& st, Flow& flow);
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
    const Type* checkOptChain(OptChainExpr& c, Flow& flow);
    const Type* checkCoalesce(CoalesceExpr& c, Flow& flow);
    void checkCondDecl(CondDecl& d, Flow& flow, const char* stmt);
    void checkChangingReceiver(const Expr& recv, const char* method, const char* what, const char* anchor);
    // Checks arguments against parameters; `what` names the callee in messages.
    void checkArgs(std::vector<ExprPtr>& args, const FuncDecl& fn, Flow& flow, Loc loc, const std::string& what);
    // One value given when building a struct or class: `Type(args)` or a `{ }` list.
    struct ConstructArg {
        std::string designator;  // field name for `.x = value`, empty for positional
        Loc loc;
        Expr* value;
        int* fieldIndex;  // where to record the field it fills, or null
    };
    void checkConstruction(const StructDecl& sd, std::vector<ConstructArg> args, Flow& flow, Loc loc);
    bool canAccess(const StructDecl& sd, Access access) const;
    void reportAccess(const StructDecl& sd, const std::string& member, bool isMethod, Loc loc);
    const Type* checkPrint(CallExpr& call, Flow& flow);
    void checkCondition(Expr& e, Flow& flow, const char* what);
    void checkShiftAmount(const Type* lhsType, const Expr* lhs, const Expr& amount);
    void requireConvertible(const Expr& e, const Type* to, const std::string& context);
    // Resolves a variable or field access. When the expression is rooted at a variable,
    // `var` and `path` name the place; otherwise var is null (e.g. `makePoint().x`).
    const Type* resolvePlace(Expr& e, Flow& flow, const VarDecl*& var, std::string& path);
    bool checkAssignable(Expr& target, Flow& flow, const char* action, const VarDecl*& var, std::string& path);
    // Reports an error when the resolved place `target` cannot be changed: it is const,
    // a field of `this` in a const method, or part of a temporary value.
    bool checkChangeable(const Expr& target, const char* action, const VarDecl* var, const std::string& path);
    const VarDecl* rootVariable(const Expr& root);
    void reportUnknownName(const NameExpr& n);

    VarDecl* lookup(const std::string& name);
    void declare(VarDecl& v);
    std::string suggestName(const std::string& name);

    Diagnostics& diags_;
    bool warnNewDelete_;
    const Program* program_ = nullptr;
    std::unordered_map<std::string, FuncDecl*> funcs_;
    std::unordered_map<std::string, StructDecl*> structs_;
    std::vector<std::unordered_map<std::string, VarDecl*>> scopes_;
    std::vector<LoopContext> loops_;
    FuncDecl* fn_ = nullptr;
    const FieldDecl* initField_ = nullptr;  // the field a constructor's initializer list is setting
    const StructDecl* defaultsOf_ = nullptr;  // the type whose field defaults are being checked
    StructDecl* owner() const { return fn_ ? fn_->owner : nullptr; }
    VarDecl* thisVar() const { return fn_ ? fn_->thisDecl.get() : nullptr; }
};

}  // namespace cplus
