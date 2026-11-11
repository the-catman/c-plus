#include "ConstEval.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace cplus {

using K = Type::Kind;
using VK = ConstValue::Kind;

namespace {
constexpr int64_t kMaxSteps = 20'000'000;  // about a second of compiling
constexpr size_t kMaxDepth = 512;          // as many calls as clang allows by default

ConstValue intValue(int64_t v, VK kind = VK::Int) {
    ConstValue out;
    out.kind = kind;
    out.i = v;
    return out;
}
ConstValue boolValue(bool v) { return intValue(v, VK::Bool); }
ConstValue enumValue(int64_t v, const Type* type) {
    ConstValue out = intValue(v, VK::Enum);
    out.enumType = type;
    return out;
}
ConstValue floatValue(double v, VK kind) {
    ConstValue out;
    out.kind = kind;
    out.d = kind == VK::Float ? (double)(float)v : v;
    return out;
}
ConstValue stringValue(std::string s) {
    ConstValue out;
    out.kind = VK::String;
    out.s = std::move(s);
    return out;
}
const Type* typeOfValue(const ConstValue& v) {
    switch (v.kind) {
        case VK::Int: return Type::intTy();
        case VK::Long: return Type::longTy();
        case VK::Float: return Type::floatTy();
        case VK::Double: return Type::doubleTy();
        case VK::Bool: return Type::boolTy();
        case VK::Char: return Type::charTy();
        case VK::String: return Type::stringTy();
        case VK::Enum: return v.enumType;
        case VK::UInt8: return Type::uint8Ty();
        case VK::UInt16: return Type::uint16Ty();
        case VK::UInt32: return Type::uint32Ty();
        case VK::UInt64: return Type::uint64Ty();
    }
    return Type::intTy();
}
VK kindOfInteger(const Type* t) {
    switch (t->kind) {
        case K::Long: return VK::Long;
        case K::UInt8: return VK::UInt8;
        case K::UInt16: return VK::UInt16;
        case K::UInt32: return VK::UInt32;
        case K::UInt64: return VK::UInt64;
        case K::Char: return VK::Char;
        default: return VK::Int;
    }
}
bool isUnsignedKind(VK k) { return k == VK::UInt8 || k == VK::UInt16 || k == VK::UInt32 || k == VK::UInt64; }
uint64_t maskOfBits(unsigned bits) { return bits >= 64 ? ~(uint64_t)0 : ((uint64_t)1 << bits) - 1; }
// Whether every value of the integer type `from` is one of `to`, as Sema's canConvert finds.
bool integerFits(const Type* from, const Type* to) {
    if (from == to) return true;
    if (from->is(K::Char) || from->isUnsigned()) return to->isInteger() && (to->bits() > from->bits() || (to->isUnsigned() && to->bits() >= from->bits()));
    return from->is(K::Int) && to->is(K::Long);
}
// The integer type arithmetic on these two happens in, as Sema's arithmeticType finds: a char, a uint8_t and a uint16_t
// become an int, and a signed type is never mixed with an unsigned one, except by a written number that fits it.
const Type* commonIntegerType(const Type* a, const Type* b) {
    a = a->promotesToInt() ? Type::intTy() : a;
    b = b->promotesToInt() ? Type::intTy() : b;
    if (integerFits(b, a)) return a;
    if (integerFits(a, b)) return b;
    return a->isUnsigned() ? a : b;
}
// The type two numbers of these types are worked out in, as Sema's commonNumber finds. Null for anything else.
const Type* commonNumberType(const Type* a, const Type* b) {
    if (!a || !b || !a->isArithmetic() || !b->isArithmetic()) return nullptr;
    if (a->is(K::Double) || b->is(K::Double)) return Type::doubleTy();
    if (a->is(K::Float) || b->is(K::Float)) return Type::floatTy();
    return commonIntegerType(a, b);
}
bool isFloating(const ConstValue& v) { return v.kind == VK::Float || v.kind == VK::Double; }
bool isIntegerLike(const ConstValue& v) { return v.kind == VK::Int || v.kind == VK::Long || v.kind == VK::Char || isUnsignedKind(v.kind); }
bool isNumberLike(const ConstValue& v) { return isIntegerLike(v) || isFloating(v); }
double asDouble(const ConstValue& v) { return isFloating(v) ? v.d : v.kind == VK::UInt64 ? (double)(uint64_t)v.i : (double)v.i; }
}  // namespace

const char* ConstValue::typeName() const {
    switch (kind) {
        case Kind::Int: return "int";
        case Kind::Long: return "long";
        case Kind::Float: return "float";
        case Kind::Double: return "double";
        case Kind::Bool: return "bool";
        case Kind::Char: return "char";
        case Kind::String: return "string";
        case Kind::Enum: return enumType->name;
        case Kind::UInt8: return "uint8_t";
        case Kind::UInt16: return "uint16_t";
        case Kind::UInt32: return "uint32_t";
        case Kind::UInt64: return "uint64_t";
    }
    return "?";
}

bool isBuiltinConstraintName(const std::string& name) {
    return name == "Comparable" || name == "Number" || name == "Integer" || name == "Floating" || name == "Hashable" ||
           name == "__Hashable" || name == "__JsonWritable" || name == "__JsonReadable" || name == "__Copyable" || name == "same_as" || name == "derived_from";
}

