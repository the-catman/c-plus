// Works out values while compiling, for `constexpr`, array sizes and template arguments. It reads
// the syntax tree and needs no types from Sema, since an array's size is part of a type, which the
// parser has to know while Sema has not run yet. The rules are those of the running program,
// so what stops a program, such as overflow, is an error here.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "AST.h"

namespace cplus {

struct ConstValue {
    enum class Kind { Int, Long, Float, Double, Bool, Char, String };
    Kind kind = Kind::Int;
    int64_t i = 0;  // for Int, Long, Bool and Char
    double d = 0;   // for Float and Double
    std::string s;  // for String
    const char* typeName() const;
};

// Why a value cannot be worked out. `loc` is where, which may be deep in a function.
struct ConstError {
    Loc loc;
    std::string message;
};

// What the evaluator asks of whoever is using it, the parser or Sema.
struct ConstHooks {
    std::function<const VarDecl*(const std::string&)> constant;   // a variable by name, or null
    std::function<const FuncDecl*(const std::string&)> function;  // a function by name, or null
    std::function<bool(const ConstraintExpr&)> constraint;        // `Number<T>`; may throw a ConstError
    std::function<int64_t(const Type*, Loc)> sizeOf;              // may throw a ConstError
    std::function<void(const FuncDecl&)> prepare;                 // before a function runs, if there is anything to do
};

// `Comparable`, `Number`, `Integer`, `Floating`, `same_as` and `derived_from`, when `name` is one of them.
std::optional<bool> builtinConstraint(const std::string& name, const Type* first, const Type* second);
bool isBuiltinConstraintName(const std::string& name);

class ConstEval {
public:
    explicit ConstEval(ConstHooks hooks) : hooks_(std::move(hooks)) {}
    // Throws a ConstError when it cannot.
    ConstValue eval(const Expr& e);
    // The value as a `to`, as a cast would convert it.
    ConstValue convert(const ConstValue& v, const Type* to, Loc loc);
    bool isTrue(const ConstValue& v, Loc loc);

private:
    struct Local {
        const Type* type;
        ConstValue value;
    };
    using Scope = std::unordered_map<std::string, Local>;
    enum class Flow { Normal, Break, Continue, Return };

    Local* findLocal(const std::string& name);
    void tick(Loc loc);
    ConstValue evalName(const NameExpr& n);
    ConstValue evalUnary(const UnaryExpr& u);
    ConstValue evalBinary(const BinaryExpr& b);
    ConstValue arith(BinOp op, const ConstValue& l, const ConstValue& r, Loc loc);
    ConstValue evalAssign(const AssignExpr& a);
    ConstValue evalIncDec(const IncDecExpr& i);
    ConstValue evalCall(const CallExpr& c);
    ConstValue evalMethodCall(const MethodCallExpr& c);
    ConstValue callFunction(const FuncDecl& f, std::vector<ConstValue> args, Loc loc);
    Flow exec(const Stmt& s);
    Flow execVar(const VarDecl& v);

    ConstHooks hooks_;
    std::vector<std::vector<Scope>> frames_;  // the scopes of each function being run
    ConstValue returned_;
    int64_t steps_ = 0;
    std::vector<const VarDecl*> evaluating_;  // constants being worked out, to find one that needs itself
};

}  // namespace cplus
