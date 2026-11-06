// The syntax tree. The parser builds it; Sema fills in types and resolves names;
// Codegen lowers it to LLVM IR.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Diagnostics.h"

namespace cplus {

struct StructDecl;
struct TemplatePattern;

struct Type {
    enum class Kind { Error, Void, Bool, Int, Long, Float, Double, Char, String, Auto, Struct, Class, Vector, Optional, Null,
                      Pointer, Opaque, Array, Weak,
                      // Only while a generic definition is read, to learn its shape; never in checked code.
                      TypeParam, Dependent };
    Kind kind;
    const char* name;
    const StructDecl* decl = nullptr;  // for Kind::Struct, Kind::Class and Kind::Opaque
    const Type* elem = nullptr;        // for Kind::Vector and Kind::Array, the value type of Kind::Optional, what a Kind::Pointer points to, the class of Kind::Weak
    bool constElem = false;            // for Kind::Pointer: `const int*`, which only reads what it points to
    int64_t length = 0;                // for Kind::Array: the N of `array<T, N>`; for Kind::TypeParam: which parameter it is
    const TemplatePattern* pattern = nullptr;  // for Kind::Dependent: `Box<T>`

    bool is(Kind k) const { return kind == k; }
    bool isVector() const { return kind == Kind::Vector; }
    bool isArray() const { return kind == Kind::Array; }
    bool isOptional() const { return kind == Kind::Optional; }
    bool isNull() const { return kind == Kind::Null; }  // the type of `null`, which converts to any optional or pointer
    bool isPointer() const { return kind == Kind::Pointer; }
    bool isWeak() const { return kind == Kind::Weak; }  // `weak<Node>`: a reference that does not keep the object alive
    // A C type known only by name, `extern "C" struct sqlite3;`, which C+ only points to.
    bool isOpaque() const { return kind == Kind::Opaque; }
    bool isError() const { return kind == Kind::Error; }
    bool isInteger() const { return kind == Kind::Int || kind == Kind::Long; }
    bool isFloating() const { return kind == Kind::Float || kind == Kind::Double; }
    bool isNumber() const { return isInteger() || isFloating(); }
    // A number, or a char, which arithmetic turns into an int first, as in C++.
    bool isArithmetic() const { return isNumber() || kind == Kind::Char; }
    bool isStruct() const { return kind == Kind::Struct; }  // a value type
    bool isClass() const { return kind == Kind::Class; }    // a reference-counted object
    bool hasFields() const { return kind == Kind::Struct || kind == Kind::Class; }
    unsigned bits() const { return kind == Kind::Long ? 64 : kind == Kind::Int ? 32 : 0; }

    static const Type* get(Kind k) {
        static const Type types[] = {{Kind::Error, "<error>"}, {Kind::Void, "void"},     {Kind::Bool, "bool"},
                                     {Kind::Int, "int"},       {Kind::Long, "long"},     {Kind::Float, "float"},
                                     {Kind::Double, "double"}, {Kind::Char, "char"},     {Kind::String, "string"},
                                     {Kind::Auto, "auto"}};
        return &types[(int)k];
    }
    static const Type* error() { return get(Kind::Error); }
    static const Type* voidTy() { return get(Kind::Void); }
    static const Type* boolTy() { return get(Kind::Bool); }
    static const Type* intTy() { return get(Kind::Int); }
    static const Type* longTy() { return get(Kind::Long); }
    static const Type* floatTy() { return get(Kind::Float); }
    static const Type* doubleTy() { return get(Kind::Double); }
    static const Type* charTy() { return get(Kind::Char); }
    static const Type* stringTy() { return get(Kind::String); }
    static const Type* autoTy() { return get(Kind::Auto); }
    static const Type* nullTy() {
        static const Type t{Kind::Null, "null"};
        return &t;
    }

    // `vector<elem>`. There is one Type per element type, so types compare by address.
    static const Type* vectorOf(const Type* elem) {
        static std::map<const Type*, std::pair<std::string, Type>> made;
        auto [it, inserted] = made.try_emplace(elem);
        if (inserted) {
            it->second.first = "vector<" + std::string(elem->name) + ">";
            it->second.second = Type{Kind::Vector, it->second.first.c_str(), nullptr, elem};
        }
        return &it->second.second;
    }