std::optional<bool> builtinConstraint(const std::string& name, const Type* first, const Type* second) {
    if (name == "Comparable") return first->isArithmetic() || first->is(K::String) || first->isPointer() || first->isEnum();
    if (name == "Number") return first->isNumber();
    if (name == "Integer") return first->isInteger();
    if (name == "Floating") return first->isFloating();
    if (name == "Hashable" || name == "__Hashable") return isHashable(first);
    if (name == "__Copyable") return !holdsUnique(first);
    if (name == "same_as") return first == second;
    if (name == "derived_from") return first->isClass() && second->isClass() && first->decl->derivesFrom(second->decl);
    return std::nullopt;
}

void ConstEval::tick(Loc loc) {
    if (++steps_ > kMaxSteps) throw ConstError{loc, "working it out takes too long, which may mean a loop that never ends"};
}

ConstEval::Local* ConstEval::findLocal(const std::string& name) {
    if (frames_.empty()) return nullptr;
    auto& scopes = frames_.back();
    for (size_t i = scopes.size(); i-- > 0;)
        if (auto it = scopes[i].find(name); it != scopes[i].end()) return &it->second;
    return nullptr;
}

bool ConstEval::isTrue(const ConstValue& v, Loc loc) {
    if (v.kind == VK::Bool) return v.i != 0;
    if (v.kind == VK::Int || v.kind == VK::Long || isUnsignedKind(v.kind)) return v.i != 0;
    if (isFloating(v)) return v.d != 0;
    throw ConstError{loc, std::string("a ") + v.typeName() + " is not a condition"};
}

ConstValue ConstEval::convert(const ConstValue& v, const Type* to, Loc loc, bool wraps) {
    if (to->is(K::Auto)) return v;
    if (to->isEnum()) {  // `Color(n)`, which only an enumerator's value passes
        if (v.kind == VK::Enum && v.enumType == to) return v;
        if (!isIntegerLike(v)) throw ConstError{loc, std::string("a ") + v.typeName() + " cannot become a '" + to->name + "'"};
        if (to->decl->enumeratorWithValue(v.i) < 0)
            throw ConstError{loc, "the value " + std::to_string(v.i) + " is not one of the enum '" + to->name + "'"};
        return enumValue(v.i, to);
    }
    if (v.kind == VK::Enum && !to->isInteger())
        throw ConstError{loc, std::string("a '") + v.typeName() + "' cannot become a '" + to->name + "'"};
    if (to->is(K::String)) {
        if (v.kind == VK::String) return v;
        throw ConstError{loc, std::string("a ") + v.typeName() + " cannot become a string here"};
    }
    if (v.kind == VK::String) throw ConstError{loc, std::string("a string cannot become a '") + to->name + "'"};
    if (to->isInteger() || to->is(K::Char)) {
        const unsigned bits = to->bits();
        const bool toUnsigned = !to->isSigned();  // a char too, from 0 to 255
        const VK kind = kindOfInteger(to);
        const uint64_t mask = maskOfBits(bits);
        const int64_t largest = toUnsigned ? (int64_t)mask : (int64_t)(mask >> 1), smallest = toUnsigned ? 0 : -(int64_t)(mask >> 1) - 1;
        if (isFloating(v)) {
            if (wraps) {  // the nearest value that fits, and NaN is 0, as the running program does
                if (std::isnan(v.d) || v.d <= (double)smallest) return intValue(std::isnan(v.d) ? 0 : smallest, kind);
                if (v.d >= std::ldexp(1.0, toUnsigned ? (int)bits : (int)bits - 1)) return intValue(largest, kind);
                return intValue(toUnsigned ? (int64_t)(uint64_t)v.d : (int64_t)v.d, kind);
            }
            // Above these the value does not fit; below them, a truncated value is in range, as the running program finds.
            const double low = toUnsigned ? -1.0 : bits >= 64 ? -std::ldexp(1.0, 63) : (double)smallest - 1;
            const double high = std::ldexp(1.0, toUnsigned ? (int)bits : (int)bits - 1);
            if (std::isnan(v.d) || (bits >= 64 && !toUnsigned ? v.d < low : v.d <= low) || v.d >= high)
                throw ConstError{loc, "the value " + std::to_string(v.d) + " does not fit in '" + to->name + "'"};
            return intValue(toUnsigned ? (int64_t)(uint64_t)v.d : (int64_t)v.d, kind);
        }
        if (wraps) {  // the lowest bits
            uint64_t low = (uint64_t)v.i & mask;
            if (!toUnsigned && bits < 64 && (low >> (bits - 1)) != 0) low |= ~mask;
            return intValue((int64_t)low, kind);
        }
        const bool aboveLong = v.kind == VK::UInt64 && v.i < 0;  // a uint64_t, held as its bits
        const bool fitsIn = aboveLong ? toUnsigned && bits >= 64 : v.i >= smallest && (toUnsigned && bits >= 64 ? true : v.i <= largest);
        if (!fitsIn)
            throw ConstError{loc, "the value " + (aboveLong ? std::to_string((uint64_t)v.i) : std::to_string(v.i)) + " does not fit in '" + to->name + "'"};
        return intValue(v.i, kind);
    }
    switch (to->kind) {
        case K::Bool: return boolValue(isFloating(v) ? v.d != 0 : v.i != 0);
        case K::Float: return floatValue(asDouble(v), VK::Float);
        case K::Double: return floatValue(asDouble(v), VK::Double);
        default: throw ConstError{loc, std::string("a value of type '") + to->name + "' cannot be worked out while compiling"};
    }
}

