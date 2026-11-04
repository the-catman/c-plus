// The syntax tree. The parser builds it; Sema fills in types and resolves names;
// Codegen lowers it to LLVM IR.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Diagnostics.h"

namespace cplus {

struct StructDecl;

struct Type {
    enum class Kind { Error, Void, Bool, Int, Long, String, Auto, Struct, Class };
    Kind kind;
    const char* name;
    const StructDecl* decl = nullptr;  // for Kind::Struct and Kind::Class

    bool is(Kind k) const { return kind == k; }
    bool isError() const { return kind == Kind::Error; }
    bool isInteger() const { return kind == Kind::Int || kind == Kind::Long; }
    bool isStruct() const { return kind == Kind::Struct; }  // a value type
    bool isClass() const { return kind == Kind::Class; }    // a reference-counted object
    bool hasFields() const { return kind == Kind::Struct || kind == Kind::Class; }
    unsigned bits() const { return kind == Kind::Long ? 64 : kind == Kind::Int ? 32 : 0; }

    static const Type* get(Kind k) {
        static const Type types[] = {{Kind::Error, "<error>"}, {Kind::Void, "void"}, {Kind::Bool, "bool"},
                                     {Kind::Int, "int"},       {Kind::Long, "long"}, {Kind::String, "string"},
                                     {Kind::Auto, "auto"}};
        return &types[(int)k];
    }
    static const Type* error() { return get(Kind::Error); }
    static const Type* voidTy() { return get(Kind::Void); }
    static const Type* boolTy() { return get(Kind::Bool); }
    static const Type* intTy() { return get(Kind::Int); }
    static const Type* longTy() { return get(Kind::Long); }
    static const Type* stringTy() { return get(Kind::String); }
    static const Type* autoTy() { return get(Kind::Auto); }
};

struct VarDecl;
struct FuncDecl;

// ---- Expressions -----------------------------------------------------------

struct Expr {
    enum class Kind { IntLit, BoolLit, StringLit, Name, This, Unary, Binary, Assign, Ternary, IncDec, Call, MethodCall,
                      Member, InitList };
    const Kind kind;
    Loc loc;
    const Type* type = nullptr;  // set by Sema