    // `array<elem, length>`. One Type per element type and length, like vectorOf.
    static const Type* arrayOf(const Type* elem, int64_t length) {
        static std::map<std::pair<const Type*, int64_t>, std::pair<std::string, Type>> made;
        auto [it, inserted] = made.try_emplace({elem, length});
        if (inserted) {
            // A negative length only comes from reading a generic definition, whose parameter gives it.
            it->second.first = "array<" + std::string(elem->name) + ", " + (length < 0 ? "..." : std::to_string(length)) + ">";
            it->second.second = Type{Kind::Array, it->second.first.c_str(), nullptr, elem, false, length};
        }
        return &it->second.second;
    }

    // `elem?`: an `elem` or null. One Type per value type, like vectorOf.
    static const Type* optionalOf(const Type* elem) {
        static std::map<const Type*, std::pair<std::string, Type>> made;
        auto [it, inserted] = made.try_emplace(elem);
        if (inserted) {
            it->second.first = std::string(elem->name) + "?";
            it->second.second = Type{Kind::Optional, it->second.first.c_str(), nullptr, elem};
        }
        return &it->second.second;
    }

    // `weak<elem>`, where elem is a class. One Type per class, like vectorOf.
    static const Type* weakOf(const Type* elem) {
        static std::map<const Type*, std::pair<std::string, Type>> made;
        auto [it, inserted] = made.try_emplace(elem);
        if (inserted) {
            it->second.first = "weak<" + std::string(elem->name) + ">";
            it->second.second = Type{Kind::Weak, it->second.first.c_str(), nullptr, elem};
        }
        return &it->second.second;
    }