ConstValue ConstEval::evalName(const NameExpr& n) {
    if (Local* local = findLocal(n.name)) return local->value;
    auto& lookup = globalsOnly_ ? hooks_.globalConstant : hooks_.constant;
    const VarDecl* decl = lookup ? lookup(hooks_.resolve ? hooks_.resolve(n.name, n.loc) : n.name) : nullptr;
    if (!decl) throw ConstError{n.loc, "'" + n.name + "' is not a constant"};
    if (!decl->isConst || !decl->init || decl->isGlobal)
        throw ConstError{n.loc, "'" + n.name + "' is a variable" + std::string(decl->isGlobal ? " outside functions" : "") + ", whose value is only known while the program runs"};
    if (auto it = known_.find(decl); it != known_.end()) return it->second;
    if (std::find(evaluating_.begin(), evaluating_.end(), decl) != evaluating_.end())
        throw ConstError{n.loc, "the value of '" + n.name + "' depends on itself"};
    evaluating_.push_back(decl);
    // Constants see no locals of whoever asks.
    std::vector<std::vector<Scope>> saved;
    saved.swap(frames_);
    if (decl->isTopLevel) ++globalsOnly_;
    struct Restore {
        ConstEval& e;
        std::vector<std::vector<Scope>>& saved;
        bool global;
        ~Restore() {
            e.frames_.swap(saved);
            e.evaluating_.pop_back();
            if (global) --e.globalsOnly_;
        }
    } restore{*this, saved, decl->isTopLevel};
    ConstValue value = convert(eval(*decl->init), decl->declType, n.loc);
    known_[decl] = value;
    return value;
}

const Type* ConstEval::staticType(const Expr& e) {
    if (e.type && !e.type->isError() && !e.type->is(K::Auto)) return e.type;
    switch (e.kind) {
        case Expr::Kind::IntLit: {
            return static_cast<const IntLitExpr&>(e).writtenType();
        }
        case Expr::Kind::FloatLit: return static_cast<const FloatLitExpr&>(e).isFloat ? Type::floatTy() : Type::doubleTy();
        case Expr::Kind::CharLit: return Type::charTy();
        case Expr::Kind::BoolLit: case Expr::Kind::Constraint: return Type::boolTy();
        case Expr::Kind::StringLit: return Type::stringTy();
        case Expr::Kind::Sizeof: return Type::longTy();
        case Expr::Kind::Name: {
            auto& n = static_cast<const NameExpr&>(e);
            if (Local* local = findLocal(n.name)) return local->type;
            auto& lookup = globalsOnly_ ? hooks_.globalConstant : hooks_.constant;
            const VarDecl* decl = lookup ? lookup(hooks_.resolve ? hooks_.resolve(n.name, n.loc) : n.name) : nullptr;
            if (!decl) return nullptr;
            return decl->declType->is(K::Auto) ? (decl->init ? staticType(*decl->init) : nullptr) : decl->declType;
        }
        case Expr::Kind::Cast: return static_cast<const CastExpr&>(e).target;
        case Expr::Kind::EnumValue: return &static_cast<const EnumValueExpr&>(e).decl->type;
        case Expr::Kind::Try: return staticType(*static_cast<const TryExpr&>(e).operand);
        case Expr::Kind::Unary: {
            auto& u = static_cast<const UnaryExpr&>(e);
            if (u.op == UnOp::Not) return Type::boolTy();
            const Type* t = staticType(*u.operand);
            return t && t->promotesToInt() ? Type::intTy() : t;
        }
        case Expr::Kind::Binary: {
            auto& b = static_cast<const BinaryExpr&>(e);
            if ((b.op >= BinOp::Eq && b.op <= BinOp::Ge) || b.op == BinOp::LogAnd || b.op == BinOp::LogOr) return Type::boolTy();
            const Type* l = staticType(*b.lhs);
            if (b.op == BinOp::Shl || b.op == BinOp::Shr) return l && l->isInteger() && !l->promotesToInt() ? l : Type::intTy();
            const Type* r = staticType(*b.rhs);
            if (l && r && (l->is(K::String) || r->is(K::String))) return Type::stringTy();
            return commonNumberType(l, r);
        }
        case Expr::Kind::Ternary: {
            auto& t = static_cast<const TernaryExpr&>(e);
            const Type* x = staticType(*t.thenExpr);
            const Type* y = staticType(*t.elseExpr);
            return x == y ? x : commonNumberType(x, y);
        }
        case Expr::Kind::Call: {
            auto& c = static_cast<const CallExpr&>(e);
            const FuncDecl* f = c.target;
            if (!f && hooks_.function) f = hooks_.function(hooks_.resolve ? hooks_.resolve(c.callee, c.loc) : c.callee);
            return f ? f->retType : nullptr;
        }
        case Expr::Kind::MethodCall: {
            auto& c = static_cast<const MethodCallExpr&>(e);
            return c.name == "empty" ? Type::boolTy() : c.name == "size" || c.name == "length" ? Type::longTy() : nullptr;
        }
        case Expr::Kind::Index: return Type::charTy();
        case Expr::Kind::Assign: return staticType(*static_cast<const AssignExpr&>(e).target);
        case Expr::Kind::IncDec: return staticType(*static_cast<const IncDecExpr&>(e).target);
        default: return nullptr;
    }
}

