#include "Sema.h"

#include <algorithm>
#include <cstring>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <functional>
#include <set>

#include "Parser.h"
#include "SpecRefs.h"

namespace cplus {

using K = Type::Kind;

static bool canConvert(const Type* from, const Type* to) {
    if (from->isError() || to->isError()) return true;
    if (from == to) return true;
    if (to->isPointer()) {
        // `null`; a `T*` to a `const T*`, which only reads; any pointer to a `void*`, as in
        // C++, keeping const.
        if (from->isNull()) return true;
        if (!from->isPointer() || (from->constElem && !to->constElem)) return false;
        return from->elem == to->elem || to->elem->is(K::Void);
    }
    // A unique reference only comes from another, since an object that may be shared has no one owner.
    if (to->isUnique()) return from->isUnique() && canConvert(from->elem, to->elem);
    if (from->isUnique() && to->isClass()) return canConvert(from->elem, to);  // it gives up being unique
    // A local reference only comes from another, and only gives one, as the count of what it refers to cannot also be used by other threads.
    if (to->isLocal()) return from->isLocal() && canConvert(from->elem, to->elem);
    if (from->isLocal() && !to->isOptional()) return false;
    if (to->isWeak()) {
        // `null`, an object that may be null, or a weak reference to a class deriving from it.
        if (from->isNull()) return true;
        const Type* target = from->isWeak() || from->isOptional() ? from->elem : from;
        return target->isClass() && canConvert(target, to->elem);
    }
    if (to->isOptional()) {
        // `null`, or a value that the optional then holds. Two optional types do not
        // convert into each other, except `Dog?` to `Animal?`.
        if (from->isNull()) return true;
        if (from->isOptional()) return from->elem->isObjectRef() && canConvert(from->elem, to->elem);
        return canConvert(from, to->elem);
    }
    // To a base class that the object holds once. Holding it twice, it is not known which is meant.
    if (from->isClass() && to->isClass()) return from->decl->partsOf(to->decl) == 1;
    if (from->is(K::Int) && to->is(K::Long)) return true;      // widening never loses information
    if (from->isInteger() && to->isFloating()) return true;    // as in C++; a very large long is rounded
    if (from->is(K::Float) && to->is(K::Double)) return true;  // never loses information
    if (from->is(K::Char) && to->isNumber()) return true;      // its code, from 0 to 255, as in C++
    return from->isNumber() && to->is(K::Bool);                // as in C++: zero is false, anything else true
}

// The pointer type that pointers `a` and `b` (or null) both convert to, for comparing
// them or choosing between them; null if there is none.
static const Type* commonPointer(const Type* a, const Type* b) {
    if (a->isPointer() && b->isNull()) return a;
    if (b->isPointer() && a->isNull()) return b;
    if (!a->isPointer() || !b->isPointer()) return nullptr;
    if (canConvert(a, b)) return b;
    if (canConvert(b, a)) return a;
    return nullptr;
}

// What two class references, either of them optional, both convert to, for comparing them
// or choosing between them: the one that is a base class of the other. Null if there is none.
static const Type* commonReference(const Type* a, const Type* b) {
    const Type* x = a->isOptional() ? a->elem : a;
    const Type* y = b->isOptional() ? b->elem : b;
    if (!x->isClass() || !y->isClass()) return nullptr;
    const Type* common = x->decl->partsOf(y->decl) == 1 ? y : y->decl->partsOf(x->decl) == 1 ? x : nullptr;
    return common && (a->isOptional() || b->isOptional()) ? Type::optionalOf(common) : common;
}

// Whether `*p`, `p[i]` and pointer arithmetic can use what `p` points to: not `void`,
// and not a C type known only by name, since neither has a known size.
static bool pointsToKnownType(const Type* p) { return !p->elem->is(K::Void) && !p->elem->isOpaque(); }

// As in C++, arithmetic on a char works on its code, as an int.
static const Type* promoted(const Type* t) { return t->is(K::Char) ? Type::intTy() : t; }

static const Type* commonInteger(const Type* a, const Type* b) {
    return (a->is(K::Long) || b->is(K::Long)) ? Type::longTy() : Type::intTy();
}

// The type arithmetic on `a` and `b` happens in, as in C++: double, then float, then
// the wider integer.
static const Type* commonNumber(const Type* a, const Type* b) {
    a = promoted(a);
    b = promoted(b);
    if (a->is(K::Double) || b->is(K::Double)) return Type::doubleTy();
    if (a->is(K::Float) || b->is(K::Float)) return Type::floatTy();
    return commonInteger(a, b);
}

// A double written as a number, such as `1.5` or `-0.25`, which also converts to a
// float: `float f = 1.5;`.
static bool isDoubleConstant(const Expr& e) {
    return e.kind == Expr::Kind::FloatLit && !static_cast<const FloatLitExpr&>(e).isFloat;
}

// The type of `c ? x : y` when one branch is null or an optional and the other fits in
// it: `c ? user : null` is a `User?`. Null otherwise.
static const Type* optionalOfBranches(const Type* x, const Type* y) {
    for (int i = 0; i < 2; ++i, std::swap(x, y)) {
        if (x->isOptional() && canConvert(y, x)) return x;
        if (x->isNull() && !y->isNull() && !y->is(K::Void) && !y->is(K::Auto)) return Type::optionalOf(y);
    }
    return nullptr;
}

static std::string q(const std::string& s) { return "'" + s + "'"; }

// An expression as it might be written in source, for help texts: `node.next`, or
// `fallback` when it is something more complicated.
// `*p`, written so that what comes after it applies to all of it.
// Whether `n` has a space outside brackets and quotes, as `p + n` has: an operator, which binds less than `.` or `*` does.
static bool hasOperator(const std::string& n) {
    int depth = 0;
    for (size_t i = 0; i < n.size(); ++i) {
        const char c = n[i];
        if (c == '"' || c == '\'') {  // a string or a char, whose insides are not brackets
            for (++i; i < n.size() && n[i] != c; ++i)
                if (n[i] == '\\') ++i;
        } else if (c == '(' || c == '[' || c == '{') ++depth;
        else if (c == ')' || c == ']' || c == '}') --depth;
        else if (c == ' ' && depth == 0) return true;
    }
    return false;
}

static std::string asBase(const std::string& n) { return !n.empty() && (n[0] == '*' || hasOperator(n)) ? "(" + n + ")" : n; }

// A string that can be shown as it is written, without a quote, a backslash or a control character.
static bool showsAsWritten(const std::string& text) {
    for (unsigned char c : text)
        if (c < 0x20 || c > 0x7e || c == '"' || c == '\\') return false;
    return true;
}

static std::string spelled(const Expr& e, const std::string& fallback = "opt");

// A char as a literal would write it: `'a'`, `'\n'`, `'\''`, or `char(7)` for one that has no escape.
static std::string charLiteral(unsigned char c) {
    switch (c) {
        case '\n': return "'\\n'";
        case '\t': return "'\\t'";
        case '\r': return "'\\r'";
        case '\0': return "'\\0'";
        case '\'': return "'\\''";
        case '\\': return "'\\\\'";
        default: return c >= 0x20 && c < 0x7f ? "'" + std::string(1, (char)c) + "'" : "char(" + std::to_string(c) + ")";
    }
}

// A string as a literal would write it, with its escapes.
static std::string stringLiteral(const std::string& text) {
    std::string out = "\"";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') out += std::string("\\") + (char)c;
        else if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else if (c == '\r') out += "\\r";
        else if (c < 0x20 || c == 0x7f) {
            static const char* digits = "0123456789abcdef";
            out += std::string("\\x") + digits[c >> 4] + digits[c & 15];
        } else out += (char)c;
    }
    return out + "\"";
}

static bool namesOnePlace(const Expr& e);

// A key or an index that means the same each time it is written: a literal or the name of a place. Nothing when it is more than that.
static std::string plainKey(const Expr& key) {
    switch (key.kind) {
        case Expr::Kind::StringLit: {
            const std::string& text = static_cast<const StringLitExpr&>(key).value;
            return showsAsWritten(text) ? "\"" + text + "\"" : "";
        }
        case Expr::Kind::IntLit: return std::to_string(static_cast<const IntLitExpr&>(key).value);
        case Expr::Kind::BoolLit: return static_cast<const BoolLitExpr&>(key).value ? "true" : "false";
        case Expr::Kind::CharLit: {
            const uint8_t c = static_cast<const CharLitExpr&>(key).value;
            return c >= 0x20 && c <= 0x7e && c != '\'' && c != '\\' ? "'" + std::string(1, (char)c) + "'" : "";
        }
        default: return namesOnePlace(key) ? spelled(key, "") : "";
    }
}

// Whether `e` names the same place each time it is written, and does nothing else: `a.b`, `v[0]`, `v[i]`, `m["k"]`, `*p`.
static bool namesOnePlace(const Expr& e) {
    switch (e.kind) {
        case Expr::Kind::Name:
        case Expr::Kind::This: return true;
        case Expr::Kind::Member: return namesOnePlace(*static_cast<const MemberExpr&>(e).base);
        case Expr::Kind::Index: {
            auto& ix = static_cast<const IndexExpr&>(e);
            return namesOnePlace(*ix.base) && !plainKey(*ix.index).empty();
        }
        case Expr::Kind::Borrow: return namesOnePlace(*static_cast<const BorrowExpr&>(e).operand);
        case Expr::Kind::Binary: {  // `p + n`, a pointer or a number, and nothing that is only a value made here, as `a + b` of strings would be
            auto& b = static_cast<const BinaryExpr&>(e);
            const bool arithmetic = b.op == BinOp::Add || b.op == BinOp::Sub || b.op == BinOp::Mul || b.op == BinOp::Div || b.op == BinOp::Rem;
            auto pure = [](const Expr& x) { return x.kind == Expr::Kind::IntLit || namesOnePlace(x); };
            return arithmetic && e.type && (e.type->isPointer() || e.type->isInteger()) && pure(*b.lhs) && pure(*b.rhs);
        }
        default: return isDeref(e) && namesOnePlace(*static_cast<const UnaryExpr&>(e).operand);
    }
}

static std::string spelled(const Expr& e, const std::string& fallback) {
    if (e.kind == Expr::Kind::Name) return static_cast<const NameExpr&>(e).name;
    if (e.kind == Expr::Kind::IntLit || e.kind == Expr::Kind::CharLit || e.kind == Expr::Kind::StringLit || e.kind == Expr::Kind::BoolLit) {
        const std::string literal = plainKey(e);
        return literal.empty() ? fallback : literal;
    }
    if (e.kind == Expr::Kind::Binary) {  // `v.size() - 1`
        auto& b = static_cast<const BinaryExpr&>(e);
        if (b.op == BinOp::Add || b.op == BinOp::Sub || b.op == BinOp::Mul || b.op == BinOp::Div || b.op == BinOp::Rem) {
            const std::string l = spelled(*b.lhs, ""), r = spelled(*b.rhs, "");
            const bool simple = b.rhs->kind != Expr::Kind::Binary && b.lhs->kind != Expr::Kind::Binary;
            if (!l.empty() && !r.empty() && simple) return l + " " + binOpSpelling(b.op) + " " + r;
        }
    }
    if (e.kind == Expr::Kind::This) return "this";
    if (e.kind == Expr::Kind::Member) {
        auto& m = static_cast<const MemberExpr&>(e);
        if (m.base->kind == Expr::Kind::Unary && static_cast<const UnaryExpr&>(*m.base).isArrow()) {
            std::string base = spelled(*static_cast<const UnaryExpr&>(*m.base).operand, "");
            if (!base.empty()) return asBase(base) + "->" + m.field;
        }
        std::string base = spelled(*m.base, "");
        if (!base.empty()) return asBase(base) + "." + m.field;
    }
    if (isDeref(e)) {
        std::string base = spelled(*static_cast<const UnaryExpr&>(e).operand, "");
        if (!base.empty()) return "*" + (hasOperator(base) ? "(" + base + ")" : base);
    }
    if (e.kind == Expr::Kind::Index) {  // `v[0]` or `v[i]`
        auto& ix = static_cast<const IndexExpr&>(e);
        std::string base = spelled(*ix.base, "");
        std::string index = plainKey(*ix.index);
        if (index.empty() && ix.index->kind != Expr::Kind::StringLit) index = spelled(*ix.index, "");
        if (!base.empty() && !index.empty()) return asBase(base) + "[" + index + "]";
    }
    if (e.kind == Expr::Kind::Borrow) return spelled(*static_cast<const BorrowExpr&>(e).operand, fallback);
    if (e.kind == Expr::Kind::Call) {
        auto& c = static_cast<const CallExpr&>(e);
        std::string types;
        for (auto& t : c.templateArgs) types += (types.empty() ? "" : ", ") + (t.type ? std::string(t.type->name) : std::to_string(t.value));
        const std::string callee = c.callee + (types.empty() ? "" : "<" + types + ">");
        std::string args;
        for (auto& a : c.args) {
            const std::string one = plainKey(*a);
            if (one.empty()) return callee + "(...)";
            args += (args.empty() ? "" : ", ") + one;
        }
        return callee + "(" + args + ")";
    }
    if (e.kind == Expr::Kind::OptChain) {  // `u?.name`
        auto& c = static_cast<const OptChainExpr&>(e);
        std::string path;
        const Expr* x = c.body.get();
        while (x->kind == Expr::Kind::Member) {
            path = "." + static_cast<const MemberExpr*>(x)->field + path;
            x = static_cast<const MemberExpr*>(x)->base.get();
        }
        std::string base = spelled(*c.base, "");
        if (x->kind == Expr::Kind::OptValue && !base.empty() && !path.empty()) return asBase(base) + "?" + path;
    }
    if (e.kind == Expr::Kind::MethodCall) {
        auto& c = static_cast<const MethodCallExpr&>(e);
        if (!c.receiver && c.lowered && c.lowered->kind == Expr::Kind::Index) {  // `m.at(key)`, which has become something else by now
            auto& ix = static_cast<const IndexExpr&>(*c.lowered);
            const std::string base = spelled(*ix.base, ""), key = plainKey(*ix.index);
            if (!base.empty()) return asBase(base) + "." + c.name + "(" + (key.empty() ? "..." : key) + ")";
        }
        std::string base = c.receiver ? spelled(*c.receiver, "") : "";
        std::string arg = c.args.size() == 1 ? plainKey(*c.args[0]) : "";
        if (!base.empty()) return asBase(base) + "." + c.name + (c.args.empty() ? "()" : "(" + (arg.empty() ? "..." : arg) + ")");
    }
    return fallback;
}

// How to get at the value of an optional named `name`.
static std::string unwrapHelp(const std::string& name) {
    return "'" + name + "' may be null. Unwrap it with `if (auto v = " + name + ") ...`, give a fallback with `" + name +
           " ?? value`, or call `" + name + ".value()`, which stops the program when it is null";
}

// Whether `e` reaches into the value of an optional through `?.`: `opt?.x` or `opt?.a.b`.
static bool throughOptionalChain(const Expr& e) {
    switch (e.kind) {
        case Expr::Kind::OptValue: return true;
        case Expr::Kind::Member: return throughOptionalChain(*static_cast<const MemberExpr&>(e).base);
        case Expr::Kind::Index: return throughOptionalChain(*static_cast<const IndexExpr&>(e).base);
        default: return false;
    }
}

static bool isPlaceKind(const Expr& e);
static bool isCopyable(const Type* t);
static bool comesWithTheLanguage(const StructDecl& sd);

static bool hasWord(const std::string& text, const std::string& word) {
    for (size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + 1)) {
        const bool starts = at == 0 || !(std::isalnum((unsigned char)text[at - 1]) || text[at - 1] == '_');
        const size_t end = at + word.size();
        const bool ends = end == text.size() || !(std::isalnum((unsigned char)text[end]) || text[end] == '_');
        if (starts && ends) return true;
    }
    return false;
}

// A name for a new variable in a help text, which means nothing where it would be written, so that nothing there changes meaning:
// in `if (auto v = o) { v.merge(v); }`, the second `v` was the program's own.
std::string Sema::freshName(const std::string& avoid, std::initializer_list<const char*> names) {
    auto isFree = [&](const std::string& name) {
        if (hasWord(avoid, name) || lookup(name)) return false;
        StructDecl* o = owner();
        return !o || findMember(*o, name, Loc{}).empty();
    };
    for (const char* name : names)
        if (isFree(name)) return name;
    for (int i = 2;; ++i) {
        const std::string name = *names.begin() + std::to_string(i);
        if (isFree(name)) return name;
    }
}

namespace {
// Where what a method of a container gave a copy of really is.
struct CopiedFrom {
    std::string place;  // as it would be written: `v[v.size() - 1]`, or nothing when it cannot be said simply
    std::string map, key;  // for what is in a map
    bool isInside = false;  // `m.get(k)`, an optional of what is at the place
};
}  // namespace

static CopiedFrom copiedFrom(const Expr& e) {
    CopiedFrom from;
    if (e.kind != Expr::Kind::MethodCall) return from;
    auto& call = static_cast<const MethodCallExpr&>(e);
    if (!call.receiver) {
        if (!call.lowered || call.lowered->kind != Expr::Kind::Index || call.name != "at") return from;  // `m.at(key)`, which has become something else by now
        auto& ix = static_cast<const IndexExpr&>(*call.lowered);
        from.key = plainKey(*ix.index);
        if (!namesOnePlace(*ix.base) || from.key.empty()) from.key.clear();
        else {
            from.map = asBase(spelled(*ix.base, ""));
            from.place = from.map + "[" + from.key + "]";
        }
        return from;
    }
    if (!call.receiver->type || !namesOnePlace(*call.receiver)) return from;
    const std::string v = asBase(spelled(*call.receiver, ""));
    if (v.empty()) return from;
    const Type* t = call.receiver->type;
    const std::string arg = call.args.size() == 1 ? plainKey(*call.args[0]) : "";
    if (t->isVector() || t->isArray() || t->is(K::String)) {
        if (call.name == "back") from.place = v + "[" + v + ".size() - 1]";
        if (call.name == "front") from.place = v + "[0]";
    }
    if (isHashmap(t) && (call.name == "get" || call.name == "at") && !arg.empty()) {
        from.map = v;
        from.key = arg;
        from.place = v + "[" + arg + "]";
        from.isInside = call.name == "get" && !t->decl->templateArgs[1].type->isOptional();  // in a map of optionals, the place holds what `get` gave
    }
    return from;
}

// Whether what `e` gives only exists as what it gave: the result of a function, and not a copy of something the program can reach.
static bool isProgramsOwnMethod(const MethodCallExpr& call) { return call.method && !comesWithTheLanguage(*call.method->owner); }

// Whether a call that can fail is anywhere in `e`, so that `try` has to be written before it wherever it goes.
static bool hasFailingCall(const Expr& e) {
    switch (e.kind) {
        case Expr::Kind::Call: {
            auto& c = static_cast<const CallExpr&>(e);
            if (c.target && c.target->callFails()) return true;
            for (auto& a : c.args)
                if (hasFailingCall(*a)) return true;
            return false;
        }
        case Expr::Kind::MethodCall: {
            auto& c = static_cast<const MethodCallExpr&>(e);
            if (c.method && c.method->callFails()) return true;
            if (c.receiver && hasFailingCall(*c.receiver)) return true;
            if (c.lowered && hasFailingCall(*c.lowered)) return true;
            for (auto& a : c.args)
                if (hasFailingCall(*a)) return true;
            return false;
        }
        case Expr::Kind::Member: return hasFailingCall(*static_cast<const MemberExpr&>(e).base);
        case Expr::Kind::Index: return hasFailingCall(*static_cast<const IndexExpr&>(e).base) || hasFailingCall(*static_cast<const IndexExpr&>(e).index);
        case Expr::Kind::Unary: return hasFailingCall(*static_cast<const UnaryExpr&>(e).operand);
        case Expr::Kind::Binary: return hasFailingCall(*static_cast<const BinaryExpr&>(e).lhs) || hasFailingCall(*static_cast<const BinaryExpr&>(e).rhs);
        case Expr::Kind::OptChain: return hasFailingCall(*static_cast<const OptChainExpr&>(e).base) || hasFailingCall(*static_cast<const OptChainExpr&>(e).body);
        case Expr::Kind::OptValue: return false;  // the chain's base, already looked at
        case Expr::Kind::Ternary: {
            auto& t = static_cast<const TernaryExpr&>(e);
            return hasFailingCall(*t.cond) || hasFailingCall(*t.thenExpr) || hasFailingCall(*t.elseExpr);
        }
        case Expr::Kind::Coalesce: return hasFailingCall(*static_cast<const CoalesceExpr&>(e).lhs) || hasFailingCall(*static_cast<const CoalesceExpr&>(e).rhs);
        case Expr::Kind::Borrow: return hasFailingCall(*static_cast<const BorrowExpr&>(e).operand);
        default: return false;
    }
}

static std::string tryIfItFails(const Expr& e) { return hasFailingCall(e) ? "try " : ""; }

static bool isTemporaryResult(const Expr& e) {
    if (e.kind == Expr::Kind::Call) return true;
    return e.kind == Expr::Kind::MethodCall && isProgramsOwnMethod(static_cast<const MethodCallExpr&>(e));
}

static const char* const kKeyMayBeAdded = ", which adds the key if it is missing";

// Why what is reached through `e` cannot change, when that is because of a `const` on the way to it. Empty when it is not.
std::string Sema::constHolderHelp(const Expr& e, bool canCopy) {
    const std::string copy = canCopy ? ", or change a copy: `auto " + freshName("", {"x", "copy", "item", "held"}) + " = ...;`, and change that" : "";
    auto throughPointer = [&](const Expr& pointer) {
        if (!pointer.type || !pointer.type->isPointer() || !pointer.type->constElem) return std::string();
        return q(spelled(pointer, "the pointer")) + " is a " + q(pointer.type->name) + ", so what it points to cannot change. Use a pointer that is not const" + copy;
    };
    const Expr* x = &e;
    for (bool walking = true; walking;) {
        if (x->kind == Expr::Kind::This) break;
        if (x->type && x->type->isClass()) return "";  // the object lives on its own, and `const` does not reach it
        if (isDeref(*x)) return throughPointer(*static_cast<const UnaryExpr*>(x)->operand);
        switch (x->kind) {
            case Expr::Kind::Member: x = static_cast<const MemberExpr*>(x)->base.get(); break;
            case Expr::Kind::Index: {
                x = static_cast<const IndexExpr*>(x)->base.get();
                if (x->type && x->type->isPointer()) return throughPointer(*x);
                break;
            }
            case Expr::Kind::OptChain: x = static_cast<const OptChainExpr*>(x)->base.get(); break;
            case Expr::Kind::OptValue: x = static_cast<const OptValueExpr*>(x)->chain->base.get(); break;
            case Expr::Kind::Borrow: x = static_cast<const BorrowExpr*>(x)->operand.get(); break;
            case Expr::Kind::MethodCall: {
                auto& call = *static_cast<const MethodCallExpr*>(x);
                if (isProgramsOwnMethod(call)) return "";  // what it gave is not what it was called on
                if (call.receiver) x = call.receiver.get();
                else if (call.lowered) x = call.lowered.get();
                else walking = false;
                break;
            }
            default: walking = false;
        }
    }
    const bool inThis = x->kind == Expr::Kind::This || (x->kind == Expr::Kind::Name && static_cast<const NameExpr*>(x)->implicitField >= 0);
    if (inThis && fn_ && fn_->isConst) return "const method " + q(fn_->name) + " cannot change fields. Remove `const` from it" + copy;
    if (x->kind != Expr::Kind::Name) return "";
    const std::string& name = static_cast<const NameExpr*>(x)->name;
    const VarDecl* holder = lookup(name);
    if (holder && holder->isConst) return q(name) + " is const, so what is in it cannot change. Remove `const` from its declaration" + copy;
    return "";
}

// Whether a method of a container gave a copy on the way to `e`, so that `[ ]` reaches what it gave a copy of.
static bool throughContainerMethod(const Expr& e) {
    const Expr* x = &e;
    while (true) {
        switch (x->kind) {
            case Expr::Kind::Member: x = static_cast<const MemberExpr*>(x)->base.get(); continue;
            case Expr::Kind::Index: x = static_cast<const IndexExpr*>(x)->base.get(); continue;
            case Expr::Kind::OptChain: x = static_cast<const OptChainExpr*>(x)->base.get(); continue;
            case Expr::Kind::OptValue: x = static_cast<const OptValueExpr*>(x)->chain->base.get(); continue;
            case Expr::Kind::MethodCall: {
                auto& call = *static_cast<const MethodCallExpr*>(x);
                if (call.lowered && call.name == "at") return true;
                if (!call.receiver || !call.receiver->type) return false;
                const Type* t = call.receiver->type;
                if (((t->isVector() || t->isArray() || t->is(K::String)) && (call.name == "back" || call.name == "front")) || (isHashmap(t) && (call.name == "get" || call.name == "at"))) return true;
                x = call.receiver.get();
                continue;
            }
            default: return false;
        }
    }
}

// When the root of `e` is the variable of a `for (auto x : list)` loop, which is a copy of the element: how to change the element.
std::string Sema::loopCopyNote(const Expr& e) {
    const Expr* x = &e;
    while (x->kind == Expr::Kind::Member || x->kind == Expr::Kind::Index)
        x = x->kind == Expr::Kind::Member ? static_cast<const MemberExpr*>(x)->base.get() : static_cast<const IndexExpr*>(x)->base.get();
    if (x->kind != Expr::Kind::Name) return "";
    for (const Expr* y = &e; y != x;) {  // a class object or what a pointer points to is the element itself, and not a copy
        if (y->type && (y->type->isClass() || y->type->isPointer())) return "";
        y = y->kind == Expr::Kind::Member ? static_cast<const MemberExpr*>(y)->base.get() : static_cast<const IndexExpr*>(y)->base.get();
    }
    if (x->type && (x->type->isClass() || x->type->isPointer())) return "";
    const VarDecl* var = lookup(static_cast<const NameExpr*>(x)->name);
    auto found = var ? loopCopies_.find(var) : loopCopies_.end();
    if (found == loopCopies_.end()) return "";
    const LoopCopy& loop = found->second;
    const std::string& list = loop.list;
    const std::string name = q(var->name);
    if (loop.overACopy) return ". " + name + " is a copy of an element of a copy, so to change the elements, loop over the outer list with indexes, and change them there";
    if (!loop.constList.empty()) return ". " + name + " is a copy of an element of " + q(loop.constList) + ", which is const, so its elements cannot change";
    if (list.empty()) return ". " + name + " is a copy of the element, so this does not change what the loop goes over";
    const bool isMap = loop.isMap;
    if (isMap) return ". " + name + " is a copy of the entry, so to change the map, store it back in `" + asBase(list) + "[" + var->name + ".first]`";
    const std::string i = freshName(list + " " + var->name, {"i", "j", "k", "index"});
    return ". " + name + " is a copy of the element, so to change the element, loop with an index, `for (long " + i + " = 0; " + i + " < " + asBase(list) + ".size(); ++" + i + ")`, and change `" + asBase(list) + "[" + i + "]`";
}

static const char* const kStoreFirst = "store it in a variable first, change that, and store it back where it came from";

// How to change what is in an optional, as what was written has it: `if (auto v = o) { v.x = 1; o = v; }`. `change` is what is done to it,
// `opening` starts the sentence, and `how` says what the code does. When the shape is not one this can be sure of, it gives no code.
std::string Sema::changeForm(const Expr& base, const std::string& change, const std::string& opening, const std::string& how, const std::string& before) {
    if (const std::string held = constHolderHelp(base, !base.type || isCopyable(base.type)); !held.empty()) return held;
    const CopiedFrom from = copiedFrom(base);
    if (from.isInside)  // `m.get(k)`: what it gave a copy of can be changed where it is
        return opening + "write `if (" + from.map + ".contains(" + from.key + ")) " + before + from.place + change + ";`, which changes it in the map";
    const std::string name = !from.place.empty() ? from.place : spelled(base, "");
    const bool canCopy = !base.type || isCopyable(base.type);
    auto store = [&](const std::string& place) {
        const std::string inner = freshName(place + " " + change);
        return canCopy ? "`if (auto " + inner + " = " + place + ") { " + before + inner + change + "; " + place + " = " + inner + "; }`"
                       : "`if (" + place + ") { auto " + inner + " = move(" + place + ").value(); " + before + inner + change + "; " + place + " = move(" + inner + "); }`";
    };
    const std::string what = canCopy ? how : "move it out and back";
    if (!name.empty() && (!from.place.empty() || namesOnePlace(base)))
        return opening + what + ": " + store(name) + (from.map.empty() ? "" : kKeyMayBeAdded) + loopCopyNote(base);
    if (isTemporaryResult(base) && !name.empty()) {
        const std::string opt = freshName(name + " " + change, {"opt", "copy", "temp", "held"});
        return opening + "store it in a variable first, as this only exists as a copy: `auto " + opt + " = " + tryIfItFails(base) + name + ";`. Then " + what + ": " + store(opt);
    }
    if (!throughContainerMethod(base)) return opening + kStoreFirst;
    return opening + "reach it with `[ ]`, and not with `back()`, `at()` or `get()`, which give copies, then unwrap it, change the copy and store it back there";
}

// What is written after a value on the way to `e`: `.a.b`, and `e` is then left at the value.
static std::string pathTo(const Expr*& e) {
    std::string path;
    while (e->kind == Expr::Kind::Member) {
        path = "." + static_cast<const MemberExpr*>(e)->field + path;
        e = static_cast<const MemberExpr*>(e)->base.get();
    }
    return path;
}

// How to change what is at `recv` in an optional with this method: unwrap each `?.` on the way to it, and store each back.
std::string Sema::storeItBack(const Expr& recv, const std::string& method, bool hasArguments, const std::string& before, const std::string& keepAs) {
    const std::string args = hasArguments ? "(...)" : "()";
    std::vector<std::string> paths;  // from each value inside to the next, the last one first
    std::vector<bool> shared;        // whether the value at that level is a class object, which needs no storing back
    const Expr* x = &recv;
    std::string path = pathTo(x);
    const Expr* root = nullptr;
    while (x->kind == Expr::Kind::OptValue) {
        paths.push_back(path);
        shared.push_back(x->type && x->type->isClass());
        const Expr* base = static_cast<const OptValueExpr*>(x)->chain->base.get();
        if (base->kind == Expr::Kind::OptChain) base = static_cast<const OptChainExpr*>(base)->body.get();
        const Expr* below = base;
        const std::string pathBelow = pathTo(below);
        if (below->kind == Expr::Kind::OptValue) {
            x = below;
            path = pathBelow;
            continue;
        }
        root = base;
        break;
    }
    if (!root || paths.empty()) return "unwrap the optional, change the copy and store it back where it came from";
    if (paths.size() == 1) return changeForm(*root, paths[0] + "." + method + args, "", "unwrap the optional, change the copy and store it back", before);
    // `if (auto a = o) { if (auto b = a.h) { b.x.f(...); a.h = b; } o = a; }`
    const bool storesInRoot = !shared.back();  // the root holds a class object: what is changed is in that object
    const bool canCopy = !root->type || isCopyable(root->type);
    if (storesInRoot)
        if (const std::string held = constHolderHelp(*root, canCopy); !held.empty()) return held;
    const std::string inTurn = "unwrap each optional, change the copy and store each one back in turn";
    const CopiedFrom from = storesInRoot ? copiedFrom(*root) : CopiedFrom{};  // read as it was written, which gives the same object
    if (from.isInside) return "start from `" + from.place + "`, as `" + static_cast<const MethodCallExpr&>(*root).name + "()` gives a copy. Then " + inTurn;
    std::string name = !from.place.empty() ? from.place : spelled(*root, "");
    if (!canCopy) return "move each optional out with `move(...).value()`, change it, and move it back in turn";
    if (name.empty() || (storesInRoot && from.place.empty() && !namesOnePlace(*root))) {
        if (isTemporaryResult(*root) && !name.empty())
            return "store it in a variable first, as this only exists as a copy: `auto " + freshName(name, {"opt", "copy", "temp", "held"}) + " = " + tryIfItFails(*root) + name + ";`. Then " + inTurn;
        if (!throughContainerMethod(*root)) return kStoreFirst;
        return "reach it with `[ ]`, and not with `back()`, `at()` or `get()`, which give copies. Then " + inTurn;
    }
    std::string used = name + " " + method;
    for (auto& p : paths) used += " " + p;
    std::string open, close;
    std::string outer = name;
    std::string keep;  // `result = `, when what it gives is used
    if (!keepAs.empty()) {
        const std::string r = freshName(used, {"result", "got", "made"});
        used += " " + r;
        open = keepAs + " " + r + " = null; ";
        keep = r + " = ";
    }
    for (size_t i = paths.size(); i-- > 0;) {
        const std::string inner = freshName(used, {"a", "b", "c", "d", "e", "f", "g", "h"});
        used += " " + inner;
        open += "if (auto " + inner + " = " + outer + ") { ";
        close = (shared[i] ? std::string() : " " + outer + " = " + inner + ";") + " }" + close;
        outer = inner + (i ? paths[i] : "");
        if (i == 0) open += keep + before + inner + paths[0] + "." + method + args + ";";
    }
    return std::string(keep.empty() ? "unwrap each optional, change the copy and store each one back: `" : "unwrap each optional, change the copy, keep what it gives, and store each one back: `") + open + close + "`" +
           (from.map.empty() || !storesInRoot ? "" : kKeyMayBeAdded) + (storesInRoot ? loopCopyNote(*root) : "");
}

static const char* const kCopyingMethods[] = {"back", "front", "at", "get", "value", "value_or", "values", "items", "keys"};

// A method of a container that gives a copy of what is in it: `v.back()`, `m.at(k)`, `o.value()`.
static bool givesACopy(const MethodCallExpr& call) {
    if (std::find_if(std::begin(kCopyingMethods), std::end(kCopyingMethods), [&](const char* name) { return call.name == name; }) == std::end(kCopyingMethods))
        return false;
    const Type* t = call.receiver ? call.receiver->type : nullptr;
    return call.vecMethod != VecMethod::None || call.lowered ||
           (t && (t->isVector() || t->isArray() || t->is(K::String) || t->isOptional() || isHashmap(t) || isJsonValue(t)));
}

// Whether `e` is something the program has, or a copy of it: a variable, a field, an element, what a pointer points to, or what
// `back()`, `?:` and the like gave of one. What a function gave is not: nothing the program has would change.
static bool reachesAPlace(const Expr& e) {
    switch (e.kind) {
        case Expr::Kind::Name:
        case Expr::Kind::This: return true;
        case Expr::Kind::Member: {
            const Expr& base = *static_cast<const MemberExpr&>(e).base;
            return (base.type && (base.type->isClass() || base.type->isPointer())) || isDeref(base) || reachesAPlace(base);
        }
        case Expr::Kind::Index: {
            const Expr& base = *static_cast<const IndexExpr&>(e).base;
            return (base.type && base.type->isPointer()) || reachesAPlace(base);
        }
        case Expr::Kind::Borrow: return reachesAPlace(*static_cast<const BorrowExpr&>(e).operand);
        case Expr::Kind::OptChain: return reachesAPlace(*static_cast<const OptChainExpr&>(e).base);
        case Expr::Kind::OptValue: return reachesAPlace(*static_cast<const OptValueExpr&>(e).chain->base);
        case Expr::Kind::Ternary: {
            auto& t = static_cast<const TernaryExpr&>(e);
            return reachesAPlace(*t.thenExpr) && reachesAPlace(*t.elseExpr);
        }
        case Expr::Kind::Coalesce: {
            auto& c = static_cast<const CoalesceExpr&>(e);
            return reachesAPlace(*c.lhs) || reachesAPlace(*c.rhs);
        }
        case Expr::Kind::Assign: return reachesAPlace(*static_cast<const AssignExpr&>(e).target);
        case Expr::Kind::MethodCall: {
            auto& call = static_cast<const MethodCallExpr&>(e);
            if (!givesACopy(call)) return false;
            if (!call.receiver) return call.lowered && reachesAPlace(*call.lowered);
            return reachesAPlace(*call.receiver) || (call.name == "value_or" && !call.args.empty() && reachesAPlace(*call.args[0]));
        }
        default: return isDeref(e);
    }
}

// What a statement throws away: `f()` in `try f();`, in `o?.f();`, and both sides of `c ? f() : g();` or `f() + 1;`.
static void thrownAway(const Expr& e, std::vector<const Expr*>& out) {
    switch (e.kind) {
        case Expr::Kind::Try: thrownAway(*static_cast<const TryExpr&>(e).operand, out); return;
        case Expr::Kind::OptChain: thrownAway(*static_cast<const OptChainExpr&>(e).body, out); return;
        case Expr::Kind::Ternary:
            thrownAway(*static_cast<const TernaryExpr&>(e).thenExpr, out);
            thrownAway(*static_cast<const TernaryExpr&>(e).elseExpr, out);
            return;
        // The left of `??`, `&&` and `||` decides whether the right runs, so it is used even when the whole is not
        case Expr::Kind::Coalesce: thrownAway(*static_cast<const CoalesceExpr&>(e).rhs, out); return;
        case Expr::Kind::Binary: {
            auto& b = static_cast<const BinaryExpr&>(e);
            if (b.op != BinOp::LogAnd && b.op != BinOp::LogOr) thrownAway(*b.lhs, out);
            thrownAway(*b.rhs, out);
            return;
        }
        case Expr::Kind::Unary:
            if (!isDeref(e)) thrownAway(*static_cast<const UnaryExpr&>(e).operand, out);
            return;
        default: out.push_back(&e);
    }
}

// Whether C++ would give a reference here, so that changing it there changes what the program has: a place, or what
// `back()`, `at()`, `value()`, `=` or `?:` of two such give of one. `value_or()` and `??` give copies in C++ too.
static bool isReferenceInCpp(const Expr& e) {
    switch (e.kind) {
        case Expr::Kind::Name:
        case Expr::Kind::This: return true;
        case Expr::Kind::Member: {
            const Expr& base = *static_cast<const MemberExpr&>(e).base;
            return (base.type && (base.type->isClass() || base.type->isPointer())) || isDeref(base) || isReferenceInCpp(base);
        }
        case Expr::Kind::Index: {
            const Expr& base = *static_cast<const IndexExpr&>(e).base;
            return (base.type && base.type->isPointer()) || isReferenceInCpp(base);
        }
        case Expr::Kind::Borrow: return isReferenceInCpp(*static_cast<const BorrowExpr&>(e).operand);
        case Expr::Kind::OptChain: return isReferenceInCpp(*static_cast<const OptChainExpr&>(e).base);
        case Expr::Kind::OptValue: return isReferenceInCpp(*static_cast<const OptValueExpr&>(e).chain->base);
        case Expr::Kind::Ternary: {
            auto& t = static_cast<const TernaryExpr&>(e);
            return isReferenceInCpp(*t.thenExpr) && isReferenceInCpp(*t.elseExpr);
        }
        case Expr::Kind::Assign: return isReferenceInCpp(*static_cast<const AssignExpr&>(e).target);
        case Expr::Kind::MethodCall: {
            auto& call = static_cast<const MethodCallExpr&>(e);
            const bool byReference = call.name == "back" || call.name == "front" || call.name == "at" || call.name == "get" || call.name == "value";
            if (!byReference || !givesACopy(call)) return false;
            return call.receiver ? isReferenceInCpp(*call.receiver) : call.lowered && isReferenceInCpp(*call.lowered);
        }
        default: return isDeref(e);
    }
}

// Whether `e` is, or may be, something the program has: a place, or a `?:` with one in either branch, however deep, and also
// through a field or an element of one, as in `(g ? a : make()).in`.
static bool holdsAPlace(const Expr& e) {
    switch (e.kind) {
        case Expr::Kind::Ternary: {
            auto& t = static_cast<const TernaryExpr&>(e);
            return holdsAPlace(*t.thenExpr) || holdsAPlace(*t.elseExpr);
        }
        case Expr::Kind::Member: return reachesAPlace(e) || holdsAPlace(*static_cast<const MemberExpr&>(e).base);
        case Expr::Kind::Index: return reachesAPlace(e) || holdsAPlace(*static_cast<const IndexExpr&>(e).base);
        default: return reachesAPlace(e);
    }
}

// What gave a copy of something the program has, on the way to `e`, as it is written: `back()` in `v.back().p`, `?:` in
// `(f ? a : b).set()`. Empty when nothing did, and when a class object or what a pointer points to is on the way, as they live on their own.
// `?:` counts when both of what it chooses from are places, as then C++ would change the one it chose.
static std::string copyOnTheWay(const Expr& e, bool resultUsed) {
    const Expr* x = &e;
    while (x->kind == Expr::Kind::Member || x->kind == Expr::Kind::Index) {
        x = x->kind == Expr::Kind::Member ? static_cast<const MemberExpr*>(x)->base.get() : static_cast<const IndexExpr*>(x)->base.get();
        if (x->type && (x->type->isClass() || x->type->isPointer())) return "";
    }
    switch (x->kind) {
        // Where C++ would change what it refers to, the habit is caught whether or not the result is used. Elsewhere C++ copies
        // too, and only a change that nothing receives is a mistake.
        case Expr::Kind::Ternary: {
            auto& t = *static_cast<const TernaryExpr*>(x);
            return isReferenceInCpp(*x) || (!resultUsed && (holdsAPlace(*t.thenExpr) || holdsAPlace(*t.elseExpr))) ? "?:" : "";
        }
        case Expr::Kind::Assign: return reachesAPlace(*x) ? "=" : "";
        case Expr::Kind::Coalesce: return !resultUsed && reachesAPlace(*x) ? "??" : "";
        case Expr::Kind::MethodCall: return isReferenceInCpp(*x) || (!resultUsed && reachesAPlace(*x)) ? static_cast<const MethodCallExpr*>(x)->name + "()" : "";
        default: return "";
    }
}

// Whether `e` lies in the copy of an optional's value that `?.` works on, rather than
// in a class object that the optional refers to.
static bool inOptionalCopy(const Expr& e) {
    switch (e.kind) {
        case Expr::Kind::OptValue: return e.type && !e.type->isClass();
        case Expr::Kind::Member: {
            const Expr& base = *static_cast<const MemberExpr&>(e).base;
            return !(base.type && base.type->isClass()) && inOptionalCopy(base);
        }
        case Expr::Kind::Index: {
            const Expr& base = *static_cast<const IndexExpr&>(e).base;
            return !(base.type && base.type->isPointer()) && inOptionalCopy(base);
        }
        default: return false;
    }
}

static const char* kChainOnlyReads =
    "'?.' only reads. Unwrap it, change the value and store it back: `if (auto u = opt) { u.field = ...; opt = u; }`. If the optional holds a class object, `u` is that object, so `u.field = ...;` is enough";

// How to turn a value of type `t` into text, for errors about joining it to a string
// with `op` (+ or +=).
// Whether `e` is a number made from a char, as `'0' + d` is, where the char it makes is what is meant.
static bool fromCharArithmetic(const Expr& e) {
    if (e.kind != Expr::Kind::Binary) return false;
    auto& b = static_cast<const BinaryExpr&>(e);
    return (b.op == BinOp::Add || b.op == BinOp::Sub) && ((b.lhs->type && b.lhs->type->is(K::Char)) || (b.rhs->type && b.rhs->type->is(K::Char)));
}

static std::string textHelp(const Type* t, const char* op = "+", const std::string& n = "opt") {
    if (t->isPointer() && t->elem->is(K::Char)) return "turn the text it points to into a string first: `string(" + n + ")`";
    if (t->isNumber())
        return std::string("turn the number into a string first: `s ") + op +
               " to_string(n)`, or build the whole text with `format(\"{}{}\", s, n)`. Unlike C++, a number is "
               "never added as a character code";
    if (t->is(K::Bool)) return "`format(\"{}\", b)` turns a bool into \"true\" or \"false\"";
    if (t->is(K::Void) || t->isError()) return "this call gives nothing to add. Call it on a line of its own, or have the function return a string";
    if (t->isOptional() && t->elem->is(K::String)) return "it may be null: write `(" + n + " ?? \"none\")`, with the parentheses, which is its value, or \"none\" when it is null";
    if (t->isOptional()) return "it may be null: `to_string(" + n + " ?? 0)` for a number, or `format(\"{}\", " + n + ")`, which gives its value or \"null\"";
    return "`format(\"{}\", value)` turns any value that can be printed into a string";
}

static bool isBuiltinFunction(const std::string& name) {
    return name == "println" || name == "print" || name == "format" || name == "to_string";
}
static std::string q(const Type* t) { return q(t->name); }
static std::string join(const std::string& path, const std::string& field) {
    return path.empty() ? field : path + "." + field;
}
static std::string placeName(const VarDecl* v, const std::string& path) {
    return path.empty() ? v->name : v->name + "." + path;
}
static bool hasDefault(const Type* t);

static std::string zeroOf(const Type* t) {
    if (t->isUnique() || t->isLocal()) return std::string(t->name) + "(...)";
    if (t->is(K::Bool)) return "false";
    if (t->is(K::Char)) return "'\\0'";
    if (t->is(K::String)) return "\"\"";
    if (t->isStruct()) return hasDefault(t) ? "{}" : t->decl->constructor ? std::string(t->name) + "(...)" : "{...}";
    if (t->isClass()) return std::string(t->name) + "(...)";
    if (t->isPointer()) return "null";
    if (t->isArray()) return "{...}";
    return "0";
}

// A declaration that gives a variable of this type a value, which would compile.
static std::string declarationOf(const Type* t, const std::string& name) {
    if (t->decl && t->decl->name == "reflect::Type") return "`reflect::Type " + name + " = reflect::type<int>();`";  // its constructor is the language's own
    if (t->isClass()) {
        const StructDecl& sd = *t->decl;
        if (sd.isInterface || sd.firstPure()) return "`" + std::string(t->name) + " " + name + " = SomeClass(...);`, with a class that derives from it";
        if (sd.templateOf && sd.templateOf->nameLoc.line == 0 && sd.role == StructDecl::Role::Opaque) return "`" + std::string(t->name) + " " + name + " = spawn f();`";
        if (sd.name == "http::WebSocket") return "`http::WebSocket " + name + " = try http::connect(url);`";
    }
    return "`" + std::string(t->name) + " " + name + " = " + zeroOf(t) + ";`";
}


// Paths deeper than this only happen with a struct that contains itself, which is
// already an error; stop instead of recursing forever.
static bool tooDeep(const std::string& path) { return std::count(path.begin(), path.end(), '.') > 32; }

// A class reference cannot be empty, so a field holding one (directly or inside a
// struct field) must be given a value when an object is built.
static bool needsValue(const Type* t, int depth = 0) {
    if (t->isObjectRef()) return true;
    if (t->isArray()) {  // its elements are made one by one, which may take a value or a constructor's arguments
        const FuncDecl* ctor = t->elem->isStruct() ? t->elem->decl->constructor : nullptr;
        return t->length > 0 && depth <= 32 && ((ctor && !ctor->params.empty()) || needsValue(t->elem, depth + 1));
    }
    if (!t->isStruct() || depth > 32) return false;
    for (auto& f : t->decl->fields)
        if (!f.defaultValue && needsValue(f.type, depth + 1)) return true;
    return false;
}

// Whether a value can be copied. What has one owner cannot, nor can what holds one.
static bool isCopyable(const Type* t) { return !holdsUnique(t); }

// Why a value of type `t` cannot be made without being given one.
static std::string elementDefaultProblem(const Type* t) {
    if (t->isArray()) return elementDefaultProblem(t->elem);
    if (t->isObjectRef()) return "a " + q(t) + " reference cannot be empty";
    if (needsValue(t)) return q(t) + " holds a class reference that cannot be empty";
    return "the constructor of " + q(t) + " needs arguments";
}

// Whether building a `from` needs a `target` that already exists: true when fields
// without defaults lead from one to the other. A type that needs itself, like a list
// node with a `Node next` field, cannot be built until references can be empty.
static bool needsExisting(const Type* from, const StructDecl* target, std::set<const StructDecl*>& seen) {
    while (from->isArray()) from = from->elem;
    if (!from->hasFields()) return false;
    if (from->decl == target) return true;
    if (!seen.insert(from->decl).second) return false;
    for (auto& f : from->decl->fields)
        if (!f.defaultValue && needsValue(f.type) && needsExisting(f.type, target, seen)) return true;
    return false;
}

static std::string cycleHelp(const Type* fieldType, const std::string& field, const StructDecl& sd) {
    std::set<const StructDecl*> seen;
    if (!needsExisting(fieldType, &sd, seen)) return "";
    return ". A " + q(sd.name) + " can only be built from an existing one, so none can ever be built; make the field "
           "optional, `" + std::string(fieldType->name) + "? " + field + ";`, so it can start out null";
}

static std::string signature(const FuncDecl& fn) {
    std::string sig = (fn.owner ? fn.owner->name + "." : "") + fn.name + "(";
    for (size_t i = 0; i < fn.params.size(); ++i)
        sig += (i ? ", " : "") + std::string(fn.params[i]->declType->name) +
               (fn.params[i]->name.empty() ? "" : " " + fn.params[i]->name);
    return sig + ")";
}
// The fields of a type, the base class's first.
static std::vector<const FieldDecl*> allFields(const StructDecl& sd) {
    std::vector<const FieldDecl*> out;
    for (const StructDecl* base : sd.bases)
        for (const FieldDecl* f : allFields(*base)) out.push_back(f);
    for (auto& f : sd.fields) out.push_back(&f);
    return out;
}
static const FieldDecl* firstNumberField(const StructDecl& sd) {
    for (const FieldDecl* f : allFields(sd))
        if (f->access == Access::Public && (f->type->isNumber() || f->type->is(K::Char) || f->type->is(K::Bool))) return f;
    return nullptr;
}

static bool comesWithTheLanguage(const StructDecl& sd) { return (sd.templateOf ? sd.templateOf->nameLoc.line : sd.nameLoc.line) == 0; }

// What is private in what comes with the language is not there, as far as a program can tell.
static std::vector<const FieldDecl*> shownFields(const StructDecl& sd) {
    std::vector<const FieldDecl*> fields = allFields(sd);
    if (comesWithTheLanguage(sd))
        fields.erase(std::remove_if(fields.begin(), fields.end(), [](const FieldDecl* f) { return f->access != Access::Public; }), fields.end());
    return fields;
}

static std::string listFields(const StructDecl& sd, const std::string& wanted = "") {
    std::vector<const FieldDecl*> fields = shownFields(sd);
    if (fields.empty() && comesWithTheLanguage(sd)) {
        std::string out = "use one of its methods: ";
        bool any = false;
        for (auto& m : sd.methods)
            if (m->role == FuncDecl::Role::Method && m->access == Access::Public && m->name.rfind("__", 0) != 0) {
                out += (any ? ", " : "") + m->name;
                any = true;
            }
        return any ? out : q(sd.name) + " has nothing that can be used this way. `cplus explain` says what to do with it";
    }
    if (fields.empty()) return wanted.empty() ? q(sd.name) + " has no fields yet. Add the fields this list fills" : q(sd.name) + " has no fields yet. Add it: `int " + wanted + " = 0;`";
    std::string out = "use one of its fields: ";
    for (size_t i = 0; i < fields.size(); ++i)
        out += (i ? ", " : "") + std::string(fields[i]->type->name) + " " + fields[i]->name;
    return out;
}

// The type of the place `path` inside variable `v`.
static const Type* typeAtPath(const VarDecl* v, const std::string& path) {
    const Type* t = v->type;
    size_t start = 0;
    while (start < path.size()) {
        size_t dot = path.find('.', start);
        std::string field = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!t->hasFields()) return Type::error();
        const StructDecl* declaring = t->decl->fieldOwner(field);
        if (!declaring) return Type::error();
        t = declaring->fields[declaring->fieldIndex(field)].type;
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return t;
}

// Expressions that name storage: a variable, `this`, a field, an element, or what a
// pointer points to.
static bool isPlaceKind(const Expr& e) {
    return e.kind == Expr::Kind::Name || e.kind == Expr::Kind::Member || e.kind == Expr::Kind::This ||
           e.kind == Expr::Kind::Index || isDeref(e);
}

// ---- Definite assignment ---------------------------------------------------

bool Sema::isAssigned(const Flow& flow, const VarDecl* var, const Type* type, const std::string& path) const {
    if (type->isError() || tooDeep(path)) return true;
    // The place itself, or something containing it, was assigned as a whole.
    std::string p = path;
    while (true) {
        if (flow.assigned.count({var, p})) return true;
        if (p.empty()) break;
        size_t dot = p.rfind('.');
        p = dot == std::string::npos ? "" : p.substr(0, dot);
    }
    // A struct also counts as assigned once every one of its fields is. So does `this`
    // in a constructor, which is where a class's fields are tracked.
    if (!type->isStruct() && !(type->isClass() && var->name == "this" && path.empty())) return false;
    for (auto& f : type->decl->fields)
        if (!isAssigned(flow, var, f.type, join(path, f.name))) return false;
    return true;
}

bool Sema::anyPartAssigned(const Flow& flow, const VarDecl* var, const std::string& path) const {
    for (auto& [v, p] : flow.assigned)
        if (v == var && (path.empty() || p == path || p.rfind(path + ".", 0) == 0)) return true;
    return false;
}

std::string Sema::firstUnassigned(const Flow& flow, const VarDecl* var, const Type* type,
                                  const std::string& path) const {
    if (isAssigned(flow, var, type, path) || tooDeep(path)) return "";
    if (type->isStruct() || (type->isClass() && path.empty() && var->name == "this"))
        for (auto& f : type->decl->fields) {
            std::string missing = firstUnassigned(flow, var, f.type, join(path, f.name));
            if (!missing.empty()) return missing;
        }
    return placeName(var, path);
}

void Sema::markAssigned(Flow& flow, const VarDecl* var, const std::string& path) {
    if (flow.reachable) flow.assigned.insert({var, path});
}

// A declaration without an initializer still gives strings and vectors their empty
// value, optionals null, array elements and fields their defaults, as in C++. Numbers
// and bools stay unassigned.
void Sema::markDefaults(Flow& flow, const VarDecl* var, const Type* type, const std::string& path) {
    if (tooDeep(path)) return;
    if (type->is(K::String) || type->isVector() || type->isOptional() || type->isWeak()) {
        markAssigned(flow, var, path);
    } else if (type->isArray()) {  // elements are not tracked one by one: all have their default, or none do
        if (!needsValue(type)) markAssigned(flow, var, path);
    } else if (type->isStruct()) {
        for (auto& f : type->decl->fields) {
            if (f.defaultValue) markAssigned(flow, var, join(path, f.name));
            else markDefaults(flow, var, f.type, join(path, f.name));
        }
    }
}

// What an object built by a constructor starts with before the constructor runs:
// every field has its default or zero value, except class references, which must be
// set by the constructor.
void Sema::markConstructed(Flow& flow, const VarDecl* var, const Type* type, const std::string& path) {
    if (tooDeep(path)) return;
    for (auto& f : type->decl->fields) markFieldConstructed(flow, var, f, path);
}

void Sema::markFieldConstructed(Flow& flow, const VarDecl* var, const FieldDecl& f, const std::string& path) {
    std::string fieldPath = join(path, f.name);
    if (f.defaultValue || !needsValue(f.type)) markAssigned(flow, var, fieldPath);
    else if (f.type->isStruct()) markConstructed(flow, var, f.type, fieldPath);
}

void Sema::requireFullyBuilt(Flow& flow, Loc loc) {
    VarDecl* self = thisVar();
    if (!flow.reachable || isAssigned(flow, self, self->type, "")) return;
    std::string missing = firstUnassigned(flow, self, self->type, "").substr(5);  // drop "this."
    const Type* missingType = typeAtPath(self, missing);
    diags_.error(loc, "the constructor of " + q(owner()->name) + " must give every class-typed field a value, but " +
                          q(missing) + " has none",
                 (missingType->isArray() ? q(missing) + " needs a value because of its elements. Set it in the initializer list, e.g. `: " + missing.substr(0, missing.find('.')) + "({...})`"
                                         : "a " + q(missingType) + " reference cannot be empty. Set it in the initializer list, e.g. `: " +
                                               missing.substr(0, missing.find('.')) + "(value)`, or give the field a default") +
                     cycleHelp(typeAtPath(self, missing.substr(0, missing.find('.'))), missing.substr(0, missing.find('.')), *owner()),
                 spec::Classes);
    markAssigned(flow, self, "");
}

Sema::Flow Sema::merge(const Flow& a, const Flow& b) const {
    if (!a.reachable) return b;
    if (!b.reachable) return a;
    Flow out;
    for (auto& [v, p] : a.assigned)
        if (isAssigned(b, v, typeAtPath(v, p), p)) out.assigned.insert({v, p});
    for (auto& [v, p] : b.assigned)
        if (isAssigned(a, v, typeAtPath(v, p), p)) out.assigned.insert({v, p});
    return out;
}

void Sema::requireAssigned(Flow& flow, const VarDecl* var, const std::string& path, const Type* type, Loc loc) {
    if (buildingBase_ && var == thisVar() && flow.reachable) {
        diags_.error(loc, (path.empty() ? "the object" : q(path.substr(0, path.find('.')))) + " cannot be used while the base class " +
                              q(buildingBase_->name) + " is being built, which comes before everything else",
                     "pass values from the constructor's parameters, and do the rest in its body", spec::Inheritance);
        return;
    }
    if (var->isTopLevel || !flow.reachable || isAssigned(flow, var, type, path)) return;
    std::string name = placeName(var, path);
    const auto moved = std::find_if(moveLog_.rbegin(), moveLog_.rend(), [&](auto& m) { return m.first == var; });
    if (initField_ && var == thisVar()) {
        const std::string& setting = initField_->name;
        std::string field = path.substr(0, path.find('.'));
        if (path.empty())
            diags_.error(loc, "the object cannot be used while " + q(setting) + " is being set, since it does not have "
                              "all of its fields yet",
                         "do this in the constructor's body, where every field has its value. If building a class-typed field needs the object, make the field optional: `" + std::string(initField_->type->name) + "? " + setting + ";`", spec::Classes);
        else
            diags_.error(loc, q(field) + " has no value yet when " + q(setting) + " is set, since fields are set in the "
                              "order they are declared",
                         "declare " + q(field) + " before " + q(setting) + ", or set " + q(setting) +
                             " in the constructor's body",
                         spec::Classes);
    } else if (var == thisVar() && path.empty()) {
        const std::string missing = firstUnassigned(flow, var, type, path).substr(5);  // drop "this."
        diags_.error(loc, "the object cannot be used until all of its fields have values: " + q(missing) + " has none yet",
                     "assign " + q(missing) + " first, or give it a default", spec::Classes);
    } else if (moved != moveLog_.rend() && moved->first != thisVar()) {
        diags_.error(loc, q(name) + " has no value here, since it may have been moved away",
                     "give it a new value first, with `" + name + " = ...;`, on every path to this line", spec::Unique);
        diags_.note(moved->second, "it is moved here");
    } else if (type->isStruct() && anyPartAssigned(flow, var, path)) {
        std::string missing = firstUnassigned(flow, var, type, path);
        diags_.error(loc, q(name) + " is used before all of its fields have values: " + q(missing) + " has none yet",
                     "assign " + q(missing) + " on every path before this line, or give the field a default in "
                     "the struct, e.g. `int x = 0;`",
                     spec::DefiniteAssignment);
    } else if (path.empty()) {
        diags_.error(loc, q(name) + " is used before it is given a value",
                     "give it a value where it is declared, e.g. " + declarationOf(type, var->name) + ", or assign it on every path before this line" +
                         (fn_ && fn_->name.find('<') != std::string::npos ? ". In a generic, `T()` is the default value of T" : ""),
                     spec::DefiniteAssignment);
    } else {
        diags_.error(loc, q(name) + " is used before it is given a value",
                     "assign `" + name + " = ...;` on every path before this line, or give the field a default "
                     "in the struct",
                     spec::DefiniteAssignment);
    }
    markAssigned(flow, var, path);  // report each place once
}

// ---- Names -----------------------------------------------------------------

VarDecl* Sema::lookup(const std::string& name) {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) return found->second;
    }
    return nullptr;
}

void Sema::declare(VarDecl& v) {
    auto& scope = scopes_.back();
    auto [it, inserted] = scope.emplace(v.name, &v);
    if (!inserted) {
        diags_.error(v.loc, q(v.name) + " is already declared in this scope",
                     "pick a different name, or assign to the existing variable without repeating its type",
                     spec::Variables);
        diags_.note(it->second->loc, q(v.name) + " was first declared here");
    }
}

// Edit distance where swapping two adjacent letters counts as one edit ("coutn" -> "count").
static size_t editDistance(const std::string& a, const std::string& b) {
    std::vector<std::vector<size_t>> d(a.size() + 1, std::vector<size_t>(b.size() + 1));
    for (size_t i = 0; i <= a.size(); ++i) d[i][0] = i;
    for (size_t j = 0; j <= b.size(); ++j) d[0][j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        for (size_t j = 1; j <= b.size(); ++j) {
            d[i][j] = std::min({d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + (a[i - 1] != b[j - 1])});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
                d[i][j] = std::min(d[i][j], d[i - 2][j - 2] + 1);
        }
    }
    return d[a.size()][b.size()];
}

static std::string closest(const std::string& name, const std::vector<std::string>& candidates) {
    std::string best;
    size_t bestDist = std::max<size_t>(1, name.size() / 3) + 1;
    for (auto& c : candidates) {
        size_t d = editDistance(name, c);
        if (d < bestDist) {
            bestDist = d;
            best = c;
        }
    }
    return best;
}

std::string Sema::suggestName(const std::string& name) {
    std::vector<std::string> candidates{"println", "print", "format", "to_string"};
    for (auto& scope : scopes_)
        for (auto& [n, _] : scope) candidates.push_back(n);
    for (auto& [n, _] : funcs_) candidates.push_back(n);
    return closest(name, candidates);
}

static std::string suggestField(const StructDecl& sd, const std::string& name) {
    std::vector<std::string> candidates;
    for (const FieldDecl* f : shownFields(sd)) candidates.push_back(f->name);
    return closest(name, candidates);
}

void Sema::reportUnknownName(const NameExpr& n) {
    std::string help;
    if (auto s = structs_.find(n.name); s != structs_.end()) {
        diags_.error(n.loc, q(n.name) + " is a type, not a value",
                     s->second->isInterface || s->second->firstPure()
                         ? "build an object of a class that derives from it: `SomeClass(...)`"
                         : "to build one, write `" + n.name + "{...}` or `" + n.name + "(...)`",
                     spec::Variables);
        return;
    }
    if (funcs_.count(n.name) || (program_->findTemplate(n.name) && program_->findTemplate(n.name)->kind == TemplateDecl::Kind::Function)) {
        diags_.error(n.loc, q(n.name) + " is a function, not a value", "call it, as in `" + n.name + "()`: functions can only be called for now", spec::Functions);
        return;
    }
    if (isBuiltinFunction(n.name) || isMathFunction(n.name) || isCharFunction(n.name)) {
        diags_.error(n.loc, q(n.name) + " is a function, not a value", "call it, as in `" + n.name + "(x)`: functions can only be called for now", spec::Functions);
        return;
    }
    if (n.name == "vector" || n.name == "array" || n.name == "hashmap") {
        diags_.error(n.loc, q(n.name) + " needs the types it holds", n.name == "vector" ? "write `vector<int>`" : n.name == "array" ? "write `array<int, 3>`" : "write `hashmap<string, int>`",
                     n.name == "vector" ? spec::Vectors : n.name == "array" ? spec::Arrays : spec::HashMaps);
        return;
    }
    if (StructDecl* o = owner()) {  // a method of this class, named without its `()`
        bool isMethod = false;
        for (const StructDecl* d = o; d && !isMethod;) {
            isMethod = d->findMethod(n.name) != nullptr;
            d = d->bases.empty() ? nullptr : d->bases[0];
        }
        if (isMethod) {
            const FuncDecl* method = nullptr;
            for (const StructDecl* d = o; d && !method; d = d->bases.empty() ? nullptr : d->bases[0]) method = d->findMethod(n.name);
            diags_.error(n.loc, q(n.name) + " is a method, not a value", "call it with `" + n.name + (method && !method->params.empty() ? "(...)" : "()") + "`", spec::Methods);
            return;
        }
    }
    if (const TemplateDecl* concept_ = program_->findTemplate(n.name); concept_ && concept_->kind == TemplateDecl::Kind::Concept) {
        diags_.error(n.loc, q(n.name) + " is a concept, not a value",
                     "use it on a type: `template<" + n.name + " T>` for a generic, or `" + n.name + "<int>` in `if constexpr (...)`", spec::Generics);
        return;
    }
    if (const StructDecl* c = program_->findStruct(n.name); c && c->opaque) {
        diags_.error(n.loc, q(n.name) + " is a C type, not a value",
                     "a C type declared by name is only used through pointers: get a `" + n.name + "*` from the C function that makes one", spec::CFunctions);
        return;
    }
    if (const TemplateDecl* tmpl = program_->findTemplate(n.name); tmpl && tmpl->kind != TemplateDecl::Kind::Function) {
        std::string example = exampleArguments(*tmpl), note = constraintNote(*tmpl);
        if (!note.empty()) {  // a constraint of the program's own: `int` or `double` may have what it requires
            std::string args;
            bool allFit = true;
            for (const TemplateParam& p : tmpl->params) {
                std::string one = p.valueType ? (p.valueType->is(K::Bool) ? "true" : p.valueType->is(K::Char) ? "'a'" : "4") : "";
                Loc where{};
                if (!p.valueType)
                    for (const Type* candidate : {Type::intTy(), Type::doubleTy()})
                        if (one.empty() && (p.constraint.empty() || constraintProblem(p.constraint, candidate, where).empty())) one = candidate->name;
                allFit = allFit && !one.empty();
                args += (args.empty() ? "" : ", ") + (one.empty() ? std::string("...") : one);
            }
            if (allFit) example = tmpl->name + "<" + args + ">", note.clear();
        }
        diags_.error(n.loc, q(n.name) + " is a generic type, not a value", "to build one, write it with its arguments: `" + example + "(...)`" + note, spec::Generics);
        return;
    }
    if (auto it = program_->notImported.find(n.name); it != program_->notImported.end()) {
        diags_.error(n.loc, it->second.first, it->second.second, spec::CHeaders);
        return;
    }
    if (program_->unreadFunctions.count(n.name)) return;  // a header that could not be read may declare it, which has been said
    if (false) {}
    else if (n.name == "cout" || n.name == "endl" || n.name == "cerr" || n.name == "console") help = "C+ prints with `println(\"{}\", value);`";
    else if (n.name == "cin") help = "there are no input functions yet. C's can be declared: `extern \"C\" int getchar();`";
    else if (n.name == "JSON") help = "`import json;`, then `json::parse<T>(text)` and `json::stringify(x)`";
    else if (n.name == "Math") help = "write `sqrt(x)`, `floor(x)` and the rest with no prefix";
    else if (n.name == "nullptr" || n.name == "NULL") help = "write `null`; only an optional, such as `User?`, or a pointer can be null";
    else if (n.name == "M_PI" || n.name == "M_E" || n.name == "INFINITY" || n.name == "NAN") {
        diags_.error(n.loc, "this compiler does not support math constants such as " + q(n.name) + " yet",
                     n.name == "INFINITY" || n.name == "NAN" ? "write `1.0 / 0` for infinity, or `0.0 / 0` for NaN, or `import <math.h>;`, which declares C's"
                                                             : "write `import <math.h>;`, which declares C's, as in C. Without it, `acos(-1)` is pi and `exp(1)` is e",
                     spec::Math);
        return;
    }
    else if (defaultsOf_ && defaultsOf_->fieldOwner(n.name))
        help = "a field's default cannot read other fields; set this field in a constructor instead";
    else if (std::string guess = suggestName(n.name); !guess.empty() && guess != n.name) help = "did you mean " + q(guess) + "?";
    else help = "declare it before this line, e.g. `auto " + n.name + " = ...;`";
    diags_.error(n.loc, q(n.name) + " is not declared", help, spec::Variables);
}

// ---- Program, structs and functions ----------------------------------------

void Sema::check(Program& program) {
    program_ = &program;
    for (auto& imp : program.imports) {
        if (!isBuiltinModule(imp.path))
            diags_.error(imp.loc, "this compiler does not support modules yet, except for `http`, `json` and `reflect`, so '" + imp.path + "' cannot be imported",
                         "remove the import for now; built-ins like println need no import", spec::Status);
#ifndef CPLUS_CURL_LINK
        if (imp.path == "http")
            diags_.error(imp.loc, "this compiler was built without libcurl, which `http` is written on",
                         "install the development files of libcurl, such as `libcurl4-openssl-dev` on Debian or Ubuntu, then run cmake and build again", spec::Http);
#endif
    }

#ifndef CPLUS_UV_LINK
    if (program.usesAsync)
        diags_.error(program.asyncLoc, "this compiler was built without libuv, which async is written on",
                     "install the development files of libuv, such as `libuv1-dev` on Debian or Ubuntu, then run cmake and build again", spec::Async);
#endif
    adoptTypes();

    for (auto& fn : program.funcs) {
        if (isBuiltinFunction(fn->name)) {
            diags_.error(fn->nameLoc, q(fn->name) + " is a built-in function and cannot be redefined",
                         "give your function a different name", spec::Printing);
            continue;
        }
        if (fn->isExtern && fn->nameLoc.line && (fn->name == "main" || fn->name.rfind("cplus_", 0) == 0)) {
            diags_.error(fn->nameLoc,
                         fn->name == "main" ? "'main' is where a C+ program starts, so it cannot be a C function"
                                            : "names starting with 'cplus_' are reserved for the C+ runtime",
                         "give the function a different name", spec::CFunctions);
            continue;
        }
        if (auto s = structs_.find(fn->name); s != structs_.end()) {
            diags_.error(fn->nameLoc, q(fn->name) + " is already the name of a " + s->second->keyword(),
                         "give the function a different name", spec::Functions);
            diags_.note(s->second->nameLoc, std::string("the ") + s->second->keyword() + " is defined here");
            continue;
        }
        auto [it, inserted] = funcs_.emplace(fn->name, fn.get());
        if (!inserted) {
            bool sameParams = it->second->params.size() == fn->params.size();
            for (size_t i = 0; sameParams && i < fn->params.size(); ++i) sameParams = it->second->params[i]->declType == fn->params[i]->declType;
            diags_.error(fn->nameLoc, sameParams ? "a function named " + q(fn->name) + " is already defined" : std::string("this compiler does not support overloaded functions yet"),
                         sameParams ? "rename one of them, or remove it" : "rename one of them, so that each name is one function", spec::Functions);
            diags_.note(it->second->nameLoc, "the first definition is here");
        }
    }

    for (auto& fn : program.funcs) prepareAsync(*fn);

    auto mainIt = funcs_.find("main");
    if (mainIt == funcs_.end()) {
        if (!diags_.hasErrors()) diags_.error(Loc{}, "this program has no main function",
                     "every program starts at `int main() { ... return 0; }`", spec::MainFunction);
    } else {
        FuncDecl* m = mainIt->second;
        if (!m->bodyRet()->is(K::Int))
            diags_.error(m->retTypeLoc, "main must return int, not " + q(m->bodyRet()),
                         std::string("write `") + (m->isAsync ? "async " : "") + "int main()`; the returned number becomes the exit code", spec::MainFunction);
        auto isArgv = [](const Type* t) { return t->isPointer() && t->elem->isPointer() && t->elem->elem->is(K::Char); };
        if (!m->params.empty() &&
            !(m->params.size() == 2 && m->params[0]->declType->is(K::Int) && isArgv(m->params[1]->declType)))
            diags_.error(m->params[0]->loc, "main takes no parameters, or the command-line arguments as in C++",
                         "write `int main()`, or `int main(int argc, char** argv)`, where `string(argv[1])` is the "
                         "first argument",
                         spec::MainFunction);
    }

    checkTemplateNames(program);
    checkConstants(program);
    // Checking one thing can make new ones, from generics, until nothing new is needed.
    size_t funcsChecked = 0;
    for (bool progress = true; progress;) {
        progress = false;
        if (structsChecked_ < program.structs.size()) {
            checkNewStructs();
            progress = true;
        }
        while (funcsChecked < program.funcs.size()) {
            checkFunction(*program.funcs[funcsChecked++]);
            progress = true;
        }
        while (!pendingMethods_.empty()) {
            FuncDecl* m = pendingMethods_.front();
            pendingMethods_.pop_front();
            checkFunction(*m);
            progress = true;
        }
    }
}

void Sema::adoptTypes() {
    size_t first = typesAdopted_;
    if (first == program_->structs.size()) return;
    typesAdopted_ = program_->structs.size();
    for (size_t i = first; i < typesAdopted_; ++i)
        if (program_->structs[i]->defined) structs_[program_->structs[i]->name] = program_->structs[i].get();
    checkBases(first);
}

void Sema::requestMethod(FuncDecl* m) {
    if (m->requested || (m->owner && m->owner->constraintFailed)) return;
    m->requested = true;
    pendingMethods_.push_back(m);
}

void Sema::checkNewStructs() {
    adoptTypes();
    size_t first = structsChecked_;
    structsChecked_ = program_->structs.size();
    size_t end = structsChecked_;
    for (size_t i = first; i < end; ++i)
        if (StructDecl& sd = *program_->structs[i]; sd.defined && sd.templateOf)
            sd.constraintFailed = !checkConstraints(const_cast<TemplateDecl&>(*sd.templateOf), sd.templateArgs, sd.usedAt);
    checkStructs(first);
    for (size_t i = first; i < end; ++i) {
        StructDecl& sd = *program_->structs[i];
        if (!sd.defined || sd.constraintFailed) continue;
        for (auto& m : sd.methods) prepareAsync(*m);
    }
    for (size_t i = first; i < end; ++i) {
        StructDecl& sd = *program_->structs[i];
        if (!sd.defined || sd.constraintFailed) continue;
        if (sd.templateOf) diags_.pushContext(sd.usedAt, "in " + q(sd.name) + ", which is used here");
        checkMembers(sd);
        if (sd.templateOf) diags_.popContext();
        if (isHashmap(&sd.type)) {
            mapMethod(&sd.type, "__same");  // for ==, wherever it is hidden in other types
            if (program_->buildsFromDescriptors()) mapMethod(&sd.type, "__put");  // for reading JSON, which goes by descriptors
        }
        // A generic class's other methods wait until they are used, as in C++, so that it
        // can have methods that not all of its arguments allow.
        for (auto& m : sd.methods)
            if (!sd.templateOf || m->isVirtual || m->role != FuncDecl::Role::Method) requestMethod(m.get());
    }
}

static const char* const kBuiltinConstraints[] = {"Comparable", "Number", "Integer", "Floating", "Hashable", "__Hashable", "__JsonWritable", "__JsonReadable", "__Copyable"};

// Each generic's name must be free, and each of its constraints must exist.
void Sema::checkTemplateNames(Program& program) {
    for (auto& t : program.templates) {
        if (t->name.empty()) continue;
        if (t->kind == TemplateDecl::Kind::Function && isBuiltinFunction(t->name))
            diags_.error(t->nameLoc, q(t->name) + " is a built-in function and cannot be redefined",
                         "give your function a different name", spec::Printing);
        if (auto f = funcs_.find(t->name); f != funcs_.end()) {
            diags_.error(t->nameLoc, "a function named " + q(t->name) + " is already defined",
                         "rename one of them; overloading is planned but not supported yet", spec::Functions);
            diags_.note(f->second->nameLoc, "it is defined here");
        }
        if (auto s = structs_.find(t->name); s != structs_.end()) {
            diags_.error(t->nameLoc, "the name " + q(t->name) + " is already used by a " + s->second->keyword(),
                         "rename one of them", spec::Generics);
            diags_.note(s->second->nameLoc, "it is defined here");
        }
        for (auto& p : t->params) {
            if (p.constraint.empty()) continue;
            bool known = structs_.count(p.constraint) > 0;
            for (const char* builtin : kBuiltinConstraints) known = known || p.constraint == builtin;
            TemplateDecl* other = program.findTemplate(p.constraint);
            known = known || (other && other->kind == TemplateDecl::Kind::Concept);
            if (!known)
                diags_.error(p.constraintLoc, "there is no constraint named " + q(p.constraint),
                             "the built-in ones are Comparable, Number, Integer, Floating and Hashable. An interface or class also "
                             "works, as does your own: `template<typename T> concept " + p.constraint +
                                 " = requires(T a) { ... };`. For any type, write `typename " + p.name + "`",
                             spec::Constraints);
        }
    }
}

// What in a constant's value cannot be worked out from literals and other constants, or
// null if nothing is.
static const Expr* notConstant(const Expr& e) {
    switch (e.kind) {
        case Expr::Kind::IntLit: case Expr::Kind::FloatLit: case Expr::Kind::CharLit: case Expr::Kind::BoolLit:
        case Expr::Kind::StringLit: case Expr::Kind::NullLit: case Expr::Kind::Sizeof:
            return nullptr;
        case Expr::Kind::Name: {
            auto& n = static_cast<const NameExpr&>(e);
            return n.decl && n.decl->isTopLevel ? nullptr : &e;
        }
        case Expr::Kind::Unary: {
            auto& u = static_cast<const UnaryExpr&>(e);
            return u.op == UnOp::Deref || u.op == UnOp::AddrOf ? &e : notConstant(*u.operand);
        }
        case Expr::Kind::Binary: {
            auto& b = static_cast<const BinaryExpr&>(e);
            const Expr* bad = notConstant(*b.lhs);
            return bad ? bad : notConstant(*b.rhs);
        }
        case Expr::Kind::Ternary: {
            auto& t = static_cast<const TernaryExpr&>(e);
            const Expr* bad = notConstant(*t.cond);
            if (!bad) bad = notConstant(*t.thenExpr);
            return bad ? bad : notConstant(*t.elseExpr);
        }
        case Expr::Kind::Cast: {
            auto& c = static_cast<const CastExpr&>(e);
            if (!c.operand) return nullptr;
            if (c.target->is(K::String) && c.operand->type->isPointer()) return &e;  // reads memory
            return notConstant(*c.operand);
        }
        case Expr::Kind::Constraint: return nullptr;
        case Expr::Kind::Call: {  // a call to a constexpr function, as in C++
            auto& c = static_cast<const CallExpr&>(e);
            if (!c.target || !c.target->isConstexpr) return &e;
            for (auto& a : c.args)
                if (const Expr* bad = notConstant(*a)) return bad;
            return nullptr;
        }
        default: return &e;
    }
}

// Constants live in the outermost scope, which stays for the whole program, so a function
// can shadow them. Each can use the others, in any order, so they are all declared first.
void Sema::checkConstants(Program& program) {
    scopes_.emplace_back();
    for (auto& c : program.constants) {
        if (auto f = funcs_.find(c->name); f != funcs_.end()) {
            diags_.error(c->loc, q(c->name) + " is already the name of a function", "give the constant a different name",
                         spec::Constants);
            diags_.note(f->second->nameLoc, "the function is defined here");
        } else if (auto s = structs_.find(c->name); s != structs_.end()) {
            diags_.error(c->loc, q(c->name) + " is already the name of a " + s->second->keyword(),
                         "give the constant a different name", spec::Constants);
            diags_.note(s->second->nameLoc, "it is defined here");
        }
        declare(*c);
    }
    for (auto& c : program.constants) checkConstant(*c);
}

void Sema::checkConstant(VarDecl& c) {
    if (constantsChecked_.count(&c)) return;
    if (!constantsBeingChecked_.insert(&c).second) {
        diags_.error(c.loc, "the value of " + q(c.name) + " depends on itself",
                     "give it a value that does not lead back to " + q(c.name), spec::Constants);
        if (!c.type) c.type = Type::error();
        return;
    }
    Flow flow;
    if (c.declType->is(K::Auto)) {
        const Type* t = c.init ? checkExpr(*c.init, flow) : Type::error();
        if (!c.type) c.type = t;
    } else {
        c.type = c.declType;
        if (c.init) checkValue(*c.init, flow, c.type, "to initialize " + q(c.name));
    }
    if (!c.init) {
        diags_.error(c.loc, "the constant " + q(c.name) + " needs a value", "write `const " +
                     std::string(c.declType->name) + " " + c.name + " = <value>;`", spec::Constants);
    } else if (const Type* t = c.type; !t->isError() && !t->is(K::Bool) && !t->isNumber() && !t->is(K::Char) &&
                                       !t->is(K::String) && !t->isPointer()) {
        diags_.error(c.typeLoc, "this compiler does not support constants of type " + q(t) + " yet",
                     "write a function that returns the value", spec::Status);
    } else if (c.isConstexpr) {
        foldConstexpr(c);
    } else if (const Expr* bad = notConstant(*c.init); !bad && !t->isError()) {
        foldConstant(c);
    } else if (bad && !t->isError()) {
        diags_.error(bad->loc, "the value of a constant is worked out from literals, other constants and operators",
                     "only calls to `constexpr` functions work here: mark the function `constexpr` if it can run while compiling. Otherwise write a function that returns the value, or use a local variable",
                     spec::Constants);
    }
    constantsBeingChecked_.erase(&c);
    constantsChecked_.insert(&c);
}

void Sema::checkMembers(StructDecl& sd) {
    if (!sd.constructor)
        for (const StructDecl* base : sd.bases) checkBaseWithoutValues(sd, *base, sd.nameLoc);
    std::unordered_map<std::string, FuncDecl*> seen;
    for (auto& m : sd.methods) {
        if (m->role != FuncDecl::Role::Method) continue;
        if (m->name == sd.name || m->name == sd.templateName) {
            diags_.error(m->nameLoc, q(m->name) + " is the name of its type, which only a constructor can have",
                         "for a constructor, leave out the return type: `" + m->name + "() { ... }`. For a method, give it another name", spec::Classes);
            continue;
        }
        if (int f = sd.fieldIndex(m->name); f >= 0) {
            diags_.error(m->nameLoc, q(sd.name) + " already has a field named " + q(m->name),
                         "rename one of them: a method and a field cannot share a name", spec::Methods);
            continue;
        }
        auto [it, inserted] = seen.emplace(m->name, m.get());
        if (!inserted) {
            diags_.error(m->nameLoc, q(sd.name) + " already has a method named " + q(m->name),
                         "rename one of them; overloading is planned but not supported yet", spec::Methods);
            diags_.note(it->second->nameLoc, "the first one is here");
        }
    }
}

bool Sema::canAccess(const StructDecl& sd, Access access, const StructDecl* through) const {
    if (access == Access::Public || owner() == &sd) return true;
    if (fn_ && fn_->nameLoc.line == 0 && sd.nameLoc.line == 0) return true;  // what comes with the language, among itself
    if (access != Access::Protected || !owner() || !owner()->derivesFrom(&sd)) return false;
    return through && through->derivesFrom(owner());
}

void Sema::reportAccess(const StructDecl& sd, const std::string& member, bool isMethod, Access access, Loc loc) {
    std::string what = isMethod ? "method" : "field";
    if (!isHashmap(&sd.type) && comesWithTheLanguage(sd)) {
        const bool isTask = sd.templateOf && sd.templateOf->nameLoc.line == 0 && sd.role == StructDecl::Role::Opaque;
        diags_.error(loc, q(member) + " is private in " + q(sd.name),
                     isTask ? "a task comes from `spawn`: " + declarationOf(&sd.type, "t") : "it is part of how " + q(sd.name) + " works inside. " + listFields(sd), spec::Status);
        return;
    }
    if (isHashmap(&sd.type)) {
        diags_.error(loc, q(member) + " is private in " + q(sd.name),
                     "it is part of how a hashmap works inside. Use `[ ]` and its methods, such as `size`, `get` and `keys`", spec::HashMaps);
        return;
    }
    if (access == Access::Protected)
        diags_.error(loc, q(member) + " is protected in " + q(sd.name),
                     "it can only be used inside the methods of " + q(sd.name) + " and of classes that derive from it, on "
                     "objects of the deriving class. Put the " + what + " under `public:` to use it here",
                     spec::Inheritance);
    else
        diags_.error(loc, q(member) + " is private in " + q(sd.name),
                     "it can only be used inside the methods of " + q(sd.name) + ". Put the " + what +
                         " under `public:` to use it here" +
                         (sd.isClass ? ", or under `protected:` to use it in classes that derive from it" : ""),
                     spec::Classes);
}

// Finds each class's base classes, before anything looks through them.
void Sema::checkBases(size_t first) {
    Program& program = *program_;
    size_t end = program.structs.size();
    for (size_t i = first; i < end; ++i) {
        StructDecl* sd = program.structs[i].get();
        if (!sd->defined) continue;
        std::set<const StructDecl*> seen;
        for (auto& name : sd->baseNames) {
            StructDecl* base = program.findStruct(name.name);
            if (!base) {
                std::vector<std::string> classes;
                for (auto& other : program.structs)
                    if (other->isClass) classes.push_back(other->name);
                std::string guess = closest(name.name, classes);
                diags_.error(name.loc, "there is no class named " + q(name.name),
                             guess.empty() ? "define it in this file, in any order" : "did you mean " + q(guess) + "?",
                             spec::Inheritance);
            } else if (!base->isClass) {
                diags_.error(name.loc, "a class can only derive from a class, but " + q(base->name) + " is a struct",
                             comesWithTheLanguage(*base) ? std::string("hold it in a field") : "make it a class, or hold it in a field", spec::Inheritance);
            } else if (sd->isInterface && !base->isInterface) {
                diags_.error(name.loc, "an interface can only derive from an interface, but " + q(base->name) + " is a class",
                             "make " + q(sd->name) + " a class", spec::Interfaces);
            } else if (!seen.insert(base).second) {
                diags_.error(name.loc, q(sd->name) + " derives from " + q(base->name) + " twice",
                             "name each base class once", spec::Inheritance);
            } else {
                sd->bases.push_back(base);
            }
        }
    }
    // A class cannot be its own ancestor. The link that closes a circle is cut, so that what
    // follows can walk up from any class.
    std::set<const StructDecl*> open;
    std::function<void(StructDecl&)> walk = [&](StructDecl& sd) {
        if (basesWalked_.count(&sd)) return;
        open.insert(&sd);
        for (size_t i = 0; i < sd.bases.size();) {
            StructDecl* base = sd.bases[i];
            if (open.count(base)) {
                diags_.error(sd.nameLoc, "class " + q(sd.name) + " would be its own base class, through " + q(base->name),
                             base == &sd ? "remove `: public " + sd.name + "`"
                                         : "remove one of the two `: public ...`, since a class can only derive from one that does not derive from it",
                             spec::Inheritance);
                sd.bases.erase(sd.bases.begin() + i);
                continue;
            }
            walk(*base);
            ++i;
        }
        open.erase(&sd);
        basesWalked_.insert(&sd);
    };
    for (size_t i = first; i < end; ++i) walk(*program.structs[i]);

    std::function<void(StructDecl&)> visit = [&](StructDecl& sd) {
        if (!overridesDone_.insert(&sd).second) return;
        for (StructDecl* base : sd.bases) visit(*base);
        checkOverrides(sd);
    };
    for (size_t i = first; i < end; ++i)
        if (program.structs[i]->defined) visit(*program.structs[i]);
}

// Works out the virtual methods of a class, whose base classes are already done, and checks
// that what it declares fits with what it inherits.
void Sema::checkOverrides(StructDecl& sd) {
    if (!sd.bases.empty()) sd.vtable = sd.bases[0]->vtable;
    auto inherited = [&](const std::string& name) {  // whatever the base classes have with this name
        std::vector<const StructDecl*> declaring;
        for (const StructDecl* base : sd.bases)
            for (const BasePath& path : base->memberPaths(name)) declaring.push_back(path.back());
        return declaring;
    };
    for (auto& f : sd.fields) {
        std::vector<const StructDecl*> declaring = inherited(f.name);
        if (declaring.empty()) continue;
        const StructDecl& other = *declaring[0];
        diags_.error(f.loc, q(sd.name) + " cannot have a field named " + q(f.name) + ", because its base class " + q(other.name) +
                                (other.fieldIndex(f.name) >= 0 ? " already has one" : " has a method with that name"),
                     other.fieldIndex(f.name) >= 0 ? "use the inherited field, or give this one another name"
                                                   : "give the field another name",
                     spec::Inheritance);
    }
    for (auto& m : sd.methods) {
        if (m->role != FuncDecl::Role::Method) continue;
        std::vector<const StructDecl*> declaring = inherited(m->name);
        if (declaring.empty()) {
            if (m->isOverride) {
                std::vector<std::string> names;
                for (const StructDecl* base : sd.bases)
                    for (const BasePath& place : base->vtablePlaces())
                        for (const FuncDecl* v : place.back()->vtable) names.push_back(v->name);
                std::string guess = closest(m->name, names);
                diags_.error(m->nameLoc, q(m->name) + " is marked `override`, but " +
                                             (sd.bases.empty() ? q(sd.name) + " has no base class"
                                                               : "no base class of " + q(sd.name) + " has a method with that name"),
                             !guess.empty() ? "did you mean " + q(guess) + "?" : "remove `override`", spec::Inheritance);
            }
            if (m->isVirtual) {
                m->vtableSlot = (int)sd.vtable.size();
                sd.vtable.push_back(m.get());
            }
            continue;
        }
        bool fits = true;
        for (const StructDecl* other : declaring) {
            const FuncDecl* method = other->ownMethod(m->name);
            if (!method) {
                diags_.error(m->nameLoc, q(sd.name) + " cannot have a method named " + q(m->name) + ", because its base class " +
                                             q(other->name) + " has a field with that name",
                             "give the method another name", spec::Inheritance);
                fits = false;
                break;
            }
            if (!method->isVirtual) {
                diags_.error(m->nameLoc, q(sd.name) + " cannot have its own " + q(m->name) + ", because " + q(other->name) +
                                             " has a method with that name that is not virtual",
                             "mark the one in " + q(other->name) + " `virtual` and this one `override`, or give this one "
                             "another name",
                             spec::Inheritance);
                diags_.note(method->nameLoc, "the method in the base class is here");
                fits = false;
                break;
            }
            if (m->isAsync != method->isAsync) {
                diags_.error(m->isAsync ? m->asyncLoc : m->nameLoc,
                             m->isAsync ? q(m->name) + " cannot be async, as the method it replaces is not"
                                        : q(m->name) + " must be async, as the method it replaces is",
                             m->isAsync ? "remove `async`, or make the method in " + q(other->name) + " async too"
                                        : "write `async` before its return type: `async " + std::string(m->retType->name) + " " + m->name + "(...)`",
                             spec::Async);
                diags_.note(method->nameLoc, "the method in the base class is here");
                fits = false;
                break;
            }
            bool same = m->retType == method->retType && m->params.size() == method->params.size() &&
                        m->isConst == method->isConst && m->isThrows == method->isThrows;
            for (size_t i = 0; same && i < m->params.size(); ++i)
                same = m->params[i]->declType == method->params[i]->declType;
            if (!same && m->retType != method->retType && m->retType->isClass() && method->retType->isClass() && m->retType->decl->derivesFrom(method->retType->decl)) {
                diags_.error(m->nameLoc, "this compiler does not support covariant return types yet",
                             "return an " + q(method->retType) + ", which can hold the object, and use `dynamic_cast` where the class matters", spec::Inheritance);
                diags_.note(method->nameLoc, "the method in the base class is here");
                fits = false;
                break;
            }
            if (!same) {
                diags_.error(m->nameLoc, q(m->name) + " must have the same parameters, return type, `const` and `throws` as the "
                                             "method it replaces",
                             "the method in " + q(other->name) + " is `" + (method->isAsync ? "async " : "") + method->bodyRet()->name + " " +
                                 signature(*method).substr(other->name.size() + 1) + (method->isConst ? " const" : "") +
                                 (method->isThrows ? " throws" : "") + "`",
                             spec::Inheritance);
                diags_.note(method->nameLoc, "it is here");
                fits = false;
                break;
            }
        }
        if (!fits) continue;
        if (!m->isOverride)
            diags_.error(m->nameLoc, q(m->name) + " replaces the virtual method of " + q(declaring[0]->name) +
                                         ", so it must be marked `override`",
                         "write `override` last, after the parameters and any `const` and `throws`", spec::Inheritance);
        m->isVirtual = true;
        // The slot of the first base class, or a new one when only another base class has it.
        m->vtableSlot = (int)sd.vtable.size();
        for (size_t i = 0; i < sd.vtable.size(); ++i)
            if (sd.vtable[i]->name == m->name) m->vtableSlot = (int)i;
        if (m->vtableSlot == (int)sd.vtable.size()) sd.vtable.push_back(m.get());
        else sd.vtable[m->vtableSlot] = m.get();
    }
}

void Sema::checkBaseWithoutValues(const StructDecl& sd, const StructDecl& base, Loc loc) {
    if (const FuncDecl* ctor = base.constructor) {
        if (!ctor->params.empty())
            diags_.error(loc, "the constructor of " + q(base.name) + " needs arguments, so " + q(sd.name) + " must pass them",
                         "write a constructor that does: `" + sd.name + "(...) : " + base.name + "(...) { }`",
                         spec::Inheritance);
        else if (ctor->access == Access::Private)
            reportAccess(base, base.name + "(...)", true, ctor->access, loc);
        return;  // it builds what is above it
    }
    for (auto& f : base.fields)
        if (!f.defaultValue && needsValue(f.type))
            diags_.error(loc, "field " + q(f.name) + " of the base class " + q(base.name) + " needs a value",
                         "give it a default there, or give " + q(base.name) + " a constructor and pass the value to it",
                         spec::Inheritance);
    for (const StructDecl* above : base.bases) checkBaseWithoutValues(base, *above, loc);
}

BasePath Sema::findQualifier(const std::string& name, const StructDecl& sd, Loc loc) {
    auto s = structs_.find(name);
    const StructDecl* found = s == structs_.end() ? nullptr : s->second;
    if (!found) {  // `Source::f()` for a base class that is `Source<int>`
        std::function<void(const StructDecl&)> search = [&](const StructDecl& d) {
            if (d.templateName == name) found = &d;
            for (const StructDecl* base : d.bases) search(*base);
        };
        search(sd);
    }
    if (!found || !sd.derivesFrom(found)) {
        diags_.error(loc, q(name) + " is not " + q(sd.name) + " or a base class of it",
                     "write the name of one of its base classes, or leave this out. Members that belong to no object, such as static ones, are planned",
                     spec::Inheritance);
        return {};
    }
    if (sd.partsOf(found) > 1) {
        diags_.error(loc, q(sd.name) + " holds " + q(name) + " more than once, so this does not say which is meant",
                     "name the base class that leads to the one you mean, which holds it once", spec::Inheritance);
        return {};
    }
    return sd.pathTo(found);
}

BasePath Sema::findMember(const StructDecl& from, const std::string& name, Loc loc) {
    std::vector<BasePath> paths = from.memberPaths(name);
    if (paths.empty()) return {};
    if (paths.size() > 1) {
        // The first class on each path where they part ways.
        size_t differ = 0;
        while (paths[0][differ] == paths[1][differ]) ++differ;
        const std::string first = paths[0][differ]->name, second = paths[1][differ]->name;
        diags_.error(loc, q(name) + " is ambiguous in " + q(from.name) + ", which has one through " + q(first) + " and one through " +
                              q(second),
                     "say which: `x." + first + "::" + name + "`, or `" + first + "::" + name + "` inside a method. A class can "
                     "also override a virtual method, which then replaces both",
                     spec::Inheritance);
    }
    return paths[0];
}

// What in `t` has no equivalent in C, or "" when C can use it: C knows numbers, chars,
// bools, pointers to those, and structs and arrays of those. `seen` stops at a struct already looked
// at, such as one that points to itself.
static std::string whatCLacks(const Type* t, std::set<const StructDecl*>& seen) {
    if (t->is(K::String)) return "a string";
    if (t->isVector()) return "a vector";
    if (t->isClass()) return "a class reference";
    if (t->isWeak()) return "a weak reference";
    if (t->isUnique()) return "a unique reference";
    if (t->isLocal()) return "a local reference";
    if (t->isOptional()) return "an optional";
    if (t->isArray()) return t->length == 0 ? "an array with no elements" : whatCLacks(t->elem, seen);
    if (t->isPointer()) {
        std::string lacks = t->elem->is(K::Void) ? "" : whatCLacks(t->elem, seen);
        return lacks.empty() || lacks.rfind("field ", 0) == 0 ? lacks : "a pointer to " + lacks;
    }
    if (!t->isStruct() || !seen.insert(t->decl).second) return "";
    for (auto& f : t->decl->fields) {
        std::string lacks = whatCLacks(f.type, seen);
        if (lacks.empty()) continue;
        if (lacks.rfind("field ", 0) == 0) return lacks;
        return "field '" + f.name + "' of '" + t->decl->name + "', which holds " + lacks;
    }
    return "";
}

static const char* kCTypesHelp =
    "C knows numbers, chars, bools, pointers, and structs and arrays of those. For text, use an `array<char, 64>` or "
    "`char*` field and read it with `string(x)`. To pass a C+ value through C without C looking inside, pass its "
    "address as a `void*`";

void Sema::checkStructs(size_t first) {
    // A struct cannot contain itself, directly or through other structs.
    auto& state = structState_;
    std::function<void(const StructDecl*)> visit = [&](const StructDecl* sd) {
        state[sd] = 1;
        for (auto& f : sd->fields) {
            // An optional struct or an array of structs holds the struct itself, so they count too.
            const Type* ft = f.type;
            while (ft->isOptional() || ft->isArray()) ft = ft->elem;
            if (!ft->isStruct()) continue;
            const StructDecl* inner = ft->decl;
            if (state[inner] == 1)
                diags_.error(f.loc,
                             "struct " + q(sd->name) + " contains itself through field " + q(f.name) +
                                 ", so it would be infinitely large",
                             "a struct can hold other structs, but not itself, not even as an optional or in an array. For a "
                             "linked structure, make it a class: a `" + std::string(sd->name) + "? next;` field in a "
                             "class refers to another object instead of holding it",
                             spec::Structs);
            else if (state[inner] == 0)
                visit(inner);
        }
        state[sd] = 2;
    };
    size_t end = program_->structs.size();
    for (size_t i = first; i < end; ++i) {
        StructDecl* sd = program_->structs[i].get();
        if (sd->defined && state[sd] == 0) visit(sd);
    }

    // Field defaults are checked with no local variables in scope.
    for (size_t i = first; i < end; ++i) {
        StructDecl* sd = program_->structs[i].get();
        if (!sd->defined || sd->constraintFailed) continue;
        if (state[sd] == 2 && !sd->isClass) checkSize(&sd->type, sd->nameLoc);
        struct Context {
            Diagnostics& diags;
            bool used;
            ~Context() {
                if (used) diags.popContext();
            }
        } context{diags_, sd->templateOf != nullptr};
        if (sd->templateOf) diags_.pushContext(sd->usedAt, "in " + q(sd->name) + ", which is used here");
        for (auto& f : sd->fields) {
            checkStorable(f.type, f.typeLoc);
            if (sd->externC) {
                std::set<const StructDecl*> seen{sd};
                std::string lacks = whatCLacks(f.type, seen);
                if (!lacks.empty())
                    diags_.error(f.typeLoc, "a C struct cannot have a field of type " + q(f.type) +
                                                ", because C has no equivalent of " + lacks,
                                 kCTypesHelp, spec::CStructs);
            }
            if (!f.defaultValue) continue;
            scopes_.emplace_back();
            Flow flow;
            defaultsOf_ = sd;
            checkValue(*f.defaultValue, flow, f.type, "as the default for field " + q(f.name) + " of " + q(sd->name));
            defaultsOf_ = nullptr;
            scopes_.pop_back();
        }
    }
}

// A C function takes and returns the types that have a C equivalent. A string becomes a
// copy that ends in a zero byte, and a returned `const char*` becomes a string.
void Sema::checkExternFunction(FuncDecl& fn) {
    auto check = [&](const Type* t, Loc loc, bool isReturn) {
        if (!checkStorable(t, loc)) return;
        if (t->isNumber() || t->is(K::Char) || t->is(K::Bool) || t->is(K::String) || t->isError()) return;
        if (t->isOptional() && t->elem->is(K::String)) return;
        if (isReturn && t->is(K::Void)) return;
        if (t->isArray()) {
            diags_.error(loc, isReturn ? "a C function cannot return an array" : "a C function cannot take an array by value",
                         isReturn ? "C returns an array as a pointer to its first element: declare the result as `" +
                                        std::string(t->elem->name) + "*`"
                                  : "C passes an array as a pointer to its first element: declare the parameter as `" +
                                        std::string(t->elem->name) + "*`, and pass `a.data()`",
                         spec::CFunctions);
            return;
        }
        if (t->isPointer() || t->isStruct()) {
            std::set<const StructDecl*> seen;
            std::string lacks = whatCLacks(t, seen);
            if (!lacks.empty())
                diags_.error(loc, std::string("a C function cannot ") + (isReturn ? "return " : "take ") + q(t) +
                                      ", because C has no equivalent of " + lacks,
                             kCTypesHelp, spec::CStructs);
            else if (t->isStruct() && t->decl->fields.empty())
                diags_.error(loc, std::string("a C function cannot ") + (isReturn ? "return " : "take ") + q(t) +
                                      ", because it has no fields, which C does not allow",
                             "add the fields the C struct has", spec::CStructs);
            return;
        }
        if (t->is(K::Void)) {
            diags_.error(loc, "parameters cannot have type void",
                         "a C function with no parameters is written `int rand();` or `int rand(void);`",
                         spec::CFunctions);
            return;
        }
        std::string message = isReturn ? "a C function can return void, a number, char, bool, a pointer, a struct, "
                                         "string or string?, but this is " + q(t)
                                       : "a C function can take a number, char, bool, a pointer, a struct, string or "
                                         "string?, but this parameter is " + q(t);
        std::string help;
        if (t->isObjectRef() || t->isWeak())
            help = "C cannot hold a C+ object; pass the fields it needs as separate arguments";
        else if (t->isVector())
            message = "this compiler does not support passing vectors to or from C functions yet",
            help = "C takes an array as a pointer to its first element. Pass the elements one at a time, or memory from "
                   "C's malloc that holds them";
        else  // only an optional is left, of something that is not text
            help = "in C, only text and pointers can be null. " + std::string(isReturn ? "Declare the result as" : "Pass") +
                   " a plain " + q(t->elem ? t->elem : t) + " instead";
        diags_.error(loc, message, help, spec::CFunctions);
    };
    check(fn.retType, fn.retTypeLoc, true);
    for (auto& p : fn.params) {
        p->type = p->declType;
        check(p->declType, p->typeLoc, false);
    }
}

static int64_t sizeOfType(const Type* t);

Sema::FreshContext::FreshContext(Sema& sema)
    : s(sema), fn(s.fn_), initField(s.initField_), buildingBase(s.buildingBase_), defaultsOf(s.defaultsOf_),
      scopes(std::move(s.scopes_)), loops(std::move(s.loops_)), tryBlocks(s.tryBlocks_), tryPrefixes(s.tryPrefixes_),
      failPoints(s.failPoints_), catches(std::move(s.catches_)) {
    s.fn_ = nullptr;
    s.initField_ = nullptr;
    s.buildingBase_ = nullptr;
    s.defaultsOf_ = nullptr;
    s.scopes_.clear();
    if (!scopes.empty()) s.scopes_.push_back(scopes[0]);  // the constants
    s.loops_.clear();
    s.tryBlocks_ = 0;
    s.tryPrefixes_ = 0;
    s.catches_.clear();
}

Sema::FreshContext::~FreshContext() {
    s.fn_ = fn;
    s.initField_ = initField;
    s.buildingBase_ = buildingBase;
    s.defaultsOf_ = defaultsOf;
    s.scopes_ = std::move(scopes);
    s.loops_ = std::move(loops);
    s.tryBlocks_ = tryBlocks;
    s.tryPrefixes_ = tryPrefixes;
    s.failPoints_ = failPoints;
    s.catches_ = std::move(catches);
}

void Sema::checkNow(FuncDecl& fn) {
    if (fn.checked || fn.isExtern) return;
    FreshContext fresh(*this);
    checkFunction(fn);
}

bool Sema::constraintHolds(const ConstraintExpr& c) {
    if (structs_.count(c.name) || (program_->findTemplate(c.name) && program_->findTemplate(c.name)->kind == TemplateDecl::Kind::Concept)) {
        Loc where;
        return constraintProblem(c.name, c.first, where).empty();
    }
    return builtinConstraint(c.name, c.first, c.second).value_or(false);
}

bool Sema::evaluate(const Expr& e, ConstValue& out, const std::string& what) {
    ConstEval eval(constHooks());
    try {
        out = eval.eval(e);
        return true;
    } catch (ConstError& error) {
        const std::string& m = error.message;
        auto has = [&](const char* piece) { return m.find(piece) != std::string::npos; };
        const std::string help =
            has("integer overflow") && has("'long'") ? "the result does not fit in a long either: use smaller numbers, or a `double` if a close value will do"
            : has("integer overflow") ? "make the value fit: declare it as a `long` and write the numbers as `long`, as in `constexpr long n = 2147483647L + 1;`, or use smaller ones"
            : has("division by zero") ? "change what it divides by to a number that is not 0"
            : has("does not fit in 'char'") ? "use a value from 0 to 255, which is what a char holds"
            : has("does not fit in") ? "use a value that fits, or a wider type such as `long` or `double`"
            : has("shift amount") ? "use a shift of less than the number of bits of the type: 0 to 31 for an int, 0 to 63 for a long"
            : has("takes too long") || has("calls itself too deeply") ? "make the `constexpr` function finish in fewer steps, or leave out `constexpr` to work it out while the program runs"
            : has("reached a `throw`") ? "give the `constexpr` function values it does not fail on, or leave out `constexpr` to work it out while the program runs"
            : has("out of range for a string") ? "use a position from 0 to the size of the string minus 1"
            : "use literals, `const` and `constexpr` values, and calls to `constexpr` functions. For a value only known when the program runs, leave out `constexpr`";
        diags_.error(e.loc, what + ", but " + m, help, spec::Constexpr);
        if (error.loc.line && error.loc.line != e.loc.line) diags_.note(error.loc, "this is what cannot be worked out");
        return false;
    }
}

void Sema::foldConstexpr(VarDecl& v) {
    if (!v.init || !v.type || v.type->isError()) return;
    if (!v.type->isNumber() && !v.type->is(K::Bool) && !v.type->is(K::Char) && !v.type->is(K::String)) {
        diags_.error(v.typeLoc, "this compiler does not support constexpr values of type " + q(v.type) + " yet",
                     "a number, a char, a bool or a string can be constexpr. For a pointer, write `int* const P = ...;`. For a local variable of any other type, write `const`", spec::Status);
        return;
    }
    ConstValue value;
    if (!evaluate(*v.init, value, "the value of " + q(v.name) + " must be known at compile time")) return;
    ConstEval eval(constHooks());
    try {
        value = eval.convert(value, v.type, v.init->loc);
    } catch (ConstError&) {
        return;  // that it does not convert has been reported, when the types were checked
    }
    putLiteral(v, value);
}

// Whether a value of type `t` can be made without being given one: what `vector<T>(n)`
// fills its elements with.
static bool hasDefault(const Type* t) {
    if (t->isStruct()) return !needsValue(t) && (!t->decl->constructor || t->decl->constructor->params.empty());
    return !t->isClass() && !needsValue(t);
}

FuncDecl* Sema::mapMethod(const Type* map, const std::string& name) {
    FuncDecl* m = map->decl->ownMethod(name);
    if (m) requestMethod(m);
    return m;
}

bool Sema::fieldHides(const std::string& name) const {
    const StructDecl* in = owner() ? owner() : defaultsOf_;
    return in && in->fieldOwner(name);
}

void Sema::putLiteral(VarDecl& v, const ConstValue& value) {
    Loc loc = v.init->loc;
    ExprPtr literal;
    switch (value.kind) {
        case ConstValue::Kind::Int: case ConstValue::Kind::Long:
            literal = std::make_unique<IntLitExpr>(loc, value.i, value.kind == ConstValue::Kind::Long);
            break;
        case ConstValue::Kind::Float: case ConstValue::Kind::Double:
            literal = std::make_unique<FloatLitExpr>(loc, value.d, value.kind == ConstValue::Kind::Float);
            break;
        case ConstValue::Kind::Bool: literal = std::make_unique<BoolLitExpr>(loc, value.i != 0); break;
        case ConstValue::Kind::Char: literal = std::make_unique<CharLitExpr>(loc, (uint8_t)value.i); break;
        case ConstValue::Kind::String: literal = std::make_unique<StringLitExpr>(loc, value.s); break;
    }
    literal->type = v.type;
    v.init = std::move(literal);
}

// Whether working out this expression calls a function.
static bool callsFunction(const Expr& e) {
    switch (e.kind) {
        case Expr::Kind::Call: return true;
        case Expr::Kind::Unary: return callsFunction(*static_cast<const UnaryExpr&>(e).operand);
        case Expr::Kind::Binary: {
            auto& b = static_cast<const BinaryExpr&>(e);
            return callsFunction(*b.lhs) || callsFunction(*b.rhs);
        }
        case Expr::Kind::Ternary: {
            auto& t = static_cast<const TernaryExpr&>(e);
            return callsFunction(*t.cond) || callsFunction(*t.thenExpr) || callsFunction(*t.elseExpr);
        }
        case Expr::Kind::Cast: {
            auto& c = static_cast<const CastExpr&>(e);
            return c.operand && callsFunction(*c.operand);
        }
        default: return false;
    }
}

void Sema::foldConstant(VarDecl& v) {
    if (!v.init || !v.type || v.type->isError()) return;
    bool holdsLiteral = v.type->isNumber() || v.type->is(K::Bool) || v.type->is(K::Char) || v.type->is(K::String);
    try {
        if (!holdsLiteral) throw ConstError{v.init->loc, "a value of type " + q(v.type) + " cannot be worked out while compiling"};
        ConstEval eval(constHooks());
        putLiteral(v, eval.convert(eval.eval(*v.init), v.type, v.init->loc));
    } catch (ConstError& error) {
        // Without a call, a value that cannot be worked out, such as `1 / 0`, stops the program when it is used. A
        // call would otherwise run again at every use, so it must be able to run once, here.
        if (callsFunction(*v.init))
            diags_.error(v.init->loc, "a function called for the value of a constant must be able to run while compiling, but " + error.message,
                         "a constant has no moment to run anything while the program runs. Fix the call, or write a function that returns the value",
                         spec::Constants);
    }
}

void Sema::checkIfConstexpr(IfStmt& st, Flow& flow) {
    checkCondition(*st.cond, flow, "condition of if");
    ConstValue value;
    bool taken = false;
    if (evaluate(*st.cond, value, "the condition of `if constexpr` must be known at compile time")) {
        ConstEval eval(constHooks());
        try {
            taken = eval.isTrue(value, st.cond->loc);
        } catch (ConstError& error) {
            diags_.error(st.cond->loc, "the condition of `if constexpr` must be a bool or a number, but " + error.message, "`if constexpr` is for what is known while compiling, such as `Integer<T>` or `N == 0`. For anything else, write `if`", spec::Constexpr);
        }
    }
    if (st.lazy) {
        StmtPtr branch = parser_.readBranch(*st.lazy, taken);
        adoptTypes();
        (taken ? st.thenStmt : st.elseStmt) = std::move(branch);
        st.lazy.reset();
    } else {
        (taken ? st.elseStmt : st.thenStmt).reset();  // not checked, and never generated
    }
    st.resolved = true;
    if (StmtPtr& kept = taken ? st.thenStmt : st.elseStmt) {
        scopes_.emplace_back();
        checkStmt(*kept, flow);
        scopes_.pop_back();
    }
}

ConstHooks Sema::constHooks() {
    ConstHooks hooks;
    hooks.constant = [this](const std::string& name) -> const VarDecl* {
        const VarDecl* decl = lookup(name);
        return decl && decl->isTopLevel && fieldHides(name) ? nullptr : decl;
    };
    hooks.globalConstant = [this](const std::string& name) -> const VarDecl* {
        if (scopes_.empty()) return nullptr;
        auto it = scopes_[0].find(name);
        return it == scopes_[0].end() ? nullptr : it->second;
    };
    hooks.function = [this](const std::string& name) -> const FuncDecl* {
        auto it = funcs_.find(name);
        return it == funcs_.end() ? nullptr : it->second;
    };
    hooks.constraint = [this](const ConstraintExpr& c) { return constraintHolds(c); };
    hooks.sizeOf = [](const Type* t, Loc) { return sizeOfType(t); };
    hooks.prepare = [this](const FuncDecl& f) { checkNow(const_cast<FuncDecl&>(f)); };
    return hooks;
}

void Sema::checkFunction(FuncDecl& fn) {
    prepareAsync(fn);
    if (fn.checked) return;
    fn.checked = true;
    if (fn.isExtern) return checkExternFunction(fn);
    bool generic = fn.owner ? fn.owner->templateOf != nullptr : fn.usedAt.line != 0;
    struct Context {
        Diagnostics& diags;
        bool used;
        ~Context() {
            if (used) diags.popContext();
        }
    } context{diags_, generic};
    if (generic)
        diags_.pushContext(fn.owner ? fn.owner->usedAt : fn.usedAt,
                           "in " + q(fn.owner ? fn.owner->name : fn.name) + ", which is used here");
    fn_ = &fn;
    Flow flow;
    bool isCtor = fn.role == FuncDecl::Role::Constructor;
    if (fn.owner) {
        fn.thisDecl = std::make_unique<VarDecl>(fn.nameLoc, "this", &fn.owner->type, fn.nameLoc);
        fn.thisDecl->type = &fn.owner->type;
        if (!isCtor) markAssigned(flow, fn.thisDecl.get(), "");
    }
    // Parameters and the body's top level share one scope, as in C++.
    scopes_.emplace_back();
    checkStorable(fn.bodyRet(), fn.retTypeLoc);
    for (auto& p : fn.params) {
        checkStorable(p->declType, p->typeLoc);
        if (p->declType->is(K::Void))
            diags_.error(p->typeLoc, "parameters cannot have type void", "write `()` for no parameters, or give the parameter a type", spec::Functions);
        p->type = p->declType;
        declare(*p);
        markAssigned(flow, p.get(), "");
    }
    if (fn.isPure) {  // no body to check
        scopes_.pop_back();
        fn_ = nullptr;
        return;
    }
    if (isCtor) {
        // Fields are initialized in the order they are declared, whatever the order of
        // the list, as in C++, so a value in the list can only read the fields before its own.
        const StructDecl& sd = *fn.owner;
        // The base classes come first, in the order they are declared, whatever the order of
        // the list.
        for (const StructDecl* base : sd.bases) {
            FuncDecl::BaseInit* given = nullptr;
            for (auto& init : fn.baseInits)
                if (init.name == base->name) given = &init;
            if (!given) {
                checkBaseWithoutValues(sd, *base, fn.nameLoc);
                continue;
            }
            std::vector<ConstructArg> args;
            for (auto& a : given->args) args.push_back({"", a->loc, a.get(), nullptr});
            buildingBase_ = base;
            checkConstruction(*base, std::move(args), flow, given->loc, true);
            buildingBase_ = nullptr;
        }
        for (auto& init : fn.baseInits)  // a name that turned out not to be a base class was reported
            if (!structs_.count(init.name) || !sd.derivesFrom(structs_[init.name]))
                for (auto& a : init.args) checkLooseValue(*a, flow);
        for (const StructDecl* base : sd.bases)
            for (const FieldDecl* f : allFields(*base)) markAssigned(flow, fn.thisDecl.get(), f->name);
        size_t ready = 0;  // the fields before this one have their values
        auto readyUpTo = [&](size_t end) {
            for (; ready < end; ++ready) markFieldConstructed(flow, fn.thisDecl.get(), sd.fields[ready], "");
        };
        for (auto& init : fn.memberInits) init.fieldIndex = sd.fieldIndex(init.field);
        std::stable_sort(fn.memberInits.begin(), fn.memberInits.end(),
                         [](auto& a, auto& b) { return a.fieldIndex < b.fieldIndex; });
        std::vector<bool> seen(sd.fields.size(), false);
        for (auto& init : fn.memberInits) {
            if (init.fieldIndex < 0) {
                std::string guess = suggestField(sd, init.field);
                if (sd.fieldOwner(init.field))
                    diags_.error(init.loc, q(init.field) + " belongs to the base class " +
                                               q(sd.fieldOwner(init.field)->name) + ", which sets its own fields",
                                 "pass the value to its constructor, or set the field in the constructor's body",
                                 spec::Inheritance);
                else if (structs_.count(init.field) && sd.isClass)
                    diags_.error(init.loc, q(init.field) + " is not the base class of " + q(sd.name),
                                 sd.bases.empty() ? "remove it, since " + q(sd.name) + " has no base class"
                                 : sd.bases.size() > 1 ? "remove it, or write one of its base classes, such as `" + sd.bases[0]->name + "(...)`"
                                                       : "its base class is " + q(sd.bases[0]->name) + ": write `" + sd.bases[0]->name + "(...)`",
                                 spec::Inheritance);
                else
                    diags_.error(init.loc, q(sd.name) + " has no field named " + q(init.field),
                                 guess.empty() ? listFields(sd, init.field) : "did you mean " + q(guess) + "?", spec::Classes);
                checkLooseValue(*init.value, flow);
                continue;
            }
            const FieldDecl& f = sd.fields[init.fieldIndex];
            readyUpTo(init.fieldIndex);
            if (seen[init.fieldIndex])
                diags_.error(init.loc, "field " + q(f.name) + " is initialized more than once", "give it one value", spec::Classes);
            seen[init.fieldIndex] = true;
            initField_ = &f;
            checkValue(*init.value, flow, f.type, "for field " + q(f.name));
            initField_ = nullptr;
            markAssigned(flow, fn.thisDecl.get(), f.name);
            ready = std::max(ready, (size_t)init.fieldIndex + 1);
        }
        readyUpTo(sd.fields.size());
    }
    for (auto& s : fn.body->stmts) checkStmt(*s, flow);
    if (isCtor) requireFullyBuilt(flow, fn.body->endLoc);
    scopes_.pop_back();

    bool isMain = fn.role == FuncDecl::Role::Free && fn.name == "main";  // falling off main returns 0
    if (flow.reachable && !fn.bodyRet()->is(K::Void) && !isMain) {
        diags_.error(fn.body->endLoc, "function " + q(fn.name) + " can reach its end without returning a value",
                     "add `return <" + std::string(fn.bodyRet()->name) + " value>;` before this closing brace",
                     spec::Functions);
    }
    fn_ = nullptr;
}

// ---- Statements ------------------------------------------------------------

const StructDecl* Sema::errorClass() {
    auto it = structs_.find("Error");
    return it != structs_.end() && it->second->isClass ? it->second : nullptr;
}

// What to do about an error with nowhere to go in the function being checked: mark it, unless it cannot be marked.
std::string Sema::whereAnErrorCanGo() const {
    const std::string handle = "handle it here with `try { ... } catch (Error e) { ... }`";
    if (!fn_) return "call it from a function instead, where the error can be handled";
    if (fn_->role == FuncDecl::Role::Destructor) return handle + ": a destructor cannot fail";
    if (fn_->role == FuncDecl::Role::Constructor) {
        const std::string type = fn_->owner ? fn_->owner->name : "T";
        return handle + ", or write a function that can fail and makes the object: `" + type + " make(...) throws { ...; return " + type + "(...); }`";
    }
    return "mark it: `... " + (fn_->name == "main" ? std::string("main()") : fn_->name.substr(0, fn_->name.find('<')) + "(...)") + " throws { ... }`, or " + handle;
}

void Sema::checkCanFail(const FuncDecl& callee, Loc loc, const Expr* receiver) {
    if (!callee.callFails()) return;
    ++failPoints_;
    if (tryBlocks_ == 0 && tryPrefixes_ == 0) {
        diags_.error(loc, q(callee.name) + " can fail, so its call must say what happens then",
                     [&] {
                         std::string before;  // what the call is on, as written
                         if (receiver && receiver->kind == Expr::Kind::OptValue) {
                             const std::string base = spelled(*static_cast<const OptValueExpr*>(receiver)->chain->base, "");
                             before = base.empty() ? "" : asBase(base) + "?.";
                         } else if (receiver) {
                             const std::string base = spelled(*receiver, "");
                             before = base.empty() ? "" : asBase(base) + ".";
                         }
                         if (receiver && before.empty()) return std::string("write `try` before the call, which passes the error on, or put it in `try { ... } catch (Error e) { ... }`");
                         return "write `try " + before + callee.name + "(...)`, which passes the error on, or put it in `try { ... } catch (Error e) { ... }`";
                     }(),
                     spec::Errors);
    } else if (tryBlocks_ == 0 && (!fn_ || !fn_->isThrows)) {
        diags_.error(loc, "an error from " + q(callee.name) + " has nowhere to go, since " +
                              (fn_ ? q(fn_->name) + " is not marked `throws`" : "this is not in a function"),
                     whereAnErrorCanGo(),
                     spec::Errors);
    }
}

// The call is checked as it is written, as what it needs is worked out here, except that it does not have to say what happens
// if it fails: that goes in the task, for `wait()` to throw. What its arguments do is another matter, as they run here.
const Type* Sema::checkSpawn(SpawnExpr& s, Flow& flow) {
    Expr& call = *s.call;
    if (call.kind == Expr::Kind::Try) {
        diags_.error(call.loc, "`try` belongs on waiting for the task, not on starting it",
                     "write `auto t = spawn f();`, then `try t.wait()`, which is where its error comes out", spec::Concurrency);
        return Type::error();
    }
    if (call.kind != Expr::Kind::Call && call.kind != Expr::Kind::MethodCall) {
        diags_.error(call.loc, "`spawn` runs a call on a thread, and this is not one",
                     "write `spawn f(x)` or `spawn object.method(x)`", spec::Concurrency);
        return Type::error();
    }
    const Expr* outer = spawned_;
    spawned_ = &call;
    const Type* result = checkExpr(call, flow);
    spawned_ = outer;
    if (result->isError()) return result;
    const FuncDecl* target = nullptr;
    if (call.kind == Expr::Kind::Call) {
        auto& c = static_cast<CallExpr&>(call);
        target = c.target ? c.target : c.method;
    } else {
        auto& m = static_cast<MethodCallExpr&>(call);
        if (!m.lowered) target = m.method;
    }
    if (!target || target->isExtern) {
        diags_.error(call.loc, std::string("`spawn` runs a function or method written in C+, and this is ") + (target ? "a C function" : "built in"),
                     "write a function that does this, and spawn that", spec::Concurrency);
        return Type::error();
    }
    // Its count is not safe to work on from two threads. This looks at the types that are passed, and so cannot see a reference
    // that a function keeps somewhere, or that `x.get()` lent: a build with --race catches those.
    auto refuseLocal = [&](const Type* t, Loc loc, const std::string& what) {
        if (t->isError() || !holdsLocal(t)) return false;
        diags_.error(loc, what + " would give another thread a local reference" + (t->isLocal() ? "" : ", which " + q(t) + " holds"),
                     "use a class reference, whose count works across threads, or give the thread a copy of what it needs from the object", spec::Local);
        return true;
    };
    const auto& args = call.kind == Expr::Kind::Call ? static_cast<CallExpr&>(call).args : static_cast<MethodCallExpr&>(call).args;
    bool refused = false;
    for (size_t i = 0; i < args.size(); ++i)
        refused = refuseLocal(args[i]->type ? args[i]->type : Type::error(), args[i]->loc, "this argument") || refused;
    if (call.kind == Expr::Kind::MethodCall) {
        const Expr& recv = *static_cast<MethodCallExpr&>(call).receiver;
        const Expr* inner = recv.kind == Expr::Kind::Borrow ? static_cast<const BorrowExpr&>(recv).operand.get() : &recv;
        refused = refuseLocal(inner->type, recv.loc, "calling a method on this") || refused;
    } else if (static_cast<CallExpr&>(call).method) {
        refused = refuseLocal(&fn_->owner->type, call.loc, "calling a method on `this`") || refused;
    }
    refused = refuseLocal(target->retType, call.loc, "what this call returns") || refused;
    if (refused) return Type::error();
    if (diags_.hasErrors() && !program_->findTemplate("Task")) return Type::error();  // it has a Task of its own, as was reported
    const Type* task = parser_.taskType(target->bodyRet(), target->isThrows, call.loc);
    adoptTypes();
    s.target = target;
    s.task = task->decl;
    auto* decl = const_cast<StructDecl*>(s.task);
    FuncDecl* finish = decl->ownMethod("__finish");
    FuncDecl* fail = decl->ownMethod("__fail");
    requestMethod(finish);
    if (fail) requestMethod(fail);
    s.finish = finish;
    s.fail = fail;
    return task;
}

// An async function's call gives a Task, made here, as soon as the function is known, so that every call sees it. Its
// body still returns what is written: see FuncDecl::bodyRet.
void Sema::prepareAsync(FuncDecl& fn) {
    if (!fn.isAsync || fn.declaredRet) return;
    fn.declaredRet = fn.retType;
    if (fn.retType->isError()) return;
    if (fn.name == "main" && fn.role == FuncDecl::Role::Free && !fn.retType->is(K::Int)) return;  // reported with main
    if (fn.owner && !fn.owner->isClass && !fn.isConst) {
        diags_.error(fn.asyncLoc, "an async method of a struct must be const, since it runs on a copy of the struct",
                     "mark it `const`: `async " + std::string(fn.retType->name) + " " + fn.name + "(...) const`, or make " + q(fn.owner->name) +
                         " a class, whose methods work on the object itself",
                     spec::Async);
        return;
    }
    // Under the rules of async, a function may go on on another thread after it waits, so it holds nothing that cannot cross.
    if (holdsLocal(fn.retType)) {
        diags_.error(fn.retTypeLoc, "an async function cannot return a local reference, since its task can be waited for on another thread",
                     "return a class reference, whose count works across threads", spec::Async);
        return;
    }
    for (auto& p : fn.params)
        if (holdsLocal(p->declType)) {
            diags_.error(p->typeLoc, "an async function cannot take a local reference, since it may go on on another thread after it waits",
                         "take a class reference, whose count works across threads", spec::Async);
            return;
        }
    if (diags_.hasErrors() && !program_->findTemplate("Task")) return;  // it has a Task of its own, as was reported
    const Type* task = parser_.taskType(fn.retType, fn.isThrows, fn.asyncLoc);
    adoptTypes();
    fn.retType = task;
    auto* decl = const_cast<StructDecl*>(task->decl);
    auto use = [&](const char* name) {
        FuncDecl* m = decl->ownMethod(name);
        if (m) requestMethod(m);
        return m;
    };
    fn.taskFinish = use("__finish");
    fn.taskFail = fn.isThrows ? use("__fail") : nullptr;
    fn.taskBind = use("__bind");
    fn.taskWait = use("wait");
}

// What `spawn` and async calls give: a `Task<T>`, or a `Task<T, throws>`.
static bool isTaskClass(const StructDecl& sd) {
    return sd.templateOf && sd.templateOf->nameLoc.line == 0 && (sd.templateName == "Task" || sd.templateName == "__TaskThrows");
}

void Sema::checkTaskKept(const FuncDecl& f, const Expr& call, Loc loc) {
    if (!f.isAsync || call.loc.line == 0 || std::find(discarded_.begin(), discarded_.end(), &call) == discarded_.end()) return;
    const std::string written = spelled(call, f.name + "(...)");
    const std::string tryFirst = f.isThrows ? "try " : "";
    diags_.error(loc, "the task of the async call " + q(f.name) + " is thrown away, so nothing waits for it",
                 (fn_ && fn_->isAsync ? "to wait for it, write `" + tryFirst + "await " + written + ";`"
                                      : "to wait for it here, write `" + tryFirst + asBase(written) + ".wait();`") +
                     ". To let it run on without waiting, keep its task: `auto t = " + written + ";`",
                 spec::Async);
}

const Type* Sema::checkAwait(AwaitExpr& a, Flow& flow) {
    const Type* t = checkExpr(*a.task, flow);
    if (t->isError()) return t;
    const std::string what = spelled(*a.task, "t");
    if (!t->isClass() || !isTaskClass(*t->decl)) {
        const Expr* inner = a.task.get();
        const FuncDecl* called = inner->kind == Expr::Kind::Call ? static_cast<const CallExpr*>(inner)->target
                               : inner->kind == Expr::Kind::MethodCall ? static_cast<const MethodCallExpr*>(inner)->method : nullptr;
        diags_.error(a.task->loc, "`await` waits for a task, and this is " + q(t),
                     called && called->nameLoc.line != 0 && !called->isExtern
                         ? "remove `await`: a call of " + q(called->name) + " is done when it returns. To have it give a task, make it async: `async " +
                               std::string(called->bodyRet()->name) + " " + called->name + "(...)`"
                         : std::string("remove `await`: only a task can be waited for, which an async call or `spawn` gives"),
                     spec::Async);
        return Type::error();
    }
    auto* decl = const_cast<StructDecl*>(t->decl);
    a.taskDecl = decl;
    a.wait = decl->ownMethod("wait");
    a.then = decl->ownMethod("__await");
    if (!a.wait || !a.then) return Type::error();
    requestMethod(const_cast<FuncDecl*>(a.wait));
    requestMethod(const_cast<FuncDecl*>(a.then));
    if (!fn_ || !fn_->isAsync) {
        const bool isMain = fn_ && fn_->role == FuncDecl::Role::Free && fn_->name == "main";
        const bool canBeAsync = fn_ && (fn_->role == FuncDecl::Role::Free || fn_->role == FuncDecl::Role::Method);
        const std::string wait = "`" + std::string(a.wait->isThrows ? "try " : "") + asBase(what) + ".wait()`";
        diags_.error(a.loc, "`await` can only be used in an async function",
                     isMain ? "make main async: `async int main() { ... }`, or wait here with " + wait
                     : canBeAsync ? "mark " + q(fn_->name) + " async: `async " + std::string(fn_->retType->name) + " " + fn_->name + "(...)`, or wait here with " + wait
                                  : "wait here with " + wait,
                     spec::Async);
        return Type::error();
    }
    // The function may go on on another thread, where a local reference cannot be used, nor a lock unlocked. Another async
    // function of this thread could also try to lock it meanwhile.
    for (auto& scope : scopes_)
        for (auto& [name, var] : scope)
            if (var && var->type && var->type->isClass() && var->type->decl->name == "LockGuard" && var->type->decl->nameLoc.line == 0) {
                diags_.error(a.loc, "this `await` comes while " + q(name) + " holds a lock, which would stay locked while other work goes on, and would then be unlocked on what may be another thread",
                             "end the block of " + q(name) + " before the `await`: `{ LockGuard " + name + " = LockGuard(m); ... }`", spec::Async);
                diags_.note(var->loc, q(name) + " is declared here");
                return Type::error();
            }
    for (auto& scope : scopes_)
        for (auto& [name, var] : scope)
            if (var && var->type && !var->type->isError() && holdsLocal(var->type)) {
                diags_.error(a.loc, "this `await` comes while " + q(name) + " holds a local reference, which cannot be kept across it, since the function may go on on another thread",
                             "use a class reference for " + q(name) + ", whose count works across threads, or end its block before the `await`", spec::Async);
                diags_.note(var->loc, q(name) + " is declared here");
                return Type::error();
            }
    if (a.wait->isThrows) {
        ++failPoints_;
        if (tryBlocks_ == 0 && tryPrefixes_ == 0)
            diags_.error(a.loc, "waiting for " + q(what) + " can fail, since what it runs can, so it must say what happens then",
                         "write `try await " + what + "`, which passes the error on, or put it in `try { ... } catch (Error e) { ... }`", spec::Errors);
        else if (tryBlocks_ == 0 && !fn_->isThrows)
            diags_.error(a.loc, "an error from waiting for " + q(what) + " has nowhere to go, since " + q(fn_->name) + " is not marked `throws`",
                         whereAnErrorCanGo(), spec::Errors);
    }
    return a.wait->retType;
}

void Sema::checkThrow(ThrowStmt& st, Flow& flow) {
    if (st.value) {
        const Type* t = checkExpr(*st.value, flow);
        const StructDecl* error = errorClass();
        if (!t->isError() && (!error || !canConvert(t, &error->type)))
            diags_.error(st.value->loc, "only an object of `Error`, or of a class deriving from it, can be thrown, but this is " + q(t),
                         t->is(K::String) ? (st.value->kind == Expr::Kind::StringLit ? "write `throw Error(" + stringLiteral(static_cast<const StringLitExpr&>(*st.value).value) + ");`"
                                                                     : "wrap the string in `Error(...)`: `throw Error(" + spelled(*st.value, "...") + ");`")
                         : t->isOptional() && error && canConvert(t->elem, &error->type)
                             ? "it may be null. Write `throw " + spelled(*st.value, "e") + ".value();`"
                             : "write `throw Error(\"what went wrong\");`, or derive a class from it: `class NotFound : public Error { ... };`",
                         spec::Errors);
    } else if (catches_.empty()) {
        diags_.error(st.loc, "`throw;` passes on the error being handled, so it can only be inside a catch",
                     "write `throw Error(\"what went wrong\");`", spec::Errors);
    } else {
        st.rethrows = catches_.back()->held.get();
    }
    ++failPoints_;
    if (tryBlocks_ == 0 && !fn_->isThrows)
        diags_.error(st.loc, q(fn_->name) + " is not marked `throws`, so an error has nowhere to go",
                     whereAnErrorCanGo(),
                     spec::Errors);
    flow.reachable = false;
}

void Sema::checkTry(TryStmt& st, Flow& flow) {
    // Any call inside can fail before anything else has happened, so a catch only knows what
    // was known before the block.
    Flow before = flow;
    size_t movesBefore = moveLog_.size();
    int failsBefore = failPoints_;
    ++tryBlocks_;
    checkStmt(*st.body, flow);
    --tryBlocks_;
    bool bodyCanFail = failPoints_ != failsBefore;
    int failsInBody = failPoints_ - failsBefore;
    const StructDecl* error = errorClass();
    Flow result = flow;
    bool takesEverything = false;
    std::vector<const CatchClause*> earlier;
    for (auto& c : st.catches) {
        if (takesEverything)
            diags_.error(c.loc, "this catch never runs, since the one before it takes every error",
                         "move it before that one, or remove it", spec::Errors);
        if (c.type) {
            if (!c.type->isClass() || (error && !c.type->decl->isInterface && !c.type->decl->derivesFrom(error))) {
                diags_.error(c.typeLoc, "only `Error` and classes deriving from it are thrown, so a catch cannot take " + q(c.type),
                             "write `catch (Error e)`, or derive a class from it: `class NotFound : public Error { ... };`",
                             spec::Errors);
            } else {
                for (const CatchClause* other : earlier)
                    if (other->type && c.type->decl->derivesFrom(other->type->decl) && other->type->decl->partsOf(c.type->decl) <= 1 &&
                        !takesEverything)
                        diags_.error(c.typeLoc, "this catch never runs, since the one before it takes every " + q(other->type),
                                     other->type == c.type ? "remove one of them" : "put the more specific catch first", spec::Errors);
            }
        }
        c.takesEverything = !c.type || (error && c.type->decl == error);
        takesEverything = takesEverything || c.takesEverything;
        earlier.push_back(&c);
        if (error) {
            c.held = std::make_unique<VarDecl>(c.loc, "error", &error->type, c.loc);
            c.held->type = &error->type;
        }
        Flow branch = before;
        dropMovedSince(branch, movesBefore);  // the call that failed may have come after a move
        scopes_.emplace_back();  // the variable and the body share one scope, as in C++
        if (c.var) {
            c.var->type = c.var->declType;
            declare(*c.var);
            markAssigned(branch, c.var.get(), "");
        }
        catches_.push_back(&c);
        for (auto& s : c.body->stmts) checkStmt(*s, branch);
        catches_.pop_back();
        scopes_.pop_back();
        result = merge(result, branch);
    }
    if (takesEverything) failPoints_ -= failsInBody;  // nothing from the body gets past here
    if (bodyCanFail && !takesEverything && tryBlocks_ == 0 && !fn_->isThrows)
        diags_.error(st.loc, "an error none of these catches takes has nowhere to go, since " + q(fn_->name) + " is not marked `throws`",
                     fn_->role == FuncDecl::Role::Free || fn_->role == FuncDecl::Role::Method
                         ? "add `catch (Error e) { ... }` at the end, or mark " + q(fn_->name) + " `throws`"
                         : "add `catch (Error e) { ... }` at the end", spec::Errors);
    flow = result;
}

void Sema::checkVarDecl(VarDecl& v, Flow& flow) {
    checkStorable(v.declType, v.typeLoc);
    if (v.init) {
        if (v.declType->is(K::Auto)) {
            const Type* t = checkExpr(*v.init, flow);
            rejectCopy(*v.init);
            if (t->is(K::Void))
                diags_.error(v.init->loc, "cannot store the result of a void function in " + q(v.name), "call it on a line of its own: it gives nothing to keep", spec::Variables);
            if (t->isNull())
                diags_.error(v.init->loc, "auto cannot tell what kind of optional " + q(v.name) + " is from null",
                             "write the type, e.g. `User? " + v.name + " = null;`", spec::Optionals);
            v.type = t->is(K::Void) || t->isNull() ? Type::error() : t;
        } else {
            v.type = v.declType;
            checkValue(*v.init, flow, v.type, "to initialize " + q(v.name));
        }
    } else {
        if (v.declType->is(K::Auto))
            diags_.error(v.loc, "auto variable " + q(v.name) + " needs an initial value to infer its type from",
                         "write `auto " + v.name + " = <value>;` or name the type", spec::Variables);
        if (v.isConst)
            diags_.error(v.loc, "const variable " + q(v.name) + " needs an initial value",
                         "write `const " + std::string(v.declType->name) + " " + v.name + " = <value>;`",
                         spec::Variables);
        v.type = v.declType->is(K::Auto) ? Type::error() : v.declType;
    }
    if (v.isConstexpr) foldConstexpr(v);
    declare(v);  // after the initializer: `int x = x;` refers to an outer x, as in C++
    if (v.init) markAssigned(flow, &v, "");
    else markDefaults(flow, &v, v.type, "");
}

// As in C++, a number used as a condition is true when it is not zero, and an optional
// when it holds a value. A `bool?` would be ambiguous, so it is not a condition.
static bool usableAsCondition(const Type* t) {
    return t->isError() || t->is(K::Bool) || t->isNumber() || t->isPointer() ||
           (t->isOptional() && !t->elem->is(K::Bool));
}

// How to read a number of this type from text, which there is no way to do but C's for now.
static std::string readingNumberHelp(const Type* t) {
    if (t->is(K::Float)) return "declare `extern \"C\" double atof(string text);`, then call `float(atof(s))`";
    if (t->isFloating()) return "declare `extern \"C\" double atof(string text);`, then call `atof(s)`";
    if (t->is(K::Long)) return "declare `extern \"C\" long atol(string text);`, then call `atol(s)`";
    return "declare `extern \"C\" int atoi(string text);`, then call `atoi(s)`";
}

// Help for a bool? used as a condition.
// What a condition must be, said so as not to contradict a `bool?`, which is an optional and still not one.
static std::string notACondition(const std::string& start, const Type* t) {
    if (t->isOptional() && t->elem->is(K::Bool))
        return start + " cannot be a `bool?`, as it could mean true or that it holds a value";
    return start + " must be a bool, a number, a pointer or an optional, but this is " + q(t);
}

static std::string boolOptionalHelp(const std::string& name) {
    return "it could mean true, or that it has a value: write `" + name + " == true`, or `" + name +
           " != null` to check that it holds a bool";
}

// Help for something that is not a condition. `negated` is for `!x`, where what to write means the opposite.
static std::string conditionHelp(const Expr& e, const Type* t, bool negated = false) {
    const std::string n = asBase(spelled(e, t->isVector() ? "items" : t->is(K::String) ? "text" : "x"));
    if (t->is(K::Void)) return "this call gives nothing back. Have the function return a bool, or call it on a line of its own and test something else";
    if (t->isWeak()) return negated ? "write `" + n + ".expired()`" : "write `!" + n + ".expired()`, or `if (auto x = " + n + ".lock())`, which also keeps the object alive";
    if (t->isUnique() || t->isLocal()) return std::string("a ") + (t->isUnique() ? "unique" : "local") + " reference is never empty. For one that can be, write `" + std::string(t->name) + "?`";
    if (t->isOptional()) return negated ? "it could mean false, or that it holds nothing: write `" + n + " != true`, or `" + n + " == null` to check that it holds nothing" : boolOptionalHelp(n);
    if (t->is(K::Char)) return negated ? "compare the char instead, e.g. `c == '\\0'`" : "a char is not a condition; compare it, e.g. `c != '\\0'` or `c == 'y'`";
    if (t->isVector() || t->is(K::String) || isHashmap(t))
        return negated ? "write `" + n + ".empty()`" : std::string("to check whether it has ") + (t->is(K::String) ? "any text" : "elements") + ", write `!" + n + ".empty()`";
    if (t->isArray()) return "an array always has its elements. To test one of them, write e.g. `a[0] != 0`";
    if (t->isClass()) return "a class reference is never empty, so there is nothing to test. For one that can be, make it `" + std::string(t->name) + "?`, which is a condition";
    return "write a comparison, or call a method that gives a bool";
}

void Sema::checkCondition(Expr& e, Flow& flow, const char* what) {
    const Type* t = checkExpr(e, flow);
    if (usableAsCondition(t)) return;
    const std::string help = conditionHelp(e, t);
    diags_.error(e.loc, notACondition(std::string("the ") + what, t), help, spec::Conditions);
}

void Sema::checkShiftAmount(const Type* lhsType, const Expr* lhs, const Expr& amount) {
    if (amount.kind != Expr::Kind::IntLit || !lhsType->isInteger()) return;
    int64_t n = static_cast<const IntLitExpr&>(amount).value;
    int64_t bits = lhsType->bits();
    if (n >= 0 && n < bits) return;
    std::string help = "the amount must be from 0 to " + std::to_string(bits - 1) + " for " + q(lhsType);
    if (lhsType->is(K::Int) && n >= 32 && n < 64)
        help += lhs && lhs->kind == Expr::Kind::IntLit
                    ? ". Write the left side as a long with an L suffix, e.g. `" +
                          std::to_string(static_cast<const IntLitExpr*>(lhs)->value) + "L << " + std::to_string(n) + "`"
                    : ". Make the left side a long to shift further";
    diags_.error(amount.loc, "shifting " + q(lhsType) + " by " + std::to_string(n) + " is out of range", help,
                 spec::IntegerOverflow);
}

void Sema::checkLoopBody(Stmt& body, Flow& flow) {
    scopes_.emplace_back();
    checkStmt(body, flow);
    scopes_.pop_back();
}

static bool isConstantTrue(const Expr* e) {
    return !e || (e->kind == Expr::Kind::BoolLit && static_cast<const BoolLitExpr*>(e)->value);
}

void Sema::checkStmt(Stmt& s, Flow& flow) {
    switch (s.kind) {
        case Stmt::Kind::VarDecl: checkVarDecl(static_cast<VarDecl&>(s), flow); return;
        case Stmt::Kind::Error:
            // A statement that failed to parse could have assigned or returned anything,
            // so stop tracking this path rather than report errors it may have caused.
            flow.reachable = false;
            return;
        case Stmt::Kind::DeclGroup:
            for (auto& v : static_cast<DeclGroupStmt&>(s).decls) checkVarDecl(*v, flow);
            return;
        case Stmt::Kind::Expr: {
            std::vector<const Expr*> outer = std::move(discarded_);
            discarded_.clear();
            thrownAway(*static_cast<ExprStmt&>(s).expr, discarded_);  // what it gives goes nowhere
            checkExpr(*static_cast<ExprStmt&>(s).expr, flow);
            discarded_ = std::move(outer);
            return;
        }
        case Stmt::Kind::Block: {
            scopes_.emplace_back();
            for (auto& st : static_cast<BlockStmt&>(s).stmts) checkStmt(*st, flow);
            scopes_.pop_back();
            return;
        }
        case Stmt::Kind::Try: checkTry(static_cast<TryStmt&>(s), flow); return;
        case Stmt::Kind::Throw: checkThrow(static_cast<ThrowStmt&>(s), flow); return;
        case Stmt::Kind::If: {
            auto& st = static_cast<IfStmt&>(s);
            if (st.isConstexpr) {
                checkIfConstexpr(st, flow);
                return;
            }
            if (st.decl) checkCondDecl(*st.decl, flow, "if");
            else checkCondition(*st.cond, flow, "if condition");
            Flow thenFlow = flow;
            scopes_.emplace_back();  // a variable declared in the condition exists in the first branch
            if (st.decl) {
                declare(*st.decl->var);
                markAssigned(thenFlow, st.decl->var.get(), "");
            }
            checkLoopBody(*st.thenStmt, thenFlow);  // a branch gets its own scope
            scopes_.pop_back();
            Flow elseFlow = flow;
            if (st.elseStmt) checkLoopBody(*st.elseStmt, elseFlow);
            flow = merge(thenFlow, elseFlow);
            return;
        }
        case Stmt::Kind::While: {
            auto& st = static_cast<WhileStmt&>(s);
            Flow head = flow;
            size_t movesBefore = moveLog_.size();
            if (st.decl) checkCondDecl(*st.decl, flow, "while");
            else checkCondition(*st.cond, flow, "while condition");
            loops_.emplace_back();
            Flow body = flow;
            scopes_.emplace_back();  // a variable declared in the condition exists in the body
            if (st.decl) {
                declare(*st.decl->var);
                markAssigned(body, st.decl->var.get(), "");
            }
            checkLoopBody(*st.body, body);
            checkMovedInLoop(head, movesBefore, body);
            scopes_.pop_back();
            Flow after = flow;
            // Only a break leaves `while (true)`.
            if (!st.decl && isConstantTrue(st.cond.get())) after.reachable = false;
            for (auto& b : loops_.back().breaks) after = merge(after, b);
            loops_.pop_back();
            flow = after;
            return;
        }
        case Stmt::Kind::DoWhile: {
            auto& st = static_cast<DoWhileStmt&>(s);
            Flow head = flow;
            size_t movesBefore = moveLog_.size();
            loops_.emplace_back();
            checkLoopBody(*st.body, flow);  // the body always runs at least once
            for (auto& c : loops_.back().continues) flow = merge(flow, c);
            checkCondition(*st.cond, flow, "do-while condition");
            checkMovedInLoop(head, movesBefore, flow);
            if (isConstantTrue(st.cond.get())) flow.reachable = false;
            for (auto& b : loops_.back().breaks) flow = merge(flow, b);
            loops_.pop_back();
            return;
        }
        case Stmt::Kind::For: {
            auto& st = static_cast<ForStmt&>(s);
            scopes_.emplace_back();
            if (st.init) checkStmt(*st.init, flow);
            Flow head = flow;
            size_t movesBefore = moveLog_.size();
            if (st.cond) checkCondition(*st.cond, flow, "for loop condition");
            loops_.emplace_back();
            Flow body = flow;
            checkLoopBody(*st.body, body);
            Flow stepFlow = body;
            for (auto& c : loops_.back().continues) stepFlow = merge(stepFlow, c);
            // If the step can never run, stepFlow is unreachable: types are still
            // checked, but definite assignment is not.
            for (auto& step : st.steps) {
                std::vector<const Expr*> outer = std::move(discarded_);
                discarded_.clear();
                thrownAway(*step, discarded_);
                checkExpr(*step, stepFlow);
                discarded_ = std::move(outer);
            }
            checkMovedInLoop(head, movesBefore, stepFlow);
            Flow after = flow;
            if (isConstantTrue(st.cond.get())) after.reachable = false;
            for (auto& b : loops_.back().breaks) after = merge(after, b);
            loops_.pop_back();
            scopes_.pop_back();
            flow = after;
            return;
        }
        case Stmt::Kind::ForEach: checkForEach(static_cast<ForEachStmt&>(s), flow); return;
        case Stmt::Kind::Return: {
            auto& st = static_cast<ReturnStmt&>(s);
            const Type* ret = fn_->bodyRet();
            if (st.value) {
                if (ret->is(K::Void)) {
                    const Type* t = checkExpr(*st.value, flow);
                    if (fn_->role == FuncDecl::Role::Constructor || fn_->role == FuncDecl::Role::Destructor)
                        diags_.error(st.value->loc, std::string("a ") + (fn_->role == FuncDecl::Role::Constructor ? "constructor" : "destructor") + " cannot return a value",
                                     "write just `return;`: it gives back nothing", spec::Classes);
                    else if (t->is(K::Void))
                        diags_.error(st.value->loc, "function " + q(fn_->name) + " returns void, so it cannot return a value",
                                     "call it on a line of its own, then write `return;`", spec::Functions);
                    else if (!t->isError())
                        diags_.error(st.value->loc, "function " + q(fn_->name) + " returns void, so it cannot return a value",
                                     "change the return type to " + q(t) + ", or write just `return;`", spec::Functions);
                } else {
                    // A local variable about to go away hands over what it holds, as in C++.
                    if (st.value->kind == Expr::Kind::Name) {
                        const VarDecl* local = lookup(static_cast<NameExpr&>(*st.value).name);
                        if (local && !local->isTopLevel && !local->isConst && local->type && !isCopyable(local->type)) {
                            std::vector<ExprPtr> args;
                            Loc loc = st.value->loc;
                            args.push_back(std::move(st.value));
                            auto call = std::make_unique<CallExpr>(loc, "move", std::move(args));
                            call->builtin = Builtin::Move;
                            st.value = std::move(call);
                        }
                    }
                    checkValue(*st.value, flow, ret, "as the return value of " + q(fn_->name));
                }
            } else if (!ret->is(K::Void)) {
                diags_.error(s.loc, "function " + q(fn_->name) + " must return " + q(ret) + " here",
                             "write `return <value>;`", spec::Functions);
            }
            if (fn_->role == FuncDecl::Role::Constructor) requireFullyBuilt(flow, s.loc);
            flow.reachable = false;
            return;
        }
        case Stmt::Kind::Break:
        case Stmt::Kind::Continue: {
            bool isBreak = s.kind == Stmt::Kind::Break;
            if (loops_.empty())
                diags_.error(s.loc, std::string(isBreak ? "'break'" : "'continue'") + " can only be used inside a loop", "use it inside `for`, `while` or `do`. To leave a function, write `return`", spec::ControlFlow);
            else if (isBreak)
                loops_.back().breaks.push_back(flow);
            else
                loops_.back().continues.push_back(flow);
            flow.reachable = false;
            return;
        }
    }
}

// `for (T x : range)`: the body runs once for each element, possibly never.
void Sema::checkForEach(ForEachStmt& st, Flow& flow) {
    const Type* rangeType = checkExpr(*st.range, flow);
    const Type* elem = Type::error();
    if (isHashmap(rangeType)) {  // its entries, as they are when the loop starts
        const Type* value = rangeType->decl->templateArgs[1].type;
        if (!isCopyable(value)) {
            diags_.error(st.range->loc, "a range-based for loop copies each entry, but a " + q(value) + " cannot be copied",
                         "loop over the keys: `for (auto key : m.keys()) ... m[key] ...`", spec::Unique);
            rangeType = Type::error();
        } else {
            Loc loc = st.range->loc;
            auto items = std::make_unique<MethodCallExpr>(loc, std::move(st.range), "items", std::vector<ExprPtr>{});
            items->method = mapMethod(rangeType, "items");
            items->path = BasePath{rangeType->decl};
            items->type = rangeType = items->method->retType;
            st.range = std::move(items);
        }
    }
    if (rangeType->isVector() || rangeType->isArray()) {
        elem = rangeType->elem;
    } else if (rangeType->is(K::String)) {
        elem = Type::charTy();  // its bytes, as in C++
    } else if (!rangeType->isError()) {
        std::string n = spelled(*st.range, "items");
        diags_.error(st.range->loc, "a range-based for loop goes over a vector, an array, a hashmap or a string, but this is " + q(rangeType),
                     rangeType->isOptional() && (rangeType->elem->isVector() || rangeType->elem->isArray() || rangeType->elem->is(K::String) || isHashmap(rangeType->elem))
                         ? "it may be null; unwrap it first: `if (auto v = " + n + ") for (auto x : v) ...`"
                         : "write the vector, array or string to loop over after the ':'",
                     spec::RangeFor);
        rangeType = Type::error();
    }
    if (rangeType->elem && !isCopyable(rangeType->elem))
        diags_.error(st.range->loc, "a range-based for loop copies each element, but a " + q(rangeType->elem) + " cannot be copied",
                     "loop over the positions: `for (int i = 0; i < v.size(); i++) ... v[i] ...`", spec::Unique);
    st.rangeVar = std::make_unique<VarDecl>(st.range->loc, "range", rangeType, st.range->loc);
    st.rangeVar->type = rangeType;

    VarDecl& v = *st.var;
    // A map is gone over as `m.items()`, its entries, which are copies whose keys say where each one is
    const Expr* list = st.range.get();
    const bool isMap = list && list->kind == Expr::Kind::MethodCall && static_cast<const MethodCallExpr*>(list)->name == "items" &&
                       static_cast<const MethodCallExpr*>(list)->receiver && static_cast<const MethodCallExpr*>(list)->receiver->type &&
                       isHashmap(static_cast<const MethodCallExpr*>(list)->receiver->type);
    if (isMap) list = static_cast<const MethodCallExpr*>(list)->receiver.get();
    LoopCopy loop{list && namesOnePlace(*list) ? spelled(*list, "") : "", isMap};
    for (const Expr* root = list; root;) {  // what the list is in: another loop's copy, or something const
        if (root->kind == Expr::Kind::Name) {
            const VarDecl* holder = lookup(static_cast<const NameExpr*>(root)->name);
            loop.overACopy = holder && loopCopies_.count(holder);
            if (holder && holder->isConst) loop.constList = holder->name;
            break;
        }
        root = root->kind == Expr::Kind::Member ? static_cast<const MemberExpr*>(root)->base.get()
             : root->kind == Expr::Kind::Index ? static_cast<const IndexExpr*>(root)->base.get()
             : root->kind == Expr::Kind::MethodCall && static_cast<const MethodCallExpr*>(root)->receiver ? static_cast<const MethodCallExpr*>(root)->receiver.get()
             : nullptr;
    }
    loopCopies_[&v] = loop;
    if (v.declType->is(K::Auto)) {
        v.type = elem;
    } else {
        v.type = v.declType;
        const bool overMap = elem->isStruct() && elem->decl->templateOf && elem->decl->templateOf->nameLoc.line == 0 && elem->decl->templateName == "hashmap_entry";
        if (!canConvert(elem, v.type))
            diags_.error(v.typeLoc, "the " + std::string(overMap ? "entries of the hashmap" : "elements of " + q(rangeType)) + " are " + q(elem) + ", which cannot be stored in " +
                                        q(v.type) + " " + q(v.name),
                         overMap ? "write `for (auto " + v.name + " : ...)`, with `" + v.name + ".first` and `" + v.name + ".second`, or loop over `m.keys()`"
                         : "write `for (auto " + v.name + " : ...)` or `for (" + std::string(elem->name) + " " + v.name + " : ...)`",
                         spec::RangeFor);
    }
    scopes_.emplace_back();
    loops_.emplace_back();
    Flow head = flow;
    size_t movesBefore = moveLog_.size();
    Flow body = flow;
    declare(v);
    markAssigned(body, &v, "");
    checkLoopBody(*st.body, body);
    checkMovedInLoop(head, movesBefore, body);
    Flow after = flow;
    for (auto& b : loops_.back().breaks) after = merge(after, b);
    loops_.pop_back();
    scopes_.pop_back();
    flow = after;
}

// ---- Expressions -----------------------------------------------------------

// Help for a value of type `from` where a `to` is expected, when one of them is a pointer.
static bool insideContainer(const Expr& e);

static std::string pointerHelp(const Expr& e, const Type* from, const Type* to, const std::string& fresh = "x") {
    std::string n = spelled(e, "x");
    if (to->isPointer()) {
        if (from->isPointer()) {
            if (from->constElem && !to->constElem && from->elem == to->elem)
                return "a " + q(from) + " only reads what it points to, so it does not convert to " + q(to) +
                       ". Declare the target as `" + from->name + "` too";
            if (from->elem->is(K::Void))
                return "a void* becomes another pointer only with a cast: `static_cast<" + std::string(to->name) + ">(" + n +
                       ")`";
            return "pointers to different types do not convert into each other. If the memory really holds " +
                   q(to->elem) + " values, cast it: `(" + to->name + ")" + (hasOperator(n) ? "(" + n + ")" : n) + "`";
        }
        if (e.kind == Expr::Kind::IntLit && static_cast<const IntLitExpr&>(e).value == 0) return "a pointer to nothing is `null`";
        if (e.kind == Expr::Kind::This) return "a method cannot get a pointer to its own object. Use `this` as a value: a method can return it, which gives a copy of a struct, or the same object of a class";
        if (from == to->elem && isPlaceKind(e) && insideContainer(e))
            return "this compiler does not support pointers into vectors and strings yet, as their elements move when they grow. Copy it into a variable, `auto " + fresh + " = " + n + ";`, and use `&" + fresh + "`";
        if (from == to->elem && isPlaceKind(e)) return "write `&" + n + "` for the address of " + q(n);
        if (from->isInteger()) return "a number is not a pointer. To make one from a number, write `reinterpret_cast<" + std::string(to->name) + ">(n)`";
        if (from->isArray() && from->elem == to->elem) return "an array does not turn into a pointer by itself: write `" + n + ".data()`";
        if (from->is(K::String) && to->elem->is(K::Char))
            return "a string is not a pointer to its bytes. Declare a C function's text parameter as `string`, and C+ "
                   "passes a copy that ends in a zero byte";
        return "";
    }
    if (to->is(K::Bool)) return "compare it with null: `" + n + " != null`";
    if (to->is(K::String) && from->elem->is(K::Char))
        return "write `string(" + n + ")` to copy the text it points to, up to its zero byte";
    if (canConvert(from->elem, to)) return "write `*" + (hasOperator(n) ? "(" + n + ")" : n) + "` for the value it points to";
    if (to->isInteger()) return "a pointer does not convert to a number. For the address it holds, write `reinterpret_cast<long>(" + n + ")`" + (to->is(K::Long) ? "" : ", and make what holds it a `long`, as an address does not fit in an int");
    return "";
}

// What to say about changing what a method gave. In C++ `v.back()` is the element, and here it is a copy. `change` is the rest of what was written.
std::string Sema::copyHelp(const Expr& target, const std::string& change, bool givesValue, const std::string& before) {
    if (const std::string held = constHolderHelp(target); !held.empty()) return held;
    const std::string fallback = "this value is a temporary copy, so changing it would have no effect. Store it in a variable first, change that, and store it back where it came from";
    const Expr* x = &target;
    const std::string path = pathTo(x) + change;
    if (x->kind == Expr::Kind::Ternary) {  // in C++ this changes the variable it chose
        auto& t = static_cast<const TernaryExpr&>(*x);
        for (const Expr* branch : {t.thenExpr.get(), t.elseExpr.get()})
            if (const std::string held = constHolderHelp(*branch); !held.empty()) return held;
        auto placeOf = [](const Expr& branch) {
            if (namesOnePlace(branch)) return asBase(spelled(branch, ""));
            const CopiedFrom from = copiedFrom(branch);
            return from.isInside || !from.map.empty() ? std::string() : from.place;
        };
        const std::string c = spelled(*t.cond, ""), a = placeOf(*t.thenExpr), b = placeOf(*t.elseExpr);
        if (!c.empty() && !a.empty() && !b.empty())
            return givesValue ? "`?:` gives a copy of what it chose. Call it on each in place: `(" + c + " ? " + before + a + path + " : " + before + b + path + ")`"
                              : "`?:` gives a copy of what it chose. Change each in place: `if (" + c + ") " + before + a + path + "; else " + before + b + path + ";`";
        const bool otherIsMadeHere = a.empty() ? !holdsAPlace(*t.thenExpr) : b.empty() ? !holdsAPlace(*t.elseExpr) : false;
        if (!c.empty() && !givesValue && (!a.empty() || !b.empty()) && otherIsMadeHere)  // the other is a value made here, which nothing keeps
            return "`?:` gives a copy of what it chose, so this changes nothing. Change " + q(!a.empty() ? a : b) + " itself when it is chosen: `if (" +
                   (!a.empty() ? c : "!(" + c + ")") + ") " + before + (!a.empty() ? a : b) + path + ";`";
        return "`?:` gives a copy of what it chose. Change each in place, with `if` and `else`";
    }
    if (x->kind == Expr::Kind::Assign && namesOnePlace(*static_cast<const AssignExpr&>(*x).target)) {
        const Expr& assigned = *static_cast<const AssignExpr&>(*x).target;
        if (const std::string held = constHolderHelp(assigned); !held.empty()) return held;
        const std::string place = asBase(spelled(assigned, ""));
        if (givesValue)
            return "`=` gives a copy of what was assigned. Assign first, on a line of its own, `" + place + " = ...;`, and then use `" + before + place + path + "`";
        return "`=` gives a copy of what was assigned. Assign first, then change it in place: `" + place + " = ...; " + before + place + path + ";`";
    }
    if (x->kind != Expr::Kind::MethodCall) return fallback;
    auto& call = static_cast<const MethodCallExpr&>(*x);
    const CopiedFrom from = copiedFrom(*x);
    if (!from.place.empty()) return "`" + call.name + "()` gives a copy. Change it in place: `" + before + from.place + path + "`" + (from.map.empty() ? "" : kKeyMayBeAdded);
    if (call.name == "value" && call.receiver && call.receiver->type && call.receiver->type->isOptional() && namesOnePlace(*call.receiver)) {
        const std::string v = asBase(spelled(*call.receiver, ""));
        if (!v.empty()) {
            const std::string inner = freshName(v + " " + path);
            if (!isCopyable(call.receiver->type))
                return "`value()` gives a copy, which this cannot be. Move it out and back: `auto " + inner + " = move(" + v + ").value(); " + before + inner + path + "; " + v + " = move(" + inner + ");`";
            if (givesValue)
                return "`value()` gives a copy. Take the copy first, `auto " + inner + " = " + v + ".value();`, use `" + before + inner + path + "` in place of this, and store it back after: `" + v + " = " + inner + ";`";
            return "`value()` gives a copy. Unwrap it, change that and store it back: `if (auto " + inner + " = " + v + ") { " + before + inner + path + "; " + v + " = " + inner + "; }`";
        }
    }
    return fallback;
}

void Sema::rejectCopy(const Expr& e) {
    if (!e.type || e.type->isError() || isCopyable(e.type)) return;
    const Expr* x = &e;
    while (x->kind == Expr::Kind::Try) x = static_cast<const TryExpr*>(x)->operand.get();
    if (x->kind == Expr::Kind::Assign) x = static_cast<const AssignExpr*>(x)->target.get();  // `b = (a = make())` would leave two
    if (x->kind == Expr::Kind::MethodCall && static_cast<const MethodCallExpr*>(x)->lowered)
        x = static_cast<const MethodCallExpr*>(x)->lowered.get();  // `m.at(key)` reads what is in the map
    if (!isPlaceKind(*x) && x->kind != Expr::Kind::OptValue) return;
    std::string n = spelled(*x, "");
    if (n.empty())
        diags_.error(x->loc, "this cannot be copied, since a " + q(e.type) + (e.type->isUnique() ? " has one owner" : " holds what has one owner"),
                     "write `move(...)` around it to hand it over, after which it has no value" +
                         std::string(e.type->isUnique() ? ", or `.get()` on it to lend the object" : ""),
                     spec::Unique);
    else {
        const Expr* root = x;  // what the fields are reached from: a call's result is a value of its own, which is not moved from
        while (root->kind == Expr::Kind::Member) root = static_cast<const MemberExpr*>(root)->base.get();
        const bool fromACall = root != x && (root->kind == Expr::Kind::Call || root->kind == Expr::Kind::MethodCall);
        const std::string from = fromACall ? spelled(*root, "") : "";
        diags_.error(x->loc, q(n) + " cannot be copied, since a " + q(e.type) +
                                 (e.type->isUnique() ? " has one owner" : " holds what has one owner"),
                     fromACall && !from.empty()
                         ? "store `" + from + "` in a variable first, and take the field from it: `auto held = " + from + ";`, then `move(held" + n.substr(from.size()) + ")`"
                         : "write `move(" + n + ")` to hand it over, after which " + n + " has no value" +
                               (e.type->isUnique() ? ", or `" + asBase(n) + ".get()` to lend the object, as a `" + e.type->elem->name + "`" : ""),
                     spec::Unique);
    }
}

void Sema::requireConvertible(const Expr& e, const Type* to, const std::string& context) {
    const Type* from = e.type;
    rejectCopy(e);
    if (canConvert(from, to)) return;
    const Type* target = to->isOptional() ? to->elem : to;
    if (target->is(K::Float) && isDoubleConstant(e)) {  // `float f = 1.5;`
        if (std::fabs(static_cast<const FloatLitExpr&>(e).value) > FLT_MAX)
            diags_.error(e.loc, "this number is too large for a float",
                         "the largest float is about 3.4e38; use a double instead", spec::FloatingPointTypes);
        return;
    }
    std::string help;
    if (from->isObjectRef() && to->isUnique()) {
        help = !from->isClass() && canConvert(from->elem, to->elem) ? "write `" + std::string(to->name) + "(...)` to build a new object. A '" + std::string(from->name) + "' cannot change who owns it"
               : from->isClass() ? "a '" + std::string(from->name) + "' may be shared, so it has no one owner. Write `" + std::string(to->name) + "(...)` to build a new object"
                               : "use one that holds a '" + std::string(to->elem->name) + "', as this object is not one";
    } else if (from->isNull() && to->isUnique()) {
        help = "a unique reference is never empty. For one that can be, write `" + std::string(to->name) + "?`";
    } else if (from->isObjectRef() && to->isLocal()) {
        help = !from->isClass() && canConvert(from->elem, to->elem) ? "write `" + std::string(to->name) + "(...)` to build a new object. A '" + std::string(from->name) + "' cannot change who owns it"
               : from->isLocal() ? "use one that holds a '" + std::string(to->elem->name) + "', as this object is not one"
                               : "a '" + std::string(from->name) + "' may be used by other threads, whose count a local reference would upset. Write `" +
                                     std::string(to->name) + "(...)` to build a new object";
    } else if (from->isNull() && to->isLocal()) {
        help = "a local reference is never empty. For one that can be, write `" + std::string(to->name) + "?`";
    } else if (from->isLocal() && (to->isClass() || to->isWeak())) {
        help = "write `" + spelled(e, "x") + ".get()`, which lends the object, and takes care that no other thread gets it";
    } else if (from->isUnique() && to->isWeak()) {
        help = "write `" + spelled(e, "u") + ".get()`, which lends the object";
    } else if (from->isWeak()) {
        help = "the object may be gone. `" + spelled(e, "w") + ".lock()` gives a `" + std::string(from->elem->name) +
               "?`, which is null when it is, and keeps it alive while you use it";
    } else if (from->isClass() && to->isClass() && from->decl->partsOf(to->decl) > 1) {
        const StructDecl* leads = from->decl->bases[0];
        for (const StructDecl* base : from->decl->bases)
            if (base->derivesFrom(to->decl) && base->partsOf(to->decl) == 1) {
                leads = base;
                break;
            }
        help = q(from) + " holds " + q(to) + " more than once. Say which by first casting to the base class that leads to "
               "it, e.g. `static_cast<" + std::string(leads->name) + ">(x)`";
    } else if (from->isPointer() || to->isPointer()) {
        help = pointerHelp(e, from, to, freshName(spelled(e, ""), {"x", "copy", "item", "value"}));
    } else if (from->is(K::Long) && to->is(K::Int)) {
        help = e.kind == Expr::Kind::IntLit ? "this number is larger than an int can hold; use long instead"
                                            : "a long may not fit in an int. Use long here, or write `int(x)`, which "
                                              "stops the program if the value does not fit";
    } else if (target->is(K::Char) && (from->isInteger() || from->is(K::String) || from->isFloating())) {
        if (e.kind == Expr::Kind::StringLit && static_cast<const StringLitExpr&>(e).value.size() == 1)
            help = [&] {
                const std::string c = charLiteral((unsigned char)static_cast<const StringLitExpr&>(e).value[0]);
                return "write " + (c[0] == '\'' ? c + ", in single quotes," : "`" + c + "`") + " for a char. " + stringLiteral(static_cast<const StringLitExpr&>(e).value) + " is a string";
            }();
        else if (from->is(K::String))
            help = "a string does not convert to a char; `s[i]` is the char at position i";
        else
            help = "a number does not convert to a char by itself. Write a character such as 'A', or `char(n)`, "
                   "which stops the program unless n is from 0 to 255. To step to the next character, write "
                   "`c++` or `c += 1`";
    } else if (from->is(K::Char) && target->is(K::String)) {
        help = "write `string(1, c)` for a string holding the char";
    } else if (from->is(K::Char) && target->is(K::Bool)) {
        help = "a char is not a bool; compare it, e.g. `c != '\\0'`";
    } else if (from->isFloating() && target->isInteger()) {
        help = std::string("a ") + from->name + " does not convert to " + (target->is(K::Int) ? "an " : "a ") +
               target->name + " by itself, since that would drop the fraction. Write `" + target->name +
               "(x)` to drop it (rounding toward zero), or `" + target->name + "(round(x))` to round to the nearest";
    } else if (from->is(K::Double) && target->is(K::Float)) {
        help = "a double does not convert to a float by itself, since that loses precision. Write `float(x)`, or "
               "write numbers as floats, e.g. `0.5f`: as in C++, `f * 0.5` is a double";
    } else if (from->is(K::Void)) {
        help = "this call gives nothing back. Call it on a line of its own, or have the function return a value";
    } else if (from->is(K::Bool) && to->isNumber()) {
        help = std::string("a bool does not convert to a number by itself; write `") + to->name + "(flag)` for 0 "
               "or 1";
    } else if (from->isOptional() && !to->isOptional() && canConvert(from->elem, to)) {
        help = spelled(e, "").empty() ? std::string("it may be null. Store it in a variable first and unwrap that with `if (auto v = x) ...`, or give a fallback with `?? value`")
                                      : unwrapHelp(spelled(e, ""));
    } else if (from->isOptional() && to->isOptional()) {
        help = "an optional only converts to an optional of the same type. Unwrap it first: `if (auto v = " +
               spelled(e, "x") + ") ...` gives the " + std::string(from->elem->name) + " inside";
    } else if (from->isNull()) {
        help = "only an optional or a pointer can be null; make the type `" + std::string(to->name) + "?`";
    }
    if (help.empty() && to->isStruct() && to->decl && to->decl->templateOf && to->decl->templateOf->name == "hashmap_entry")
        help = "write each entry as `{key, value}`, in braces of its own, as in `{{\"a\", 1}, {\"b\", 2}}`";
    if (help.empty() && isJsonValue(to) && !from->isError())
        help = "make one with `json::of`: `json::of(" + (spelled(e, "").empty() || spelled(e, "").find("...") != std::string::npos ? std::string("x") : spelled(e, "")) + ")`";
    if (help.empty() && to->is(K::String) && from->isNumber()) help = "turn it into a string: `to_string(x)`";
    if (help.empty() && to->is(K::String) && from->is(K::Bool)) help = "turn it into a string: `format(\"{}\", b)`";
    if (help.empty() && from->is(K::String) && to->isNumber()) help = "this compiler cannot read a number from a string yet: " + readingNumberHelp(to);
    if (help.empty() && to->isStruct() && !isHashmap(to) && from->isNumber() && to->decl->constructor)
        help = comesWithTheLanguage(*to->decl) ? "give it a value such as " + declarationOf(to, "x") : "build one with its constructor: `" + std::string(to->name) + "(...)`";
    if (help.empty() && to->isStruct() && !isHashmap(to) && from->isNumber() && !to->decl->fields.empty() && canConvert(from, to->decl->fields[0].type))
        help = "build one from it: `" + std::string(to->name) + "{" + (e.kind == Expr::Kind::IntLit ? std::to_string(static_cast<const IntLitExpr&>(e).value) : spelled(e, "...")) + "}`";
    if (help.empty() && to->isClass() && from->isClass() && to->decl->derivesFrom(from->decl))
        help = "write `dynamic_cast<" + std::string(to->name) + ">(" + spelled(e, "x") + ")`, which stops the program when it is not one, or test it with `if (auto d = dynamic_cast<" +
               std::string(to->name) + "?>(" + spelled(e, "x") + "))`";
    if (help.empty() && to->isClass() && initField_)  // `: n("x")`, which builds nothing by itself
        help = "build it in the initializer list: `: " + initField_->name + "(" + std::string(to->name) + "(...))`";
    if (help.empty() && to->isClass()) help = "give it a value such as " + declarationOf(to, "x");
    if (help.empty()) help = "write a value of type " + q(to) + " here, or change the type that is expected";
    diags_.error(e.loc, "expected " + q(to) + " " + context + ", but this is " + q(from), help,
                 spec::ImplicitConversions);
}

void Sema::checkValue(Expr& e, Flow& flow, const Type* expected, const std::string& context) {
    if (e.kind == Expr::Kind::InitList && !static_cast<InitListExpr&>(e).explicitType) {
        // `Point? p = {1, 2}` builds the Point the optional holds.
        checkInitList(static_cast<InitListExpr&>(e), flow, expected->isOptional() ? expected->elem : expected);
        return;
    }
    checkExpr(e, flow);
    requireConvertible(e, expected, context);
}

void Sema::checkLooseValue(Expr& e, Flow& flow) {
    // A typeless { } list nested in something already wrong would only repeat the error.
    if (e.kind == Expr::Kind::InitList && !static_cast<InitListExpr&>(e).explicitType) {
        for (auto& el : static_cast<InitListExpr&>(e).elements) checkLooseValue(*el.value, flow);
        e.type = Type::error();
        return;
    }
    checkExpr(e, flow);
}

void Sema::checkInitList(InitListExpr& list, Flow& flow, const Type* type) {
    list.type = type;
    if (isHashmap(type) && !list.elements.empty()) {  // its entries, as `{key, value}`
        const Type* entry = type->decl->fields[hashmapEntriesField(*type->decl)].type->elem;
        // `{1, 2}` or `{"a", 1}`: keys and values written flat, where each entry would be in braces of its own. Only literals are
        // taken for that, as a variable can already be an entry: `{e, {"c", 3}}` in a loop over a map.
        auto isLiteral = [](const Expr& x) {
            const Expr* y = &x;
            if (y->kind == Expr::Kind::Unary && static_cast<const UnaryExpr*>(y)->op == UnOp::Neg) y = static_cast<const UnaryExpr*>(y)->operand.get();
            return y->kind == Expr::Kind::IntLit || y->kind == Expr::Kind::FloatLit || y->kind == Expr::Kind::StringLit || y->kind == Expr::Kind::CharLit || y->kind == Expr::Kind::BoolLit;
        };
        bool flat = !list.elements.empty() && list.elements.size() % 2 == 0;
        for (auto& el : list.elements) flat = flat && el.designator.empty() && isLiteral(*el.value);
        if (flat) {
            std::string example;
            for (size_t i = 0; i + 1 < list.elements.size(); i += 2) {
                const std::string k = spelled(*list.elements[i].value, ""), v = spelled(*list.elements[i + 1].value, "");
                if (k.empty() || v.empty()) { example.clear(); break; }
                example += (example.empty() ? "" : ", ") + std::string("{") + k + ", " + v + "}";
            }
            diags_.error(list.loc, "each entry of a hashmap's { } list is in braces of its own",
                         "write each entry as `{key, value}`: `{" + (example.empty() ? [&] {
                             std::string pairs;
                             for (size_t i = 0; i + 1 < list.elements.size(); i += 2) pairs += std::string(pairs.empty() ? "" : ", ") + "{key, value}";
                             return pairs;
                         }() : example) + "}`", spec::HashMaps);
            for (auto& el : list.elements) checkLooseValue(*el.value, flow);
            list.type = Type::error();
            return;
        }
        for (auto& el : list.elements) {
            if (!el.designator.empty()) {
                diags_.error(el.loc, "a hashmap's { } list holds its entries, not named fields",
                             "write each entry as `{key, value}`, e.g. `{{\"a\", 1}, {\"b\", 2}}`", spec::HashMaps);
                checkLooseValue(*el.value, flow);
                continue;
            }
            checkValue(*el.value, flow, entry, "as an entry of " + q(type));
        }
        list.mapFill = mapMethod(type, "__fill");
        return;
    }
    if (type->isVector()) {  // the elements, in order
        for (auto& el : list.elements) {
            if (!el.designator.empty()) {
                diags_.error(el.loc, "a vector's { } list holds its elements, not named fields",
                             "write the values alone, e.g. `{1, 2, 3}`", spec::Vectors);
                checkLooseValue(*el.value, flow);
                continue;
            }
            checkValue(*el.value, flow, type->elem, "as an element of " + q(type));
        }
        return;
    }
    if (type->isArray()) {  // the first elements, in order; the others get their default
        checkSize(type, list.loc);
        for (auto& el : list.elements) {
            if (!el.designator.empty()) {
                diags_.error(el.loc, "an array's { } list holds its elements, not named fields",
                             "write the values alone, e.g. `{1, 2, 3}`", spec::Arrays);
                checkLooseValue(*el.value, flow);
                continue;
            }
            checkValue(*el.value, flow, type->elem, "as an element of " + q(type));
        }
        int64_t given = (int64_t)list.elements.size();
        if (given > type->length)
            diags_.error(list.elements[type->length].loc,
                         q(type) + " has room for " + std::to_string(type->length) + (type->length == 1 ? " element" : " elements") +
                             ", but this list has " + std::to_string(given),
                         "remove the extra values, or make the array larger: `array<" + std::string(type->elem->name) + ", " +
                             std::to_string(given) + ">`",
                         spec::Arrays);
        else if (given < type->length && needsValue(type))
            diags_.error(list.loc,
                         "this list gives " + std::to_string(given) + " of the " + std::to_string(type->length) +
                             " elements of " + q(type) + ", and the others cannot be made on their own, because " +
                             elementDefaultProblem(type->elem),
                         "list every element", spec::Arrays);
        return;
    }
    if (type->isError() || (!type->hasFields() && !type->isUnique() && !type->isLocal())) {
        if (!type->isError())
            diags_.error(list.loc, "a { } list builds a struct, a class object, a vector or an array, but " + q(type) +
                                       " is expected here",
                         "write the value without braces", spec::Structs);
        list.type = Type::error();
        for (auto& el : list.elements) checkLooseValue(*el.value, flow);
        return;
    }
    std::vector<ConstructArg> args;
    for (auto& el : list.elements) args.push_back({el.designator, el.loc, el.value.get(), &el.fieldIndex});
    checkConstruction(*type->decl, std::move(args), flow, list.loc);  // `unique<Node>{...}` builds a Node
}

void Sema::checkConstruction(const StructDecl& sd, std::vector<ConstructArg> args, Flow& flow, Loc loc, bool asBase) {
    auto loose = [&](const ConstructArg& a) {
        if (!a.checked) checkLooseValue(*a.value, flow);
    };
    auto value = [&](const ConstructArg& a, const Type* expected, const std::string& context) {
        if (a.checked) requireConvertible(*a.value, expected, context);
        else checkValue(*a.value, flow, expected, context);
    };
    if (const FuncDecl* pure = sd.firstPure(); pure && !asBase) {
        for (auto& a : args) loose(a);
        diags_.error(loc, "cannot create a " + q(sd.name) + " object, because its method " + q(pure->name) +
                              (pure->owner == &sd ? "" : " from " + q(pure->owner->name)) + " has no body",
                     "create an object of a class that derives from it and overrides " + q(pure->name), spec::Inheritance);
        return;
    }
    // With a constructor, the values are its arguments, in order.
    if (const FuncDecl* ctor = sd.constructor) {
        if (!canAccess(sd, ctor->access, asBase ? owner() : nullptr)) reportAccess(sd, sd.name + "(...)", true, ctor->access, loc);
        for (auto& a : args)
            if (!a.designator.empty()) {
                diags_.error(a.loc, q(sd.name) + " has a constructor, so its values are passed in order",
                             "pass the values in the order of its constructor, `" + signature(*ctor).substr(sd.name.size() + 1) + "`, as in `" + sd.name + "(...)`",
                             spec::Classes);
                break;
            }
        if (args.size() != ctor->params.size()) {
            for (auto& a : args) loose(a);
            diags_.error(loc, "the constructor of " + q(sd.name) + " takes " + std::to_string(ctor->params.size()) +
                                  " argument" + (ctor->params.size() == 1 ? "" : "s") + " but " +
                                  std::to_string(args.size()) + (args.size() == 1 ? " was" : " were") + " given",
                         "its signature is `" + signature(*ctor).substr(sd.name.size() + 1) + "`", spec::Classes);
            return;
        }
        for (size_t i = 0; i < args.size(); ++i) {
            if (args[i].fieldIndex) *args[i].fieldIndex = (int)i;
            value(args[i], ctor->params[i]->declType,
                  "for parameter " + q(ctor->params[i]->name) + " of the " + q(sd.name) + " constructor");
        }
        return;
    }

    if (!sd.bases.empty() && !args.empty()) {
        for (auto& a : args) loose(a);
        diags_.error(loc, q(sd.name) + " has a base class, so it needs a constructor to take values",
                     sd.bases[0]->isInterface ? "write one: `" + sd.name + "(...) { ... }`"
                                              : "write one: `" + sd.name + "(...) : " + sd.bases[0]->name + "(...) { }`, with `: " + sd.bases[0]->name + "(...)` if the base class needs values",
                     spec::Inheritance);
        return;
    }

    // Without one, the values fill the fields, by position or by name.
    bool anyNamed = false, anyPositional = false;
    for (auto& a : args) (a.designator.empty() ? anyPositional : anyNamed) = true;
    if (anyNamed && anyPositional)
        diags_.error(loc, "a { } list cannot mix named fields (.x = ...) and values given by position",
                     "name every field, or none", spec::Structs);
    std::vector<bool> seen(sd.fields.size(), false);
    for (size_t i = 0; i < args.size(); ++i) {
        auto& a = args[i];
        int idx = -1;
        if (a.designator.empty()) {
            if (anyNamed) {
                // already reported
            } else if (i < sd.fields.size()) {
                idx = (int)i;
            } else if (i == sd.fields.size()) {
                diags_.error(a.fieldIndex ? a.loc : loc,
                             q(sd.name) + " has " + std::to_string(sd.fields.size()) + " field" +
                                 (sd.fields.size() == 1 ? "" : "s") + " but " +
                                 (a.fieldIndex ? "the list has " + std::to_string(args.size()) + " values"
                                               : std::to_string(args.size()) + " values were given"),
                             "remove the values past the last field, " + std::to_string(sd.fields.size()) + " at most. " + listFields(sd), spec::Structs);
            }
        } else {
            idx = sd.fieldIndex(a.designator);
            if (idx < 0) {
                std::string guess = suggestField(sd, a.designator);
                diags_.error(a.loc, q(sd.name) + " has no field named " + q(a.designator),
                             guess.empty() ? listFields(sd, a.designator) : "did you mean " + q(guess) + "?", spec::Structs);
            } else if (seen[idx]) {
                diags_.error(a.loc, "field " + q(a.designator) + " is given more than once", "name each field once", spec::Structs);
                idx = -1;
            }
        }
        if (a.fieldIndex) *a.fieldIndex = idx;
        if (idx >= 0) {
            seen[idx] = true;
            const FieldDecl& f = sd.fields[idx];
            if (!canAccess(sd, f.access, asBase ? owner() : nullptr)) reportAccess(sd, f.name, false, f.access, a.loc);
            value(a, f.type, "for field " + q(f.name) + " of " + q(sd.name));
        } else {
            loose(a);
        }
    }
    if (anyNamed && anyPositional) return;
    for (size_t i = 0; i < sd.fields.size(); ++i) {
        const FieldDecl& f = sd.fields[i];
        if (seen[i] || f.defaultValue || !needsValue(f.type)) continue;
        diags_.error(loc, "field " + q(f.name) + " of " + q(sd.name) + " needs a value, because " +
                              (f.type->isClass() ? "a " + q(f.type) + " reference cannot be empty"
                                                 : q(f.type) + " holds a class reference that cannot be empty"),
                     "give it a value here, or give the field a default in " + q(sd.name) + cycleHelp(f.type, f.name, sd),
                     spec::Classes);
    }
}

const Type* Sema::resolvePlace(Expr& e, Flow& flow, const VarDecl*& var, std::string& path) {
    var = nullptr;
    path.clear();
    if (e.kind == Expr::Kind::Name) {
        auto& n = static_cast<NameExpr&>(e);
        n.decl = lookup(n.name);
        // A member of the type hides a constant of the file, as in C++. A local variable hides both.
        if (n.decl && n.decl->isTopLevel && fieldHides(n.name)) n.decl = nullptr;
        if (!n.decl) {
            // Inside a method, a field can be used by its name alone, as in C++.
            if (StructDecl* o = owner()) {
                if (BasePath found = findMember(*o, n.name, e.loc);
                    !found.empty() && found.back()->fieldIndex(n.name) >= 0) {
                    const StructDecl* declaring = found.back();
                    n.implicitField = declaring->fieldIndex(n.name);
                    n.path = std::move(found);
                    const FieldDecl& f = declaring->fields[n.implicitField];
                    if (!canAccess(*declaring, f.access, o)) reportAccess(*declaring, f.name, false, f.access, e.loc);
                    var = thisVar();
                    path = n.name;
                    return e.type = f.type;
                }
            }
            reportUnknownName(n);
            return e.type = Type::error();
        }
        if (n.decl->isTopLevel) checkConstant(*n.decl);  // another constant may use it first
        var = n.decl;
        return e.type = n.decl->type;
    }
    if (e.kind == Expr::Kind::This) {
        if (!owner()) {
            diags_.error(e.loc, "'this' can only be used inside a method or constructor", "use it inside a method or constructor. A function gets what it needs as parameters", spec::Methods);
            return e.type = Type::error();
        }
        var = thisVar();
        return e.type = &owner()->type;
    }
    if (isDeref(e)) {  // `*p`, or the `p->` of `p->x`: what the pointer points to
        auto& u = static_cast<UnaryExpr&>(e);
        const Type* pt = checkExpr(*u.operand, flow);
        e.type = Type::error();
        if (pt->isError()) return e.type;
        if (!pt->isPointer()) {
            reportNotPointer(u, pt);
            return e.type;
        }
        if (!pointsToKnownType(pt)) {
            reportUnknownPointee(u.loc, pt, u.isArrow() ? "->" : "*");
            return e.type;
        }
        return e.type = pt->elem;
    }
    if (e.kind == Expr::Kind::Index) {
        // An element of a vector or an array always holds a value, so it is not tracked.
        auto& ix = static_cast<IndexExpr&>(e);
        const Type* baseType = isPlaceKind(*ix.base) ? resolvePlace(*ix.base, flow, var, path) : checkExpr(*ix.base, flow);
        if (var && !baseType->isError()) requireAssigned(flow, var, path, baseType, ix.base->loc);
        var = nullptr;
        path.clear();
        if (isHashmap(baseType)) return e.type = checkMapIndex(ix, flow, baseType);
        const Type* indexType = checkExpr(*ix.index, flow);
        e.type = Type::error();
        if (baseType->isError()) return e.type;
        if (baseType->isPointer()) {  // `p[i]` is `*(p + i)`, with no bounds to check
            if (!pointsToKnownType(baseType)) {
                reportUnknownPointee(e.loc, baseType, "[ ]");
                return e.type;
            }
            if (!indexType->isError() && !indexType->isInteger())
                diags_.error(ix.index->loc, "an index into a pointer must be an int or long, but this is " + q(indexType),
                             indexType->is(K::Char)       ? "convert the char to its code first: `int(c)`"
                             : indexType->isFloating() ? "convert it first: `int(x)`"
                                                       : "write an int or a long, such as `p[0]`",
                             spec::Pointers);
            return e.type = baseType->elem;
        }
        if (baseType->isOptional()) {
            std::string n = spelled(*ix.base, "(...)");
            diags_.error(e.loc, q(baseType) + " may be null, so it cannot be indexed directly",
                         baseType->elem->isVector() || baseType->elem->isArray() || baseType->elem->is(K::String) || isHashmap(baseType->elem) || baseType->elem->isPointer()
                             ? !isCopyable(baseType) ? changeForm(*ix.base, "[i] = ...", "To reach an element, ", "")
                               : "unwrap it first: `if (auto " + freshName(n) + " = " + n + ") ... " + freshName(n) + "[i]`. " + changeForm(*ix.base, "[i] = ...", "To change an element, ", "unwrap it, change the copy and store it back")
                             : "remove the `[ ]`: unwrap it, as `" + n + " ?? value`, for the " + q(baseType->elem) + " inside",
                         spec::Optionals);
            return e.type;
        }
        bool str = baseType->is(K::String);
        bool arr = baseType->isArray();
        if (!str && !arr && !baseType->isVector()) {
            diags_.error(e.loc, "'[ ]' picks an element of a vector or an array, or a char of a string, but this is " + q(baseType),
                         baseType->hasFields() ? "read a field with '.', e.g. `p.x`" : "use `[ ]` on a vector, an array, a string, a hashmap or a pointer, which are what can be indexed", spec::Vectors);
            return e.type;
        }
        const char* what = str ? "a string" : arr ? "an array" : "a vector";
        const char* anchor = str ? spec::Strings : arr ? spec::Arrays : spec::Vectors;
        if (!indexType->isError() && !indexType->isInteger())
            diags_.error(ix.index->loc, std::string("an index into ") + what + " must be an int or long, but this is " +
                                            q(indexType),
                         indexType->is(K::Char) ? "convert the char to its code first: `int(c)`"
                         : indexType->isFloating() ? "convert it first: `int(x)`"
                                                   : "write an int or a long, such as `v[0]`",
                         anchor);
        else if (ix.index->kind == Expr::Kind::IntLit && static_cast<IntLitExpr&>(*ix.index).value < 0)
            diags_.error(ix.index->loc, std::string("an index into ") + what + " cannot be negative",
                         str   ? "positions are numbered from 0; the last char is `s[s.size() - 1]` or `s.back()`"
                         : arr ? "elements are numbered from 0; the last one is `a[a.size() - 1]` or `a.back()`"
                               : "elements are numbered from 0; the last one is `v[v.size() - 1]` or `v.back()`",
                         anchor);
        else if (arr && ix.index->kind == Expr::Kind::IntLit && static_cast<IntLitExpr&>(*ix.index).value >= baseType->length)
            diags_.error(ix.index->loc, "index " + std::to_string(static_cast<IntLitExpr&>(*ix.index).value) +
                                            " is past the end of " + q(baseType),
                         baseType->length == 0 ? "make the array larger: this one has no elements"
                                               : "use an index from 0 to " + std::to_string(baseType->length - 1),
                         anchor);
        return e.type = str ? Type::charTy() : baseType->elem;
    }
    if (e.kind != Expr::Kind::Member) return checkExpr(e, flow);

    auto& m = static_cast<MemberExpr&>(e);
    if (!m.qualifier.empty() && m.base->kind == Expr::Kind::This && !owner()) {
        diags_.error(e.loc, q(m.qualifier + "::" + m.field) + " can only be used inside a method of a class that derives from " +
                                q(m.qualifier),
                     "reach it through an object: `x.field`. Members that belong to no object, such as static ones, are planned", spec::Inheritance);
        var = nullptr;
        return e.type = Type::error();
    }
    bool baseIsPlace = isPlaceKind(*m.base);
    const Type* baseType = baseIsPlace ? resolvePlace(*m.base, flow, var, path)
                                       : checkExpr(*m.base, flow);  // e.g. makePoint().x
    e.type = Type::error();
    if (baseType->isError()) {
        var = nullptr;
        return e.type;
    }
    if (baseIsPlace && var && baseType->isClass() && m.base->kind != Expr::Kind::This) {
        // Reading the reference itself; its object's fields are always initialized.
        requireAssigned(flow, var, path, baseType, m.base->loc);
        var = nullptr;
        path.clear();
    }
    if (baseType->isUnique() || baseType->isLocal()) {  // the field of the object it owns or refers to
        if (baseIsPlace && var) requireAssigned(flow, var, path, baseType, m.base->loc);
        bool isVariable = m.base->kind == Expr::Kind::Name && static_cast<NameExpr&>(*m.base).implicitField < 0;
        Loc loc = m.base->loc;
        m.base = std::make_unique<BorrowExpr>(loc, std::move(m.base), !isVariable);
        m.base->type = baseType->elem;
        return resolvePlace(e, flow, var, path);
    }
    if (baseType->isWeak()) {
        std::string n = asBase(spelled(*m.base, "w"));
        diags_.error(e.loc, "the object of a " + q(baseType) + " may be gone, so '." + m.field + "' cannot be used on it directly",
                     "write `" + n + ".lock()?." + m.field + "`, which is null when the object is gone, or `if (auto v = " + n +
                         ".lock()) ... v." + m.field + "`",
                     spec::Weak);
        var = nullptr;
        return e.type;
    }
    if (baseType->isOptional()) {
        std::string n = asBase(spelled(*m.base));
        diags_.error(e.loc, q(baseType) + " may be null, so '." + m.field + "' cannot be used on it directly",
                     "write `" + n + "?." + m.field + "`, which is null when " + n + " is, or unwrap it: `if (auto v = " +
                         n + ") ... v." + m.field + "`",
                     spec::Optionals);
        var = nullptr;
        return e.type;
    }
    if (baseType->isPointer()) {
        std::string n = spelled(*m.base, "p");
        diags_.error(e.loc, q(n) + " is a pointer, so its fields are reached with '->'",
                     "write `" + asBase(n) + "->" + m.field + "`", spec::Pointers);
        var = nullptr;
        return e.type;
    }
    if (!baseType->hasFields()) {
        std::string help;
        bool str = baseType->is(K::String);
        if (str || baseType->isVector() || baseType->isArray())
            help = m.field == "size" || m.field == "length" || m.field == "empty" || m.field == "front" ||
                           m.field == "back"
                       ? q(m.field == "length" && !str ? "size" : m.field) + " is a method; call it with `" +
                             (m.field == "length" && !str ? "size" : m.field) + "()`"
                   : str                  ? "strings have methods like size(), substr(pos, n) and find(text)"
                   : baseType->isArray()  ? "arrays have methods like size(), fill(x) and data(), and elements are read with `a[i]`"
                                          : "vectors have methods like size(), push_back(x) and back(), and elements are read with `v[i]`";
        else
            help = "use a struct or a class here: only they have fields, and " + q(baseType) + " is neither";
        diags_.error(e.loc, q(baseType) + " has no fields, so '." + m.field + "' cannot be used", help,
                     str                     ? spec::Strings
                     : baseType->isVector()  ? spec::Vectors
                     : baseType->isArray()   ? spec::Arrays
                                             : spec::Structs);
        var = nullptr;
        return e.type;
    }
    const StructDecl& sd = *baseType->decl;
    BasePath toStart{&sd};
    if (!m.qualifier.empty()) {
        toStart = findQualifier(m.qualifier, sd, e.loc);
        if (toStart.empty()) {
            var = nullptr;
            return e.type;
        }
    }
    BasePath found = findMember(*toStart.back(), m.field, e.loc);
    if (found.empty() || found.back()->fieldIndex(m.field) < 0) {
        std::string help;
        if (const FuncDecl* method = sd.findMethod(m.field)) help = q(m.field) + " is a method; call it with `" + m.field + (method->params.empty() ? "()" : "(...)") + "`";
        else if (std::string guess = suggestField(sd, m.field); !guess.empty()) help = "did you mean " + q(guess) + "?";
        else help = listFields(sd, m.field);
        diags_.error(e.loc, q(sd.name) + " has no field named " + q(m.field), help, spec::Structs);
        var = nullptr;
        return e.type;
    }
    m.fieldOwner = found.back();
    m.path = toStart;
    m.path.insert(m.path.end(), found.begin() + 1, found.end());
    m.fieldIndex = m.fieldOwner->fieldIndex(m.field);
    const FieldDecl& field = m.fieldOwner->fields[m.fieldIndex];
    if (!canAccess(*m.fieldOwner, field.access, &sd)) reportAccess(*m.fieldOwner, m.field, false, field.access, e.loc);
    if (var) path = join(path, m.field);
    return e.type = field.type;
}

// Where the value at a place is stored: in a variable (or `this`), inside a class
// object reached through a reference, in memory a pointer points to, or in a temporary
// such as a function's result.
enum class Storage { Variable, Object, Pointer, Temporary };

// Follows fields and elements from the place `e` back to what holds them. For a
// variable, `root` is set to the Name or This expression it starts from; for memory
// reached through a pointer, to the `*p` or `p[i]` that follows it.
static Storage storageOf(const Expr& e, const Expr*& root) {
    switch (e.kind) {
        case Expr::Kind::Name:
        case Expr::Kind::This: root = &e; return Storage::Variable;
        case Expr::Kind::Member: {
            const Expr& base = *static_cast<const MemberExpr&>(e).base;
            if (base.type && base.type->isClass() && base.kind != Expr::Kind::This) return Storage::Object;
            return storageOf(base, root);
        }
        case Expr::Kind::Index: {
            const Expr& base = *static_cast<const IndexExpr&>(e).base;
            if (base.type && base.type->isPointer()) {
                root = &e;
                return Storage::Pointer;
            }
            return storageOf(base, root);
        }
        case Expr::Kind::Unary:
            if (!isDeref(e)) return Storage::Temporary;
            root = &e;
            return Storage::Pointer;
        default: return Storage::Temporary;
    }
}

// The pointer that the `*p` or `p[i]` found by storageOf follows.
static const Expr& followedPointer(const Expr& root) {
    if (root.kind == Expr::Kind::Index) return *static_cast<const IndexExpr&>(root).base;
    return *static_cast<const UnaryExpr&>(root).operand;
}

// Whether a place lies in the elements of a vector or the bytes of a string, which move
// when the vector grows or the string is copied before a change. The elements of an array
// stay where the array is.
static bool insideContainer(const Expr& e) {
    switch (e.kind) {
        case Expr::Kind::Member: {
            const Expr& base = *static_cast<const MemberExpr&>(e).base;
            return !(base.type && base.type->isClass()) && insideContainer(base);
        }
        case Expr::Kind::Index: {
            const Expr& base = *static_cast<const IndexExpr&>(e).base;
            if (base.type->isArray()) return insideContainer(base);
            return !base.type->isPointer();
        }
        default: return false;
    }
}

// Whether a place lies in a class object that only a temporary refers to, as in
// `makeUser().name`. The object is freed when the statement ends.
static bool inTemporaryObject(const Expr& e) {
    if (e.kind == Expr::Kind::Index) {
        const Expr& base = *static_cast<const IndexExpr&>(e).base;
        return base.type && base.type->isArray() && inTemporaryObject(base);
    }
    if (e.kind != Expr::Kind::Member) return false;
    const Expr& base = *static_cast<const MemberExpr&>(e).base;
    if (base.type && base.type->isClass() && base.kind != Expr::Kind::This) return !isPlaceKind(base);
    return inTemporaryObject(base);
}

static bool containsIndex(const Expr& e) {
    if (e.kind == Expr::Kind::Index) return true;
    return e.kind == Expr::Kind::Member && containsIndex(*static_cast<const MemberExpr&>(e).base);
}

// The vector whose elements a place lies in, written as in the source: `grid` for `grid[i][j].x`.
static std::string outerVectorName(const Expr& e) {
    switch (e.kind) {
        case Expr::Kind::Name: return static_cast<const NameExpr&>(e).name;
        case Expr::Kind::This: return "this";
        case Expr::Kind::Member: {
            auto& m = static_cast<const MemberExpr&>(e);
            if (m.base->kind == Expr::Kind::This) return m.field;
            std::string base = outerVectorName(*m.base);
            return containsIndex(*m.base) ? base : base + "." + m.field;
        }
        case Expr::Kind::Index: return outerVectorName(*static_cast<const IndexExpr&>(e).base);
        default: return "";
    }
}

const VarDecl* Sema::rootVariable(const Expr& root) {
    if (root.kind == Expr::Kind::This) return thisVar();
    auto& n = static_cast<const NameExpr&>(root);
    return n.implicitField >= 0 ? thisVar() : n.decl;
}

// `m[key]`. It finds the entry, as `v[i]` finds an element, so it can be read, assigned to and changed in place.
// As in C++, a key that is missing gets an entry with a default value, unless this is the target of `=`, which
// gives the value itself. A type with no default value has nothing to put there, so then the program stops.
const Type* Sema::checkMapIndex(IndexExpr& ix, Flow& flow, const Type* map) {
    const Type* value = map->decl->templateArgs[1].type;
    checkValue(*ix.index, flow, map->decl->templateArgs[0].type, "as a key of " + q(map));
    if (ix.mapAssign) {
        ix.mapSlot = mapMethod(map, "__place_to_assign");
        return value;
    }
    bool adds = isPlaceKind(*ix.base) && hasDefault(value);
    if (adds && !ix.mapTarget) {
        const Expr* root = nullptr;
        Storage storage = storageOf(*ix.base, root);
        if (storage == Storage::Pointer && followedPointer(*root).type->constElem) {
            std::string n = spelled(*ix.base, "m");
            if (ix.base->kind == Expr::Kind::Unary) n = "(" + n + ")";  // `*p.at(key)` would be `*(p.at(key))`
            const Expr& ptr = followedPointer(*root);
            diags_.error(ix.loc, "'[ ]' adds a key that is missing, so it cannot be used through " + q(spelled(ptr, "the pointer")) + ", a " + q(ptr.type),
                         "to only read, write `" + n + ".at(key)`, which stops the program if the key is missing, or `" + n + ".get(key)`, which gives null",
                         spec::HashMaps);
            constMapReported_ = &ix;
        }
        if (storage == Storage::Variable) {
            const VarDecl* rootVar = rootVariable(*root);
            bool inConstMethod = rootVar == thisVar() && fn_->isConst;
            if (rootVar && (inConstMethod || rootVar->isConst)) {
                std::string n = spelled(*ix.base, "m");
                diags_.error(ix.loc, "'[ ]' adds a key that is missing, so it cannot be used on " +
                                         (inConstMethod ? q(n) + " inside const method " + q(fn_->name) : "the const " + q(n)),
                             "to only read, write `" + n + ".at(key)`, which stops the program if the key is missing, or `" + n +
                                 ".get(key)`, which gives null",
                             spec::HashMaps);
                constMapReported_ = &ix;
            }
        }
    }
    ix.mapSlot = mapMethod(map, adds ? "__place" : "__existing");
    return value;
}

bool Sema::checkChangeable(const Expr& target, const char* action, const VarDecl* var, const std::string& path, const std::string& change, const std::string& before) {
    const Expr* root = nullptr;
    switch (storageOf(target, root)) {
        // A field of a class object can always be changed: the object lives on its
        // own, not inside a temporary. `const` on a variable only stops the variable
        // itself from being reassigned, as with `const` in JavaScript.
        case Storage::Object: return true;
        case Storage::Temporary:
            diags_.error(target.loc, std::string("only a variable or one of its fields can be ") + action,
                         copyHelp(target, change, false, before), spec::Variables);
            return false;
        case Storage::Pointer: {
            const Expr& ptr = followedPointer(*root);
            if (!ptr.type->constElem) return true;
            diags_.error(target.loc,
                         q(spelled(ptr, "the pointer")) + " is a " + q(ptr.type) + ", so what it points to cannot be " +
                             action,
                         "a pointer to const only reads. If what it points to is itself `const`, remove that `const` as well. Declare it as `" + std::string(ptr.type->elem->name) +
                             "*` if it needs to change what it points to",
                         spec::Pointers);
            return false;
        }
        case Storage::Variable: break;
    }
    const VarDecl* rootVar = rootVariable(*root);
    if (!rootVar) return false;  // an unknown name, already reported
    for (const Expr* at = &target; at;) {  // `m[key]` itself was reported, and also what is changed through it
        if (at == constMapReported_) return false;
        at = at->kind == Expr::Kind::Member ? static_cast<const MemberExpr*>(at)->base.get()
             : at->kind == Expr::Kind::Index ? static_cast<const IndexExpr*>(at)->base.get() : nullptr;
    }
    bool element = containsIndex(target);
    bool inString = target.kind == Expr::Kind::Index && static_cast<const IndexExpr&>(target).base->type &&
                    static_cast<const IndexExpr&>(target).base->type->is(K::String);
    const char* parts = inString ? "chars" : "elements";
    if (rootVar == thisVar() && fn_->isConst) {
        std::string what = element ? std::string("the ") + parts + " of " + q(outerVectorName(target))
                                   : q(var ? path : rootVar->name);
        diags_.error(target.loc, what + " cannot be " + action + " inside const method " + q(fn_->name),
                     "remove `const` after the parameter list of " + q(fn_->name) + " if it needs to change fields",
                     spec::Methods);
        return false;
    }
    if (rootVar->isConst) {
        std::string what = element ? std::string("its ") + parts : "its fields";
        if (!element && path.empty())
            diags_.error(target.loc, q(rootVar->name) + " is const and cannot be " + action,
                         rootVar->isTopLevel ? "a constant never changes. Copy it into a local variable and change that, or keep the value in a class object" : "remove `const` from its declaration if it needs to change", spec::Variables);
        else
            diags_.error(target.loc, q(rootVar->name) + " is const, so " + what + " cannot be " + action,
                         rootVar->isTopLevel ? "a constant never changes. Copy it into a local variable and change that, or keep the value in a class object" : "remove `const` from its declaration if it needs to change", spec::Variables);
        diags_.note(rootVar->loc, q(rootVar->name) + " is declared here");
        return false;
    }
    return true;
}

bool Sema::isReadOnlyPlace(const Expr& target) {
    const Expr* root = nullptr;
    switch (storageOf(target, root)) {
        case Storage::Object:
        case Storage::Temporary: return false;
        case Storage::Pointer: return followedPointer(*root).type->constElem;
        case Storage::Variable: break;
    }
    const VarDecl* rootVar = rootVariable(*root);
    if (!rootVar) return false;
    if (rootVar == thisVar() && fn_->isConst) return true;
    return rootVar->isConst;
}

void Sema::reportNotPointer(const UnaryExpr& u, const Type* t) {
    std::string n = spelled(*u.operand, "value");
    std::string help;
    if (u.isArrow()) {
        if (t->isOptional())
            help = "write `" + asBase(n) + "?." + u.arrowMember + "`, which is null when " + n + " is, or unwrap it first: `if (auto v = " +
                   n + ") ... v." + u.arrowMember + "`";
        else if (t->isVector() || t->isArray() || t->is(K::String) || isHashmap(t)) {
            // A method of the container, or a field of what it holds
            static const char* const listMethods[] = {"size", "empty", "push_back", "pop_back", "front", "back", "clear", "data", "fill", "length",
                                                      "substr", "find", "contains", "starts_with", "ends_with"};
            const bool isMethod = isHashmap(t) ? t->decl->findMethod(u.arrowMember) != nullptr || u.arrowMember == "at"
                                               : std::find_if(std::begin(listMethods), std::end(listMethods), [&](const char* m) { return u.arrowMember == m; }) != std::end(listMethods);
            const bool takesNothing = u.arrowMember == "size" || u.arrowMember == "empty" || u.arrowMember == "clear" || u.arrowMember == "pop_back" ||
                                      u.arrowMember == "front" || u.arrowMember == "back" || u.arrowMember == "length" || u.arrowMember == "data" ||
                                      u.arrowMember == "keys" || u.arrowMember == "values" || u.arrowMember == "items";
            const Type* held = isHashmap(t) ? t->decl->templateArgs[1].type : t->elem;
            help = isMethod ? "only a pointer is followed with '->': write `" + n + "." + u.arrowMember + (takesNothing ? "()" : "(...)") + "`"
                 : t->is(K::String) ? q(n) + " is a string, which has no fields: only a pointer is followed with '->'"
                 : held && (held->isNumber() || held->is(K::Char) || held->is(K::Bool) || held->is(K::String))
                     ? "remove `->" + u.arrowMember + "`: " + q(n) + " holds " + q(held) + ", which has no fields, and only a pointer is followed with '->'"
                 : held && (held->isPointer() || held->isOptional() || held->isWeak())
                     ? [&] {
                           const std::string at = asBase(n) + (isHashmap(t) ? "[key]" : "[i]");
                           const bool weakMethod = u.arrowMember == "lock" || u.arrowMember == "expired" || u.arrowMember == "reset";
                           const std::string to = held->isPointer() ? at + "->" + u.arrowMember
                                                : held->isOptional() ? at + "?." + u.arrowMember
                                                : weakMethod ? at + "." + u.arrowMember + "()" : at + ".lock()?." + u.arrowMember;
                           return "only a pointer is followed with '->', and " + q(n) + " holds " + q(held) + ": write `" + to + "`";
                       }()
                 : "only a pointer is followed with '->'. For a field of what it holds, write `" + asBase(n) + (isHashmap(t) ? "[key]." : "[i].") + u.arrowMember + "`";
        } else if (t->hasFields() || t->isUnique() || t->isLocal())
            help = std::string(t->isClass() ? "a class reference is used directly, so " : t->isUnique() ? "a unique reference is used directly, so " : t->isLocal() ? "a local reference is used directly, so " : "") +
                   "fields and methods are reached with '.': write `" + asBase(n) + "." + u.arrowMember + "`";
        else
            help = "remove `->" + u.arrowMember + "`: " + q(n) + " is a " + q(t) + ", which has no fields, and only a pointer is followed with '->'";
        diags_.error(u.loc, "'->' follows a pointer, but " + q(n) + " is " + q(t), help, spec::Pointers);
        return;
    }
    if (t->isOptional())
        help = "the value inside an optional is `" + n + ".value()`, `" + n + " ?? fallback`, or `v` in `if (auto v = " + n +
               ")`";
    else if (u.operand->kind == Expr::Kind::This)
        help = "`this` is used directly: `return this;` gives a copy of a struct, or the same object of a class";
    else if (t->isClass())
        help = "a class reference is used directly: write `" + n + "` without '*'";
    else if (t->isUnique() || t->isLocal())
        help = std::string("a ") + (t->isUnique() ? "unique" : "local") + " reference is used directly: `" + n + ".field`, or `" + n + ".get()` for the object";
    else
        help = "write it without '*': this is already the value, and only a pointer can be followed";
    diags_.error(u.loc, "'*' follows a pointer, but this is " + q(t), help, spec::Pointers);
}

void Sema::reportUnknownPointee(Loc loc, const Type* ptr, const std::string& op) {
    if (ptr->elem->isOpaque())
        diags_.error(loc, "'" + op + "' cannot be used on a " + q(ptr) + ", because " + q(ptr->elem) +
                              " is a C type known only by name",
                     "pass the pointer to the C functions that take it; C+ never sees what is inside", spec::CTypes);
    else
        diags_.error(loc, "'" + op + "' cannot be used on a " + q(ptr) + ", which points to memory of no known type",
                     "convert it first to a pointer to what is stored there, e.g. `static_cast<int*>(p)`", spec::Pointers);
}

bool Sema::checkStorable(const Type* t, Loc loc) {
    const Type* inner = t;
    while (inner->isVector() || inner->isOptional() || inner->isArray() || inner->isPointer()) {
        if (inner->isArray() && !checkSize(inner, loc)) return false;
        if (inner->isPointer() && inner->elem->isOpaque()) return true;  // what such a type is for
        inner = inner->elem;
    }
    if (!inner->isOpaque()) return true;
    diags_.error(loc, q(inner) + " is a C type known only by name, so C+ can only hold a pointer to it",
                 "write `" + std::string(inner->name) + "*`", spec::CTypes);
    return false;
}

namespace {
// How much memory a value takes, as LLVM lays it out on 64-bit targets. Sizes stop
// growing at a value far above the limit, so they cannot overflow.
constexpr uint64_t kMaxValueBytes = (uint64_t)1 << 31;  // 2 GiB
constexpr uint64_t kSizeCap = (uint64_t)1 << 40;

struct Layout {
    uint64_t size = 0, align = 1;
};

// `seen` also stops at a struct that contains itself, which is already reported.
Layout layoutOf(const Type* t, std::unordered_map<const StructDecl*, Layout>& seen) {
    auto capped = [](uint64_t n) { return std::min(n, kSizeCap); };
    switch (t->kind) {
        case K::Bool: case K::Char: return {1, 1};
        case K::Int: case K::Float: return {4, 4};
        case K::String: return {24, 8};
        case K::Array: {
            Layout elem = layoutOf(t->elem, seen);
            uint64_t length = capped((uint64_t)t->length);
            return {elem.size && length > kSizeCap / elem.size ? kSizeCap : elem.size * length, elem.align};
        }
        case K::Optional: {
            if (t->elem->isObjectRef()) return {8, 8};
            Layout elem = layoutOf(t->elem, seen);
            uint64_t end = (elem.align + elem.size + elem.align - 1) / elem.align * elem.align;  // a bool comes first
            return {capped(end), elem.align};
        }
        case K::Struct: {
            auto [it, inserted] = seen.try_emplace(t->decl);
            if (!inserted) return it->second;
            Layout out;
            for (auto& f : t->decl->fields) {
                Layout field = layoutOf(f.type, seen);
                out.size = capped((out.size + field.align - 1) / field.align * field.align + field.size);
                out.align = std::max(out.align, field.align);
            }
            out.size = capped((out.size + out.align - 1) / out.align * out.align);
            return seen[t->decl] = out;
        }
        default: return {8, 8};  // long, double, and everything held by a pointer
    }
}
}  // namespace

// A value larger than this cannot be stored, whatever holds it.
static int64_t sizeOfType(const Type* t) {
    std::unordered_map<const StructDecl*, Layout> seen;
    return (int64_t)layoutOf(t, seen).size;
}

bool Sema::checkSize(const Type* t, Loc loc) {
    std::unordered_map<const StructDecl*, Layout> seen;
    uint64_t size = layoutOf(t, seen).size;
    if (size <= kMaxValueBytes) return true;
    const Type* elem = t;
    while (elem->isArray()) elem = elem->elem;
    diags_.error(loc, q(t) + " is too large: " + (size >= kSizeCap ? "it takes more than 1 TiB" : "it takes " + std::to_string(size) + " bytes") +
                          ", and a value can take at most 2 GiB",
                 t->isArray() ? "a vector can hold more, split among its elements: `vector<" + std::string(elem->name) + ">`"
                              : "hold the large parts in vectors, which keep their elements elsewhere",
                 spec::Arrays);
    return false;
}

// `&x`: the address of a variable, a field, or what a pointer points to.
const Type* Sema::checkAddressOf(UnaryExpr& u, Flow& flow) {
    Expr& target = *u.operand;
    if (target.kind == Expr::Kind::This) {
        checkExpr(target, flow);
        diags_.error(u.loc, "the address of 'this' cannot be taken",
                     "a method cannot get a pointer to its own object. Use `this` as a value: a method can return it, which gives a copy of a struct, or the same object of a class", spec::Pointers);
        return Type::error();
    }
    if (target.kind == Expr::Kind::Name) {
        const VarDecl* found = lookup(static_cast<NameExpr&>(target).name);
        if (found && found->isTopLevel && !fieldHides(found->name)) {
            checkExpr(target, flow);
            diags_.error(u.loc, q(found->name) + " is a constant, which has no address",
                         "copy it into a variable first: `auto " + freshName(found->name, {"copy", "value", "held"}) + " = " + found->name + ";`, then `&" + freshName(found->name, {"copy", "value", "held"}) + "`",
                         spec::Constants);
            return Type::error();
        }
    }
    if (!isPlaceKind(target)) {
        const Type* t = checkExpr(target, flow);
        if (t->isError()) return t;
        diags_.error(u.loc, "'&' takes the address of a variable, a field, or what a pointer points to, but this is a "
                            "temporary value",
                     target.kind == Expr::Kind::OptChain ? "unwrap it first, and take the address of what is inside: `if (auto u = opt) ... &u.x`. For a struct, `u` is a copy, so store it back after writing: `opt = u;`"
                                                         : "store it in a variable first, and take the address of that",
                     spec::Pointers);
        return Type::error();
    }
    const VarDecl* var = nullptr;
    std::string path;
    const Type* t = resolvePlace(target, flow, var, path);
    if (t->isError()) return t;
    if (var && flow.reachable && !isAssigned(flow, var, t, path)) {
        std::string name = placeName(var, path);
        diags_.error(target.loc, "'&" + name + "' points to " + q(name) + ", which has no value yet",
                     (path.empty() ? "give it a value first, e.g. " + declarationOf(t, name) : std::string("assign it first")) +
                         ". C+ cannot tell whether code that gets the pointer writes through it before reading",
                     spec::Pointers);
        markAssigned(flow, var, path);
    }
    if (insideContainer(target)) {
        diags_.error(u.loc, "this compiler does not support pointers into vectors and strings yet",
                     "their elements move when a vector grows or a shared string is changed. Copy the element into a "
                     "variable and take the address of that",
                     spec::Status);
        return Type::error();
    }
    const Expr* root = nullptr;
    if (storageOf(target, root) == Storage::Temporary || inTemporaryObject(target)) {
        diags_.error(u.loc, "'&' takes the address of a variable, a field, or what a pointer points to, but this is "
                            "part of a temporary value",
                     "it goes away when the statement ends. Store it in a variable first, and take the address of that",
                     spec::Pointers);
        return Type::error();
    }
    return Type::pointerTo(t, isReadOnlyPlace(target));
}

// `p + n`, `p - q`, `p == q`, `p < q` and the like, where one side is a pointer.
const Type* Sema::checkPointerBinary(BinaryExpr& b, const Type* l, const Type* r) {
    std::string sym = binOpSpelling(b.op);
    bool comparison = b.op >= BinOp::Eq && b.op <= BinOp::Ge;
    bool equality = b.op == BinOp::Eq || b.op == BinOp::Ne;
    bool leftPtr = l->isPointer();
    const Type* ptr = leftPtr ? l : r;
    const Type* other = leftPtr ? r : l;
    const Expr& ptrSide = leftPtr ? *b.lhs : *b.rhs;
    const Expr& otherSide = leftPtr ? *b.rhs : *b.lhs;
    if (comparison) {
        if (const Type* common = commonPointer(l, r)) {
            if (!equality && other->isNull())
                diags_.error(otherSide.loc, "null can only be compared with == and !=", "write `x == null` or `x != null`", spec::Pointers);
            b.operandType = common;
            return Type::boolTy();
        }
        std::string help;
        if (otherSide.kind == Expr::Kind::IntLit && static_cast<const IntLitExpr&>(otherSide).value == 0)
            help = "a pointer to nothing is `null`: write `" + spelled(ptrSide, "p") + " " + sym + " null`";
        else if (other->isPointer())
            help = "pointers to different types cannot be compared; convert both first, e.g. `(" + std::string(ptr->constElem || other->constElem ? "const void*" : "void*") + ")" +
                   asBase(spelled(ptrSide, "p")) + " " + sym + " (" + (ptr->constElem || other->constElem ? "const void*" : "void*") + ")" + asBase(spelled(otherSide, "q")) + "`";
        else
            help = ptr->elem->is(K::Char) && other->is(K::String) ? "turn the text it points to into a string first: `string(" + spelled(ptrSide, "p") + ") " + sym + " ...`"
                 : canConvert(other, ptr->elem) ? "write `*" + asBase(spelled(ptrSide, "p")) + " " + sym + " ...` to compare the value it points to. A pointer is compared with another pointer to the same type, or with null"
                                                : "compare it with another pointer to the same type, or with null";
        diags_.error(otherSide.loc, "'" + sym + "' compares " + q(ptr) + " with a pointer or null, but this is " + q(other),
                     help, spec::Pointers);
        return Type::boolTy();
    }
    if (b.op == BinOp::Sub && l->isPointer() && r->isPointer()) {
        // How many elements apart two pointers into the same memory are.
        if (l->elem != r->elem)
            diags_.error(b.loc, "'-' between two pointers needs them to point to the same type, but they are " + q(l) +
                                    " and " + q(r), "subtract pointers into one array, which have the same type", spec::Pointers);
        else if (!pointsToKnownType(l))
            reportUnknownPointee(b.loc, l, "-");
        b.operandType = l;
        return Type::longTy();
    }
    bool integer = other->isInteger() || other->is(K::Char);
    if ((b.op == BinOp::Add && integer) || (b.op == BinOp::Sub && leftPtr && integer)) {  // p + n, n + p, p - n
        if (!pointsToKnownType(ptr)) reportUnknownPointee(b.loc, ptr, sym);
        b.operandType = ptr;
        return ptr;
    }
    std::string help = ptr->elem->is(K::Char) && other->is(K::String)
                           ? "turn the text it points to into a string first: `string(" + spelled(ptrSide, "p") + ")`"
                           : "pointers work with `p + n`, `p - n`, `p - q` and comparisons. The value a pointer points to is `*" + asBase(spelled(ptrSide, "p")) + "`";
    if (b.op == BinOp::Sub && !leftPtr && other->isInteger())
        diags_.error(b.loc, "a number cannot have a pointer subtracted from it", "write `p - n`, or `n - *" + asBase(spelled(ptrSide, "p")) + "` for the value it points to", spec::Pointers);
    else if ((b.op == BinOp::Add || b.op == BinOp::Sub) && !other->isPointer())
        diags_.error(otherSide.loc, "'" + sym + "' moves a pointer by an int or a long, but this is " + q(other), help,
                     spec::Pointers);
    else
        diags_.error(b.loc, "'" + sym + "' cannot be used with " + q(ptr), help, spec::Pointers);
    return Type::error();
}

// Checks that `target` can be changed. `var` and `path` are set to the variable and
// field path to track for definite assignment; `var` stays null for a place inside a
// class object or a vector, which always holds a value.
bool Sema::checkAssignable(Expr& target, Flow& flow, const char* action, const VarDecl*& var, std::string& path, const std::string& change, const std::string& before) {
    var = nullptr;
    if (target.kind == Expr::Kind::OptChain) {
        checkExpr(target, flow);
        diags_.error(target.loc, std::string("a value reached through '?.' cannot be ") + action, kChainOnlyReads,
                     spec::Optionals);
        return false;
    }
    if (target.kind != Expr::Kind::Name && target.kind != Expr::Kind::Member && target.kind != Expr::Kind::Index &&
        !isDeref(target)) {
        checkExpr(target, flow);
        std::string help = target.kind == Expr::Kind::This ? "assign to its fields, or to a variable: 'this' always refers to the current object" : copyHelp(target, change, false, before);
        diags_.error(target.loc, std::string("only a variable or one of its fields can be ") + action, help,
                     spec::Variables);
        return false;
    }
    const Type* t = resolvePlace(target, flow, var, path);
    if (t->isError()) return false;
    // In a constructor's initializer list, a field that gets its value later would lose this one.
    if ((initField_ || buildingBase_) && var && var == thisVar() && !path.empty() && !isAssigned(flow, var, t, path)) {
        diags_.error(target.loc, q(path.substr(0, path.find('.'))) + " cannot be " + action + " while " +
                                     (buildingBase_ ? "the base class " + q(buildingBase_->name) + " is being built"
                                                    : q(initField_->name) + " is being set") +
                                     ", because it gets its own value after that, and this one would be lost",
                     "do this in the constructor's body", spec::Classes);
        var = nullptr;
        return false;
    }
    if (throughOptionalChain(target)) {
        diags_.error(target.loc, std::string("a value reached through '?.' cannot be ") + action, kChainOnlyReads,
                     spec::Optionals);
        var = nullptr;
        return false;
    }
    if (!checkChangeable(target, action, var, path, change, before)) {
        var = nullptr;
        return false;
    }
    const Expr* root = nullptr;
    if (storageOf(target, root) == Storage::Object) var = nullptr;
    return true;
}

const Type* Sema::checkExpr(Expr& e, Flow& flow) {
    const Type* result = Type::error();
    switch (e.kind) {
        case Expr::Kind::IntLit: {
            auto& lit = static_cast<IntLitExpr&>(e);
            result = (!lit.isLong && lit.value >= INT32_MIN && lit.value <= INT32_MAX) ? Type::intTy() : Type::longTy();
            break;
        }
        case Expr::Kind::FloatLit:
            result = static_cast<FloatLitExpr&>(e).isFloat ? Type::floatTy() : Type::doubleTy();
            break;
        case Expr::Kind::Cast: result = checkCast(static_cast<CastExpr&>(e), flow); break;
        case Expr::Kind::Borrow: result = e.type; break;  // its operand was checked when it was made
        case Expr::Kind::Constraint: {
            auto& c = static_cast<ConstraintExpr&>(e);
            c.value = constraintHolds(c);
            result = Type::boolTy();
            break;
        }
        case Expr::Kind::Try: {
            if (static_cast<TryExpr&>(e).operand->kind == Expr::Kind::Spawn) {
                diags_.error(e.loc, "`try` belongs on waiting for the task, not on starting it",
                             "write `auto t = spawn f();`, then `try t.wait()`. If an argument can fail, the `try` goes on that: `spawn f(try g())`", spec::Concurrency);
                result = Type::error();
                break;
            }
            ++tryPrefixes_;
            result = checkExpr(*static_cast<TryExpr&>(e).operand, flow);
            --tryPrefixes_;
            break;
        }
        case Expr::Kind::CharLit: result = Type::charTy(); break;
        case Expr::Kind::BoolLit: result = Type::boolTy(); break;
        case Expr::Kind::StringLit: result = Type::stringTy(); break;
        case Expr::Kind::NullLit: result = Type::nullTy(); break;
        case Expr::Kind::OptValue: result = e.type ? e.type : Type::error(); break;  // set by its chain
        case Expr::Kind::OptChain: result = checkOptChain(static_cast<OptChainExpr&>(e), flow); break;
        case Expr::Kind::Coalesce: result = checkCoalesce(static_cast<CoalesceExpr&>(e), flow); break;
        case Expr::Kind::Name:
        case Expr::Kind::This:
        case Expr::Kind::Member:
        case Expr::Kind::Index: {
            const VarDecl* var;
            std::string path;
            result = resolvePlace(e, flow, var, path);
            if (var && !result->isError()) requireAssigned(flow, var, path, result, e.loc);
            break;
        }
        case Expr::Kind::MethodCall: result = checkMethodCall(static_cast<MethodCallExpr&>(e), flow); break;
        case Expr::Kind::InitList: {
            auto& list = static_cast<InitListExpr&>(e);
            if (list.explicitType) {
                checkInitList(list, flow, list.explicitType);
                result = list.type;
            } else {
                diags_.error(e.loc, "this { } list needs a type here",
                             "name the type, e.g. `Point{1, 2}` or `vector<int>{1, 2}`", spec::Structs);
                for (auto& el : list.elements) checkLooseValue(*el.value, flow);
            }
            break;
        }
        case Expr::Kind::Unary: {
            auto& u = static_cast<UnaryExpr&>(e);
            if (u.op == UnOp::Deref) {
                const VarDecl* var;
                std::string path;
                result = resolvePlace(e, flow, var, path);
                break;
            }
            if (u.op == UnOp::AddrOf) {
                result = checkAddressOf(u, flow);
                break;
            }
            const Type* t = checkExpr(*u.operand, flow);
            if (t->isError()) break;
            if (u.op == UnOp::Not) {
                if (!usableAsCondition(t))
                    diags_.error(u.operand->loc, t->isOptional() && t->elem->is(K::Bool) ? notACondition("what '!' tests", t) : "'!' needs a bool, a number, a pointer or an optional, but this is " + q(t),
                                 conditionHelp(*u.operand, t, true),
                                 spec::Conditions);
                result = Type::boolTy();
            } else {
                const char* sym = u.op == UnOp::Neg ? "-" : u.op == UnOp::Plus ? "+" : "~";
                t = promoted(t);  // `-c` and `+c` are ints, as in C++
                if (u.op == UnOp::BitNot ? !t->isInteger() : !t->isNumber()) {
                    diags_.error(u.operand->loc,
                                 std::string("'") + sym + "' needs " + (u.op == UnOp::BitNot ? "an int or a long" : "a number") +
                                     ", but this is " + q(t),
                                 u.op == UnOp::BitNot && t->isFloating() ? std::string("use it on an integer, as it flips bits. Convert first: `~long(x)`")
                                 : t->is(K::Bool) ? std::string("convert it first: `") + sym + "int(b)`"
                                 : t->isOptional() ? unwrapHelp(spelled(*u.operand))
                                 : std::string("'") + sym + "' only works on numbers. Use it on a number", spec::Operators);
                    break;
                }
                result = t;
            }
            break;
        }
        case Expr::Kind::Binary: {
            auto& b = static_cast<BinaryExpr&>(e);
            const char* sym = binOpSpelling(b.op);
            bool comparison = b.op >= BinOp::Eq && b.op <= BinOp::Ge;
            if (b.op == BinOp::LogAnd || b.op == BinOp::LogOr) {
                const Type* l = checkExpr(*b.lhs, flow);
                Flow rhsFlow = flow;  // the right side may not run, so what it assigns does not count
                size_t movesBefore = moveLog_.size();
                const Type* r = checkExpr(*b.rhs, rhsFlow);
                dropMovedSince(flow, movesBefore);
                for (auto [side, t] : {std::pair{b.lhs.get(), l}, std::pair{b.rhs.get(), r}})
                    if (!usableAsCondition(t))
                        diags_.error(side->loc, notACondition(std::string("both sides of '") + sym + "'", t), conditionHelp(*side, t), spec::Conditions);
                b.operandType = Type::boolTy();
                result = Type::boolTy();
                break;
            }
            const Type* l = checkExpr(*b.lhs, flow);
            const Type* r = checkExpr(*b.rhs, flow);
            if (l->isError() || r->isError()) {
                if (comparison) result = Type::boolTy();
                break;
            }
            bool equality = b.op == BinOp::Eq || b.op == BinOp::Ne;
            // Values of the same type can always be compared for equality: strings by
            // contents, structs field by field, and class references by identity.
            if (equality && l->is(K::Void) && r->is(K::Void)) {
                diags_.error(b.lhs->loc, std::string("'") + sym + "' compares two values, but a void function returns none",
                             "call the functions as statements of their own, or have them return a value",
                             spec::Functions);
                result = Type::boolTy();
                break;
            }
            if (equality && l == r && !l->isNumber()) {
                b.operandType = l;
                result = Type::boolTy();
                break;
            }
            if (const Type* common = equality ? commonReference(l, r) : nullptr) {  // a Dog and an Animal
                b.operandType = common;
                result = Type::boolTy();
                break;
            }
            if (l->isPointer() || r->isPointer()) {
                result = checkPointerBinary(b, l, r);
                break;
            }
            // `opt == null` checks for a value; `opt == x` is true when it holds x, as with
            // std::optional.
            if (equality && (l->isOptional() || r->isOptional())) {
                const Type* opt = l->isOptional() ? l : r;
                const Type* other = opt == l ? r : l;
                if (other->isNull() || canConvert(other, opt->elem)) {
                    b.operandType = opt;
                    result = Type::boolTy();
                    break;
                }
                const Expr* side = opt == l ? b.rhs.get() : b.lhs.get();
                diags_.error(side->loc, std::string("'") + sym + "' compares " + q(opt) + " with " + q(opt->elem) +
                                            " or null, but this is " + q(other),
                             "compare it with a value of type " + q(opt->elem) + ", or with null",
                             spec::Optionals);
                result = Type::boolTy();
                break;
            }
            // Strings are joined with + and ordered by their bytes, as in C++.
            bool lStr = l->is(K::String), rStr = r->is(K::String);
            if ((lStr || rStr) && (b.op == BinOp::Add || comparison)) {
                // `s + c` and `c + s` add one character, as with std::string.
                if ((lStr && rStr) || (b.op == BinOp::Add && (l->is(K::Char) || r->is(K::Char)))) {
                    if (!lStr || !rStr) {
                        b.operandType = Type::stringTy();
                        result = Type::stringTy();
                        break;
                    }
                    b.operandType = l;
                    result = comparison ? Type::boolTy() : l;
                    break;
                }
                const Expr* other = lStr ? b.rhs.get() : b.lhs.get();
                const Type* ot = lStr ? r : l;
                auto whole = [](const std::string& written) { return written.find("...") == std::string::npos ? written : std::string(); };  // nothing left out
                const std::string str = whole(spelled(lStr ? *b.lhs : *b.rhs, ""));
                const std::string otherName = whole(spelled(*other, ""));
                auto inOrder = [&](const std::string& fixed) {  // the string where it was, and the other side as it should be
                    if (str.empty()) return fixed + "` in place of `" + otherName;
                    return lStr ? str + " " + sym + " " + fixed : fixed + " " + sym + " " + str;
                };
                if (b.op == BinOp::Add)
                    diags_.error(other->loc, "'+' joins a string with a string or a char, but this is " + q(ot),
                                 ot->isNumber() && fromCharArithmetic(*other) && !otherName.empty()
                                     ? "to add the character it makes, write `" + inOrder("char(" + otherName + ")") + "`; for the number as text, `to_string(" + otherName + ")`"
                                 : ot->isNumber() ? (otherName.empty() ? std::string("turn the number into a string first: put it in `to_string(...)`")
                                                                     : "turn the number into a string first: `" + inOrder("to_string(" + otherName + ")") + "`") +
                                                      ". Unlike C++, a number is never added as a character code"
                                                : textHelp(ot, "+", spelled(*other, "opt")),
                                 spec::Strings);
                else
                    diags_.error(other->loc, std::string("'") + sym + "' compares two strings, but this is " + q(ot),
                                 ot->isNumber()    ? "turn the number into a string first with `to_string(n)`"
                                 : ot->is(K::Char) ? "compare chars with chars, e.g. `s[0] == 'a'`, or strings with "
                                                     "strings: `s == \"a\"`"
                                 : ot->isNull() ? std::string("a string is never null. Write `s.empty()` for no text, or make it `string?`")
                                 : otherName.empty() ? std::string("turn it into a string first: put it in `format(\"{}\", ...)`")
                                 : ot->isOptional() && ot->elem->is(K::String) ? "give it a fallback: `" + inOrder("(" + otherName + " ?? \"\")") + "`"
                                                   : "turn it into a string: `" + inOrder("format(\"{}\", " + otherName + ")") + "`",
                                 spec::Strings);
                if (comparison) result = Type::boolTy();
                break;
            }
            bool integerOnly = b.op == BinOp::Rem || b.op == BinOp::Shl || b.op == BinOp::Shr ||
                               b.op == BinOp::BitAnd || b.op == BinOp::BitOr || b.op == BinOp::BitXor;
            if (integerOnly && l->isArithmetic() && r->isArithmetic() && (l->isFloating() || r->isFloating())) {
                const Expr* side = l->isFloating() ? b.lhs.get() : b.rhs.get();
                diags_.error(side->loc,
                             std::string("'") + sym + "' needs an int or a long, but this is " +
                                 q(l->isFloating() ? l : r),
                             b.op == BinOp::Rem ? "for the remainder of a division of floating-point numbers, write "
                                                  "`fmod(a, b)`, as in C++"
                                                : "bit operations work on integers; convert with `long(x)` first",
                             spec::Operators);
                break;
            }
            if (!l->isArithmetic() || !r->isArithmetic()) {
                const Type* bad = !l->isArithmetic() ? l : r;
                const Expr* side = !l->isArithmetic() ? b.lhs.get() : b.rhs.get();
                std::string help;
                const Type* notNull = l->isNull() ? r : r->isNull() ? l : nullptr;  // `n == null`, of what can never be null
                const Expr* notNullSide = l->isNull() ? b.rhs.get() : b.lhs.get();
                if ((l->isWeak() || r->isWeak()) && (equality || comparison)) help = "to check whether the object is gone, write `w.expired()`. To compare it with an object, write `w.lock() == x`";
                else if (equality && notNull && isJsonValue(notNull))
                    help = "write `" + asBase(spelled(*notNullSide, "doc")) + ".isNull()`, which tests for JSON's null";
                else if (equality && notNull && !notNull->isOptional() && !notNull->isPointer() && !notNull->isError() && !notNull->is(K::Void))
                    help = q(spelled(*notNullSide, "it")) + " is a " + q(notNull) + ", which is never null: remove the test, or declare it `" + std::string(notNull->name) + "?` if it may hold nothing";
                else if (bad->is(K::Void)) help = "this call gives nothing to compare or use. Call it on a line of its own";
                else if (bad->isOptional()) help = unwrapHelp(spelled(*side));
                else if (bad->isNull()) help = "write `x == null` or `x != null`, which are all that null can be used with";
                else if (bad->is(K::String)) help = "use + to join strings, and == != < <= > >= to compare them";
                else if (equality) help = "both sides of '" + std::string(sym) + "' must have the same type, but they are " + q(l) + " and " + q(r) + ". Convert one of them, or compare a field of each";
                else if (comparison && (bad->hasFields() || bad->isVector() || bad->isArray())) help = "use == or !=, which are all that " + q(bad) + " values have, or compare a field or an element of each";
                else if (!bad->isArithmetic() && !bad->is(K::Bool)) help = std::string("'") + sym + "' works on numbers. Use a field or a method of this value that gives one";
                else if (bad->is(K::Bool) && (b.op == BinOp::BitAnd || b.op == BinOp::BitOr)) help = std::string("use '") + (b.op == BinOp::BitAnd ? "&&" : "||") + "' for logic";
                else help = "a bool is not a number. Write `int(b)` for 1 or 0";
                diags_.error(side->loc, std::string("'") + sym + "' cannot be used with " + q(bad), help, spec::Operators);
                if (comparison) result = Type::boolTy();
                break;
            }
            if (b.op == BinOp::Shl || b.op == BinOp::Shr) {
                b.operandType = promoted(l);  // a shift keeps the type of its left side
                result = b.operandType;
                checkShiftAmount(b.operandType, b.lhs.get(), *b.rhs);
            } else {
                b.operandType = commonNumber(l, r);
                result = comparison ? Type::boolTy() : b.operandType;
            }
            break;
        }
        case Expr::Kind::Assign: {
            auto& a = static_cast<AssignExpr&>(e);
            const VarDecl* var = nullptr;
            std::string path;
            if (a.target->kind == Expr::Kind::Index) {
                static_cast<IndexExpr&>(*a.target).mapTarget = true;
                static_cast<IndexExpr&>(*a.target).mapAssign = !a.op;
            }
            bool ok = checkAssignable(*a.target, flow, a.op ? "changed" : "assigned to", var, path, a.op ? " " + std::string(binOpSpelling(*a.op)) + "= ..." : " = ...");
            const Type* t = a.target->type ? a.target->type : Type::error();
            std::string name = var                                 ? q(placeName(var, path))
                               : a.target->kind == Expr::Kind::Member ? "field " + q(static_cast<MemberExpr&>(*a.target).field)
                               : isDeref(*a.target)                   ? q(spelled(*a.target, "*p"))
                                                                      : "this";
            if (a.op) {
                if (ok && var) requireAssigned(flow, var, path, t, a.target->loc);  // `x += v` reads x first
                checkLooseValue(*a.value, flow);
                std::string opName = std::string("'") + binOpSpelling(*a.op) + "='";
                const Type* vt = a.value->type;
                if (t->is(K::String) && *a.op == BinOp::Add) {
                    if (!vt->isError() && !vt->is(K::String) && !vt->is(K::Char))
                        diags_.error(a.value->loc, "'+=' on a string adds a string or a char, but this is " + q(vt),
                                     fromCharArithmetic(*a.value) && !spelled(*a.value, "").empty()
                                         ? "to add the character it makes, write `" + std::string("char(") + spelled(*a.value, "") + ")`; for the number as text, `to_string(" + spelled(*a.value, "") + ")`"
                                         : textHelp(vt, "+=", spelled(*a.value, "opt")),
                                     spec::Strings);
                } else if (t->is(K::Char)) {
                    // `c += 1` and `c -= 1` step through the characters; the result must be a char.
                    if (*a.op != BinOp::Add && *a.op != BinOp::Sub)
                        diags_.error(a.target->loc, opName + " cannot be used with 'char'",
                                     "a char steps with `c += n` and `c -= n`; for other arithmetic, convert it to its "
                                     "code first: `int(c)`",
                                     spec::Char);
                    else if (!vt->isError() && !vt->isInteger() && !vt->is(K::Char))
                        diags_.error(a.value->loc, opName + " on a char needs an int or a long, but this is " + q(vt),
                                     vt->isFloating() ? "convert it first: `int(x)`" : "use an int or a long", spec::Char);
                } else if (t->isPointer()) {
                    // `p += n` and `p -= n` move the pointer by n elements.
                    if (*a.op != BinOp::Add && *a.op != BinOp::Sub)
                        diags_.error(a.target->loc, opName + " cannot be used with a pointer",
                                     "a pointer moves with `p += n` and `p -= n`", spec::Pointers);
                    else if (!pointsToKnownType(t))
                        reportUnknownPointee(a.loc, t, opName);
                    else if (!vt->isError() && !vt->isInteger() && !vt->is(K::Char))
                        diags_.error(a.value->loc, opName + " on a pointer needs an int or a long, but this is " + q(vt), "a pointer moves by a whole number of elements, such as `p += 2`", spec::Pointers);
                } else if (t->is(K::String))
                    diags_.error(a.target->loc, opName + " cannot be used with 'string'",
                                 "use +=, which appends, and is the only compound assignment on strings", spec::Strings);
                else if (!t->isError() && !t->isNumber())
                    diags_.error(a.target->loc, opName + " needs a number, but this is " + q(t), t->is(K::Bool) ? (*a.op == BinOp::BitXor ? std::string("write `b = b != c`") : std::string("write `b = b || c`, or `b = b && c`"))
                                 : t->isVector() && *a.op == BinOp::Add && !a.value->type->isVector() ? std::string("write `v.push_back(x)` to add to it")
                                 : std::string("use it on a number. Another type is changed with `=`"), spec::Operators);
                else if (!a.value->type->isError() && !a.value->type->isArithmetic())
                    diags_.error(a.value->loc, opName + " needs a number, but this is " + q(a.value->type), a.value->type->isOptional() ? unwrapHelp(spelled(*a.value))
                                 : a.value->type->is(K::Bool) ? std::string("convert it first: `int(b)`")
                                 : a.value->type->is(K::String) ? "this compiler cannot read a number from a string yet: " + readingNumberHelp(t)
                                                                : std::string("give it a number"), spec::Operators);
                else if ((*a.op == BinOp::Rem || *a.op == BinOp::Shl || *a.op == BinOp::Shr || *a.op == BinOp::BitAnd ||
                          *a.op == BinOp::BitOr || *a.op == BinOp::BitXor) &&
                         (t->isFloating() || a.value->type->isFloating()))
                    diags_.error(t->isFloating() ? a.target->loc : a.value->loc,
                                 opName + " needs an int or a long, but this is " +
                                     q(t->isFloating() ? t : a.value->type),
                                 *a.op == BinOp::Rem ? (t->isInteger() || t->is(K::Float) ? "for floating-point numbers, write `x = " + std::string(t->name) + "(fmod(x, y))`" : std::string("for floating-point numbers, write `x = fmod(x, y)`")) : "use an int or a long",
                                 spec::Operators);
                else if (*a.op != BinOp::Shl && *a.op != BinOp::Shr)
                    requireConvertible(*a.value, t, "on the right of " + opName);
                else
                    checkShiftAmount(t, nullptr, *a.value);
            } else {
                checkValue(*a.value, flow, t, "when assigning to " + name);
                if (ok && var) markAssigned(flow, var, path);
            }
            result = t;
            break;
        }
        case Expr::Kind::Ternary: {
            auto& t = static_cast<TernaryExpr&>(e);
            checkCondition(*t.cond, flow, "condition of ?:");
            Flow a = flow, b = flow;
            const Type* x = checkExpr(*t.thenExpr, a);
            const Type* y = checkExpr(*t.elseExpr, b);
            rejectCopy(*t.thenExpr);
            rejectCopy(*t.elseExpr);
            flow = merge(a, b);
            if (x->isError() || y->isError()) break;
            if (x == y) result = x;
            else if (x->isArithmetic() && y->isArithmetic()) result = commonNumber(x, y);
            else if (const Type* ptr = commonPointer(x, y)) result = ptr;  // `c ? p : null` is a pointer
            else if (const Type* ref = commonReference(x, y)) result = ref;  // `c ? dog : animal` is an Animal
            else if (x->isWeak() && y->isNull()) result = x;  // `c ? w : null` is an empty weak reference or `w`
            else if (y->isWeak() && x->isNull()) result = y;
            else if (const Type* opt = optionalOfBranches(x, y)) result = opt;
            else
                diags_.error(t.loc, "the two branches of ?: have different types, " + q(x) + " and " + q(y),
                             "make both branches the same type. For text, `to_string(x)` or `format(\"{}\", x)` turns the other into a string", spec::Operators);
            break;
        }
        case Expr::Kind::IncDec: {
            auto& i = static_cast<IncDecExpr&>(e);
            const char* sym = i.increment ? "++" : "--";
            const VarDecl* var = nullptr;
            std::string path;
            bool ok = checkAssignable(*i.target, flow, i.increment ? "incremented" : "decremented", var, path, i.prefix ? "" : sym, i.prefix ? sym : "");
            const Type* t = i.target->type ? i.target->type : Type::error();
            if (ok) {
                if (var) requireAssigned(flow, var, path, t, i.target->loc);
                if (t->isPointer()) {
                    if (!pointsToKnownType(t)) reportUnknownPointee(i.loc, t, sym);
                } else if (!t->isNumber() && !t->is(K::Char)) {
                    diags_.error(i.target->loc, std::string("'") + sym + "' needs a number, a char or a pointer, but this is " + q(t), "use it on a number, a char or a pointer. A string grows with `s += text`", spec::Operators);
                }
            }
            result = t;
            break;
        }
        case Expr::Kind::Call: result = checkCall(static_cast<CallExpr&>(e), flow); break;
        case Expr::Kind::Spawn: result = checkSpawn(static_cast<SpawnExpr&>(e), flow); break;
        case Expr::Kind::Await: result = checkAwait(static_cast<AwaitExpr&>(e), flow); break;
        case Expr::Kind::New: result = checkNew(static_cast<NewExpr&>(e), flow); break;
        case Expr::Kind::Delete: result = checkDelete(static_cast<DeleteExpr&>(e), flow); break;
        case Expr::Kind::Sizeof: {
            auto& z = static_cast<SizeofExpr&>(e);
            if (z.what == SizeofExpr::What::Zero) {
                result = z.target;
                break;
            }
            if (z.what == SizeofExpr::What::Descriptor) {
                result = Type::pointerTo(Type::voidTy());
                break;
            }
            if (z.operand) {
                Flow unevaluated = flow;  // `sizeof x` does not read x
                unevaluated.reachable = false;
                z.target = checkExpr(*z.operand, unevaluated);
            }
            const Type* t = z.target;
            result = Type::longTy();
            if (t->isError() || (!t->isOpaque() && !checkStorable(t, e.loc))) break;
            if (t->is(K::Void) || t->isNull() || t->isOpaque() || t->is(K::Auto))
                diags_.error(e.loc, "sizeof needs a type with a size, but this is " + q(t),
                             t->isOpaque() ? "use a pointer to it: `sizeof(" + std::string(t->name) + "*)`. Or declare its fields, in C's order, in `extern \"C\" struct`, if you know them" : "give it a type that has a size, such as `sizeof(int)`",
                             t->isOpaque() ? spec::CTypes : spec::Sizeof);
            break;
        }
    }
    e.type = result;
    return result;
}

void Sema::checkArgs(std::vector<ExprPtr>& args, const FuncDecl& fn, Flow& flow, Loc loc, const std::string& what) {
    if (args.size() != fn.params.size()) {
        for (auto& a : args) checkLooseValue(*a, flow);
        diags_.error(loc, what + " takes " + std::to_string(fn.params.size()) + " argument" +
                              (fn.params.size() == 1 ? "" : "s") + " but " + std::to_string(args.size()) +
                              (args.size() == 1 ? " was" : " were") + " given",
                     "its signature is `" + signature(fn) + "`", fn.owner ? spec::Methods : spec::Functions);
        diags_.note(fn.nameLoc, q(fn.name) + " is defined here");
        return;
    }
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& n = fn.params[i]->name;  // a C function's parameters may have no names
        checkValue(*args[i], flow, fn.params[i]->declType,
                   (n.empty() ? "for argument " + std::to_string(i + 1) : "for parameter " + q(n)) + " of " + q(fn.name));
    }
}

namespace {
// The math functions, from C++'s <cmath>. Floating ones return a float when every
// argument is a float, and a double otherwise, as in C++; tests return a bool; and abs,
// min and max keep the type of their arguments.
enum class MathKind { Floating, Test, Same };
struct MathInfo {
    const char* name;
    MathFn fn;
    size_t args;
    MathKind kind;
};
constexpr MathInfo kMath[] = {
    {"sqrt", MathFn::Sqrt, 1, MathKind::Floating},     {"cbrt", MathFn::Cbrt, 1, MathKind::Floating},
    {"pow", MathFn::Pow, 2, MathKind::Floating},       {"exp", MathFn::Exp, 1, MathKind::Floating},
    {"exp2", MathFn::Exp2, 1, MathKind::Floating},     {"log", MathFn::Log, 1, MathKind::Floating},
    {"log2", MathFn::Log2, 1, MathKind::Floating},     {"log10", MathFn::Log10, 1, MathKind::Floating},
    {"sin", MathFn::Sin, 1, MathKind::Floating},       {"cos", MathFn::Cos, 1, MathKind::Floating},
    {"tan", MathFn::Tan, 1, MathKind::Floating},       {"asin", MathFn::Asin, 1, MathKind::Floating},
    {"acos", MathFn::Acos, 1, MathKind::Floating},     {"atan", MathFn::Atan, 1, MathKind::Floating},
    {"atan2", MathFn::Atan2, 2, MathKind::Floating},   {"sinh", MathFn::Sinh, 1, MathKind::Floating},
    {"cosh", MathFn::Cosh, 1, MathKind::Floating},     {"tanh", MathFn::Tanh, 1, MathKind::Floating},
    {"floor", MathFn::Floor, 1, MathKind::Floating},   {"ceil", MathFn::Ceil, 1, MathKind::Floating},
    {"round", MathFn::Round, 1, MathKind::Floating},   {"trunc", MathFn::Trunc, 1, MathKind::Floating},
    {"fabs", MathFn::Fabs, 1, MathKind::Floating},     {"fmod", MathFn::Fmod, 2, MathKind::Floating},
    {"hypot", MathFn::Hypot, 2, MathKind::Floating},   {"isnan", MathFn::IsNan, 1, MathKind::Test},
    {"isinf", MathFn::IsInf, 1, MathKind::Test},       {"isfinite", MathFn::IsFinite, 1, MathKind::Test},
    {"abs", MathFn::Abs, 1, MathKind::Same},           {"min", MathFn::Min, 2, MathKind::Same},
    {"max", MathFn::Max, 2, MathKind::Same},
};
const MathInfo* findMath(const std::string& name) {
    for (auto& m : kMath)
        if (name == m.name) return &m;
    return nullptr;
}
}  // namespace

bool isMathFunction(const std::string& name) { return findMath(name) != nullptr; }

namespace {
// The <cctype> functions, for one char. The tests return a bool, and toupper and tolower a
// char. They know only ASCII, like C's in the default "C" locale.
struct CharFnInfo {
    const char* name;
    CharFn fn;
};
constexpr CharFnInfo kCharFns[] = {
    {"isalpha", CharFn::IsAlpha}, {"isdigit", CharFn::IsDigit}, {"isalnum", CharFn::IsAlnum},
    {"isspace", CharFn::IsSpace}, {"isupper", CharFn::IsUpper}, {"islower", CharFn::IsLower},
    {"ispunct", CharFn::IsPunct}, {"isxdigit", CharFn::IsXDigit}, {"isprint", CharFn::IsPrint},
    {"isgraph", CharFn::IsGraph}, {"iscntrl", CharFn::IsCntrl}, {"isblank", CharFn::IsBlank}, {"toupper", CharFn::ToUpper},
    {"tolower", CharFn::ToLower},
};
const CharFnInfo* findCharFn(const std::string& name) {
    for (auto& f : kCharFns)
        if (name == f.name) return &f;
    return nullptr;
}
}  // namespace

bool isCharFunction(const std::string& name) { return findCharFn(name) != nullptr; }

// `isdigit(c)`, `toupper(c)`, ... A function of the program with the same name takes
// their place, as with the math functions.
const Type* Sema::checkCharFn(CallExpr& call, Flow& flow) {
    const CharFnInfo& f = *findCharFn(call.callee);
    call.builtin = Builtin::CharFn;
    call.charFn = f.fn;
    bool converts = f.fn == CharFn::ToUpper || f.fn == CharFn::ToLower;
    const Type* result = converts ? Type::charTy() : Type::boolTy();
    if (call.args.size() != 1) {
        for (auto& a : call.args) checkLooseValue(*a, flow);
        diags_.error(call.loc, call.callee + "() takes one argument but " + std::to_string(call.args.size()) +
                                   (call.args.size() == 1 ? " was" : " were") + " given",
                     "write `" + call.callee + "(c)` for a char c", spec::Char);
        return result;
    }
    Expr& arg = *call.args[0];
    const Type* t = checkExpr(arg, flow);
    if (!t->isError() && !t->is(K::Char))
        diags_.error(arg.loc, call.callee + "() takes a char, but this is " + q(t),
                     t->is(K::String)  ? "pass one char of it, e.g. `" + call.callee + "(s[0])`"
                     : t->isInteger()  ? "C's version takes a char's code as an int; here pass the char itself, or "
                                         "convert the code with `char(n)`"
                     : t->isOptional() ? unwrapHelp(spelled(arg))
                                       : "pass a char, such as 'a' or `s[i]`",
                     spec::Char);
    return result;
}

// `string(count, c)`: count copies of the char c, as in C++. `string()` is empty.
const Type* Sema::checkStringFill(CallExpr& call, Flow& flow) {
    call.builtin = Builtin::StringFill;
    if (call.args.size() != 2 && !call.args.empty()) {
        for (auto& a : call.args) checkLooseValue(*a, flow);
        diags_.error(call.loc, "'string' is built from no arguments or two, but " + std::to_string(call.args.size()) +
                                   " were given",
                     "write `string(count, c)` for count copies of the char c, e.g. `string(3, '-')`", spec::Strings);
        return Type::stringTy();
    }
    if (call.args.empty()) return Type::stringTy();
    checkValue(*call.args[0], flow, Type::longTy(), "as the count for string(count, c)");
    Expr& c = *call.args[1];
    bool oneLetter = c.kind == Expr::Kind::StringLit && static_cast<StringLitExpr&>(c).value.size() == 1;
    if (checkExpr(c, flow)->is(K::String) && !oneLetter)
        diags_.error(c.loc, "string(count, c) repeats one char, but this is 'string'",
                     "build repeated text with a loop: `for (int i = 0; i < n; i++) s += text;`", spec::Strings);
    else
        requireConvertible(c, Type::charTy(), "as the char for string(count, c)");
    return Type::stringTy();
}

// `sqrt(x)`, `pow(a, b)`, `max(a, b)`, ... A function of the program with the same name
// takes its place, so `extern "C" double sqrt(double)` still works.
const Type* Sema::checkMath(CallExpr& call, Flow& flow) {
    const MathInfo& m = *findMath(call.callee);
    call.builtin = Builtin::Math;
    call.math = m.fn;
    std::string usage = call.callee + (m.args == 1 ? "(x)" : m.fn == MathFn::Pow ? "(base, exponent)" : "(a, b)");
    if (call.args.size() != m.args) {
        for (auto& a : call.args) checkLooseValue(*a, flow);
        diags_.error(call.loc, call.callee + "() takes " + (m.args == 1 ? "one argument" : "two arguments") + " but " +
                                   std::to_string(call.args.size()) + (call.args.size() == 1 ? " was" : " were") +
                                   " given",
                     "write `" + usage + "`", spec::Math);
        return m.kind == MathKind::Test ? Type::boolTy() : Type::error();
    }
    const Type* common = nullptr;
    bool allFloat = true, allChar = true, ok = true;
    for (auto& a : call.args) {
        const Type* t = checkExpr(*a, flow);
        if (t->isError()) {
            ok = false;
            continue;
        }
        if (!t->isArithmetic()) {  // a char counts as its code, as in C++
            std::string help = t->isOptional()   ? unwrapHelp(spelled(*a))
                               : t->is(K::Bool)   ? "a bool is not a number here; write `int(b)` for 1 or 0"
                               : t->is(K::String) && (call.callee == "min" || call.callee == "max")
                                   ? std::string("for strings, which compare by their bytes, write `a ") + (call.callee == "min" ? "<" : ">") + " b ? a : b`"
                               : t->is(K::String) ? "this compiler cannot read a number from a string yet. Declare C's: `extern \"C\" double atof(string text);`, and call `atof(s)`"
                                                  : "pass an int, a long, a float or a double";
            diags_.error(a->loc, call.callee + "() takes " + (m.args == 1 ? "a number" : "numbers") + ", but this is " +
                                     q(t),
                         help, spec::Math);
            ok = false;
            continue;
        }
        allFloat = allFloat && t->is(K::Float);
        allChar = allChar && t->is(K::Char);
        common = common ? commonNumber(common, t) : promoted(t);
    }
    if (m.kind == MathKind::Test) return Type::boolTy();
    if (!ok) return Type::error();
    // `max('a', c)` is a char, as with std::max; `abs(c)` is an int.
    if (m.kind == MathKind::Same) return allChar && m.fn != MathFn::Abs ? Type::charTy() : common;
    return allFloat ? Type::floatTy() : Type::doubleTy();
}

const Type* Sema::checkCall(CallExpr& call, Flow& flow) {
    if (call.loc.line == 0 && call.callee == "__hash") {  // see Builtins.h
        checkExpr(*call.args[0], flow);
        call.builtin = Builtin::Hash;
        return Type::longTy();
    }
    if (call.loc.line == 0 && call.callee == "__move") {
        call.builtin = Builtin::Move;
        call.clears = true;
    }
    if (call.builtin == Builtin::Move) return checkMove(call, flow);
    if (call.uniqueType) {  // `unique<Node>(args)` builds a new Node
        call.constructs = call.uniqueType->decl;
        std::vector<ConstructArg> args;
        for (auto& a : call.args) args.push_back({"", a->loc, a.get(), nullptr});
        checkConstruction(*call.constructs, std::move(args), flow, call.loc);
        return call.uniqueType;
    }
    if (call.vectorType) return checkVectorConstruction(call, flow);
    if (call.callee == "println" || call.callee == "print" || call.callee == "format") return checkPrint(call, flow);
    if (call.callee == "to_string") return checkToString(call, flow);
    if (call.callee == "string") return checkStringFill(call, flow);
    Loc nameLoc = call.loc;
    nameLoc.len = (uint32_t)call.callee.size();

    // Inside a method, `name(...)` calls another method on `this`, as in C++.
    if (StructDecl* o = owner()) {
        BasePath found = findMember(*o, call.callee, nameLoc);
        if (FuncDecl* m = found.empty() ? nullptr : found.back()->ownMethod(call.callee)) {
            call.method = m;
            call.path = std::move(found);
            requestMethod(m);
            if (!canAccess(*m->owner, m->access, o)) reportAccess(*m->owner, m->name, true, m->access, nameLoc);
            if (&call != spawned_) checkCanFail(*m, nameLoc);
            requireAssigned(flow, thisVar(), "", &o->type, call.loc);
            if (!m->isConst && fn_->isConst)
                diags_.error(nameLoc, "const method " + q(fn_->name) + " cannot call " + q(m->name) + ", which may change fields",
                             "remove `const` from " + q(fn_->name) + ", or mark " + q(m->name) + " `const` if it does not change any field", spec::Methods);
            checkArgs(call.args, *m, flow, call.loc, q(m->name));
            checkTaskKept(*m, call, nameLoc);
            return m->retType;
        }
    }

    // `Point(1, 2)` builds a struct or class object.
    if (auto s = structs_.find(call.callee); s != structs_.end()) {
        const StructDecl& sd = *s->second;
        call.constructs = &sd;
        // `Point(p)` with a Point copies it, as in C++, and a class object is shared. What the
        // type's constructor or first field can take comes first, so the type of the value is
        // found out by trying it.
        bool checked = false;
        if (call.args.size() == 1 && call.args[0]->kind != Expr::Kind::InitList) {
            const Type* given = checkExpr(*call.args[0], flow);
            checked = true;
            bool ctorTakes = sd.constructor && sd.constructor->params.size() == 1 &&
                             canConvert(given, sd.constructor->params[0]->declType);
            bool fieldTakes = !sd.constructor && sd.bases.empty() && !sd.fields.empty() && canConvert(given, sd.fields[0].type);
            if (!given->isError() && !ctorTakes && !fieldTakes && canConvert(given, &sd.type)) {
                rejectCopy(*call.args[0]);
                call.copies = true;
                return &sd.type;
            }
        }
        std::vector<ConstructArg> args;
        for (auto& a : call.args) args.push_back({"", a->loc, a.get(), nullptr, checked});
        checkConstruction(sd, std::move(args), flow, call.loc);
        return &sd.type;
    }

    if (TemplateDecl* t = program_->findTemplate(call.callee); t && t->kind == TemplateDecl::Kind::Function)
        return checkTemplateCall(call, *t, flow);
    auto it = funcs_.find(call.callee);
    if (it == funcs_.end() && call.callee == "move") {
        call.builtin = Builtin::Move;
        return checkMove(call, flow);
    }
    if (it == funcs_.end() && findMath(call.callee)) return checkMath(call, flow);
    if (it == funcs_.end() && findCharFn(call.callee)) return checkCharFn(call, flow);
    if (it == funcs_.end()) {
        for (auto& a : call.args) checkLooseValue(*a, flow);
        if (auto why = program_->notImported.find(call.callee); why != program_->notImported.end()) {
            diags_.error(nameLoc, why->second.first, why->second.second, spec::CHeaders);
            return Type::error();
        }
        std::string help;
        const std::string& name = call.callee;
        std::string stem = name.size() > 3 ? name.substr(0, name.size() - 1) : "";
        if (name == "sleep" || name == "usleep" || name == "nanosleep" || name == "Sleep" || name == "setTimeout") {
            const bool inAsync = fn_ && fn_->isAsync;
            diags_.error(nameLoc, name == "setTimeout" ? std::string("C+ has no `setTimeout`, as an async function can wait, then do the work")
                                                       : "C+ waits with `delay`, which counts milliseconds, and has no `" + name + "`",
                         name == "setTimeout"
                             ? std::string("write an async function that waits first, with `await delay(ms);`, then does the work. Calling it starts it: `auto t = later();`")
                             : "to wait, write `" + std::string(inAsync ? "await delay(1000);" : "delay(1000).wait();") +
                                   "`" + (inAsync ? ", which lets other async work go on meanwhile" : "") +
                                   (cDeclarationOf(name).empty() ? "" : ". C's can be declared instead: `extern \"C\" " + cDeclarationOf(name) + ";`"),
                         spec::Async);
            return Type::error();
        }
        if (name.starts_with("sto") && !cNumberReader(name).empty()) {
            diags_.error(nameLoc, "this compiler does not support reading a number from a string yet",
                         "until it does, declare `extern \"C\" " + cNumberReader(name) + ";`, and call `" + cReaderName(name) + "(s)` instead of `" + name + "`", spec::Status);
            return Type::error();
        }
        if (name.find("::") != std::string::npos) {
            const std::string module = name.substr(0, name.find("::"));
            help = "`cplus explain " + module + "` says what the module '" + module + "' has";
        } else if (lookup(name)) {
            help = q(name) + " is a variable, and C+ has no function values yet. Write a function, and call it by name";
        } else if (name == "fetch")
            help = "to get what is at a URL: `import http;`, then `try http::get(url)`";
        else if (name == "vector" || name == "array")
            help = name == "vector" ? "write what it holds: `vector<int>(n, value)`" : "write what it holds: `array<int, 3>{1, 2, 3}`";
        else if (name == "pthread_create")
            help = "write `auto t = spawn f(x);`, then `t.wait()`, which gives what `f` returned";
        else if (name == "sprintf" || name == "snprintf")
            help = "`format(\"{}\", x)` returns the string";
        else if (name == "assert")
            help = "write `if (!ok) throw Error(\"...\");`, or stop the program with a C function";
        else if (!cDeclarationOf(name).empty())
            help = "a C function must be declared: `extern \"C\" " + cDeclarationOf(name) + ";`";
        else if (name == "sort" || name == "stable_sort" || name == "reverse" || name == "accumulate" || name == "min_element" || name == "max_element")
            help = "this compiler has no sorting yet, nor the other algorithms of C++. Write the loop";
        else if (name == "swap")
            help = "write it with a third variable: `auto t = a; a = b; b = t;`";
        else if (name == "fmin" || name == "fmax" || name == "fminf" || name == "fmaxf")
            help = "write `" + std::string(name.compare(0, 4, "fmin") == 0 ? "min" : "max") + "(a, b)`, which works for doubles and floats too";
        else if (name == "log1p") help = "write `log(1 + x)`";
        else if (name == "expm1") help = "write `exp(x) - 1`";
        else if (name == "printf" || name == "puts" || name == "putchar")
            help = "C+ prints with println: `printf(\"%d\\n\", x)` becomes `println(\"{}\", x)`";
        else if (std::string reader = cNumberReader(name); !reader.empty())
            help = "a C function must be declared: `extern \"C\" " + reader + ";`";
        else if ((name.back() == 'f' || name.back() == 'l') && isMathFunction(stem))
            help = name.back() == 'f' ? "write `" + stem + "`, which returns a float when given floats, as in C++"
                                      : "C+ has no long double; write `" + stem + "`";
        else if (std::string guess = suggestName(name); !guess.empty())
            help = "did you mean " + q(guess) + "?";
        else
            help = "define it in this file, in any order, or declare a C function with `extern \"C\"`";
        if (!program_->unreadFunctions.count(call.callee)) diags_.error(nameLoc, "there is no function named " + q(call.callee), help, spec::Functions);
        return Type::error();
    }
    FuncDecl* fn = it->second;
    call.target = fn;
    if (&call != spawned_) checkCanFail(*fn, nameLoc);
    checkArgs(call.args, *fn, flow, call.loc, q(fn->name));
    checkTaskKept(*fn, call, nameLoc);
    return fn->retType;
}

namespace {
// Works out template arguments by matching what a parameter is written as with the type of a value.
struct Deduction;
static std::string argumentsUpTo(const TemplateDecl& t, const Deduction& d, size_t count);
static std::string writtenArguments(const TemplateDecl& t, const Deduction& d, size_t count);

struct Deduction {
    const TemplateDecl& t;
    std::vector<TemplateArg>& args;
    std::vector<bool> known;
    size_t given;  // that many were written, and values convert to them
    std::string conflict;
    int conflictAt = -1;                // the parameter it is about
    const Type* conflictOther = nullptr;  // the other type it would have to be, or null for a value

    void setType(int i, const Type* a) {
        if ((size_t)i < given) return;
        if (!known[i]) {
            args[i].type = a;
            known[i] = true;
        } else if (args[i].type != a && conflict.empty()) {
            conflict = "'" + t.params[i].name + "' cannot be both '" + args[i].type->name + "' and '" + a->name + "'";
            conflictAt = i;
            conflictOther = a;
        }
    }
    void setValue(int i, int64_t value) {
        if ((size_t)i < given) return;
        if (!known[i]) {
            args[i].value = value;
            known[i] = true;
        } else if (args[i].value != value && conflict.empty()) {
            conflict = "'" + t.params[i].name + "' cannot be both " + std::to_string(args[i].value) + " and " + std::to_string(value);
            conflictAt = i;
        }
    }
    void match(const Type* p, const Type* a) {
        if (a->isNull() || a->isError()) return;
        switch (p->kind) {
            case K::TypeParam: setType((int)p->length, a); break;
            case K::Vector:
                if (a->isVector()) match(p->elem, a->elem);
                break;
            case K::Array:
                if (!a->isArray()) break;
                match(p->elem, a->elem);
                if (p->length < 0) setValue((int)(-1 - p->length), a->length);
                break;
            case K::Optional: match(p->elem, a->isOptional() ? a->elem : a); break;
            case K::Pointer:
                if (a->isPointer()) match(p->elem, a->elem);
                break;
            case K::Weak: match(p->elem, a->isWeak() || a->isOptional() ? a->elem : a); break;
            case K::Unique:
                if (a->isUnique()) match(p->elem, a->elem);
                break;
            case K::Local:
                if (a->isLocal()) match(p->elem, a->elem);
                break;
            case K::Dependent: {
                if (a->isOptional()) a = a->elem;
                if (a->isWeak() || a->isUnique() || a->isLocal()) a = a->elem;
                if (!a->hasFields()) break;
                if (const StructDecl* made = findMade(*a->decl, p->pattern->tmpl))
                    for (size_t i = 0; i < p->pattern->args.size(); ++i) {
                        const TemplateArg& written = p->pattern->args[i];
                        if (written.type) match(written.type, made->templateArgs[i].type);
                        else if (written.valueParam >= 0) setValue(written.valueParam, made->templateArgs[i].value);
                    }
                break;
            }
            default: break;
        }
    }
    // `d`, or a base class of it, made from generic `tmpl`, as C++ also looks in base classes.
    static const StructDecl* findMade(const StructDecl& d, const TemplateDecl* tmpl) {
        if (d.templateOf == tmpl) return &d;
        for (const StructDecl* base : d.bases)
            if (const StructDecl* found = findMade(*base, tmpl)) return found;
        return nullptr;
    }
};

// What a generic's arguments would be, written up to `count` of them.
static std::string argumentsUpTo(const TemplateDecl& t, const Deduction& d, size_t count) {
    std::string out;
    for (size_t i = 0; i < count && i < t.params.size(); ++i) {
        std::string one;
        if (t.params[i].valueType) {
            one = d.known[i] ? std::to_string(d.args[i].value) : "16";
        } else if (!d.known[i]) {
            one = "int";
        } else {
            const Type* type = d.args[i].type;
            // Of two numbers, the one that can hold the other: char, int, long, float, double, in that order.
            auto rank = [](const Type* x) { return x->is(K::Char) ? 1 : x->is(K::Int) ? 2 : x->is(K::Long) ? 3 : x->is(K::Float) ? 4 : x->is(K::Double) ? 5 : 0; };
            if ((int)i == d.conflictAt && d.conflictOther && rank(type) && rank(d.conflictOther) && rank(d.conflictOther) > rank(type)) type = d.conflictOther;
            one = type->name;
        }
        out += (i ? ", " : "") + one;
    }
    return out;
}

static std::string writtenArguments(const TemplateDecl& t, const Deduction& d, size_t) {
    if (!d.conflictOther) return q(t.params[d.conflictAt].name) + " must be the same for both. Give both arguments the same " + q(t.params[d.conflictAt].name);
    auto isNumberLike = [](const Type* x) { return x->isNumber() || x->is(K::Char); };
    const Type* one = d.args[d.conflictAt].type;
    size_t direct = 0;  // parameters written as the type parameter itself, `T a`, to which a narrower number converts
    bool onlyDirect = true;  // and none has it inside another type, as `Box<T> c`, which a wider number does not fit
    std::function<bool(const Type*)> mentions = [&](const Type* x) {
        if (!x) return false;
        if (x->is(K::TypeParam)) return x->length == d.conflictAt;
        if (x->pattern)
            for (const TemplateArg& a : x->pattern->args)
                if (mentions(a.type)) return true;
        return mentions(x->elem);
    };
    if (t.pattern)
        for (const auto& p : t.pattern->params) {
            const bool isDirect = p->declType && p->declType->is(K::TypeParam) && p->declType->length == d.conflictAt;  // `T a`, as written
            direct += isDirect;
            if (!isDirect && mentions(p->declType)) onlyDirect = false;
        }
    if (!onlyDirect) direct = 0;
    if (one && (!(isNumberLike(one) && isNumberLike(d.conflictOther)) || direct < 2))  // no type holds both a Box<int> and a Box<long>
        return q(t.params[d.conflictAt].name) + " cannot be both " + q(one) + " and " + q(d.conflictOther) + ". Convert an argument, so that both have the same type";
    return "write the type you mean and those before it: `" + t.name + "<" + argumentsUpTo(t, d, d.conflictAt + 1) + ">(...)`, or convert an argument";
}
}  // namespace

const Type* Sema::checkTemplateCall(CallExpr& call, TemplateDecl& t, Flow& flow) {
    Loc nameLoc = call.loc;
    nameLoc.len = (uint32_t)call.callee.size();
    if (t.failed || !t.pattern) {  // reported where it is defined
        for (auto& a : call.args) checkLooseValue(*a, flow);
        return Type::error();
    }
    const FuncDecl& pattern = *t.pattern;
    if (call.args.size() != pattern.params.size()) {
        for (auto& a : call.args) checkLooseValue(*a, flow);
        diags_.error(nameLoc, q(t.name) + " takes " + std::to_string(pattern.params.size()) + " argument" +
                                  (pattern.params.size() == 1 ? "" : "s") + " but " + std::to_string(call.args.size()) +
                                  (call.args.size() == 1 ? " was" : " were") + " given",
                     "its signature is `" + signature(pattern) + "`", spec::Functions);
        diags_.note(t.nameLoc, q(t.name) + " is defined here");
        return Type::error();
    }
    std::vector<TemplateArg> args(t.params.size());
    for (size_t i = 0; i < call.templateArgs.size(); ++i) args[i] = call.templateArgs[i];
    Deduction deduction{t, args, std::vector<bool>(t.params.size(), false), call.templateArgs.size(), ""};
    for (size_t i = 0; i < call.templateArgs.size(); ++i) deduction.known[i] = true;
    // `{ }` lists take their type from the parameter, so they wait until it is known.
    bool bad = false;
    for (size_t i = 0; i < call.args.size(); ++i) {
        Expr& a = *call.args[i];
        if (a.kind == Expr::Kind::InitList && !static_cast<InitListExpr&>(a).explicitType) continue;
        const Type* type = checkExpr(a, flow);
        bad = bad || type->isError();
        if (type->is(K::Void)) {
            diags_.error(a.loc, "this argument has no value", "a call of a void function gives nothing to pass. Call it first, on a line of its own", spec::Functions);
            bad = true;
            continue;
        }
        deduction.match(pattern.params[i]->declType, type);
    }
    auto finishLists = [&](FuncDecl* fn) {
        for (size_t i = 0; i < call.args.size(); ++i) {
            Expr& a = *call.args[i];
            if (a.kind != Expr::Kind::InitList || static_cast<InitListExpr&>(a).explicitType) continue;
            if (fn) checkValue(a, flow, fn->params[i]->declType, "for parameter " + q(fn->params[i]->name) + " of " + q(fn->name));
            else checkLooseValue(a, flow);
        }
    };
    if (bad) {
        finishLists(nullptr);
        return Type::error();
    }
    if (!deduction.conflict.empty()) {
        finishLists(nullptr);
        diags_.error(nameLoc, "the arguments of " + q(t.name) + " do not agree: " + deduction.conflict,
                     writtenArguments(t, deduction, t.params.size()),
                     spec::Generics);
        return Type::error();
    }
    for (size_t i = 0; i < t.params.size(); ++i) {
        if (deduction.known[i]) continue;
        finishLists(nullptr);
        diags_.error(nameLoc, "cannot work out " + q(t.params[i].name) + " of " + q(t.name) + " from the arguments",
                     "arguments are written in order, so write all up to it: `" + t.name + "<" + argumentsUpTo(t, deduction, i + 1) + ">(...)`", spec::Generics);
        return Type::error();
    }
    if (!checkConstraints(t, args, nameLoc)) {
        finishLists(nullptr);
        return Type::error();
    }
    int depth = fn_ ? fn_->genericDepth + 1 : 1;
    if (depth > 64) {
        finishLists(nullptr);
        diags_.error(nameLoc, "the generic function " + q(t.name) + " keeps needing larger versions of itself, which would never end",
                     "have it call itself with the same types, or end the chain with `if constexpr (...) { ... } else { ... }` on a value parameter", spec::Generics);
        return Type::error();
    }
    FuncDecl* fn = parser_.instantiateFunction(t, args, call.loc, depth);
    adoptTypes();
    finishLists(fn);
    if (!fn) return Type::error();
    prepareAsync(*fn);
    call.target = fn;
    if (&call != spawned_) checkCanFail(*fn, nameLoc);
    for (size_t i = 0; i < call.args.size(); ++i) {
        Expr& a = *call.args[i];
        if (a.kind == Expr::Kind::InitList && !static_cast<InitListExpr&>(a).explicitType) continue;
        requireConvertible(a, fn->params[i]->declType, "for parameter " + q(fn->params[i]->name) + " of " + q(fn->name));
    }
    checkTaskKept(*fn, call, nameLoc);
    return fn->retType;
}

bool Sema::checkConstraints(TemplateDecl& t, const std::vector<TemplateArg>& args, Loc usedAt) {
    bool ok = true;
    for (size_t i = 0; i < t.params.size(); ++i) {
        const TemplateParam& p = t.params[i];
        if (p.constraint.empty() || !args[i].type) continue;
        Loc where;
        std::string problem = constraintProblem(p.constraint, args[i].type, where);
        if (problem.empty()) continue;
        ok = false;
        std::string shown = p.constraint.rfind("__", 0) == 0 ? p.constraint.substr(2) : p.constraint;
        diags_.error(usedAt, q(args[i].type) + " cannot be used for " + q(p.name) + " of " + q(t.name) + ", which must be " + q(shown),
                     problem, t.nameLoc.line ? spec::Constraints : t.name == "hashmap" ? spec::HashMaps : spec::Json);
        if (where.line) diags_.note(where, q(p.constraint) + " requires this, which does not work for " + q(args[i].type));
        else if (p.constraintLoc.line) diags_.note(p.constraintLoc, "the constraint is written here");
    }
    return ok;
}

// Why a value of this type cannot be turned into JSON, or made from it. `path` is how it was reached, and `seen` stops
// at a struct or class already looked at, since a type can lead back to itself. A private field is left out of JSON.
static std::string jsonProblem(const Program& program, const Type* t, bool reading, const std::string& path, std::vector<const StructDecl*>& seen, bool inList = false) {
    std::string where = path.empty() ? q(t) : inList ? "each element of " + q(path) + ", a " + q(t) + "," : q(path) + ", a " + q(t) + ",";
    if (t->isNumber() || t->is(K::Bool) || t->is(K::Char) || t->is(K::String) || t->isWeak()) return "";
    if (t->isOptional() || t->isVector() || t->isArray() || t->isUnique() || t->isLocal()) return jsonProblem(program, t->elem, reading, path, seen, inList || t->isVector() || t->isArray());
    if (isJsonValue(t)) return "";
    if (isHashmap(t)) {
        const Type* key = t->decl->templateArgs[0].type;
        if (!key->is(K::String) && !key->is(K::Int) && !key->is(K::Long))
            return where + " has keys of type " + q(key) + ", while a JSON object has strings. Use a string, an int or a long for the key, which is written as a string";
        return jsonProblem(program, t->decl->templateArgs[1].type, reading, path.empty() ? "[key]" : path + "[key]", seen, true);
    }
    if (!t->hasFields())
        return where + " is a pointer, which only means something to this program. " +
               (t->isOpaque() || (t->isPointer() && (t->elem->is(K::Void) || t->elem->isOpaque())) ? std::string("Use a type with values, such as a struct, as JSON cannot hold an address")
                : inList ? "Copy what they point to into values of type " + q(t->isPointer() ? t->elem : t) + ", and use those"
                : path.empty() ? (reading ? "Read what it points to: `json::parse<" + std::string(t->isPointer() ? t->elem->name : t->name) + ">`" : std::string("Write `*p` for what it points to"))
                                     : std::string("Make it a private field, which is left out of JSON, and reach it through methods of the class"));
    const StructDecl& sd = *t->decl;
    for (const StructDecl* d : seen)
        if (d == &sd) return "";
    seen.push_back(&sd);
    if (reading) {
        if (sd.isInterface || sd.firstPure()) return where + " has methods without a body, so nothing can be made from JSON for it. Use a class that has them all";
        if (sd.constructor && !sd.constructor->params.empty())
            return where + " has a constructor that needs arguments, which JSON does not know. Give it a constructor without parameters, or none";
    }
    std::vector<std::pair<const FieldDecl*, bool>> fields;  // each with whether a constructor can give it its value
    std::function<void(const StructDecl&, bool)> collect = [&](const StructDecl& d, bool built) {
        built = built || d.constructor;
        for (const StructDecl* base : d.bases) collect(*base, built);
        for (auto& f : d.fields) fields.push_back({&f, built});
    };
    collect(sd, false);
    size_t shown = 0;
    std::set<std::string> names;
    for (auto [f, built] : fields) {
        std::string inner = path.empty() ? f->name : path + "." + f->name;
        if (f->access != Access::Public) {
            if (reading && !built && !f->defaultValue && needsValue(f->type))
                return where + " has a private field " + q(f->name) + " that needs a value, which JSON cannot give it. Give the field a default value";
            continue;
        }
        ++shown;
        if (!names.insert(f->name).second)
            return where + " has two fields named " + q(f->name) + ", from two base classes, and JSON could not tell them apart. Rename one of them";
        if (std::string problem = jsonProblem(program, f->type, reading, inner, seen); !problem.empty()) return problem;
    }
    if (!fields.empty() && !shown)
        return where + " has no public fields, so nothing of it would go in JSON. " +
               (sd.templateOf && sd.templateOf->nameLoc.line == 0 && sd.role == StructDecl::Role::Opaque ? std::string("Write what it gives: `t.wait()`")
                : comesWithTheLanguage(sd) ? std::string("Pass the values it protects or holds instead") : std::string("Put what JSON should hold under `public:`. It holds values, such as numbers and strings, and not pointers"));
    if (!reading && sd.isClass)  // what is written is whatever the object really is
        for (auto& other : program.structs)
            if (other->defined && other.get() != &sd && other->derivesFrom(&sd))
                if (std::string problem = jsonProblem(program, &other->type, reading, path, seen); !problem.empty()) return problem;
    return "";
}

std::string Sema::constraintProblem(const std::string& constraint, const Type* t, Loc& where) {
    // One written in the program comes first, as it would with any other name.
    if (auto s = structs_.find(constraint); s != structs_.end()) {  // an interface or a class
        if (t->isClass() && t->decl->partsOf(s->second) == 1) return "";
        return t->isClass() ? q(t) + " does not derive from " + q(constraint) + ". Write `public " + constraint + "` in its definition"
                            : q(t) + " is not a class, so it cannot derive from " + q(constraint) + ". Use a class that does";
    }
    if (TemplateDecl* c = program_->findTemplate(constraint); c && c->kind == TemplateDecl::Kind::Concept) {
        auto key = std::pair{constraint, t};
        if (auto done = constraintResults_.find(key); done != constraintResults_.end()) {
            where = done->second.second;
            return done->second.first;
        }
        constraintResults_[key] = {"", Loc{}};  // while it is worked out, in case it asks about itself
        TemplateArg arg;
        arg.type = t;
        Parser::ConceptBody body;
        if (!parser_.instantiateConcept(*c, arg, body)) return "";  // reported where it is defined
        // Whether the requirements would compile is found out by trying them, in a scope of their own.
        std::string problem;
        {
            FreshContext fresh(*this);
            scopes_.emplace_back();
            Flow flow;
            diags_.mute();
            for (auto& p : body.params->params) {
                p->type = p->declType;
                declare(*p);
                markAssigned(flow, p.get(), "");
            }
            for (auto& requirement : body.requirements) {
                int before = diags_.mutedErrors();
                checkExpr(*requirement, flow);
                if (diags_.mutedErrors() == before) continue;
                problem = "use a type that has what " + q(constraint) + " requires, which " + q(t) + " does not";
                where = requirement->loc;
                break;
            }
            diags_.unmute();
        }
        adoptTypes();
        constraintResults_[key] = {problem, where};
        return problem;
    }
    if (constraint == "Comparable")
        return t->isArithmetic() || t->is(K::String) || t->isPointer()
                   ? ""
                   : q(t) + " has no `<`. Only numbers, chars, strings and pointers do, until operators can be overloaded. Compare a field instead: `a.x < b.x`";
    if (constraint == "Number") return t->isNumber() ? "" : q(t) + " is not int, long, float or double. Pass one of those, or change the constraint";
    if (constraint == "Integer") return t->isInteger() ? "" : q(t) + " is not int or long. Use one of those, or convert it with `long(x)`";
    if (constraint == "Floating") return t->isFloating() ? "" : q(t) + " is not float or double. Use one of those, or convert it with `double(x)`";
    if (constraint == "__JsonWritable" || constraint == "__JsonReadable") {
        std::vector<const StructDecl*> seen;
        return jsonProblem(*program_, t, constraint == "__JsonReadable", "", seen);
    }
    if (constraint == "Hashable" || constraint == "__Hashable")
        return isHashable(t) ? ""
                             : "use another type for the key, as " + q(t) + " cannot be one. Numbers, chars, bools, strings, pointers and class references can, and so can "
                                      "optionals, vectors, arrays and structs of those. A unique or weak reference or a hashmap cannot";
    return "";  // unknown, which was reported at the generic
}

void Sema::markMoved(Flow& flow, const VarDecl* var, Loc loc) {
    if (!flow.reachable) return;  // `sizeof(move(x))` moves nothing
    moveLog_.push_back({var, loc});
    for (auto it = flow.assigned.begin(); it != flow.assigned.end();) {
        if (it->first == var) it = flow.assigned.erase(it);
        else ++it;
    }
}

void Sema::dropMovedSince(Flow& flow, size_t logStart) {
    for (size_t i = logStart; i < moveLog_.size(); ++i) {
        const VarDecl* var = moveLog_[i].first;
        for (auto it = flow.assigned.begin(); it != flow.assigned.end();) {
            if (it->first == var) it = flow.assigned.erase(it);
            else ++it;
        }
    }
}

void Sema::checkMovedInLoop(const Flow& head, size_t logStart, const Flow& end) {
    std::set<const VarDecl*> reported;
    for (size_t i = logStart; i < moveLog_.size(); ++i) {
        auto [var, loc] = moveLog_[i];
        if (!head.reachable || !isAssigned(head, var, var->type, "") || reported.count(var)) continue;
        bool gone = end.reachable && !isAssigned(end, var, var->type, "");
        for (auto& c : loops_.back().continues) gone = gone || (c.reachable && !isAssigned(c, var, var->type, ""));
        if (!gone) continue;
        reported.insert(var);
        diags_.error(loc, q(var->name) + " is moved inside a loop, so it would have no value the next time round",
                     "give it a new value before the loop goes round, or leave the loop right after, with `break` or `return`",
                     spec::Unique);
    }
}

const Type* Sema::checkMove(CallExpr& call, Flow& flow) {
    if (call.args.size() != 1) {
        for (auto& a : call.args) checkLooseValue(*a, flow);
        diags_.error(call.loc, "move takes one value", "write `move(x)`", spec::Unique);
        return Type::error();
    }
    Expr& x = *call.args[0];
    if (!isPlaceKind(x)) return checkExpr(x, flow);  // a value nothing else holds has nothing to give up
    const VarDecl* var = nullptr;
    std::string path;
    const Type* t = resolvePlace(x, flow, var, path);
    if (t->isError()) return t;
    if (var) requireAssigned(flow, var, path, t, x.loc);
    if (x.kind == Expr::Kind::This) {
        diags_.error(x.loc, "'this' cannot be moved, since the object is still in use",
                     "write `this` to pass a reference to it", spec::Unique);
        return t;
    }
    if (var && var->isTopLevel) {
        diags_.error(x.loc, "the constant " + q(var->name) + " cannot be moved", "remove `move`", spec::Unique);
        return t;
    }
    if (!checkChangeable(x, "moved", var, path)) return t;
    bool wholeVariable = x.kind == Expr::Kind::Name && static_cast<NameExpr&>(x).implicitField < 0;
    if (wholeVariable) markMoved(flow, var, call.loc);
    return t;
}

const Type* Sema::checkMethodCall(MethodCallExpr& call, Flow& flow) {
    const VarDecl* var = nullptr;
    std::string path;
    Expr& recv = *call.receiver;
    if (!call.qualifier.empty() && recv.kind == Expr::Kind::This && !owner()) {
        for (auto& a : call.args) checkLooseValue(*a, flow);
        diags_.error(call.loc, q(call.qualifier + "::" + call.name) + " can only be called inside a method of a class that "
                               "derives from " + q(call.qualifier),
                     "call it on an object: `x.method(...)`. Methods that belong to no object, such as static ones, are planned", spec::Inheritance);
        return Type::error();
    }
    bool isPlace = isPlaceKind(recv);
    const Type* t = isPlace ? resolvePlace(recv, flow, var, path) : checkExpr(recv, flow);
    if (var && !t->isError()) requireAssigned(flow, var, path, t, recv.loc);
    if (t->isError()) {
        for (auto& a : call.args) checkLooseValue(*a, flow);
        return Type::error();
    }
    if (t->isOptional()) return checkOptionalMethod(call, flow, t);
    if (t->isWeak()) return checkWeakMethod(call, flow, t);
    if (t->isUnique() || t->isLocal()) {
        if (call.name == "get" && call.args.empty()) {  // lends the object, as a counted reference
            call.uniqueGet = true;
            return t->elem;
        }
        // Everything else is a method of the object.
        bool isVariable = recv.kind == Expr::Kind::Name && static_cast<NameExpr&>(recv).implicitField < 0;
        Loc loc = recv.loc;
        call.receiver = std::make_unique<BorrowExpr>(loc, std::move(call.receiver), !isVariable);
        call.receiver->type = t->elem;
        return checkMethodCall(call, flow);
    }
    if (t->isPointer()) {
        for (auto& a : call.args) checkLooseValue(*a, flow);
        std::string n = spelled(recv, "p");
        diags_.error(call.loc, q(n) + " is a pointer, so the methods of what it points to are called with '->'",
                     "write `" + asBase(n) + "->" + call.name + "(...)`", spec::Pointers);
        return Type::error();
    }
    if (t->isVector()) return checkVectorMethod(call, flow, t);
    if (t->isArray()) return checkArrayMethod(call, flow, t);
    if (t->is(K::String)) return checkStringMethod(call, flow);
    if (!t->hasFields()) {
        for (auto& a : call.args) checkLooseValue(*a, flow);
        if (&call == spawned_ && (call.name == "wait" || call.name == "done"))
            diags_.error(call.loc, "`spawn` takes all of the call after it, so this asks for '." + call.name + "()' on what the function gives, and not on the task",
                         "put the `spawn` in parentheses: `(spawn f(x))." + call.name + "()`", spec::Concurrency);
        else
            diags_.error(call.loc, q(t) + " has no methods, so '." + call.name + "()' cannot be used", "remove the call, or use a value of a type that has this method", spec::Methods);
        return Type::error();
    }
    const StructDecl& sd = *t->decl;
    if (isHashmap(t)) {
        const Type* value = sd.templateArgs[1].type;
        if (call.name == "at" && call.args.size() != 1) {
            for (auto& a : call.args) checkLooseValue(*a, flow);
            diags_.error(call.loc, "'at' takes the key to look for, but this gives it " + std::to_string(call.args.size()) + " arguments",
                         "write `" + spelled(recv, "m") + ".at(key)`", spec::HashMaps);
            return Type::error();
        }
        if (call.name == "at") {  // `m[key]`, but only for a key that is there
            auto ix = std::make_unique<IndexExpr>(call.loc, std::move(call.receiver), std::move(call.args[0]));
            call.args.clear();
            checkValue(*ix->index, flow, sd.templateArgs[0].type, "as a key of " + q(t));
            ix->mapSlot = mapMethod(t, "__existing");
            ix->type = value;
            call.lowered = std::move(ix);
            return value;
        }
        if ((call.name == "get" || call.name == "values" || call.name == "items") && !isCopyable(value)) {
            for (auto& a : call.args) checkLooseValue(*a, flow);
            std::string n = spelled(recv, "m");
            diags_.error(call.loc, q(call.name) + " would copy the values, but a " + q(value) + " cannot be copied",
                         "reach a value where it is, with `" + n + "[key]`, and check for it with `" + n + ".contains(key)`", spec::Unique);
            return Type::error();
        }
        if (call.name == "get" && value->isWeak()) {
            for (auto& a : call.args) checkLooseValue(*a, flow);
            std::string n = spelled(recv, "m");
            diags_.error(call.loc, "'get' gives null for a missing key, but a " + q(value) + " cannot be null",
                         "check with `" + n + ".contains(key)`, then use `" + n + ".at(key)`", spec::HashMaps);
            return Type::error();
        }
    }
    BasePath toStart{&sd};
    if (!call.qualifier.empty()) {
        toStart = findQualifier(call.qualifier, sd, call.loc);
        if (toStart.empty()) {
            for (auto& a : call.args) checkLooseValue(*a, flow);
            return Type::error();
        }
    }
    BasePath found = findMember(*toStart.back(), call.name, call.loc);
    FuncDecl* m = found.empty() ? nullptr : found.back()->ownMethod(call.name);
    if (!m) {
        for (auto& a : call.args) checkLooseValue(*a, flow);
        std::string help;
        if (isHashmap(t) && call.name == "find") {
            diags_.error(call.loc, "this compiler does not support find() on a hashmap yet",
                         "there are no iterators yet: `m.contains(key)` says whether it is there, and `m.get(key)` gives the value or null", spec::HashMaps);
            return Type::error();
        }
        const char* planned = nullptr;
        const char* until = nullptr;
        if (sd.templateOf && sd.templateOf->nameLoc.line == 0 && sd.role == StructDecl::Role::Opaque && (call.name == "wait_for" || call.name == "wait_until"))
            planned = "waiting with a time limit", until = "`t.done()` says whether it has finished, without waiting";
        else if (isTaskClass(sd) && (call.name == "cancel" || call.name == "abort" || call.name == "stop" || call.name == "kill"))
            planned = "stopping a task", until = "have what it runs check a field of a class that says it should stop, and return";
        else if (sd.name == "reflect::Field" && (call.name == "get" || call.name == "set"))
            planned = "reading a field through its reflect::Field", until = "`f.name`, `f.offset` and `f.type` say what it is. Use the field by its name";
        else if (sd.name == "reflect::Type" && call.name == "methods")
            planned = "the methods of a type in reflect", until = "`t.fields()` gives its fields";
        if (planned) {
            diags_.error(call.loc, std::string("this compiler does not support ") + planned + " yet", until, sd.name.rfind("reflect::", 0) == 0 ? spec::Reflection : spec::Concurrency);
            return Type::error();
        }
        const Expr* held = call.receiver.get();  // a unique or local reference, which is lent for the call
        while (held && held->kind == Expr::Kind::Borrow) held = static_cast<const BorrowExpr*>(held)->operand.get();
        const Type* recvType = held ? held->type : nullptr;
        const std::string recv = call.receiver ? asBase(spelled(*call.receiver, "x")) : "x";
        if (sd.fieldOwner(call.name)) help = q(call.name) + " is a field; read it without parentheses";
        else if (recvType && (recvType->isUnique() || recvType->isLocal()) && call.name == "reset")
            help = std::string("a ") + (recvType->isUnique() ? "unique" : "local") + " reference is never empty: to be able to empty one, declare it `" + recvType->name + "?` and write `" + recv + " = null`";
        else if (sd.name == "http::Response" && call.name == "json") help = "read the body as JSON: `try json::parse<T>(" + recv + ".body)`, with `import json;`";
        else if (sd.name == "http::Response" && call.name == "text") help = "the text of the response is `" + recv + ".body`";
        else if (sd.templateOf && sd.templateOf->nameLoc.line == 0 && sd.role == StructDecl::Role::Opaque && call.name == "detach")
            help = "remove it: a task left alone keeps running, and the program waits for it before it ends";
        else if (sd.name == "LockGuard" && (call.name == "unlock" || call.name == "lock"))
            help = "it unlocks when the block it is in ends: put it in a block of its own, `{ LockGuard g = LockGuard(m); ... }`";
        else if (isHashmap(t) && (call.name == "emplace" || call.name == "try_emplace"))
            help = "write `m.insert({key, value})`, which also keeps the value that is there";
        else if (isHashmap(t) && (call.name == "insert_or_assign" || call.name == "set" || call.name == "put"))
            help = "write `m[key] = value`";
        else if (isHashmap(t) && (call.name == "begin" || call.name == "end"))
            help = "there are no iterators yet: `for (auto e : m)` goes over the entries, as `e.first` and `e.second`";
        else {
            std::vector<std::string> names;
            std::function<void(const StructDecl&)> collect = [&](const StructDecl& d) {
                for (auto& mm : d.methods)
                    if (mm->role == FuncDecl::Role::Method && mm->name.rfind("__", 0) != 0) names.push_back(mm->name);
                for (const StructDecl* base : d.bases) collect(*base);
            };
            collect(sd);
            if (std::string guess = closest(call.name, names); !guess.empty()) {
                help = "did you mean " + q(guess) + "?";
            } else if (names.empty()) {
                help = sd.nameLoc.line ? q(sd.name) + " has no methods. Write the method inside the type: `void " + call.name + "() { ... }`"
                                       : q(sd.name) + " has no methods. " + listFields(sd);
            } else {
                help = "use one of its methods: ";
                for (size_t i = 0; i < names.size(); ++i) help += (i ? ", " : "") + names[i];
                if (!shownFields(sd).empty()) help += ". It also has fields, such as `" + shownFields(sd)[0]->name + "`";
            }
        }
        diags_.error(call.loc, q(sd.name) + " has no method named " + q(call.name), help, spec::Methods);
        return Type::error();
    }
    call.method = m;
    call.path = toStart;
    call.path.insert(call.path.end(), found.begin() + 1, found.end());
    requestMethod(m);
    if (fn_ && fn_->isAsync && call.name == "wait" && isTaskClass(sd) && call.loc.line != 0) {
        diags_.error(call.loc, "in an async function, a task is waited for with `await`",
                     (call.receiver && spelled(*call.receiver, "").empty()
                          ? "write `" + std::string(m->isThrows ? "try " : "") + "await` in front of the task, instead of `.wait()`"
                          : "write `" + std::string(m->isThrows ? "try " : "") + "await " + asBase(call.receiver ? spelled(*call.receiver, "t") : "t") + "`") +
                         ": `wait()` would hold up everything else that runs on this thread meanwhile",
                     spec::Async);
        return Type::error();
    }
    if (&call != spawned_) checkCanFail(*m, call.loc, call.receiver.get());
    if (!canAccess(*m->owner, m->access, &sd)) reportAccess(*m->owner, m->name, true, m->access, call.loc);
    if (!call.qualifier.empty() && m->isPure)
        diags_.error(call.loc, q(m->owner->name + "::" + m->name) + " has no body, so it cannot be called directly",
                     q(m->owner->name) + " only declares it, so there is nothing to call. Inside an override, write what the method does here. Elsewhere, `x." + m->name + "(...)` calls the version the object really has", spec::Inheritance);
    const bool givesValue = m->retType && !m->retType->is(K::Void);
    const bool resultUsed = givesValue && std::find(discarded_.begin(), discarded_.end(), &call) == discarded_.end();
    const std::string tryFirst = m->callFails() ? "try " : "";
    // `spawn` runs a method of a struct on a copy of it anyway, and what a function gave changes nothing the program has
    if (&call != spawned_ && !m->isConst && t->isStruct() && inOptionalCopy(recv) && (isReferenceInCpp(recv) || !resultUsed)) {
        const std::string keepType = std::string(m->retType->name) + (m->retType->isOptional() || m->retType->isPointer() ? "" : "?");
        std::string help = storeItBack(recv, m->name, !call.args.empty(), tryFirst, resultUsed ? keepType : "");
        const Expr* chainBase = recv.kind == Expr::Kind::OptValue ? static_cast<const OptValueExpr&>(recv).chain->base.get() : nullptr;
        if (resultUsed && chainBase && chainBase->type && constHolderHelp(*chainBase).empty() && loopCopyNote(*chainBase).empty()) {
            // Keep what it gives, which the store-back form would lose
            const CopiedFrom from = namesOnePlace(*chainBase) ? CopiedFrom{} : copiedFrom(*chainBase);
            const std::string o = namesOnePlace(*chainBase) ? asBase(spelled(*chainBase, "")) : from.place;
            if (!o.empty()) {
                const std::string v = freshName(o), r = freshName(o + " " + v, {"result", "got", "made"});
                const std::string type = std::string(m->retType->name) + (m->retType->isOptional() || m->retType->isPointer() ? "" : "?");
                const std::string call_ = "." + m->name + (call.args.empty() ? "()" : "(...)");
                help = from.isInside
                    ? "to keep what it gives, call it where the value is: `" + type + " " + r + " = null; if (" + from.map + ".contains(" + from.key + ")) " + r + " = " + tryFirst + o + call_ + ";`"
                    : !isCopyable(chainBase->type)
                    ? "to keep what it gives, move it out, call it, and move it back: `" + type + " " + r + " = null; if (" + o + ") { auto " + v + " = move(" + o + ").value(); " + r + " = " + tryFirst + v + call_ +
                          "; " + o + " = move(" + v + "); }`"
                    : "to keep what it gives, unwrap it and call it on the copy: `" + type + " " + r + " = null; if (auto " + v + " = " + o + ") { " + r + " = " + tryFirst + v + call_ + "; " + o +
                          " = " + v + "; }`, and leave out `" + o + " = " + v + ";` to keep " + q(o) + " as it is" + (from.map.empty() ? "" : kKeyMayBeAdded);
            }
        }
        diags_.error(call.loc, q(m->name) + " may change the struct, but '?.' works on a copy of it",
                     help + (comesWithTheLanguage(*m->owner) ? "" : ", or mark " + q(m->name) + " `const` if it does not change any field"),
                     spec::Optionals);
    } else if (const std::string copied = &call != spawned_ && !m->isConst && t->isStruct() && isCopyable(t) ? copyOnTheWay(recv, resultUsed) : ""; !copied.empty()) {  // what cannot be copied has been reported
        // In C++ this changes the element, or the variable `?:` chose. Here what was given is a copy, so it would quietly do nothing.
        diags_.error(call.loc, q(m->name) + " may change the struct, but '" + copied + "' gives a copy of it",
                     copyHelp(recv, "." + m->name + (call.args.empty() ? "()" : "(...)"), givesValue, tryFirst) + (comesWithTheLanguage(*m->owner) ? "" : ", or mark " + q(m->name) + " `const` if it does not change any field"), spec::Structs);
    } else if (!m->isConst) {
        // A method that may change fields changes the struct value it is called on,
        // wherever that value is stored.
        const VarDecl* holder = nullptr;
        const Expr* root = nullptr;
        Storage storage = isPlace ? storageOf(recv, root) : Storage::Temporary;
        if (t->isStruct() && storage == Storage::Variable) holder = rootVariable(*root);
        else if (var && var == thisVar() && path.empty()) holder = var;  // `this` of a class
        std::string markConst = isHashmap(t) ? "use a method that does not change it, such as `size`, `empty`, `contains`, `count`, `get`, "
                                                            "`at`, `items`, `keys` or `values`"
                                             : "mark " + q(m->name) + " `const` if it does not change any field";
        if (t->isStruct() && storage == Storage::Pointer && followedPointer(*root).type->constElem)
            diags_.error(call.loc, "cannot call " + q(m->name) + " through a " + q(followedPointer(*root).type) +
                                       ", because it may change fields",
                         markConst + ", or use a pointer that is not const",
                         spec::Pointers);
        else if (holder && holder == thisVar() && fn_->isConst)
            diags_.error(call.loc, "const method " + q(fn_->name) + " cannot call " + q(m->name) + ", which may change fields",
                         "remove `const` from " + q(fn_->name) + ", or " + markConst, spec::Methods);
        else if (t->isStruct() && holder && holder->isConst)
            diags_.error(call.loc, "cannot call " + q(m->name) + " on const " + q(holder->name) + ", because it may change fields",
                         "declare " + q(holder->name) + " without `const`, or " + markConst, spec::Methods);
    }
    checkArgs(call.args, *m, flow, call.loc, q(m->name));
    checkTaskKept(*m, call, call.loc);
    return m->retType;
}

namespace {
struct VectorMethodInfo {
    const char* name;
    VecMethod method;
    bool takesElement;  // push_back(x); the others take no arguments
    bool changes;       // changes the vector
};
const VectorMethodInfo kVectorMethods[] = {
    {"size", VecMethod::Size, false, false},       {"empty", VecMethod::Empty, false, false},
    {"push_back", VecMethod::PushBack, true, true}, {"pop_back", VecMethod::PopBack, false, true},
    {"front", VecMethod::Front, false, false},     {"back", VecMethod::Back, false, false},
    {"clear", VecMethod::Clear, false, true},
};
}  // namespace

// A method that changes a vector or string, like push_back() or clear(), needs a
// receiver that can be changed: a variable or a field, and not a const one.
void Sema::checkChangingReceiver(const Expr& recv, const char* method, const char* what, const char* anchor) {
    const std::string name = method;
    const bool noArguments = name == "clear" || name == "pop_back" || name == "reset";
    const std::string change = "." + name + (noArguments ? "()" : "(...)");
    if (inOptionalCopy(recv)) {
        diags_.error(recv.loc, std::string(method) + "() changes the " + what + ", but '?.' works on a copy of it",
                     storeItBack(recv, method, !noArguments),
                     spec::Optionals);
        return;
    }
    if (!isPlaceKind(recv)) {
        diags_.error(recv.loc, std::string(method) + "() changes the " + what + ", so it must be called on a variable or a field",
                     copyHelp(recv, change), anchor);
        return;
    }
    const VarDecl* var = nullptr;
    std::string path;
    const Expr* root = nullptr;
    if (storageOf(recv, root) == Storage::Variable && root && !containsIndex(recv)) {
        var = rootVariable(*root);
        if (recv.kind == Expr::Kind::Member ||
            (recv.kind == Expr::Kind::Name && static_cast<const NameExpr&>(recv).implicitField >= 0))
            path = outerVectorName(recv);
    }
    checkChangeable(recv, "changed", var, path, change);
}

const Type* Sema::checkVectorMethod(MethodCallExpr& call, Flow& flow, const Type* vecType) {
    const VectorMethodInfo* info = nullptr;
    for (auto& m : kVectorMethods)
        if (call.name == m.name) info = &m;
    if (!isCopyable(vecType->elem) && (call.name == "front" || call.name == "back"))
        diags_.error(call.loc, call.name + "() gives a copy, but a " + q(vecType->elem) + " cannot be copied",
                     [&] {
                         const std::string v = asBase(spelled(*call.receiver, "v"));
                         const std::string at = call.name == "back" ? v + "[" + v + ".size() - 1]" : v + "[0]";
                         return "read the element in place, through `" + at + "`, or take it with `move(" + at + ")`";
                     }(), spec::Unique);
    if (!info) {
        for (auto& a : call.args) checkLooseValue(*a, flow);
        std::string help;
        if (call.name == "emplace_back") help = "write `push_back(value)`";
        else if (call.name == "at") help = "write `v[i]`; indexing is always checked in C+";
        else if (call.name == "length" || call.name == "count") help = "did you mean 'size'?";
        else if (call.name == "insert" || call.name == "erase" || call.name == "resize" || call.name == "reserve" ||
                 call.name == "begin" || call.name == "end") {
            diags_.error(call.loc, "this compiler does not support " + call.name + "() on vectors yet",
                         call.name == "begin" || call.name == "end" ? "go over it with `for (auto x : v)`"
                         : call.name == "reserve" ? "remove the call: a vector grows by itself"
                         : call.name == "resize" ? "grow it with `while (v.size() < n) v.push_back(value);`, and shrink it with `pop_back`"
                                                 : "build a new vector with a loop and `push_back`",
                         spec::Status);
            return Type::error();
        } else {
            if (call.name == "data") {
                diags_.error(call.loc, "this compiler does not support pointers into vectors and strings yet",
                             "copy it into an `array<" + std::string(vecType->elem->name) + ", N>` and use `a.data()`", spec::Status);
                return Type::error();
            }
            std::vector<std::string> names;
            for (auto& m : kVectorMethods) names.push_back(m.name);
            std::string guess = closest(call.name, names);
            help = guess.empty() ? "use one of the vector methods: size, empty, push_back, pop_back, front, back and clear"
                                 : "did you mean " + q(guess) + "?";
            if (call.receiver && call.receiver->kind == Expr::Kind::Member && static_cast<const MemberExpr&>(*call.receiver).field == "headers") {
                const Expr& response = *static_cast<const MemberExpr&>(*call.receiver).base;
                if (response.type && response.type->decl && response.type->decl->name == "http::Response")
                    help = "write `" + asBase(spelled(response, "resp")) + ".header(name)`, which gives the value of a header, or null";
            }
        }
        diags_.error(call.loc, q(vecType) + " has no method named " + q(call.name), help, spec::Vectors);
        return Type::error();
    }
    call.vecMethod = info->method;
    if (info->changes) checkChangingReceiver(*call.receiver, info->name, "vector", spec::Vectors);
    size_t want = info->takesElement ? 1 : 0;
    if (call.args.size() != want) {
        for (auto& a : call.args) checkLooseValue(*a, flow);
        diags_.error(call.loc, std::string(info->name) + "() takes " + (want ? "one argument" : "no arguments") + " but " +
                                   std::to_string(call.args.size()) + (call.args.size() == 1 ? " was" : " were") +
                                   " given",
                     want ? "write `" + std::string(info->name) + "(value)`" : "write `" + std::string(info->name) + "()`",
                     spec::Vectors);
    } else if (want) {
        checkValue(*call.args[0], flow, vecType->elem, std::string("for ") + info->name + "() on " + q(vecType));
    }
    switch (info->method) {
        case VecMethod::Size: return Type::longTy();
        case VecMethod::Empty: return Type::boolTy();
        case VecMethod::Front:
        case VecMethod::Back: return vecType->elem;
        default: return Type::voidTy();
    }
}

namespace {
const VectorMethodInfo kArrayMethods[] = {
    {"size", VecMethod::Size, false, false},   {"empty", VecMethod::Empty, false, false},
    {"front", VecMethod::Front, false, false}, {"back", VecMethod::Back, false, false},
    {"fill", VecMethod::Fill, true, true},     {"data", VecMethod::Data, false, false},
};
}  // namespace

const Type* Sema::checkArrayMethod(MethodCallExpr& call, Flow& flow, const Type* arrayType) {
    const VectorMethodInfo* info = nullptr;
    for (auto& m : kArrayMethods)
        if (call.name == m.name) info = &m;
    if (!isCopyable(arrayType->elem) && (call.name == "front" || call.name == "back" || call.name == "fill"))
        diags_.error(call.loc, call.name + "() copies an element, but a " + q(arrayType->elem) + " cannot be copied",
                     call.name == "fill" ? "set each one: `a[i] = " + std::string(arrayType->elem->isOptional() ? arrayType->elem->elem->name : arrayType->elem->name) + "(...)`" +
                                               (arrayType->elem->isOptional() ? ", or `a[i] = null`" : "")
                     : call.name == "back" ? "reach the element in place, as in `a[a.size() - 1]" + std::string(arrayType->elem->isOptional() ? "?" : "") + ".field`, or take it with `move(a[a.size() - 1])`"
                                           : "reach the element in place, as in `a[0]" + std::string(arrayType->elem->isOptional() ? "?" : "") + ".field`, or take it with `move(a[0])`", spec::Unique);
    if (!info) {
        for (auto& a : call.args) checkLooseValue(*a, flow);
        static const std::set<std::string> grow = {"push_back", "emplace_back", "pop_back", "clear", "insert",
                                                   "erase",     "resize",       "reserve"};
        std::string elem = arrayType->elem->name;
        std::string help;
        if (grow.count(call.name)) help = "an array always has the same number of elements. For a list that can grow, use `vector<" + elem + ">`";
        else if (call.name == "at") help = "write `a[i]`; indexing is always checked in C+";
        else if (call.name == "length" || call.name == "count") help = "did you mean 'size'?";
        else if (call.name == "begin" || call.name == "end" || call.name == "swap") {
            diags_.error(call.loc, "this compiler does not support " + call.name + "() on arrays yet",
                         call.name == "swap" ? std::string("write it with a third variable: `auto t = a; a = b; b = t;`") : std::string("go over it with `for (auto x : a)`"), spec::Status);
            return Type::error();
        } else {
            std::vector<std::string> names;
            for (auto& m : kArrayMethods) names.push_back(m.name);
            std::string guess = closest(call.name, names);
            help = guess.empty() ? "use one of the array methods: size, empty, front, back, fill and data"
                                 : "did you mean " + q(guess) + "?";
        }
        diags_.error(call.loc, q(arrayType) + " has no method named " + q(call.name), help, spec::Arrays);
        return Type::error();
    }
    call.vecMethod = info->method;
    Expr& recv = *call.receiver;
    if (info->changes) checkChangingReceiver(recv, info->name, "array", spec::Arrays);
    size_t want = info->takesElement ? 1 : 0;
    if (call.args.size() != want) {
        for (auto& a : call.args) checkLooseValue(*a, flow);
        diags_.error(call.loc, std::string(info->name) + "() takes " + (want ? "one argument" : "no arguments") + " but " +
                                   std::to_string(call.args.size()) + (call.args.size() == 1 ? " was" : " were") +
                                   " given",
                     want ? "write `" + std::string(info->name) + "(value)`" : "write `" + std::string(info->name) + "()`",
                     spec::Arrays);
    } else if (want) {
        checkValue(*call.args[0], flow, arrayType->elem, std::string("for ") + info->name + "() on " + q(arrayType));
    }
    switch (info->method) {
        case VecMethod::Size: return Type::longTy();
        case VecMethod::Empty: return Type::boolTy();
        case VecMethod::Front:
        case VecMethod::Back: return arrayType->elem;
        case VecMethod::Data: {
            // The same rules as `&a[0]`: the pointer must have something lasting to point to.
            const Expr* root = nullptr;
            if (!isPlaceKind(recv) || storageOf(recv, root) == Storage::Temporary || inTemporaryObject(recv) ||
                throughOptionalChain(recv)) {
                diags_.error(call.loc, "data() gives a pointer into the array, but this array is a temporary value",
                             throughOptionalChain(recv) ? "unwrap it first, and call data() on what is inside: `if (auto u = opt) { ... u.a.data() ... }`. For a struct, `u` is a copy, so store it back after writing: `opt = u;`"
                                                        : "it goes away when the statement ends. Store it in a variable first, and call data() on that",
                             spec::Arrays);
                return Type::error();
            }
            if (insideContainer(recv)) {
                diags_.error(call.loc, "this compiler does not support pointers into vectors and strings yet",
                             "their elements move when a vector grows or a shared string is changed. Copy the array into "
                             "a variable and call data() on that",
                             spec::Status);
                return Type::error();
            }
            return Type::pointerTo(arrayType->elem, isReadOnlyPlace(recv));
        }
        default: return Type::voidTy();
    }
}


// `vector<T>()`, `vector<T>(n)` or `vector<T>(n, value)`.
const Type* Sema::checkVectorConstruction(CallExpr& call, Flow& flow) {
    const Type* vt = call.vectorType;
    std::string name = vt->name;
    if (call.args.size() > 2) {
        for (auto& a : call.args) checkLooseValue(*a, flow);
        diags_.error(call.loc, name + "(...) takes a size and an optional value, but " + std::to_string(call.args.size()) +
                                   " values were given",
                     "to list the elements, use braces: `" + name + (vt->elem->isNumber() ? "{1, 2, 3}" : "{a, b, c}") + "`", spec::Vectors);
        return vt;
    }
    if (!call.args.empty() && call.args[0]->kind == Expr::Kind::InitList) {
        checkValue(*call.args[0], flow, Type::longTy(), "as the size of " + q(vt));
    } else if (!call.args.empty()) {
        const Type* given = checkExpr(*call.args[0], flow);
        if (given == vt && call.args.size() == 1) {  // `vector<int>(v)` copies it, as in C++
            rejectCopy(*call.args[0]);
            call.copies = true;
            return vt;
        }
        requireConvertible(*call.args[0], Type::longTy(), "as the size of " + q(vt));
    }
    if (call.args.size() == 2) {
        checkValue(*call.args[1], flow, vt->elem, "as the element value of " + q(vt));
        if (!isCopyable(vt->elem))
            diags_.error(call.args[1]->loc, "every element would be a copy of this, but a " + q(vt->elem) + " cannot be copied",
                         "add them one by one with `push_back(...)`", spec::Unique);
    } else if (call.args.size() == 1 && !hasDefault(vt->elem)) {
        diags_.error(call.loc, name + "(n) cannot make its elements on its own, because " + elementDefaultProblem(vt->elem),
                     vt->elem->isClass() ? "start empty, and `push_back` a new object in a loop: every element of `" + name + "(n, value)` would be one object"
                     : isCopyable(vt->elem) ? "give the value to fill it with, as in `" + name + "(n, value)`, or start empty and use push_back"
                                          : "start empty and add them with `push_back`: `" + name + " v;`",
                     spec::Vectors);
    }
    return vt;
}

static const char* kAllowNewDelete =
    " To use new and delete on purpose, write `[[allow(new_delete)]]` before the function, or `[[allow(new_delete)]];` "
    "at the top of the file";

void Sema::warnManualMemory(Loc loc, const std::string& message, const std::string& alternative) {
    if (!warnNewDelete_ || program_->allowNewDelete || (fn_ && fn_->allowNewDelete)) return;
    diags_.warning(loc, message, alternative + "." + kAllowNewDelete, spec::NewDelete);
}

// `new T`, `new T(...)`, `new T{...}` or `new T[n]`: a `T*` to memory that stays until it
// is deleted.
const Type* Sema::checkNew(NewExpr& n, Flow& flow) {
    const Type* t = n.target;
    std::string name = t->name;
    if (n.count)
        warnManualMemory(n.loc, "memory from 'new[]' must be freed by hand, with 'delete[]'",
                         "a vector frees its elements by itself and checks every index: `auto v = vector<" + name +
                             ">(n);`");
    else
        warnManualMemory(n.loc, "memory from 'new' must be freed by hand, with 'delete'",
                         "C+ frees memory by itself when it can: a local variable lasts until its block ends, and a "
                         "class object, made with `Name(...)`, is freed when nothing refers to it");
    if (!checkStorable(t, n.typeLoc)) return Type::error();
    if (t->is(K::Void)) {
        diags_.error(n.typeLoc, "new needs the type of the value it makes, not void",
                     "for memory of no particular type, use C's malloc, declared as `extern \"C\" void* malloc(long "
                     "size);`",
                     spec::NewDelete);
        return Type::error();
    }
    if (n.count) {
        checkValue(*n.count, flow, Type::longTy(), "as the number of elements in `new " + name + "[...]`");
        if (n.count->kind == Expr::Kind::IntLit && static_cast<IntLitExpr&>(*n.count).value < 0)
            diags_.error(n.count->loc, "an array cannot have a negative number of elements", "give it zero or more elements", spec::NewDelete);
    }
    if (n.init) {
        checkValue(*n.init, flow, t, "for the value of `new " + name + "`");
    } else if (!hasDefault(t)) {
        std::string why = elementDefaultProblem(t);
        diags_.error(n.loc,
                     (n.count ? "`new " + name + "[n]` cannot make its elements" : "`new " + name + "` cannot make its value") +
                         " on its own, because " + why,
                     !n.count && (t->isUnique() || t->isLocal()) ? "give the value: `new " + name + "(" + name + "(...))`"
                     : n.count ? "use a vector, which can start empty and grow with push_back: `vector<" + name + "> v;`"
                             : "give the value: `new " + name + "{...}` or `new " + name + "(...)`",
                     spec::NewDelete);
    }
    return Type::pointerTo(t);
}

// `delete p` or `delete[] p`, on a pointer that new returned.
const Type* Sema::checkDelete(DeleteExpr& d, Flow& flow) {
    const Type* t = checkExpr(*d.operand, flow);
    std::string op = d.array ? "delete[]" : "delete";
    warnManualMemory(d.loc, "'" + op + "' frees memory by hand, so nothing stops it from being used afterward",
                     "memory that a variable, a vector, a string or a class object holds is freed automatically");
    Loc loc = d.operand->loc;
    if (t->isError() || (t->isPointer() && !t->elem->is(K::Void) && !t->elem->isOpaque())) return Type::voidTy();
    if (t->isClass())
        diags_.error(loc, "a class object is freed automatically, when nothing refers to it any more",
                     "to free it sooner, let go of its references: assign another object to the variable, or make "
                     "the variable optional, `" + std::string(t->name) + "?`, and set it to null",
                     spec::Classes);
    else if (t->isNull())
        diags_.error(loc, "'" + op + "' needs a pointer, but this is null", "deleting null does nothing; remove it",
                     spec::NewDelete);
    else if (t->isPointer() && t->elem->is(K::Void))
        diags_.error(loc, "'" + op + "' cannot free a " + q(t) + ", since C+ does not know what it points to",
                     "cast it back to the pointer new returned, e.g. `" + op + " static_cast<int*>(p);`. Memory "
                     "from C's malloc is freed with C's free",
                     spec::NewDelete);
    else if (t->isPointer())
        diags_.error(loc, q(t->elem) + " is a C type known only by name, so its memory belongs to its C library",
                     "free it with the C function made for it, such as fclose for a FILE*", spec::CTypes);
    else
        diags_.error(loc, "'" + op + "' frees memory from new through a pointer, but this is " + q(t),
                     t->isVector() || t->is(K::String) ? "remove it: a " + std::string(t->isVector() ? "vector" : "string") +
                                                             " frees its memory by itself"
                                                       : "remove it: `delete` is only for what `new` gave",
                     spec::NewDelete);
    return Type::voidTy();
}

// print, println and format: a format string, then one value for each {} in it.
const Type* Sema::checkPrint(CallExpr& call, Flow& flow) {
    call.builtin = call.callee == "println" ? Builtin::Println
                   : call.callee == "print" ? Builtin::Print
                                            : Builtin::Format;
    bool format = call.builtin == Builtin::Format;
    const Type* result = format ? Type::stringTy() : Type::voidTy();
    for (auto& a : call.args) checkExpr(*a, flow);
    if (call.args.empty()) {
        if (call.builtin != Builtin::Println)
            diags_.error(call.loc, call.callee + " needs a format string",
                         "write e.g. `" + call.callee + "(\"{}\", value)`", spec::Printing);
        call.formatPieces = {""};
        return result;
    }
    Expr& fmtExpr = *call.args[0];
    if (fmtExpr.kind != Expr::Kind::StringLit) {
        diags_.error(fmtExpr.loc, "the first argument of " + call.callee + " must be a format string written in quotes",
                     format ? "to turn a value into a string, write `format(\"{}\", value)`"
                            : "to print a value, write `" + call.callee + "(\"{}\", value);`",
                     spec::Printing);
        return result;
    }
    const std::string& fmt = static_cast<StringLitExpr&>(fmtExpr).value;
    std::vector<std::string> pieces{""};
    std::vector<FormatSpec> specs;
    std::vector<std::string> specTexts;  // as written, for errors
    for (size_t i = 0; i < fmt.size(); ++i) {
        char c = fmt[i];
        char n = i + 1 < fmt.size() ? fmt[i + 1] : '\0';
        if (c == '{' && n == '{') { pieces.back() += '{'; ++i; }
        else if (c == '}' && n == '}') { pieces.back() += '}'; ++i; }
        else if (c == '{') {
            // `{}`, or `{:.2f}`: a precision and a type, f, e or g, for a floating-point value.
            size_t close = fmt.find('}', i);
            std::string inside = close == std::string::npos ? "" : fmt.substr(i + 1, close - i - 1);
            FormatSpec spec;
            bool ok = close != std::string::npos && (inside.empty() || inside[0] == ':');
            size_t j = 1;
            if (ok && j < inside.size() && inside[j] == '.') {
                size_t start = ++j;
                while (j < inside.size() && std::isdigit((unsigned char)inside[j]) && j - start < 4) ++j;
                ok = j > start;
                if (ok) spec.precision = std::stoi(inside.substr(start, j - start));
            }
            if (ok && j < inside.size() && (inside[j] == 'f' || inside[j] == 'e' || inside[j] == 'g')) spec.type = inside[j++];
            ok = ok && j >= inside.size();
            if (!ok) {
                std::string shown = close == std::string::npos ? fmt.substr(i) : "{" + inside + "}";
                if (close != std::string::npos && !inside.empty() && std::isdigit((unsigned char)inside[0]))
                    diags_.error(fmtExpr.loc, "this compiler does not support numbered placeholders like " + shown + " yet",
                                 "write {} for each value, in order", spec::Printing);
                else if (close == std::string::npos)
                    diags_.error(fmtExpr.loc, "this format string has a '{' with no matching '}'",
                                 "write {{ for a literal '{'", spec::Printing);
                else
                    diags_.error(fmtExpr.loc, "this compiler does not support the format option " + shown + " yet",
                                 shown.size() > 1 && shown[1] != ':'
                                     ? "a placeholder is `{}`, with the value after the format string: `println(\"{}\", name)`. A literal brace is written {{"
                                     : "write {} to print it as it is, or give the precision of a floating-point value and f, e or g, such "
                                       "as {:.2f}, {:e} or {:.3g}. Width and alignment are planned",
                                 spec::Printing);
                return result;
            }
            pieces.emplace_back();
            specs.push_back(spec);
            specTexts.push_back("{" + inside + "}");
            i = close;
        } else if (c == '}') {
            diags_.error(fmtExpr.loc, "this format string has a '}' with no matching '{'",
                         "write }} for a literal '}'", spec::Printing);
            return result;
        } else pieces.back() += c;
    }
    size_t placeholders = pieces.size() - 1, values = call.args.size() - 1;
    if (placeholders != values) {
        diags_.error(call.loc, "the format string has " + std::to_string(placeholders) + " {} placeholder" +
                                   (placeholders == 1 ? "" : "s") + " but " + std::to_string(values) + " value" +
                                   (values == 1 ? " was" : "s were") + " given",
                     [&] {
                         const Expr& format = *call.args[0];
                         const std::string text = format.kind == Expr::Kind::StringLit ? static_cast<const StringLitExpr&>(format).value : "";
                         if (text.find('%') == std::string::npos) return std::string("use one {} for each value after the format string");
                         std::string out;  // `%d` as `{}`, `%.2f` as `{:.2f}`
                         for (size_t i = 0; i < text.size(); ++i) {
                             if (text[i] != '%') { out += text[i]; continue; }
                             if (i + 1 < text.size() && text[i + 1] == '%') { out += '%'; ++i; continue; }
                             size_t j = i + 1;
                             while (j < text.size() && std::strchr("-+ 0#", text[j])) ++j;
                             while (j < text.size() && std::isdigit((unsigned char)text[j])) ++j;
                             std::string precision;
                             if (j < text.size() && text[j] == '.') {
                                 size_t k = j + 1;
                                 while (k < text.size() && std::isdigit((unsigned char)text[k])) ++k;
                                 precision = text.substr(j, k - j);
                                 j = k;
                             }
                             while (j < text.size() && std::strchr("hlzjtL", text[j])) ++j;
                             const char kind = j < text.size() ? text[j] : 'd';
                             out += kind == 'f' || kind == 'F' ? (precision.empty() ? "{}" : "{:" + precision + "f}")
                                  : kind == 'e' || kind == 'g' || kind == 'x' || kind == 'X' || kind == 'o' ? "{:" + precision + std::string(1, kind) + "}"
                                  : std::string("{}");
                             i = j;
                         }
                         return "C+ marks each value with `{}`, as C++'s `std::format` does, where C writes `%d`: write " + stringLiteral(out);
                     }(), spec::Printing);
        return result;
    }
    for (size_t i = 1; i < call.args.size(); ++i) {
        const Type* t = call.args[i]->type;
        const FormatSpec& spec = specs[i - 1];
        if (t->is(K::Void))
            diags_.error(call.args[i]->loc, "this expression produces no value, so it cannot be printed", "call it on a line of its own, and print what it changes", spec::Printing);
        else if (t->isPointer() && t->elem->is(K::Char) && !spec.type && spec.precision < 0) {
            std::string n = spelled(*call.args[i], "p");
            diags_.error(call.args[i]->loc,
                         "C++ prints a " + q(t) + " as the text it points to, but C+ cannot tell where that text ends",
                         "write `string(" + n + ")` to print the text, which must end with a zero byte, or `(" + (t->constElem ? "const " : "") + "void*)" + asBase(n) +
                             "` to print the address",
                         spec::Printing);
        }
        else if ((spec.type || spec.precision >= 0) && !t->isFloating() && !t->isError())
            diags_.error(call.args[i]->loc, specTexts[i - 1] + " formats a float or a double, but this is " + q(t),
                         t->isInteger() ? "write {} for an integer, or convert it first: `double(n)`"
                         : t->isOptional() && t->elem->isFloating() ? "give it a fallback where it is passed: `" + asBase(spelled(*call.args[i], "d")) + " ?? " + (t->elem->is(K::Float) ? "0.0f" : "0.0") + "`"
                                        : "write {} to print it as it is",
                         spec::Printing);
    }
    call.formatPieces = std::move(pieces);
    call.formatSpecs = std::move(specs);
    return result;
}

// `to_string(n)`, as in C++: the decimal digits of an int or a long.
const Type* Sema::checkToString(CallExpr& call, Flow& flow) {
    call.builtin = Builtin::ToString;
    call.formatPieces = {"", ""};
    if (call.args.size() != 1) {
        for (auto& a : call.args) checkLooseValue(*a, flow);
        diags_.error(call.loc, "to_string() takes one argument but " + std::to_string(call.args.size()) +
                                   (call.args.size() == 1 ? " was" : " were") + " given",
                     "write `to_string(n)`; to build text from several values, write `format(\"{} {}\", a, b)`",
                     spec::Strings);
        return Type::stringTy();
    }
    const Type* t = checkExpr(*call.args[0], flow);
    if (!t->isError() && !t->isNumber())
        diags_.error(call.args[0]->loc, "to_string() takes a number, but this is " + q(t),
                     t->is(K::String) ? "remove `to_string`: it is already a string"
                     : t->is(K::Char) ? "in C++, to_string(c) gives the code of the char, such as \"97\". Write "
                                        "`string(1, c)` for the char itself, or `to_string(int(c))` for its code"
                     : t->is(K::Void) ? "the call gives nothing to turn into text. Call it on a line of its own, and pass `to_string` a number"
                                      : textHelp(t, "+", spelled(*call.args[0], "x")),
                     spec::Strings);
    return Type::stringTy();
}

// `int(x)`, `static_cast<int>(x)` and `(int)x` convert between the number types and bool,
// as in C++. A conversion that cannot keep the value stops the program (see Codegen).
// `reinterpret_cast`: between a pointer and a number, which no other cast does, for
// addresses that C libraries use as signals, such as `reinterpret_cast<void*>(-1)`. Between
// pointers it is like the other casts, as in C++.
const Type* Sema::checkReinterpretCast(CastExpr& c, const Type* from, const Type* to) {
    if (from->isError()) return to;
    if (to->isPointer() && from->isInteger()) return to;
    if (to->isPointer() && (from->isPointer() || from->isNull())) return checkPointerCast(c, from, to);
    if (from->isPointer() && to->is(K::Long)) return to;
    if (from->isPointer())
        diags_.error(c.loc, "reinterpret_cast turns a pointer into a long, but this asks for " + q(to),
                     "write `reinterpret_cast<long>(p)`: only a long is sure to hold an address", spec::Casts);
    else
        diags_.error(c.loc, "reinterpret_cast converts between a pointer and an integer, or between pointers, but this "
                            "goes from " + q(from) + " to " + q(to),
                     from->isNumber() && to->is(K::String) ? std::string("to turn a number into text, write `to_string(x)`")
                     : from->isFloating() && to->isPointer() ? "turn it into a long first: `reinterpret_cast<" + std::string(to->name) + ">(long(x))`"
                     : from->isNumber() ? "to convert a number, write `" + std::string(to->name) + "(x)`"
                     : from->isClass() && to->isClass() ? "write `dynamic_cast<" + std::string(to->name) + "?>(x)`, which gives null when it is not one"
                                                        : std::string("write a number or a pointer here: nothing else can be reinterpreted"),
                     spec::Casts);
    return to;
}

// `dynamic_cast<Dog?>(a)` gives null when `a` is not a Dog, as C++ does for `Dog*`, and
// `dynamic_cast<Dog>(a)` stops the program. The type written is the type of the result.
const Type* Sema::checkDynamicCast(CastExpr& c, const Type* from, const Type* to) {
    const Type* toClass = to->isOptional() ? to->elem : to;
    const Type* fromClass = from->isOptional() ? from->elem : from;
    if (!toClass->isClass()) {
        bool toPointer = to->isPointer() && to->elem->isClass();
        diags_.error(c.loc, "dynamic_cast finds out whether an object is of a class, but this asks for " + q(to),
                     toPointer ? "a class variable is already a reference, so write `dynamic_cast<" +
                                     std::string(to->elem->name) + "?>(x)`, which gives null when it is not"
                               : "write e.g. `dynamic_cast<Dog?>(x)`, which gives null when it is not, for a class named Dog. "
                                 "Between numbers, write `static_cast`",
                     spec::Inheritance);
        return to;
    }
    if (from->isError()) return to;
    if (!fromClass->isClass()) {
        diags_.error(c.operand->loc, "dynamic_cast needs a class object, but this is " + q(from),
                     from->isWeak() ? "the object may be gone. Write `dynamic_cast<" + std::string(toClass->name) + "?>(" +
                                          spelled(*c.operand, "w") + ".lock())`"
                     : from->isUnique() || from->isLocal() ? "write `dynamic_cast<" + std::string(toClass->name) + "?>(" + spelled(*c.operand, "u") + ".get())`"
                                        : "use it on a class reference",
                     spec::Inheritance);
        return to;
    }
    // An object can be of both only if some class derives from both.
    for (auto& sd : program_->structs)
        if (sd->derivesFrom(toClass->decl) && sd->derivesFrom(fromClass->decl)) return to;
    diags_.error(c.loc, "a " + q(fromClass) + " can never be a " + q(toClass) + ", because no class derives from both",
                 "check which class you meant, or derive a class from both", spec::Inheritance);
    return to;
}

const Type* Sema::checkCast(CastExpr& c, Flow& flow) {
    if (!c.operand) {  // `T()` where T is not a struct or class: its default value
        if (needsValue(c.target))
            diags_.error(c.loc, "a " + q(c.target) + " has no default value, since it is never empty",
                         "give it a value. For one that can be empty, use `" + std::string(c.target->name) + "?`", spec::Unique);
        return c.target;
    }
    const Type* from = checkExpr(*c.operand, flow);
    const Type* to = c.target;
    checkStorable(to, c.loc);
    if (c.reinterpret) return checkReinterpretCast(c, from, to);
    if (c.dynamic) return checkDynamicCast(c, from, to);
    if (from == to) {  // a copy, which is what `T(x)` is when x is a T
        rejectCopy(*c.operand);
        return to;
    }
    if (from->isLocal() || to->isLocal()) {
        diags_.error(c.loc, "a cast cannot change a reference that only this thread counts, as this one from " + q(from) + " to " + q(to) + " would",
                     from->isLocal() && to->isOptional() && to->elem->isObjectRef() &&
                             ((to->elem->isLocal() && to->elem->elem && (to->elem->elem->decl == from->elem->decl || from->elem->decl->derivesFrom(to->elem->elem->decl))) ||
                              (to->elem->isClass() && (to->elem->decl == from->elem->decl || from->elem->decl->derivesFrom(to->elem->decl))))
                         ? (to->elem->isLocal() ? "no cast is needed: `" + std::string(to->name) + " x = " + asBase(spelled(*c.operand, "l")) + ";`"
                                                : "`" + std::string(to->name) + " x = " + asBase(spelled(*c.operand, "l")) + ".get();` lends the object")
                     : from->isLocal() && to->isOptional() && to->elem->isObjectRef()
                         ? "this cannot become a " + q(to) + ": build a new object, as in `" + std::string(to->elem->name) + "(...)`"
                     : from->isLocal() && !to->isObjectRef()
                         ? std::string("this holds an object, not a number. `x.get()` lends the object, and `x.field` reads a field")
                         : !from->isLocal() ? (from->isClass() ? "a '" + std::string(from->name) + "' may be used by other threads. " : std::string()) + "Write `" + std::string(to->name) + "(...)` to build a new object"
                                            : std::string("a local reference converts by itself to one of a base class. `x.get()` lends the object"),
                     spec::Local);
        return to;
    }
    if (from->isUnique() || to->isUnique()) {
        diags_.error(c.loc, "a cast cannot change who owns an object, as this one from " + q(from) + " to " + q(to) + " would",
                     from->isUnique() && to->isOptional() && to->elem->isObjectRef() &&
                             !((to->elem->isUnique() && to->elem->elem && (to->elem->elem->decl == from->elem->decl || from->elem->decl->derivesFrom(to->elem->elem->decl))) ||
                               (to->elem->isClass() && (to->elem->decl == from->elem->decl || from->elem->decl->derivesFrom(to->elem->decl))))
                         ? "this cannot become a " + q(to) + ": build a new object, as in `" + std::string(to->elem->name) + "(...)`"
                     : from->isUnique() && to->isOptional() && to->elem->isObjectRef()
                         ? (to->elem->isUnique() ? "no cast is needed: `" + std::string(to->name) + " x = " +
                                                       (spelled(*c.operand, "u").rfind("move(", 0) == 0 ? spelled(*c.operand, "u") : "move(" + spelled(*c.operand, "u") + ")") + ";`"
                                                 : "`" + std::string(to->name) + " x = " + asBase(spelled(*c.operand, "u")) + ".get();` lends the object")
                     : from->isUnique() && !to->isObjectRef()
                         ? std::string("this holds an object, not a number. `u.get()` lends the object, and `u.field` reads a field")
                         : !from->isUnique() ? (from->isClass() ? "a '" + std::string(from->name) + "' may be shared, so it has no one owner. " : std::string()) + "Write `" + std::string(to->name) + "(...)` to build a new object"
                                             : "a unique reference converts by itself when it is moved: `" + std::string(to->name) + " x = move(u);`. `u.get()` lends the object",
                     spec::Unique);
        return to;
    }
    if (to->isWeak()) {  // `weak<Node>(object)`
        if (!canConvert(from, to))
            diags_.error(c.operand->loc, std::string(to->name) + "(x) refers to an object of class " + q(to->elem) + ", but this is " + q(from), "pass an object of that class, or of one that derives from it", spec::Weak);
        return to;
    }
    if (from->isWeak()) {
        diags_.error(c.loc, "a cast cannot read a weak reference, since the object may be gone",
                     "write `" + spelled(*c.operand, "w") + ".lock()`, which gives a `" + std::string(from->elem->name) + "?`", spec::Weak);
        return to;
    }
    const Type* toClass = to->isOptional() ? to->elem : to;
    const Type* fromClass = from->isOptional() ? from->elem : from;
    if (toClass->isClass() && fromClass->isClass()) {
        if (canConvert(from, to)) return to;  // `static_cast<Animal>(dog)`, to choose a type for `?:`
        if (fromClass->decl->partsOf(toClass->decl) > 1) {
            diags_.error(c.loc, q(fromClass) + " holds " + q(toClass) + " more than once, so this does not say which is meant",
                         "first cast to the base class that leads to the one you mean, which holds it once", spec::Inheritance);
            return to;
        }
        if (toClass->decl->derivesFrom(fromClass->decl)) {
            diags_.error(c.loc, "this cast could not know whether the object really is a " + q(toClass),
                         "write `dynamic_cast<" + std::string(toClass->name) + "?>(x)`, which gives null when it is not",
                         spec::Inheritance);
            return to;
        }
    }
    if (to->isPointer() || (from->isPointer() && !to->is(K::Bool) && !to->is(K::String)))
        return checkPointerCast(c, from, to);
    std::string cast = std::string(to->name) + "(x)";
    bool castable = to->isNumber() || to->is(K::Char) || to->is(K::Bool);
    if (from->isError()) return castable ? to : Type::error();
    if (to->is(K::String) && from->is(K::String)) return to;  // `string("text")`, as in C++: a copy
    if (to->is(K::String) && from->isArray() && from->elem->is(K::Char)) return to;  // its text, up to a zero byte
    if (to->is(K::String) && from->isPointer()) {  // `string(p)` copies the text a `char*` points to
        if (!from->elem->is(K::Char))
            diags_.error(c.operand->loc, "string(p) copies the text a char* points to, but this is " + q(from), "give it a `char*`, or `to_string(x)` for a number", spec::Pointers);
        return to;
    }
    if (to->is(K::String)) {
        diags_.error(c.loc, "a cast cannot turn a value into a string",
                     from->isNumber()   ? "write `to_string(x)`, or `format(\"{}\", x)`"
                     : from->is(K::Char) ? "write `string(1, c)` for a string holding the char"
                                         : "write `format(\"{}\", x)`, which turns any value that can be printed "
                                           "into a string",
                     spec::Casts);
        return Type::stringTy();
    }
    if (!castable) {
        diags_.error(c.loc, "a cast converts between numbers, char and bool, but this converts to " + q(to),
                     to->isOptional() && canConvert(from, to) ? "remove the cast: a value converts to an optional "
                                                                "by itself"
                                                              : to->isStruct() || to->isVector() || to->isClass()
                                                                    ? (to->isClass() ? "give it a value such as " + declarationOf(to, "x") : "build one instead: `" + std::string(to->name) + "{...}`")
                                                                    : "a cast can only convert to int, long, float, double, char or bool",
                     spec::Casts);
        return to;
    }
    if (from->isNumber() || from->is(K::Char) || from->is(K::Bool) || from->isPointer()) return to;
    if (from->is(K::String) && to->is(K::Char))
        diags_.error(c.loc, "a cast cannot turn a string into a char",
                     "`s[i]` is the char at position i of a string, and 'a' in single quotes is a char", spec::Casts);
    else if (from->is(K::String))
        diags_.error(c.loc, "this compiler does not support reading a number from a string yet",
                     to->is(K::Bool)      ? "compare the text instead, e.g. `s == \"true\"`"
                                          : readingNumberHelp(to),
                     spec::Status);
    else
        diags_.error(c.operand->loc, "'" + cast + "' converts a number, a char or a bool, but this is " + q(from),
                     from->isOptional() ? unwrapHelp(spelled(*c.operand))
                     : from->isVector() || from->isArray() ? "convert one of its elements instead, e.g. `" + std::string(to->name) + "(" + asBase(spelled(*c.operand, "v")) + "[0])`"
                     : from->hasFields() && !isHashmap(from) && firstNumberField(*from->decl) ? "convert one of its fields instead, e.g. `" + std::string(to->name) + "(" + asBase(spelled(*c.operand, "x")) + "." + firstNumberField(*from->decl)->name + ")`"
                                         : std::string("give it a number, a char or a bool"),
                     spec::Casts);
    return to;
}

// A cast to a pointer type, or from a pointer to anything but bool and string.
const Type* Sema::checkPointerCast(CastExpr& c, const Type* from, const Type* to) {
    if (from->isError()) return to;
    if (!to->isPointer()) {
        diags_.error(c.loc, "a cast cannot turn a pointer into " + q(to),
                     "for a bool, compare it with null: `p != null`. The distance between two pointers into the same "
                     "memory is `p - q`. The address as a number is `reinterpret_cast<long>(p)`",
                     spec::Casts);
        return to;
    }
    if (from->isNull()) return to;
    if (!from->isPointer()) {
        diags_.error(c.operand->loc, "a cast to " + q(to) + " converts another pointer, but this is " + q(from),
                     from->isInteger()       ? "a pointer to nothing is `null`. To make a pointer from a number, such as a "
                                               "C constant that is -1 as a pointer, write `reinterpret_cast<void*>(-1)`"
                     : from->isClass() && to->elem->isClass()
                         ? "a class variable is already a reference, so no pointer is needed. To find out whether an object "
                           "is of a class, write `dynamic_cast<" + std::string(to->elem->name) + "?>(x)`"
                     : from->isArray()         ? "for a pointer to its first element, write `" + spelled(*c.operand, "a") + ".data()`"
                     : from->is(K::String) ? std::string("there are no pointers into a string yet. A C function with a `string` parameter takes `s` itself; for a buffer, use `array<char, N>` and `a.data()`")
                     : from->isVector() ? "there are no pointers into a vector yet. Copy it into an `array<" + std::string(from->elem->name) + ", N>` and use `a.data()`"
                     : from->isClass() ? std::string("C cannot hold a C+ object: pass the fields it needs as separate arguments. `&x` would be the address of the variable, not of the object")
                     : from->isOptional() ? unwrapHelp(spelled(*c.operand))
                     : isPlaceKind(*c.operand) ? "write `&" + spelled(*c.operand, "x") + "` for its address"
                                               : "give it another pointer",
                     spec::Casts);
        return to;
    }
    if (from->constElem && !to->constElem)
        diags_.error(c.loc, "a cast cannot remove the const of " + q(from),
                     "C+ has no const_cast. Cast to `const " + std::string(to->elem->name) + "*` instead, and keep a "
                     "pointer that is not const for the code that changes the memory",
                     spec::Casts);
    return to;
}

namespace {
struct StringMethodInfo {
    const char* name;
    StrMethod method;
    const char* usage;  // how it is called, for messages
    size_t minArgs, maxArgs;
    bool takesText;  // the first argument is a string; any others are positions
    bool changes;    // changes the string
};
const StringMethodInfo kStringMethods[] = {
    {"size", StrMethod::Size, "size()", 0, 0, false, false},
    {"length", StrMethod::Size, "length()", 0, 0, false, false},
    {"empty", StrMethod::Empty, "empty()", 0, 0, false, false},
    {"substr", StrMethod::Substr, "substr(pos, count)", 0, 2, false, false},
    {"find", StrMethod::Find, "find(text)` or `s.find(text, pos)", 1, 2, true, false},
    {"contains", StrMethod::Contains, "contains(text)", 1, 1, true, false},
    {"starts_with", StrMethod::StartsWith, "starts_with(text)", 1, 1, true, false},
    {"ends_with", StrMethod::EndsWith, "ends_with(text)", 1, 1, true, false},
    {"clear", StrMethod::Clear, "clear()", 0, 0, false, true},
    {"push_back", StrMethod::PushBack, "push_back(c)", 1, 1, false, true},
    {"pop_back", StrMethod::PopBack, "pop_back()", 0, 0, false, true},
    {"front", StrMethod::Front, "front()", 0, 0, false, false},
    {"back", StrMethod::Back, "back()", 0, 0, false, false},
};
const char* kStringMethodList =
    "use one of the string methods today: size, length, empty, substr, find, contains, starts_with, ends_with, clear, "
    "push_back, pop_back, front and back";

std::string argumentCount(size_t min, size_t max) {
    if (max == 0) return "no arguments";
    if (min == max) return min == 1 ? "one argument" : std::to_string(min) + " arguments";
    if (min == 0) return "at most " + std::string(max == 1 ? "one argument" : std::to_string(max) + " arguments");
    return std::to_string(min) + " or " + std::to_string(max) + " arguments";
}
}  // namespace

const Type* Sema::checkStringMethod(MethodCallExpr& call, Flow& flow) {
    const StringMethodInfo* info = nullptr;
    for (auto& m : kStringMethods)
        if (call.name == m.name) info = &m;
    if (!info) {
        for (auto& a : call.args) checkLooseValue(*a, flow);
        const std::string& n = call.name;
        std::string notYet = "this compiler does not support " + n + "() on strings yet";
        if (n == "at")
            diags_.error(call.loc, "'string' has no method named 'at'",
                         "write `s[i]`, which always checks that i is a position in the string", spec::Strings);
        else if (n == "append")
            diags_.error(call.loc, notYet, "write `s += text`", spec::Status);
        else if (n == "insert" || n == "erase" || n == "replace" || n == "rfind" || n == "compare" ||
                 n == "find_first_of" || n == "find_last_of" || n == "find_first_not_of" ||
                 n == "find_last_not_of" || n == "resize" || n == "reserve" || n == "c_str" || n == "data" ||
                 n == "begin" || n == "end")
            diags_.error(call.loc, notYet,
                         n == "c_str" || n == "data" ? "pass `s` itself: a `string` parameter of a C function converts at each call"
                         : n == "erase" ? "build it from what stays: `s = s.substr(0, pos) + s.substr(pos + n);`"
                         : n == "insert" ? "build it: `s = s.substr(0, pos) + t + s.substr(pos);`"
                         : n == "replace" ? "build it: `s = s.substr(0, pos) + t + s.substr(pos + n);`"
                         : n == "resize" ? "build it: `s = s.substr(0, n);` to shorten it, or `s += string(k, ' ');` to add k spaces"
                         : n == "rfind" && !call.args.empty() && call.args[0]->kind == Expr::Kind::CharLit
                             ? "go back from the end: `for (long i = s.size() - 1; i >= 0; i--) if (s[i] == " + charLiteral(static_cast<const CharLitExpr&>(*call.args[0]).value) + ") { ...; break; }`"
                         : n == "rfind" ? "go back from the end: `for (long i = s.size() - t.size(); i >= 0; i--) if (s.substr(i, t.size()) == t) { ...; break; }`, with `t` what is looked for"
                         : n == "find_first_of" ? "go over it and test each char: `for (long i = 0; i < s.size(); i++) if (chars.contains(string(1, s[i]))) { ...; break; }`"
                         : n == "find_last_of" ? "go back from the end and test each char: `for (long i = s.size() - 1; i >= 0; i--) if (chars.contains(string(1, s[i]))) { ...; break; }`"
                         : n == "find_first_not_of" ? "go over it and test each char: `for (long i = 0; i < s.size(); i++) if (!chars.contains(string(1, s[i]))) { ...; break; }`"
                         : n == "find_last_not_of" ? "go back from the end and test each char: `for (long i = s.size() - 1; i >= 0; i--) if (!chars.contains(string(1, s[i]))) { ...; break; }`"
                         : n == "compare" ? "compare with `a < b` and `a == b`"
                         : n == "begin" || n == "end" ? "go over it with `for (char c : s)`"
                         : n == "reserve" ? "remove the call: a string grows by itself"
                                          : kStringMethodList,
                         spec::Status);
        else {
            std::vector<std::string> names;
            for (auto& m : kStringMethods) names.push_back(m.name);
            std::string guess = closest(n, names);
            diags_.error(call.loc, "'string' has no method named " + q(n),
                         guess.empty() ? kStringMethodList : "did you mean " + q(guess) + "?", spec::Strings);
        }
        return Type::error();
    }
    call.strMethod = info->method;
    if (info->changes) checkChangingReceiver(*call.receiver, info->name, "string", spec::Strings);
    if (call.args.size() < info->minArgs || call.args.size() > info->maxArgs) {
        for (auto& a : call.args) checkLooseValue(*a, flow);
        diags_.error(call.loc, std::string(info->name) + "() takes " + argumentCount(info->minArgs, info->maxArgs) +
                                   " but " + std::to_string(call.args.size()) + (call.args.size() == 1 ? " was" : " were") +
                                   " given",
                     "write `s." + std::string(info->usage) + "`", spec::Strings);
    } else {
        for (size_t i = 0; i < call.args.size(); ++i) {
            bool text = info->takesText && i == 0;  // a string, or one char, as in C++
            std::string what = text ? "for " : info->method == StrMethod::Substr && i == 1 ? "as the length for "
                                                                                             : "as the position for ";
            if (info->method == StrMethod::PushBack) {
                Expr& arg = *call.args[i];
                bool oneLetter = arg.kind == Expr::Kind::StringLit && static_cast<StringLitExpr&>(arg).value.size() == 1;
                if (checkExpr(arg, flow)->is(K::String) && !oneLetter)
                    diags_.error(arg.loc, "push_back() adds one char, but this is 'string'",
                                 "write `s += text` to add a string", spec::Strings);
                else
                    requireConvertible(arg, Type::charTy(), "for push_back()");
            } else if (text) {
                if (!checkExpr(*call.args[i], flow)->is(K::Char))
                    requireConvertible(*call.args[i], Type::stringTy(), what + info->name + "()");
            } else {
                checkValue(*call.args[i], flow, Type::longTy(), what + info->name + "()");
            }
        }
    }
    switch (info->method) {
        case StrMethod::Size:
        case StrMethod::Find: return Type::longTy();
        case StrMethod::Empty:
        case StrMethod::Contains:
        case StrMethod::StartsWith:
        case StrMethod::EndsWith: return Type::boolTy();
        case StrMethod::Substr: return Type::stringTy();
        case StrMethod::Front:
        case StrMethod::Back: return Type::charTy();
        default: return Type::voidTy();
    }
}

// `if (auto u = found)` and `while (auto u = next())`. When the value is an optional, the
// variable holds what is inside it, and the branch or body runs only when there is
// something. Otherwise, as in C++, the variable holds the value and the condition is
// that value, which must be a bool or a number.
void Sema::checkCondDecl(CondDecl& d, Flow& flow, const char* stmt) {
    VarDecl& v = *d.var;
    const Type* t = checkExpr(*v.init, flow);
    rejectCopy(*v.init);
    const Type* declared = v.declType;
    v.type = declared->is(K::Auto) ? Type::error() : declared;
    if (t->isError()) return;
    if (t->is(K::Void) || t->isNull()) {
        diags_.error(v.init->loc,
                     t->isNull() ? std::string("null never holds a value, so this ") + stmt + " would never run"
                                 : "this expression produces no value",
                     "declare the variable from a value that may be null, e.g. `" + std::string(stmt) +
                         " (auto u = find(name))`",
                     t->isNull() ? spec::Optionals : spec::Variables);
        return;
    }
    if (t->isOptional() && !declared->isOptional()) {
        d.unwraps = true;
        if (declared->is(K::Auto)) v.type = t->elem;
        else if (!canConvert(t->elem, declared))
            diags_.error(v.init->loc, "expected " + q(declared) + " to initialize " + q(v.name) + ", but this " +
                                          q(t) + " holds " + q(t->elem), "declare it as what the optional holds, or write `auto`", spec::ImplicitConversions);
        return;
    }
    if (declared->is(K::Auto)) v.type = t;
    else requireConvertible(*v.init, declared, "to initialize " + q(v.name));
    if (!usableAsCondition(v.type))
        diags_.error(v.init->loc, v.type->isOptional() && v.type->elem->is(K::Bool)
                                      ? std::string("the variable declared in the ") + stmt + " condition cannot be a `bool?`, as it could mean true or that it holds a value"
                                      : std::string("the variable declared in the ") + stmt + " condition must be an optional, a bool, a number or a pointer, but " + q(v.name) + " is " + q(v.type),
                     v.type->isOptional() ? "unwrap it with `" + std::string(stmt) + " (auto " + v.name + " = " + spelled(*v.init, "...") + ")`, which runs when it holds a value, and test `" + v.name +
                                                "` inside; or declare it before the `" + std::string(stmt) + "` and write `" + v.name + " == true`"
                     : v.type->isWeak()     ? "write `" + std::string(stmt) + " (auto " + v.name + " = " + spelled(*v.init, "w") +
                                                  ".lock())`, which runs the branch when the object is still there"
                                          : "declare it before the `" + std::string(stmt) + "`, and write the test as the condition. For this one, " + conditionHelp(*v.init, v.type),
                     spec::Optionals);
}

// `base?.rest`: the rest runs only when `base` holds a value, and its result becomes an
// optional, unless it already is one or is void.
const Type* Sema::checkOptChain(OptChainExpr& c, Flow& flow) {
    const Type* base = checkExpr(*c.base, flow);
    const Type* inner = base;
    if (base->isOptional()) {
        inner = base->elem;
    } else if (base->isWeak()) {
        diags_.error(c.loc, "'?.' reads from an optional, but this is " + q(base),
                     "write `" + spelled(*c.base, "w") + ".lock()?.`, which is null when the object is gone", spec::Weak);
        inner = Type::error();
    } else if (base->isPointer()) {
        diags_.error(c.loc, "'?.' reads from an optional, but this is " + q(base),
                     "a pointer is followed with '->' once it is known not to be null: `if (p != null) ... p->x`",
                     spec::Pointers);
        inner = Type::error();
    } else if (!base->isError()) {
        diags_.error(c.loc, "'?.' reads from an optional, but this is " + q(base) + ", which is never null",
                     "write '.' instead of '?.'", spec::Optionals);
    }
    c.value->type = inner;
    Flow bodyFlow = flow;  // the rest of the chain may not run
    size_t movesBefore = moveLog_.size();
    const Type* body = checkExpr(*c.body, bodyFlow);
    dropMovedSince(flow, movesBefore);
    if (base->isError() || body->isError()) return Type::error();
    if (!isCopyable(body))
        diags_.error(c.loc, "'?.' gives a copy, but a " + q(body) + " cannot be copied",
                     "go on to a field or a method, as in `x?.field.name`", spec::Unique);
    if (!base->isOptional() || body->is(K::Void) || body->isOptional()) return body;
    return Type::optionalOf(body);
}

// `opt ?? fallback`. With a fallback that cannot be null, the result cannot be either.
const Type* Sema::checkCoalesce(CoalesceExpr& c, Flow& flow) {
    const Type* l = checkExpr(*c.lhs, flow);
    rejectCopy(*c.lhs);
    Flow rhsFlow = flow;  // the fallback may not run
    size_t movesBefore = moveLog_.size();
    struct FallbackMoves {
        Sema& s;
        Flow& flow;
        size_t start;
        ~FallbackMoves() { s.dropMovedSince(flow, start); }
    } fallbackMoves{*this, flow, movesBefore};
    if (l->isError() || !l->isOptional()) {
        checkLooseValue(*c.rhs, rhsFlow);
        if (l->isError()) return Type::error();
        if (l->isWeak()) {
            diags_.error(c.loc, "'\?\?' gives a fallback for an optional, but the left side is " + q(l),
                         "write `" + spelled(*c.lhs, "w") + ".lock() ?? other`", spec::Weak);
            return l->elem;
        }
        if (l->isPointer()) {
            diags_.error(c.loc, "'\?\?' gives a fallback for an optional, but the left side is " + q(l),
                         "for a pointer, write `p != null ? p : other`", spec::Pointers);
            return l;
        }
        diags_.error(c.loc, "'\?\?' gives a fallback for an optional, but the left side is " + q(l) + ", which is never null",
                     "remove `?? ...`: the left side always has a value", spec::Optionals);
        return l;
    }
    const Type* inner = l->elem;
    const Type* r;
    if (c.rhs->kind == Expr::Kind::InitList && !static_cast<InitListExpr&>(*c.rhs).explicitType) {
        checkInitList(static_cast<InitListExpr&>(*c.rhs), rhsFlow, inner);  // `p ?? {0, 0}`
        r = inner;
    } else {
        r = checkExpr(*c.rhs, rhsFlow);
    }
    rejectCopy(*c.rhs);
    if (r->isError()) return inner;
    if (r == l || r->isNull()) return l;  // the fallback may itself be null
    if (canConvert(r, inner)) return inner;
    if (const Type* common = commonReference(inner, r)) return common;  // `maybeDog ?? animal` is an Animal
    if (r->isWeak()) {
        diags_.error(c.rhs->loc, "the fallback after '\?\?' cannot be a weak reference, since its object may be gone",
                     "write `" + spelled(*c.rhs, "w") + ".lock()`, which gives a `" + std::string(r->elem->name) + "?`", spec::Weak);
        return inner;
    }
    if (r->isInteger() && inner->isInteger()) return commonInteger(r, inner);
    diags_.error(c.rhs->loc, "the fallback after '\?\?' must be " + q(inner) + ", but this is " + q(r),
                 "write a fallback of type " + q(inner) + ", which is what the optional holds", spec::Optionals);
    return inner;
}

// The methods of std::optional: has_value(), value() and value_or(fallback). Anything
// else is a method of the value inside, which needs `?.` or unwrapping.
const Type* Sema::checkWeakMethod(MethodCallExpr& call, Flow& flow, const Type* weak) {
    std::string n = spelled(*call.receiver, "w");
    for (auto& a : call.args) checkLooseValue(*a, flow);
    if (call.name == "lock") call.weakMethod = WeakMethod::Lock;
    else if (call.name == "expired") call.weakMethod = WeakMethod::Expired;
    else if (call.name == "reset") call.weakMethod = WeakMethod::Reset;
    else {
        bool exists = false;
        for (const StructDecl* d = weak->elem ? weak->elem->decl : nullptr; d && !exists; d = d->bases.empty() ? nullptr : d->bases[0]) exists = d->findMethod(call.name) != nullptr;
        if (!exists) {
            diags_.error(call.loc, q(weak) + " has no method named " + q(call.name),
                         "a weak reference has `lock()`, `expired()` and `reset()`, and " + q(weak->elem) + " has no method named " + q(call.name) +
                             ". Its object, reached with `" + n + ".lock()?.`, has the methods of " + q(weak->elem),
                         spec::Weak);
            return Type::error();
        }
        diags_.error(call.loc, "the object of a " + q(weak) + " may be gone, so " + q(call.name) + " cannot be called on it directly",
                     "write `" + n + ".lock()?." + call.name + "(...)`, which does nothing when the object is gone. A weak "
                     "reference itself has `lock()`, `expired()` and `reset()`",
                     spec::Weak);
        return Type::error();
    }
    if (!call.args.empty())
        diags_.error(call.loc, call.name + "() takes no arguments", "write `" + n + "." + call.name + "()`", spec::Weak);
    if (call.weakMethod == WeakMethod::Lock) return Type::optionalOf(weak->elem);
    if (call.weakMethod == WeakMethod::Expired) return Type::boolTy();
    checkChangingReceiver(*call.receiver, "reset", "weak reference", spec::Weak);
    return Type::voidTy();
}

const Type* Sema::checkOptionalMethod(MethodCallExpr& call, Flow& flow, const Type* opt) {
    const Type* inner = opt->elem;
    const std::string written = spelled(*call.receiver, "");
    std::string n = asBase(written.empty() ? "(...)" : written);
    if (!isCopyable(inner) && (call.name == "value" || call.name == "value_or") && isPlaceKind(*call.receiver)) {
        const std::string held = constHolderHelp(*call.receiver, false);
        diags_.error(call.loc, call.name + "() gives a copy, but a " + q(inner) + " cannot be copied",
                     written.empty() ? std::string("read through it with `?.` in place of `.`, or store it in a variable first and move it out of that with `move(x).value()`")
                     : !held.empty() ? "read through it with `" + n + "?.field`. To take it out: " + held
                     : "read through it with `" + n + "?.field`, or take it: `auto " + freshName(n) + " = move(" + n + ")." + call.name + (call.args.empty() ? "()" : "(...)") + ";`, and, to keep it, `" + n + " = move(" + freshName(n) + ");` when done",
                     spec::Unique);
    }
    size_t want = 0;
    if (call.name == "has_value") call.optMethod = OptMethod::HasValue;
    else if (call.name == "value") call.optMethod = OptMethod::Value;
    else if (call.name == "value_or") call.optMethod = OptMethod::ValueOr, want = 1;
    else {
        for (auto& a : call.args) checkLooseValue(*a, flow);
        const std::string callArgs = call.args.empty() ? "()" : "(...)";
        bool changes = true;  // whether the method changes the value, so that a copy of it must be stored back
        bool fails = false;
        const StructDecl* owner = inner->isUnique() || inner->isLocal() ? (inner->elem ? inner->elem->decl : nullptr) : inner->decl;
        if (inner->isVector() || inner->isArray() || inner->is(K::String)) {
            static const char* const changing[] = {"push_back", "pop_back", "clear", "insert", "erase", "fill", "resize", "append", "swap"};
            changes = std::find_if(std::begin(changing), std::end(changing), [&](const char* m) { return call.name == m; }) != std::end(changing);
        } else if (owner) {
            if (const FuncDecl* method = owner->findMethod(call.name)) changes = !method->isConst, fails = method->callFails();
            else if (isHashmap(inner) && call.name == "at") changes = false;  // read where it is, by the compiler
        }
        const std::string tryFirst = fails ? "try " : "";
        bool exists = true;  // what is inside has a method of that name
        if (inner->isVector() || inner->isArray() || inner->is(K::String)) {
            static const char* const known[] = {"size", "empty", "push_back", "pop_back", "front", "back", "clear", "data", "fill", "length", "substr", "find",
                                                "contains", "starts_with", "ends_with", "at", "insert", "erase", "resize", "append", "swap", "rfind"};
            exists = std::find_if(std::begin(known), std::end(known), [&](const char* m) { return call.name == m; }) != std::end(known);
        } else if (owner) {
            exists = (isHashmap(inner) && call.name == "at") || ((inner->isUnique() || inner->isLocal()) && call.name == "get");
            for (const StructDecl* d = owner; d && !exists; d = d->bases.empty() ? nullptr : d->bases[0]) exists = d->findMethod(call.name) != nullptr;
        } else {
            exists = false;  // a number, a bool or a char has no methods
        }
        if (!exists) {
            diags_.error(call.loc, q(opt) + " has no method named " + q(call.name),
                         call.name == "reset" ? "to empty it, write `" + n + " = null`"
                         : call.name == "emplace" ? "to give it a value, write `" + n + " = value`"
                         : "check the name: a " + q(inner) + " has no method named " + q(call.name) + ", and the optional itself has `has_value()`, `value()` and `value_or(x)`",
                         spec::Optionals);
            return Type::error();
        }
        const bool needsAPlace = call.name == "data" && inner->isArray();
        const bool noPointerYet = call.name == "data" && (inner->isVector() || inner->is(K::String));
        diags_.error(call.loc, q(opt) + " may be null, so " + q(call.name) + " cannot be called on it directly",
                     noPointerYet ? std::string("this compiler does not support pointers into vectors and strings yet, as their elements move when they grow. Copy the values into an `array<T, N>` variable, and call `data()` on that")
                     : needsAPlace && !written.empty()
                         ? "`data()` needs the value itself, and `?.` gives a copy. Unwrap it, use `v.data()`, and store it back: `if (auto " + freshName(n) + " = " + n + ") { ... " + freshName(n) + ".data() ...; " + n + " = " + freshName(n) + "; }`"
                     : written.empty() ? (inner->isClass() || inner->isUnique() || inner->isLocal() || !changes
                                            ? "write `?.` in place of `.` before " + q(call.name) + ", which does nothing when it is null"
                                            : "to change it, " + std::string(kStoreFirst))
                     : !changes && !inner->isClass() && !inner->isUnique() && !inner->isLocal()
                         ? "read with `" + tryFirst + n + "?." + call.name + callArgs + "`, which is null when " + n + " is"
                     : opt->elem->isUnique() || opt->elem->isLocal()
                         ? "write `" + tryFirst + n + "?." + call.name + (call.args.empty() ? "()" : "(...)") + "`, which does nothing when " + n + " is null"
                     : opt->elem->isClass()
                         ? "write `" + tryFirst + n + "?." + call.name + (call.args.empty() ? "()" : "(...)") + "`, which does nothing when " + n + " is null, or unwrap it: `if (auto " + freshName(n) + " = " + n + ") " + tryFirst + freshName(n) + "." + call.name + (call.args.empty() ? "()" : "(...)") + ";`"
                         : (changes ? std::string() : "read with `" + n + "?." + call.name + (call.args.empty() ? "()" : "(...)") + "`. ") +
                               changeForm(*call.receiver, "." + call.name + (call.args.empty() ? "()" : "(...)"), "To change it, ", "unwrap it, change the copy and store it back", tryFirst),
                     spec::Optionals);
        return Type::error();
    }
    if (call.args.size() != want) {
        for (auto& a : call.args) checkLooseValue(*a, flow);
        diags_.error(call.loc, call.name + "() takes " + (want ? "one argument" : "no arguments") + " but " +
                                   std::to_string(call.args.size()) + (call.args.size() == 1 ? " was" : " were") +
                                   " given",
                     want ? "write `" + n + ".value_or(fallback)`" : "write `" + n + "." + call.name + "()`",
                     spec::Optionals);
    } else if (want) {
        checkValue(*call.args[0], flow, inner, "for value_or() on " + q(opt));
    }
    return call.optMethod == OptMethod::HasValue ? Type::boolTy() : inner;
}

}  // namespace cplus