    // `elem*`, or `const elem*` with `constElem`: a raw pointer, which may be null. One
    // Type per pointee and constness, like vectorOf.
    static const Type* pointerTo(const Type* elem, bool constElem = false) {
        static std::map<std::pair<const Type*, bool>, std::pair<std::string, Type>> made;
        auto [it, inserted] = made.try_emplace({elem, constElem});
        if (inserted) {
            // `const int*`, but `int* const*` for a pointer to a const pointer, as in C++.
            it->second.first = !constElem          ? std::string(elem->name) + "*"
                               : elem->isPointer() ? std::string(elem->name) + " const*"
                                                   : "const " + std::string(elem->name) + "*";
            it->second.second = Type{Kind::Pointer, it->second.first.c_str(), nullptr, elem, constElem};
        }
        return &it->second.second;
    }
};

struct VarDecl;
struct FuncDecl;
struct TemplateDecl;

// What is given for a parameter of a generic: a type, or a value such as the 16 of `array<T, 16>`.
struct TemplateArg {
    const Type* type = nullptr;  // null for a value
    int64_t value = 0;
    int valueParam = -1;  // while a generic definition is read: the parameter that gives the value
};

// `Box<T>`, as it is written in a generic definition, before T is known.
struct TemplatePattern {
    TemplateDecl* tmpl;
    std::vector<TemplateArg> args;
    std::string name;
    Type type;
};

// ---- Expressions -----------------------------------------------------------

struct Expr {
    enum class Kind { IntLit, FloatLit, CharLit, BoolLit, StringLit, NullLit, Name, This, Unary, Binary, Assign, Ternary, IncDec,
                      Call, MethodCall, Member, Index, InitList, OptChain, OptValue, Coalesce, Cast, Sizeof, New,
                      Delete };
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

struct FloatLitExpr : Expr {
    double value;
    bool isFloat;  // written with an f suffix
    FloatLitExpr(Loc l, double v, bool f) : Expr(Kind::FloatLit, l), value(v), isFloat(f) {}
};

// `'a'`: one byte, from 0 to 255.
struct CharLitExpr : Expr {
    uint8_t value;
    CharLitExpr(Loc l, uint8_t v) : Expr(Kind::CharLit, l), value(v) {}
};

// `int(x)`, `static_cast<int>(x)` or `(int)x`: an explicit conversion between numbers,
// char and bool, between pointer types, or from a class to its base class.
struct CastExpr : Expr {
    const Type* target;
    ExprPtr operand;           // null for `T()`, the default value of a type parameter
    bool reinterpret = false;  // written `reinterpret_cast`, the one cast between numbers and pointers
    bool dynamic = false;      // written `dynamic_cast`, which looks at what the object really is
    CastExpr(Loc l, const Type* t, ExprPtr e) : Expr(Kind::Cast, l), target(t), operand(std::move(e)) {}
};

struct NullLitExpr : Expr {
    explicit NullLitExpr(Loc l) : Expr(Kind::NullLit, l) {}
};

struct BoolLitExpr : Expr {
    bool value;
    BoolLitExpr(Loc l, bool v) : Expr(Kind::BoolLit, l), value(v) {}
};

struct StringLitExpr : Expr {
    std::string value;
    StringLitExpr(Loc l, std::string v) : Expr(Kind::StringLit, l), value(std::move(v)) {}
};

// A class, then one of its base classes, then one of that one's, and so on: which part of an
// object is meant, since a diamond holds the same base class twice.
using BasePath = std::vector<const StructDecl*>;

struct NameExpr : Expr {
    std::string name;
    VarDecl* decl = nullptr;  // set by Sema for a variable
    int implicitField = -1;   // set by Sema when the name is a field of `this`, inside a method
    BasePath path;            // set with implicitField: from the class of `this` to the one declaring the field
    NameExpr(Loc l, std::string n) : Expr(Kind::Name, l), name(std::move(n)) {}
};

struct ThisExpr : Expr {
    explicit ThisExpr(Loc l) : Expr(Kind::This, l) {}
};

// `*p` is Deref and `&x` AddrOf. `p->x` is parsed as `(*p).x`.
enum class UnOp { Neg, Plus, Not, BitNot, Deref, AddrOf };

struct UnaryExpr : Expr {
    UnOp op;
    ExprPtr operand;
    std::string arrowMember;  // for a Deref written as `p->name`: the name after the arrow
    UnaryExpr(Loc l, UnOp o, ExprPtr e) : Expr(Kind::Unary, l), op(o), operand(std::move(e)) {}
    bool isArrow() const { return op == UnOp::Deref && !arrowMember.empty(); }
};

inline bool isDeref(const Expr& e) {
    return e.kind == Expr::Kind::Unary && static_cast<const UnaryExpr&>(e).op == UnOp::Deref;
}

// `sizeof(T)` or `sizeof x`: the bytes a value takes, as a long. The operand is not
// evaluated.
struct SizeofExpr : Expr {
    const Type* target;  // `sizeof(T)`, or null
    ExprPtr operand;     // `sizeof x`, or null
    SizeofExpr(Loc l, const Type* t, ExprPtr e) : Expr(Kind::Sizeof, l), target(t), operand(std::move(e)) {}
};

// `new T`, `new T(args)`, `new T{...}` or `new T[n]`: memory for one T, or for n of them,
// that stays until `delete` frees it. The result is a `T*`.
struct NewExpr : Expr {
    const Type* target;
    Loc typeLoc;
    ExprPtr count;  // `new T[n]`, or null for one value
    ExprPtr init;   // the value; null for the default value of T
    NewExpr(Loc l, const Type* t, Loc tl) : Expr(Kind::New, l), target(t), typeLoc(tl) {}
};

// `delete p` or `delete[] p`. Its type is void.
struct DeleteExpr : Expr {
    bool array;
    ExprPtr operand;
    DeleteExpr(Loc l, bool a, ExprPtr e) : Expr(Kind::Delete, l), array(a), operand(std::move(e)) {}
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

enum class Builtin { None, Print, Println, Format, ToString, Math, CharFn, StringFill };

// The math functions of C++'s <cmath>, and abs, min and max.
enum class MathFn {
    Sqrt, Cbrt, Pow, Exp, Exp2, Log, Log2, Log10, Sin, Cos, Tan, Asin, Acos, Atan, Atan2, Sinh, Cosh, Tanh,
    Floor, Ceil, Round, Trunc, Fabs, Fmod, Hypot, IsNan, IsInf, IsFinite, Abs, Min, Max
};
bool isMathFunction(const std::string& name);  // `sqrt`, `pow`, ...: defined in Sema.cpp

// The character tests and conversions of C++'s <cctype>, for one char.
enum class CharFn { IsAlpha, IsDigit, IsAlnum, IsSpace, IsUpper, IsLower, IsPunct, IsXDigit, ToUpper, ToLower };
bool isCharFunction(const std::string& name);  // `isdigit`, `toupper`, ...: defined in Sema.cpp

// The C function that reads a number from text the way C++'s `std::stoi`, `std::stod`
// and so on do, as a declaration to suggest: `int atoi(string text)`. Empty for other names.
inline std::string cNumberReader(const std::string& name) {
    if (name == "stoi" || name == "atoi") return "int atoi(string text)";
    if (name == "stol" || name == "stoll" || name == "atol" || name == "strtol") return "long atol(string text)";
    if (name == "stod" || name == "stof" || name == "atof" || name == "strtod") return "double atof(string text)";
    return "";
}

// The options of one placeholder, `{:.2f}`: a precision, and f, e or g. Both are for
// floating-point values only.
struct FormatSpec {
    char type = 0;       // 'f', 'e', 'g', or 0 for none
    int precision = -1;  // -1 for none
};

// `f(args)`, a built-in like println, or `Point(1, 2)`, which builds a struct.
struct CallExpr : Expr {
    std::string callee;
    std::vector<ExprPtr> args;
    FuncDecl* target = nullptr;              // set by Sema for user functions
    FuncDecl* method = nullptr;              // set by Sema for `name(...)` meaning `this.name(...)`
    BasePath path;                           // set with method: from the class of `this` to the one declaring it
    std::vector<TemplateArg> templateArgs;   // `max<long>(a, b)`
    const StructDecl* constructs = nullptr;  // set by Sema for `TypeName(...)`
    bool copies = false;                     // set by Sema for `Point(p)` where `p` is a Point
    Builtin builtin = Builtin::None;       // set by Sema for built-ins
    const Type* vectorType = nullptr;      // `vector<int>(n)` or `vector<int>(n, value)`
    std::vector<std::string> formatPieces;  // print, println, format, to_string: literal text around each {}
    std::vector<FormatSpec> formatSpecs;    // one for each {}
    MathFn math{};                          // for Builtin::Math
    CharFn charFn{};                        // for Builtin::CharFn
    CallExpr(Loc l, std::string c, std::vector<ExprPtr> a)
        : Expr(Kind::Call, l), callee(std::move(c)), args(std::move(a)) {}
};

// The methods of vectors and arrays. `Fill` and `Data` are only on arrays, and `PushBack`,
// `PopBack` and `Clear` only on vectors.
enum class VecMethod { None, Size, Empty, PushBack, PopBack, Front, Back, Clear, Fill, Data };
// The methods every string has. `length()` is `size()`.
enum class StrMethod { None, Size, Empty, Substr, Find, Contains, StartsWith, EndsWith, Clear, PushBack, PopBack, Front,
                       Back };
// The methods every optional has, as in std::optional.
enum class OptMethod { None, HasValue, Value, ValueOr };
// The methods of a weak reference, as in std::weak_ptr.
enum class WeakMethod { None, Lock, Expired, Reset };

// `receiver.name(args)`
struct MethodCallExpr : Expr {
    ExprPtr receiver;
    std::string name;
    std::vector<ExprPtr> args;
    FuncDecl* method = nullptr;               // set by Sema for a struct or class method
    VecMethod vecMethod = VecMethod::None;    // set by Sema for a vector or array method
    StrMethod strMethod = StrMethod::None;    // set by Sema for a string method
    OptMethod optMethod = OptMethod::None;    // set by Sema for an optional method
    WeakMethod weakMethod = WeakMethod::None;  // set by Sema for a method of a weak reference
    std::string qualifier;                    // `d.Animal::speak()`: that class's own version, without dispatch
    BasePath path;                            // set with method: from the receiver's type to the one declaring it
    MethodCallExpr(Loc nameLoc, ExprPtr r, std::string n, std::vector<ExprPtr> a)
        : Expr(Kind::MethodCall, nameLoc), receiver(std::move(r)), name(std::move(n)), args(std::move(a)) {}
};

// `base.field`
struct MemberExpr : Expr {
    ExprPtr base;
    std::string field;
    int fieldIndex = -1;  // set by Sema: the field's place in `fieldOwner`
    const StructDecl* fieldOwner = nullptr;  // set by Sema: the type that declares the field
    BasePath path;                           // set by Sema: from the type of `base` to `fieldOwner`
    std::string qualifier;                   // `d.Animal::name`
    MemberExpr(Loc fieldLoc, ExprPtr b, std::string f) : Expr(Kind::Member, fieldLoc), base(std::move(b)), field(std::move(f)) {}
};

// `base[index]`, an element of a vector or an array, or a char of a string.
struct IndexExpr : Expr {
    ExprPtr base, index;
    IndexExpr(Loc bracketLoc, ExprPtr b, ExprPtr i) : Expr(Kind::Index, bracketLoc), base(std::move(b)), index(std::move(i)) {}
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
    const Type* explicitType = nullptr;  // `Point{...}`, `vector<int>{...}` or `array<int, 3>{...}`
    std::vector<Element> elements;
    explicit InitListExpr(Loc l) : Expr(Kind::InitList, l) {}
};

struct OptValueExpr;

// `base?.rest`, where `rest` is the chain of fields, calls and indexes after `?.`, as in
// JavaScript: when `base` is null the whole chain is null and `rest` is not evaluated.
// Inside `body`, an OptValueExpr stands for the value in `base`.
struct OptChainExpr : Expr {
    ExprPtr base;
    ExprPtr body;
    OptValueExpr* value = nullptr;  // the placeholder in `body`
    OptChainExpr(Loc questionDotLoc, ExprPtr b) : Expr(Kind::OptChain, questionDotLoc), base(std::move(b)) {}
};

// The value inside the optional of an OptChainExpr, known to be there.
struct OptValueExpr : Expr {
    OptChainExpr* chain;
    OptValueExpr(Loc l, OptChainExpr* c) : Expr(Kind::OptValue, l), chain(c) {}
};

// `opt ?? fallback`: the value in `opt`, or `fallback` when it is null. `fallback` is only
// evaluated when needed.
struct CoalesceExpr : Expr {
    ExprPtr lhs, rhs;
    CoalesceExpr(Loc l, ExprPtr a, ExprPtr b) : Expr(Kind::Coalesce, l), lhs(std::move(a)), rhs(std::move(b)) {}
};

// ---- Statements ------------------------------------------------------------

struct Stmt {
    enum class Kind { VarDecl, DeclGroup, Expr, Block, If, While, DoWhile, For, ForEach, Return, Break, Continue, Error };
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
    bool isTopLevel = false;  // a constant outside any function
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

// A variable declared in the condition of an if or while: `if (auto u = find(name))`.
// When its value is an optional, the variable holds what is inside, and the branch or
// loop body runs only when there is something.
struct CondDecl {
    std::unique_ptr<VarDecl> var;
    bool unwraps = false;  // set by Sema
};

struct IfStmt : Stmt {
    ExprPtr cond;              // null when `decl` is set
    std::optional<CondDecl> decl;
    StmtPtr thenStmt, elseStmt;
    IfStmt(Loc l, ExprPtr c, StmtPtr t, StmtPtr e)
        : Stmt(Kind::If, l), cond(std::move(c)), thenStmt(std::move(t)), elseStmt(std::move(e)) {}
};

struct WhileStmt : Stmt {
    ExprPtr cond;  // null when `decl` is set
    std::optional<CondDecl> decl;
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

// `for (auto x : items) body`
struct ForEachStmt : Stmt {
    std::unique_ptr<VarDecl> var;  // the loop variable, a new one for each element
    ExprPtr range;
    StmtPtr body;
    std::unique_ptr<VarDecl> rangeVar;  // holds the vector while the loop runs; set by Sema
    ForEachStmt(Loc l, std::unique_ptr<VarDecl> v, ExprPtr r, StmtPtr b)
        : Stmt(Kind::ForEach, l), var(std::move(v)), range(std::move(r)), body(std::move(b)) {}
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
    bool isExtern = false;        // `extern "C" int abs(int n);`, defined in C, with no body
    bool allowNewDelete = false;  // marked `[[allow(new_delete)]]`: no warning for new and delete
    bool isVirtual = false;       // marked `virtual`, or overriding a virtual method
    bool isOverride = false;      // marked `override`
    bool isPure = false;          // `virtual int f() = 0;`, which has no body
    bool requested = false;       // it will be checked and generated; a generic class's methods wait for their first use
    Loc usedAt;                   // for a function made from a generic: where it was first needed
    int genericDepth = 0;         // for a function made from a generic: how many others led to it
    int vtableSlot = -1;          // set by Sema for a virtual method
    Access access = Access::Public;
    std::vector<MemberInit> memberInits;
    struct BaseInit {  // `: Animal(name, 3)` in a constructor
        std::string name;
        Loc loc;
        std::vector<ExprPtr> args;
    };
    std::vector<BaseInit> baseInits;
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
    bool opaque = false;   // `extern "C" struct sqlite3;`: a C type known only by name
    bool externC = false;  // defined inside `extern "C"`: every field must be a C type
    bool isInterface = false;  // `interface Speaker { ... }`: a class with only methods that have no body
    const TemplateDecl* templateOf = nullptr;  // for `Box<int>`: the generic it is made from
    std::vector<TemplateArg> templateArgs;
    std::string templateName;   // for `Box<int>`: `Box`, the name its constructor has
    Loc usedAt;                 // for `Box<int>`: where it was first needed
    bool constraintFailed = false;  // for `Box<int>`: int does not fit, which was reported
    struct BaseName {
        std::string name;
        Loc loc;
    };
    std::vector<BaseName> baseNames;  // `class Dog : public Animal, public Pet`
    std::vector<StructDecl*> bases;   // set by Sema, in the order written
    // Set by Sema: by slot, the version of each virtual method this class uses, from its first
    // base class and its own. Those of its other base classes have vtables of their own.
    std::vector<FuncDecl*> vtable;
    std::vector<FieldDecl> fields;
    std::vector<std::unique_ptr<FuncDecl>> methods;
    FuncDecl* constructor = nullptr;
    FuncDecl* destructor = nullptr;
    Type type;  // the type this declaration defines