ConstValue ConstEval::evalUnary(const UnaryExpr& u) {
    if (u.op == UnOp::Deref || u.op == UnOp::AddrOf) throw ConstError{u.loc, "pointers cannot be followed while compiling"};
    ConstValue v = eval(*u.operand);
    switch (u.op) {
        case UnOp::Not: return boolValue(!isTrue(v, u.operand->loc));
        case UnOp::Plus:
        case UnOp::Neg: {
            if (!isNumberLike(v)) throw ConstError{u.loc, std::string("'-' does not work on a ") + v.typeName()};
            if (isFloating(v)) return floatValue(u.op == UnOp::Neg ? -v.d : v.d, v.kind);
            VK kind = kindOfInteger(commonIntegerType(typeOfValue(v), typeOfValue(v)));  // a char, uint8_t or uint16_t is an int
            if (u.op == UnOp::Plus) return intValue(v.i, kind);
            return arith(BinOp::Sub, intValue(0, kind), intValue(v.i, kind), u.loc, u.checked);
        }
        case UnOp::BitNot:
            if (!isIntegerLike(v)) throw ConstError{u.loc, std::string("'~' does not work on a ") + v.typeName()};
            return arith(BinOp::BitXor, v, intValue(-1, kindOfInteger(commonIntegerType(typeOfValue(v), typeOfValue(v)))), u.loc, false);
        default: break;
    }
    throw ConstError{u.loc, "this operator cannot be worked out while compiling"};
}

ConstValue ConstEval::arith(BinOp op, const ConstValue& l, const ConstValue& r, Loc loc, bool checked) {
    std::string sym = binOpSpelling(op);
    bool comparison = op == BinOp::Eq || op == BinOp::Ne || op == BinOp::Lt || op == BinOp::Le || op == BinOp::Gt || op == BinOp::Ge;
    auto compared = [&](auto a, auto b) {
        switch (op) {
            case BinOp::Eq: return a == b;
            case BinOp::Ne: return a != b;
            case BinOp::Lt: return a < b;
            case BinOp::Le: return a <= b;
            case BinOp::Gt: return a > b;
            default: return a >= b;
        }
    };
    if (l.kind == VK::Enum || r.kind == VK::Enum) {
        if (comparison && l.kind == r.kind && l.enumType == r.enumType) return boolValue(compared(l.i, r.i));
        throw ConstError{loc, "'" + sym + "' cannot be used with a '" + l.typeName() + "' and a '" + r.typeName() + "'"};
    }
    if (l.kind == VK::String || r.kind == VK::String) {
        if (op == BinOp::Add && l.kind == VK::String && r.kind == VK::String) return stringValue(l.s + r.s);
        if (op == BinOp::Add && l.kind == VK::String && r.kind == VK::Char) return stringValue(l.s + (char)r.i);
        if (op == BinOp::Add && l.kind == VK::Char && r.kind == VK::String) return stringValue((char)l.i + r.s);
        if (comparison && l.kind == VK::String && r.kind == VK::String) return boolValue(compared(l.s, r.s));
        throw ConstError{loc, "'" + sym + "' cannot be used with a " + l.typeName() + " and a " + r.typeName()};
    }
    if (l.kind == VK::Bool || r.kind == VK::Bool) {
        if ((op == BinOp::Eq || op == BinOp::Ne) && l.kind == r.kind) return boolValue(compared(l.i, r.i));
        throw ConstError{loc, "'" + sym + "' cannot be used with a bool"};
    }
    if (isFloating(l) || isFloating(r)) {
        VK kind = l.kind == VK::Double || r.kind == VK::Double ? VK::Double : VK::Float;
        double a = asDouble(l), b = asDouble(r);
        if (kind == VK::Float) {
            a = (float)a;
            b = (float)b;
        }
        if (comparison) return boolValue(compared(a, b));
        switch (op) {
            case BinOp::Add: return floatValue(a + b, kind);
            case BinOp::Sub: return floatValue(a - b, kind);
            case BinOp::Mul: return floatValue(a * b, kind);
            case BinOp::Div: return floatValue(a / b, kind);
            default: throw ConstError{loc, "'" + sym + "' works on integers only"};
        }
    }
    // A shift keeps the type of its left side, and a char, uint8_t or uint16_t is an int in arithmetic.
    const Type* type = op == BinOp::Shl || op == BinOp::Shr ? commonIntegerType(typeOfValue(l), typeOfValue(l))
                                                            : commonIntegerType(typeOfValue(l), typeOfValue(r));
    const VK kind = kindOfInteger(type);
    const unsigned bits = type->bits();
    const bool isUnsigned = type->isUnsigned();
    const uint64_t mask = maskOfBits(bits);
    // Each side as a value of `type`: a number written beside an unsigned one fits it, as Sema checks.
    int64_t a = isUnsigned ? (int64_t)((uint64_t)l.i & mask) : l.i, b = r.i;
    if (op != BinOp::Shl && op != BinOp::Shr && isUnsigned) b = (int64_t)((uint64_t)b & mask);
    if (comparison) return isUnsigned ? boolValue(compared((uint64_t)a, (uint64_t)b)) : boolValue(compared(a, b));
    const std::string typeName = type->name;
    auto overflow = [&]() -> ConstError {
        return ConstError{loc, "integer overflow: the result of '" + sym + "' does not fit in '" + typeName + "'"};
    };
    if (isUnsigned) {
        const uint64_t x = (uint64_t)a, y = (uint64_t)b;
        uint64_t out = 0;
        bool over = false;
        switch (op) {
            // Wrapped around, as the running program does, unless the arithmetic is checked.
            case BinOp::Add: over = __builtin_add_overflow(x, y, &out); break;
            case BinOp::Sub: over = __builtin_sub_overflow(x, y, &out); break;
            case BinOp::Mul: over = __builtin_mul_overflow(x, y, &out); break;
            case BinOp::Div:
            case BinOp::Rem:
                if (y == 0) throw ConstError{loc, std::string("division by zero: the right side of '") + sym + "' is 0"};
                out = op == BinOp::Div ? x / y : x % y;
                break;
            case BinOp::Shl:
            case BinOp::Shr: {
                const bool outOfRange = r.kind == VK::UInt64 ? (uint64_t)b >= bits : b < 0 || b >= (int64_t)bits;
                if (outOfRange && checked)
                    throw ConstError{loc, "shift amount " + (r.kind == VK::UInt64 ? std::to_string((uint64_t)b) : std::to_string(b)) + " is out of range for '" + typeName + "'"};
                const unsigned n = (unsigned)((uint64_t)b & (bits - 1));  // only its lowest bits count
                out = op == BinOp::Shl ? x << n : x >> n;
                break;
            }
            case BinOp::BitAnd: out = x & y; break;
            case BinOp::BitOr: out = x | y; break;
            case BinOp::BitXor: out = x ^ y; break;
            default: throw ConstError{loc, "this operator cannot be worked out while compiling"};
        }
        if ((over || (out & ~mask) != 0) && checked && op != BinOp::Shl) throw overflow();
        return intValue((int64_t)(out & mask), kind);
    }
    int64_t out = 0;
    switch (op) {
        // Wrapped around, as the running program does, unless the arithmetic is checked.
        case BinOp::Add: if (__builtin_add_overflow(a, b, &out) && checked) throw overflow(); break;
        case BinOp::Sub: if (__builtin_sub_overflow(a, b, &out) && checked) throw overflow(); break;
        case BinOp::Mul: if (__builtin_mul_overflow(a, b, &out) && checked) throw overflow(); break;
        case BinOp::Div:
        case BinOp::Rem:
            if (b == 0) throw ConstError{loc, std::string("division by zero: the right side of '") + sym + "' is 0"};
            if (b == -1) {
                if (op == BinOp::Rem) return intValue(0, kind);
                if (a == (kind == VK::Long ? INT64_MIN : (int64_t)INT32_MIN)) {
                    if (checked) throw overflow();
                    return intValue(a, kind);  // the smallest number divided by -1 wraps around to itself
                }
            }
            out = op == BinOp::Div ? a / b : a % b;
            break;
        case BinOp::Shl:
        case BinOp::Shr: {
            const bool outOfRange = r.kind == VK::UInt64 ? (uint64_t)b >= bits : b < 0 || b >= (int64_t)bits;
            if (outOfRange && checked)
                throw ConstError{loc, "shift amount " + (r.kind == VK::UInt64 ? std::to_string((uint64_t)b) : std::to_string(b)) + " is out of range for '" + typeName + "'"};
            b &= bits - 1;  // only its lowest bits count
            if (op == BinOp::Shr) return intValue(a >> b, kind);
            return intValue(kind == VK::Long ? (int64_t)((uint64_t)a << b) : (int64_t)(int32_t)((uint32_t)a << b), kind);
        }
        case BinOp::BitAnd: out = a & b; break;
        case BinOp::BitOr: out = a | b; break;
        case BinOp::BitXor: out = a ^ b; break;
        default: throw ConstError{loc, "this operator cannot be worked out while compiling"};
    }
    if (kind == VK::Int && (out < INT32_MIN || out > INT32_MAX)) {
        if (checked) throw overflow();
        out = (int32_t)out;
    }
    return intValue(out, kind);
}