    Expr(Kind k, Loc l) : kind(k), loc(l) {}
    virtual ~Expr() = default;
};
using ExprPtr = std::unique_ptr<Expr>;

struct IntLitExpr : Expr {
    int64_t value;
    bool isLong;  // written with an L suffix
    IntLitExpr(Loc l, int64_t v, bool longSuffix) : Expr(Kind::IntLit, l), value(v), isLong(longSuffix) {}
};

struct BoolLitExpr : Expr {
    bool value;
    BoolLitExpr(Loc l, bool v) : Expr(Kind::BoolLit, l), value(v) {}
};

struct StringLitExpr : Expr {
    std::string value;
    StringLitExpr(Loc l, std::string v) : Expr(Kind::StringLit, l), value(std::move(v)) {}
};

struct NameExpr : Expr {
    std::string name;
    VarDecl* decl = nullptr;  // set by Sema for a variable
    int implicitField = -1;   // set by Sema when the name is a field of `this`, inside a method
    NameExpr(Loc l, std::string n) : Expr(Kind::Name, l), name(std::move(n)) {}
};

struct ThisExpr : Expr {
    explicit ThisExpr(Loc l) : Expr(Kind::This, l) {}
};

enum class UnOp { Neg, Plus, Not, BitNot };

struct UnaryExpr : Expr {
    UnOp op;
    ExprPtr operand;
    UnaryExpr(Loc l, UnOp o, ExprPtr e) : Expr(Kind::Unary, l), op(o), operand(std::move(e)) {}
};

enum class BinOp { Add, Sub, Mul, Div, Rem, Shl, Shr, BitAnd, BitOr, BitXor, Eq, Ne, Lt, Le, Gt, Ge, LogAnd, LogOr };
const char* binOpSpelling(BinOp op);

struct BinaryExpr : Expr {
    BinOp op;
    ExprPtr lhs, rhs;
    const Type* operandType = nullptr;  // type both sides are converted to (lhs type for shifts)
    BinaryExpr(Loc opLoc, BinOp o, ExprPtr l, ExprPtr r)
        : Expr(Kind::Binary, opLoc), op(o), lhs(std::move(l)), rhs(std::move(r)) {}
};

// `x = v` or a compound assignment such as `x += v` (op holds the arithmetic).
struct AssignExpr : Expr {
    std::optional<BinOp> op;
    ExprPtr target, value;
    AssignExpr(Loc opLoc, std::optional<BinOp> o, ExprPtr t, ExprPtr v)
        : Expr(Kind::Assign, opLoc), op(o), target(std::move(t)), value(std::move(v)) {}
};

struct TernaryExpr : Expr {
    ExprPtr cond, thenExpr, elseExpr;
    TernaryExpr(Loc l, ExprPtr c, ExprPtr t, ExprPtr e)
        : Expr(Kind::Ternary, l), cond(std::move(c)), thenExpr(std::move(t)), elseExpr(std::move(e)) {}
};

struct IncDecExpr : Expr {
    bool increment;
    bool prefix;
    ExprPtr target;
    IncDecExpr(Loc l, bool inc, bool pre, ExprPtr t)
        : Expr(Kind::IncDec, l), increment(inc), prefix(pre), target(std::move(t)) {}
};

enum class Builtin { None, Print, Println };

// `f(args)`, a built-in like println, or `Point(1, 2)`, which builds a struct.
struct CallExpr : Expr {
    std::string callee;
    std::vector<ExprPtr> args;
    FuncDecl* target = nullptr;              // set by Sema for user functions
    FuncDecl* method = nullptr;              // set by Sema for `name(...)` meaning `this.name(...)`
    const StructDecl* constructs = nullptr;  // set by Sema for `TypeName(...)`
    Builtin builtin = Builtin::None;       // set by Sema for built-ins
    std::vector<std::string> formatPieces;  // print/println: literal text around each {} (args.size() pieces)
    CallExpr(Loc l, std::string c, std::vector<ExprPtr> a)
        : Expr(Kind::Call, l), callee(std::move(c)), args(std::move(a)) {}
};

// `receiver.name(args)`
struct MethodCallExpr : Expr {
    ExprPtr receiver;
    std::string name;
    std::vector<ExprPtr> args;
    FuncDecl* method = nullptr;  // set by Sema
    MethodCallExpr(Loc nameLoc, ExprPtr r, std::string n, std::vector<ExprPtr> a)
        : Expr(Kind::MethodCall, nameLoc), receiver(std::move(r)), name(std::move(n)), args(std::move(a)) {}
};

// `base.field`
struct MemberExpr : Expr {
    ExprPtr base;
    std::string field;
    int fieldIndex = -1;  // set by Sema
    MemberExpr(Loc fieldLoc, ExprPtr b, std::string f) : Expr(Kind::Member, fieldLoc), base(std::move(b)), field(std::move(f)) {}
};

// `{1, 2}`, `{.x = 1, .y = 2}`, or `Point{1, 2}`. Without a type name, the list takes its
// type from where it is used, e.g. the declared type of the variable it initializes.
struct InitListExpr : Expr {
    struct Element {
        std::string designator;  // field name for `.x = value`, empty for positional
        Loc loc;
        ExprPtr value;
        int fieldIndex = -1;  // set by Sema
    };
    const Type* explicitType = nullptr;  // `Point{...}`
    std::vector<Element> elements;
    explicit InitListExpr(Loc l) : Expr(Kind::InitList, l) {}
};

// ---- Statements ------------------------------------------------------------

struct Stmt {
    enum class Kind { VarDecl, DeclGroup, Expr, Block, If, While, DoWhile, For, Return, Break, Continue, Error };
    const Kind kind;
    Loc loc;
    Stmt(Kind k, Loc l) : kind(k), loc(l) {}
    virtual ~Stmt() = default;
};
using StmtPtr = std::unique_ptr<Stmt>;

struct VarDecl : Stmt {
    std::string name;
    const Type* declType;  // as written; may be auto
    Loc typeLoc;
    bool isConst = false;
    bool isParam = false;
    ExprPtr init;
    const Type* type = nullptr;  // resolved by Sema
    VarDecl(Loc nameLoc, std::string n, const Type* t, Loc tl)
        : Stmt(Kind::VarDecl, nameLoc), name(std::move(n)), declType(t), typeLoc(tl) {}
};

// `int a = 1, b = 2;` declares several variables in the current scope.
struct DeclGroupStmt : Stmt {
    std::vector<std::unique_ptr<VarDecl>> decls;
    explicit DeclGroupStmt(Loc l) : Stmt(Kind::DeclGroup, l) {}
};

struct ExprStmt : Stmt {
    ExprPtr expr;
    ExprStmt(Loc l, ExprPtr e) : Stmt(Kind::Expr, l), expr(std::move(e)) {}
};

struct BlockStmt : Stmt {
    std::vector<StmtPtr> stmts;
    Loc endLoc;
    explicit BlockStmt(Loc l) : Stmt(Kind::Block, l) {}
};

// Stands where a statement failed to parse, so later checks know something is missing.
struct ErrorStmt : Stmt {
    explicit ErrorStmt(Loc l) : Stmt(Kind::Error, l) {}
};

struct IfStmt : Stmt {
    ExprPtr cond;
    StmtPtr thenStmt, elseStmt;
    IfStmt(Loc l, ExprPtr c, StmtPtr t, StmtPtr e)
        : Stmt(Kind::If, l), cond(std::move(c)), thenStmt(std::move(t)), elseStmt(std::move(e)) {}
};

struct WhileStmt : Stmt {
    ExprPtr cond;
    StmtPtr body;
    WhileStmt(Loc l, ExprPtr c, StmtPtr b) : Stmt(Kind::While, l), cond(std::move(c)), body(std::move(b)) {}
};

struct DoWhileStmt : Stmt {
    StmtPtr body;
    ExprPtr cond;
    DoWhileStmt(Loc l, StmtPtr b, ExprPtr c) : Stmt(Kind::DoWhile, l), body(std::move(b)), cond(std::move(c)) {}
};

struct ForStmt : Stmt {
    StmtPtr init;                // VarDecl, DeclGroup, ExprStmt, or null
    ExprPtr cond;                // null means "forever"
    std::vector<ExprPtr> steps;  // `i++, j--` runs each in order
    StmtPtr body;
    ForStmt(Loc l) : Stmt(Kind::For, l) {}
};

struct ReturnStmt : Stmt {
    ExprPtr value;
    ReturnStmt(Loc l, ExprPtr v) : Stmt(Kind::Return, l), value(std::move(v)) {}
};

struct BreakStmt : Stmt {
    explicit BreakStmt(Loc l) : Stmt(Kind::Break, l) {}
};

struct ContinueStmt : Stmt {
    explicit ContinueStmt(Loc l) : Stmt(Kind::Continue, l) {}
};

// ---- Declarations ----------------------------------------------------------

enum class Access { Public, Private, Protected };

struct FuncDecl {
    enum class Role { Free, Method, Constructor, Destructor };
    struct MemberInit {  // `: name(value)` in a constructor
        std::string field;
        Loc loc;
        ExprPtr value;
        int fieldIndex = -1;  // set by Sema
    };