    StructDecl(std::string n, bool cls)
        : name(std::move(n)), isClass(cls), type{cls ? Type::Kind::Class : Type::Kind::Struct, nullptr, this} {
        type.name = name.c_str();
    }
    const char* keyword() const { return isInterface ? "interface" : isClass ? "class" : "struct"; }
    // The method with this name that this type declares itself.
    FuncDecl* ownMethod(const std::string& methodName) const {
        for (auto& m : methods)
            if (m->role == FuncDecl::Role::Method && m->name == methodName) return m.get();
        return nullptr;
    }
    // Everywhere in this type a field or method of this name is declared, as the path to it. A
    // type's own member hides what its base classes have. More than one path means the name
    // alone does not say which is meant.
    std::vector<BasePath> memberPaths(const std::string& member) const {
        std::vector<BasePath> out;
        BasePath path;
        collectMemberPaths(member, path, out);
        return out;
    }
    void collectMemberPaths(const std::string& member, BasePath& path, std::vector<BasePath>& out) const {
        path.push_back(this);
        if (fieldIndex(member) >= 0 || ownMethod(member)) out.push_back(path);
        else
            for (const StructDecl* base : bases) base->collectMemberPaths(member, path, out);
        path.pop_back();
    }
    // The nearest method with this name, in this type or a base class.
    FuncDecl* findMethod(const std::string& methodName) const {
        std::vector<BasePath> paths = memberPaths(methodName);
        return paths.empty() ? nullptr : paths[0].back()->ownMethod(methodName);
    }
    // Whether this is `other`, or derives from it.
    bool derivesFrom(const StructDecl* other) const { return partsOf(other) > 0; }
    // How many times an object of this type holds a part of type `other`. A diamond holds its
    // shared base class twice, and only a type with one such part converts to it.
    int partsOf(const StructDecl* other) const {
        if (this == other) return 1;
        int parts = 0;
        for (const StructDecl* base : bases) parts += base->partsOf(other);
        return parts;
    }
    // The way to the part of type `other`, or the first of them.
    BasePath pathTo(const StructDecl* other) const {
        if (this == other) return {this};
        for (const StructDecl* base : bases) {
            BasePath rest = base->pathTo(other);
            if (rest.empty()) continue;
            rest.insert(rest.begin(), this);
            return rest;
        }
        return {};
    }
    // Each vtable of an object of this class, as the path to the part that points to it. The first
    // base class shares the vtable of the class deriving from it, whose vtable starts like
    // it. Every other base class has its own, with the slots its own class has.
    std::vector<BasePath> vtablePlaces() const {
        std::vector<BasePath> out{{this}};
        BasePath path{this};
        collectVtablePlaces(path, out);
        return out;
    }
    void collectVtablePlaces(BasePath& path, std::vector<BasePath>& out) const {
        for (size_t i = 0; i < bases.size(); ++i) {
            path.push_back(bases[i]);
            if (i > 0) out.push_back(path);
            bases[i]->collectVtablePlaces(path, out);
            path.pop_back();
        }
    }
    // The method that runs for virtual method `m` in the vtable at `place`: the one declared
    // closest to the whole object on the way to that part. Other branches do not count.
    static const FuncDecl* finalOverrider(const BasePath& place, const FuncDecl* m) {
        for (const StructDecl* d : place)
            if (const FuncDecl* own = d->ownMethod(m->name)) return own;
        return m;
    }
    // A virtual method without a body that this class still has, which makes it abstract.
    const FuncDecl* firstPure() const {
        for (const BasePath& place : vtablePlaces())
            for (const FuncDecl* m : place.back()->vtable)
                if (const FuncDecl* f = finalOverrider(place, m); f->isPure) return f;
        return nullptr;
    }
    StructDecl(const StructDecl&) = delete;
    StructDecl& operator=(const StructDecl&) = delete;