ConstValue ConstEval::evalBinary(const BinaryExpr& b) {
    if (b.op == BinOp::LogAnd || b.op == BinOp::LogOr) {
        bool left = isTrue(eval(*b.lhs), b.lhs->loc);
        if (b.op == BinOp::LogAnd && !left) return boolValue(false);
        if (b.op == BinOp::LogOr && left) return boolValue(true);
        return boolValue(isTrue(eval(*b.rhs), b.rhs->loc));
    }
    ConstValue l = eval(*b.lhs);
    ConstValue r = eval(*b.rhs);
    return arith(b.op, l, r, b.loc, b.checked);
}

ConstValue ConstEval::evalAssign(const AssignExpr& a) {
    if (a.target->kind != Expr::Kind::Name)
        throw ConstError{a.loc, "only a local variable can be assigned while compiling"};
    const std::string& name = static_cast<const NameExpr&>(*a.target).name;
    Local* local = findLocal(name);
    if (!local) throw ConstError{a.loc, "'" + name + "' cannot be changed while compiling, since it is not a local variable of this function"};
    ConstValue value = eval(*a.value);
    local = findLocal(name);  // what the value called may have moved it
    if (a.op) value = arith(*a.op, local->value, value, a.loc, a.checked);
    local->value = convert(value, local->type, a.loc, a.op && !a.checked);  // `c += 1` on a char wraps at 256
    return local->value;
}

ConstValue ConstEval::evalIncDec(const IncDecExpr& i) {
    if (i.target->kind != Expr::Kind::Name) throw ConstError{i.loc, "only a local variable can be changed while compiling"};
    const std::string& name = static_cast<const NameExpr&>(*i.target).name;
    Local* local = findLocal(name);
    if (!local) throw ConstError{i.loc, "'" + name + "' cannot be changed while compiling, since it is not a local variable of this function"};
    ConstValue old = local->value;
    ConstValue next = arith(i.increment ? BinOp::Add : BinOp::Sub, old, intValue(1), i.loc, i.checked);
    local->value = convert(next, local->type, i.loc, !i.checked);
    return i.prefix ? local->value : old;
}