    std::string name;
    Loc nameLoc;
    const Type* retType;
    Loc retTypeLoc;
    std::vector<std::unique_ptr<VarDecl>> params;
    std::unique_ptr<BlockStmt> body;

    Role role = Role::Free;
    StructDecl* owner = nullptr;  // the struct or class a method belongs to
    bool isConst = false;         // `int area() const`
    Access access = Access::Public;
    std::vector<MemberInit> memberInits;
    std::unique_ptr<VarDecl> thisDecl;  // stands for `this` in definite assignment; set by Sema
};


struct FieldDecl {
    std::string name;
    Loc loc;
    const Type* type;
    Loc typeLoc;
    ExprPtr defaultValue;  // `int x = 0;` inside the struct; may be null
    Access access = Access::Public;
};

// A struct (value type) or a class (reference-counted object). They share fields,
// methods, constructors and access control.
struct StructDecl {
    std::string name;
    Loc nameLoc;
    bool isClass;
    bool defined = false;  // false if only referenced, or its definition failed to parse
    std::vector<FieldDecl> fields;
    std::vector<std::unique_ptr<FuncDecl>> methods;
    FuncDecl* constructor = nullptr;
    FuncDecl* destructor = nullptr;
    Type type;  // the type this declaration defines

    StructDecl(std::string n, bool cls)
        : name(std::move(n)), isClass(cls), type{cls ? Type::Kind::Class : Type::Kind::Struct, nullptr, this} {
        type.name = name.c_str();
    }
    const char* keyword() const { return isClass ? "class" : "struct"; }
    FuncDecl* findMethod(const std::string& methodName) const {
        for (auto& m : methods)
            if (m->role == FuncDecl::Role::Method && m->name == methodName) return m.get();
        return nullptr;
    }
    StructDecl(const StructDecl&) = delete;
    StructDecl& operator=(const StructDecl&) = delete;

    int fieldIndex(const std::string& fieldName) const {
        for (size_t i = 0; i < fields.size(); ++i)
            if (fields[i].name == fieldName) return (int)i;
        return -1;
    }
};

struct ImportDecl {
    std::string path;  // dotted, e.g. "net.server"
    Loc loc;
};

struct Program {
    std::vector<ImportDecl> imports;
    std::vector<std::unique_ptr<StructDecl>> structs;
    std::vector<std::unique_ptr<FuncDecl>> funcs;

    StructDecl* findStruct(const std::string& name) const {
        for (auto& s : structs)
            if (s->name == name) return s.get();
        return nullptr;
    }
};

inline const char* binOpSpelling(BinOp op) {
    switch (op) {
        case BinOp::Add: return "+";
        case BinOp::Sub: return "-";
        case BinOp::Mul: return "*";
        case BinOp::Div: return "/";
        case BinOp::Rem: return "%";
        case BinOp::Shl: return "<<";
        case BinOp::Shr: return ">>";
        case BinOp::BitAnd: return "&";
        case BinOp::BitOr: return "|";
        case BinOp::BitXor: return "^";
        case BinOp::Eq: return "==";
        case BinOp::Ne: return "!=";
        case BinOp::Lt: return "<";
        case BinOp::Le: return "<=";
        case BinOp::Gt: return ">";
        case BinOp::Ge: return ">=";
        case BinOp::LogAnd: return "&&";
        case BinOp::LogOr: return "||";
    }
    return "?";
}

}  // namespace cplus