    // The place of a field declared in this type itself, not a base class.
    int fieldIndex(const std::string& fieldName) const {
        for (size_t i = 0; i < fields.size(); ++i)
            if (fields[i].name == fieldName) return (int)i;
        return -1;
    }
    // The nearest type that declares this field: this one or a base class. Null if none does.
    const StructDecl* fieldOwner(const std::string& fieldName) const {
        for (const BasePath& path : memberPaths(fieldName))
            if (path.back()->fieldIndex(fieldName) >= 0) return path.back();
        return nullptr;
    }
};

// `template<typename T, int N> ...`: a function, a type or a constraint, which the compiler reads
// again from its tokens for each set of arguments, with the parameters standing for them.
struct TemplateParam {
    std::string name;
    Loc loc;
    const Type* valueType = nullptr;  // `int N`; null for a type parameter
    std::string constraint;           // `Comparable T`; empty for `typename T`
    Loc constraintLoc;
    const Type* placeholder = nullptr;  // stands for a type parameter while the definition is read
};

struct TemplateDecl {
    enum class Kind { Function, Record, Concept };
    Kind kind = Kind::Function;
    std::string name;
    Loc nameLoc;
    std::vector<TemplateParam> params;
    size_t start = 0, end = 0;   // the tokens of the declaration, after the `template<...>`
    bool failed = false;         // it has a syntax error, so nothing is made from it
    bool allowNewDelete = false;  // `[[allow(new_delete)]]` is written before it
    std::unique_ptr<FuncDecl> pattern;  // a function, with its parameter types as they are written
    std::map<std::string, StructDecl*> records;  // what has been made, by name
    std::map<std::string, FuncDecl*> functions;
};

struct ImportDecl {
    std::string path;  // dotted, e.g. "net.server"
    Loc loc;
};

struct Program {
    std::vector<ImportDecl> imports;
    std::vector<std::unique_ptr<StructDecl>> structs;
    std::vector<std::unique_ptr<FuncDecl>> funcs;
    std::vector<std::unique_ptr<VarDecl>> constants;
    std::vector<std::unique_ptr<TemplateDecl>> templates;
    bool allowNewDelete = false;  // `[[allow(new_delete)]];` at the top of the file

    StructDecl* findStruct(const std::string& name) const {
        for (auto& s : structs)
            if (s->name == name) return s.get();
        return nullptr;
    }
    TemplateDecl* findTemplate(const std::string& name) const {
        for (auto& t : templates)
            if (t->name == name) return t.get();
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