ConstValue ConstEval::evalCall(const CallExpr& c) {
    if (c.vectorType || c.constructs) throw ConstError{c.loc, "objects and vectors cannot be built while compiling"};
    const FuncDecl* f = c.target;
    const std::string callee = hooks_.resolve ? hooks_.resolve(c.callee, c.loc) : c.callee;
    if (!f && hooks_.function) f = hooks_.function(callee);
    if (!f) {
        // The built-in ones that C++ can also work out.
        if (c.callee == "abs" && c.args.size() == 1) {
            ConstValue v = eval(*c.args[0]);
            if (isFloating(v)) return floatValue(std::fabs(v.d), v.kind);
            if (!isIntegerLike(v)) throw ConstError{c.loc, std::string("abs() does not work on a ") + v.typeName()};
            const VK kind = kindOfInteger(commonIntegerType(typeOfValue(v), typeOfValue(v)));
            if (isUnsignedKind(kind)) return v;
            return v.i < 0 ? arith(BinOp::Sub, intValue(0, kind), v, c.loc, c.checked) : intValue(v.i, kind);
        }
        if ((c.callee == "min" || c.callee == "max") && c.args.size() == 2) {
            ConstValue x = eval(*c.args[0]);
            ConstValue y = eval(*c.args[1]);
            bool pickSecond = isTrue(arith(BinOp::Lt, c.callee == "min" ? y : x, c.callee == "min" ? x : y, c.loc, true), c.loc);
            return pickSecond ? y : x;
        }
        if (hooks_.generic && hooks_.generic(callee))
            throw ConstError{c.loc, "this compiler does not support a generic function here yet"};
        throw ConstError{c.loc, "'" + c.callee + "' cannot run while compiling. Only functions marked `constexpr` can"};
    }
    if (!f->isConstexpr)
        throw ConstError{c.loc, "'" + c.callee + "' is not marked `constexpr`, so it cannot run while compiling"};
    if (f->params.size() != c.args.size())
        throw ConstError{c.loc, "'" + c.callee + "' takes " + std::to_string(f->params.size()) + " arguments, not " + std::to_string(c.args.size())};
    std::vector<ConstValue> args;
    for (auto& a : c.args) args.push_back(eval(*a));
    return callFunction(*f, std::move(args), c.loc);
}

ConstValue ConstEval::callFunction(const FuncDecl& f, std::vector<ConstValue> args, Loc loc) {
    if (frames_.size() >= kMaxDepth)
        throw ConstError{loc, "'" + f.name + "' calls itself too deeply to work out while compiling"};
    if (!f.body) throw ConstError{loc, "'" + f.name + "' has no body"};
    if (hooks_.prepare) hooks_.prepare(f);
    Scope params;
    for (size_t i = 0; i < args.size(); ++i)
        params[f.params[i]->name] = Local{f.params[i]->declType, convert(args[i], f.params[i]->declType, loc)};
    frames_.emplace_back();
    frames_.back().push_back(std::move(params));
    ++globalsOnly_;  // a function sees its own locals and the file's constants, not its caller's
    struct Leave {
        ConstEval& e;
        ~Leave() {
            e.frames_.pop_back();
            --e.globalsOnly_;
        }
    } leave{*this};
    Flow flow = Flow::Normal;
    for (auto& s : f.body->stmts) {
        flow = exec(*s);
        if (flow != Flow::Normal) break;
    }
    if (flow != Flow::Return) {
        if (f.retType->is(K::Void)) return intValue(0);
        throw ConstError{f.nameLoc, "'" + f.name + "' reached its end without returning a value"};
    }
    return f.retType->is(K::Void) ? intValue(0) : convert(returned_, f.retType, f.nameLoc);
}

ConstValue ConstEval::evalMethodCall(const MethodCallExpr& c) {
    if (c.lowered) throw ConstError{c.loc, "a hashmap cannot be used while compiling"};
    if (c.receiver->kind == Expr::Kind::This) throw ConstError{c.loc, "methods cannot run while compiling"};
    ConstValue recv = eval(*c.receiver);
    if (recv.kind == VK::String && c.args.empty()) {
        if (c.name == "size" || c.name == "length") return intValue((int64_t)recv.s.size(), VK::Long);
        if (c.name == "empty") return boolValue(recv.s.empty());
    }
    throw ConstError{c.loc, "the method '" + c.name + "' cannot run while compiling"};
}

ConstValue ConstEval::eval(const Expr& e) {
    tick(e.loc);
    switch (e.kind) {
        case Expr::Kind::IntLit: {
            auto& lit = static_cast<const IntLitExpr&>(e);
            return intValue(lit.value, kindOfInteger(lit.writtenType()));
        }
        case Expr::Kind::FloatLit: {
            auto& lit = static_cast<const FloatLitExpr&>(e);
            return floatValue(lit.value, lit.isFloat ? VK::Float : VK::Double);
        }
        case Expr::Kind::CharLit: return intValue(static_cast<const CharLitExpr&>(e).value, VK::Char);
        case Expr::Kind::BoolLit: return boolValue(static_cast<const BoolLitExpr&>(e).value);
        case Expr::Kind::StringLit: return stringValue(static_cast<const StringLitExpr&>(e).value);
        case Expr::Kind::Name: return evalName(static_cast<const NameExpr&>(e));
        case Expr::Kind::Unary: return evalUnary(static_cast<const UnaryExpr&>(e));
        case Expr::Kind::Binary: return evalBinary(static_cast<const BinaryExpr&>(e));
        case Expr::Kind::Assign: return evalAssign(static_cast<const AssignExpr&>(e));
        case Expr::Kind::IncDec: return evalIncDec(static_cast<const IncDecExpr&>(e));
        case Expr::Kind::Ternary: {
            // Its type comes from both branches, as `c ? 1 : 2.5` is a double whichever runs.
            auto& t = static_cast<const TernaryExpr&>(e);
            ConstValue value = eval(isTrue(eval(*t.cond), t.cond->loc) ? *t.thenExpr : *t.elseExpr);
            const Type* type = staticType(e);
            return type && type->isArithmetic() ? convert(value, type, e.loc) : value;
        }
        case Expr::Kind::Call: return evalCall(static_cast<const CallExpr&>(e));
        case Expr::Kind::MethodCall: return evalMethodCall(static_cast<const MethodCallExpr&>(e));
        case Expr::Kind::Index: {
            auto& ix = static_cast<const IndexExpr&>(e);
            ConstValue base = eval(*ix.base);
            ConstValue index = eval(*ix.index);
            if (base.kind != VK::String || !isIntegerLike(index)) throw ConstError{e.loc, "only a string can be indexed while compiling"};
            if (index.i < 0 || index.i >= (int64_t)base.s.size())
                throw ConstError{e.loc, "index " + std::to_string(index.i) + " is out of range for a string of size " + std::to_string(base.s.size())};
            return intValue((uint8_t)base.s[(size_t)index.i], VK::Char);
        }
        case Expr::Kind::Cast: {
            auto& c = static_cast<const CastExpr&>(e);
            if (c.reinterpret || c.dynamic) throw ConstError{e.loc, "this cast cannot be worked out while compiling"};
            if (!c.operand) {  // `T()`
                if (c.target->is(K::String)) return stringValue("");
                return convert(intValue(0), c.target, e.loc);
            }
            return convert(eval(*c.operand), c.target, e.loc, !c.checked);
        }
        case Expr::Kind::Sizeof: {
            auto& z = static_cast<const SizeofExpr&>(e);
            if (z.target && hooks_.sizeOf) return intValue(hooks_.sizeOf(z.target, e.loc), VK::Long);
            if (z.operand) {
                ConstValue v = eval(*z.operand);
                switch (v.kind) {
                    case VK::Bool: case VK::Char: case VK::UInt8: return intValue(1, VK::Long);
                    case VK::UInt16: return intValue(2, VK::Long);
                    case VK::Int: case VK::Float: case VK::UInt32: return intValue(4, VK::Long);
                    case VK::UInt64: return intValue(8, VK::Long);
                    case VK::Long: case VK::Double: return intValue(8, VK::Long);
                    case VK::String: return intValue(24, VK::Long);
                    case VK::Enum: return intValue(v.enumType->decl->underlying && v.enumType->decl->underlying->is(K::Long) ? 8 : 4, VK::Long);
                }
            }
            throw ConstError{e.loc, "the size of this type is not known here"};
        }
        case Expr::Kind::Try: return eval(*static_cast<const TryExpr&>(e).operand);
        case Expr::Kind::Constraint: {
            auto& c = static_cast<const ConstraintExpr&>(e);
            if (!hooks_.constraint) throw ConstError{e.loc, "this cannot be asked here"};
            return boolValue(hooks_.constraint(c));
        }
        case Expr::Kind::EnumValue: {
            auto& v = static_cast<const EnumValueExpr&>(e);
            if (!v.decl->valuesKnown) throw ConstError{e.loc, "the values of '" + v.decl->name + "' are not known yet where this is read"};
            int at = v.index >= 0 ? v.index : v.decl->enumeratorIndex(v.name);
            if (at < 0) throw ConstError{v.nameLoc, "the enum '" + v.decl->name + "' has no enumerator '" + v.name + "'"};
            return enumValue(v.decl->enumerators[(size_t)at].value, &v.decl->type);
        }
        case Expr::Kind::NullLit: throw ConstError{e.loc, "null is only a value while the program runs"};
        case Expr::Kind::New: throw ConstError{e.loc, "memory cannot be allocated while compiling"};
        case Expr::Kind::InitList: throw ConstError{e.loc, "a { } list cannot be worked out while compiling"};
        default: throw ConstError{e.loc, "this kind of expression cannot be worked out while compiling"};
    }
}

ConstEval::Flow ConstEval::execVar(const VarDecl& v) {
    ConstValue value;
    if (v.init) value = eval(*v.init);
    else if (v.declType->is(K::String)) value = stringValue("");
    if (!v.init && !v.declType->is(K::String)) value = convert(intValue(0), v.declType, v.loc);
    else value = convert(value, v.declType, v.loc);
    // An `auto` variable keeps the type of its first value, as it does in the running program.
    const Type* type = v.declType->is(K::Auto) ? typeOfValue(value) : v.declType;
    frames_.back().back()[v.name] = Local{type, std::move(value)};
    return Flow::Normal;
}

ConstEval::Flow ConstEval::exec(const Stmt& s) {
    tick(s.loc);
    struct ScopeGuard {
        ConstEval& e;
        explicit ScopeGuard(ConstEval& eval) : e(eval) { e.frames_.back().emplace_back(); }
        ~ScopeGuard() { e.frames_.back().pop_back(); }
    };
    switch (s.kind) {
        case Stmt::Kind::VarDecl: return execVar(static_cast<const VarDecl&>(s));
        case Stmt::Kind::DeclGroup:
            for (auto& v : static_cast<const DeclGroupStmt&>(s).decls) execVar(*v);
            return Flow::Normal;
        case Stmt::Kind::Expr:
            eval(*static_cast<const ExprStmt&>(s).expr);
            return Flow::Normal;
        case Stmt::Kind::Block: {
            ScopeGuard scope(*this);
            for (auto& st : static_cast<const BlockStmt&>(s).stmts) {
                Flow flow = exec(*st);
                if (flow != Flow::Normal) return flow;
            }
            return Flow::Normal;
        }
        case Stmt::Kind::If: {
            auto& st = static_cast<const IfStmt&>(s);
            if (st.decl) throw ConstError{s.loc, "a variable in a condition cannot be worked out while compiling"};
            if (st.isConstexpr && st.cond && !st.thenStmt && !st.elseStmt && st.lazy)
                throw ConstError{s.loc, "this generic function cannot be worked out yet, since it has not been checked"};
            if (st.isConstexpr && st.resolved) {  // Sema kept the branch that counts
                const Stmt* kept = st.thenStmt ? st.thenStmt.get() : st.elseStmt.get();
                if (!kept) return Flow::Normal;
                ScopeGuard scope(*this);
                return exec(*kept);
            }
            const Stmt* chosen = isTrue(eval(*st.cond), st.cond->loc) ? st.thenStmt.get() : st.elseStmt.get();
            if (!chosen) return Flow::Normal;
            ScopeGuard scope(*this);
            return exec(*chosen);
        }
        case Stmt::Kind::While: {
            auto& st = static_cast<const WhileStmt&>(s);
            if (st.decl) throw ConstError{s.loc, "a variable in a condition cannot be worked out while compiling"};
            while (isTrue(eval(*st.cond), st.cond->loc)) {
                ScopeGuard scope(*this);
                Flow flow = exec(*st.body);
                if (flow == Flow::Break) break;
                if (flow == Flow::Return) return flow;
            }
            return Flow::Normal;
        }
        case Stmt::Kind::DoWhile: {
            auto& st = static_cast<const DoWhileStmt&>(s);
            do {
                ScopeGuard scope(*this);
                Flow flow = exec(*st.body);
                if (flow == Flow::Break) break;
                if (flow == Flow::Return) return flow;
            } while (isTrue(eval(*st.cond), st.cond->loc));
            return Flow::Normal;
        }
        case Stmt::Kind::For: {
            auto& st = static_cast<const ForStmt&>(s);
            ScopeGuard scope(*this);
            if (st.init) exec(*st.init);
            while (!st.cond || isTrue(eval(*st.cond), st.cond->loc)) {
                {
                    ScopeGuard body(*this);
                    Flow flow = exec(*st.body);
                    if (flow == Flow::Break) break;
                    if (flow == Flow::Return) return flow;
                }
                for (auto& step : st.steps) eval(*step);
            }
            return Flow::Normal;
        }
        case Stmt::Kind::Return: {
            auto& st = static_cast<const ReturnStmt&>(s);
            returned_ = st.value ? eval(*st.value) : intValue(0);
            return Flow::Return;
        }
        case Stmt::Kind::Break: return Flow::Break;
        case Stmt::Kind::Continue: return Flow::Continue;
        case Stmt::Kind::Try: return exec(*static_cast<const TryStmt&>(s).body);  // if nothing fails, nothing else runs
        case Stmt::Kind::Switch: return execSwitch(static_cast<const SwitchStmt&>(s));
        case Stmt::Kind::Fallthrough: return Flow::Normal;
        case Stmt::Kind::Throw: throw ConstError{s.loc, "it reached a `throw` while compiling"};
        default: throw ConstError{s.loc, "this kind of statement cannot run while compiling"};
    }
}

// The section the value has a label for, or the default, and the ones it runs on into.
ConstEval::Flow ConstEval::execSwitch(const SwitchStmt& s) {
    ConstValue value = eval(*s.value);
    size_t start = s.sections.size(), fallback = s.sections.size();
    for (size_t i = 0; i < s.sections.size() && start == s.sections.size(); ++i)
        for (auto& label : s.sections[i].labels) {
            if (!label.value) fallback = i;
            else if (isTrue(arith(BinOp::Eq, value, eval(*label.value), label.loc, true), label.loc)) {
                start = i;
                break;
            }
        }
    if (start == s.sections.size()) start = fallback;
    if (start == s.sections.size()) {
        if (s.exhaustive) throw ConstError{s.value->loc, "the switch has no case for " + std::string(value.typeName()) + "(" + std::to_string(value.i) + ")"};
        return Flow::Normal;
    }
    for (size_t i = start; i < s.sections.size(); ++i) {
        frames_.back().emplace_back();
        struct Pop {
            ConstEval& e;
            ~Pop() { e.frames_.back().pop_back(); }
        } pop{*this};
        for (auto& st : s.sections[i].body->stmts) {
            Flow flow = exec(*st);
            if (flow == Flow::Break) return Flow::Normal;
            if (flow != Flow::Normal) return flow;
        }
    }
    return Flow::Normal;
}

}  // namespace cplus
