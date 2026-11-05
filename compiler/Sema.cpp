#include "Sema.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <functional>
#include <set>

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
    if (to->isOptional()) {
        // `null`, or a value that the optional then holds. Two optional types do not
        // convert into each other.
        if (from->isNull()) return true;
        return !from->isOptional() && canConvert(from, to->elem);
    }
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
static std::string spelled(const Expr& e, const std::string& fallback = "opt") {
    if (e.kind == Expr::Kind::Name) return static_cast<const NameExpr&>(e).name;
    if (e.kind == Expr::Kind::This) return "this";
    if (e.kind == Expr::Kind::Member) {
        auto& m = static_cast<const MemberExpr&>(e);
        if (m.base->kind == Expr::Kind::Unary && static_cast<const UnaryExpr&>(*m.base).isArrow()) {
            std::string base = spelled(*static_cast<const UnaryExpr&>(*m.base).operand, "");
            if (!base.empty()) return base + "->" + m.field;
        }
        std::string base = spelled(*m.base, "");
        if (!base.empty()) return base + "." + m.field;
    }
    if (isDeref(e)) {
        std::string base = spelled(*static_cast<const UnaryExpr&>(e).operand, "");
        if (!base.empty()) return "*" + base;
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

// Whether `e` lies in the copy of an optional's value that `?.` works on, rather than
// in a class object that the optional refers to.
static bool inOptionalCopy(const Expr& e) {
    switch (e.kind) {
        case Expr::Kind::OptValue: return e.type && !e.type->isClass();
        case Expr::Kind::Member: {
            const Expr& base = *static_cast<const MemberExpr&>(e).base;
            return !(base.type && base.type->isClass()) && inOptionalCopy(base);
        }
        case Expr::Kind::Index: return inOptionalCopy(*static_cast<const IndexExpr&>(e).base);
        default: return false;
    }
}

static const char* kChainOnlyReads =
    "'?.' only reads. To change the value, unwrap the optional first: `if (auto u = opt) u.field = ...;`";

// How to turn a value of type `t` into text, for errors about joining it to a string
// with `op` (+ or +=).
static std::string textHelp(const Type* t, const char* op = "+") {
    if (t->isNumber())
        return std::string("turn the number into a string first: `s ") + op +
               " to_string(n)`, or build the whole text with `format(\"{}{}\", s, n)`. Unlike C++, a number is "
               "never added as a character code";
    if (t->is(K::Bool)) return "`format(\"{}\", b)` turns a bool into \"true\" or \"false\"";
    if (t->is(K::Void) || t->isError()) return "";
    if (t->isOptional()) return "it may be null: `opt ?? \"none\"` is its value, or \"none\" when it is null";
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
static std::string zeroOf(const Type* t) {
    if (t->is(K::Bool)) return "false";
    if (t->is(K::Char)) return "'\\0'";
    if (t->is(K::String)) return "\"\"";
    if (t->isStruct()) return "{}";
    if (t->isClass()) return std::string(t->name) + "(...)";
    if (t->isPointer()) return "null";
    return "0";
}

// Paths deeper than this only happen with a struct that contains itself, which is
// already an error; stop instead of recursing forever.
static bool tooDeep(const std::string& path) { return std::count(path.begin(), path.end(), '.') > 32; }

// A class reference cannot be empty, so a field holding one (directly or inside a
// struct field) must be given a value when an object is built.
static bool needsValue(const Type* t, int depth = 0) {
    if (t->isClass()) return true;
    if (!t->isStruct() || depth > 32) return false;
    for (auto& f : t->decl->fields)
        if (!f.defaultValue && needsValue(f.type, depth + 1)) return true;
    return false;
}

// Whether building a `from` needs a `target` that already exists: true when fields
// without defaults lead from one to the other. A type that needs itself, like a list
// node with a `Node next` field, cannot be built until references can be empty.
static bool needsExisting(const Type* from, const StructDecl* target, std::set<const StructDecl*>& seen) {
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
static std::string listFields(const StructDecl& sd) {
    if (sd.fields.empty()) return q(sd.name) + " has no fields";
    std::string out = "its fields are ";
    for (size_t i = 0; i < sd.fields.size(); ++i)
        out += (i ? ", " : "") + std::string(sd.fields[i].type->name) + " " + sd.fields[i].name;
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
        int idx = t->decl->fieldIndex(field);
        if (idx < 0) return Type::error();
        t = t->decl->fields[idx].type;
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
// value, optionals null and fields their defaults, as in C++. Numbers and bools stay
// unassigned.
void Sema::markDefaults(Flow& flow, const VarDecl* var, const Type* type, const std::string& path) {
    if (tooDeep(path)) return;
    if (type->is(K::String) || type->isVector() || type->isOptional()) {
        markAssigned(flow, var, path);
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
    for (auto& f : type->decl->fields) {
        std::string fieldPath = join(path, f.name);
        if (f.defaultValue || !needsValue(f.type)) markAssigned(flow, var, fieldPath);
        else if (f.type->isStruct()) markConstructed(flow, var, f.type, fieldPath);
    }
}

void Sema::requireFullyBuilt(Flow& flow, Loc loc) {
    VarDecl* self = thisVar();
    if (!flow.reachable || isAssigned(flow, self, self->type, "")) return;
    std::string missing = firstUnassigned(flow, self, self->type, "").substr(5);  // drop "this."
    const Type* missingType = typeAtPath(self, missing);
    diags_.error(loc, "the constructor of " + q(owner()->name) + " must give every class-typed field a value, but " +
                          q(missing) + " has none",
                 "a " + q(missingType) + " reference cannot be empty. Set it in the initializer list, e.g. `: " +
                     missing.substr(0, missing.find('.')) + "(value)`, or give the field a default" +
                     cycleHelp(missingType, missing.substr(0, missing.find('.')), *owner()),
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
    if (!flow.reachable || isAssigned(flow, var, type, path)) return;
    std::string name = placeName(var, path);
    if (type->isStruct() && anyPartAssigned(flow, var, path)) {
        std::string missing = firstUnassigned(flow, var, type, path);
        diags_.error(loc, q(name) + " is used before all of its fields have values: " + q(missing) + " has none yet",
                     "assign " + q(missing) + " on every path before this line, or give the field a default in "
                     "the struct, e.g. `int x = 0;`",
                     spec::DefiniteAssignment);
    } else if (path.empty()) {
        diags_.error(loc, q(name) + " is used before it is given a value",
                     "give it a value where it is declared, e.g. `" + std::string(type->name) + " " + var->name +
                         " = " + zeroOf(type) + ";`, or assign it on every path before this line",
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
    for (auto& f : sd.fields) candidates.push_back(f.name);
    return closest(name, candidates);
}

void Sema::reportUnknownName(const NameExpr& n) {
    std::string help;
    if (auto s = structs_.find(n.name); s != structs_.end())
        help = q(n.name) + " is a " + s->second->keyword() + " type. To build one, write `" + n.name + "{...}` or `" +
               n.name + "(...)`";
    else if (funcs_.count(n.name)) help = "functions can only be called for now, e.g. `" + n.name + "()`";
    else if (n.name == "cout" || n.name == "endl" || n.name == "cerr") help = "C+ prints with `println(\"{}\", value);`";
    else if (n.name == "nullptr" || n.name == "NULL") help = "write `null`; only an optional, such as `User?`, can be null";
    else if (n.name == "M_PI" || n.name == "M_E" || n.name == "INFINITY" || n.name == "NAN")
        help = n.name == "INFINITY" || n.name == "NAN"
                   ? "write `1.0 / 0` for infinity, or `0.0 / 0` for NaN; constants for them are planned"
                   : "math constants are planned. Until then, `acos(-1)` is pi and `exp(1)` is e";
    else if (defaultsOf_ && defaultsOf_->fieldIndex(n.name) >= 0)
        help = "a field's default cannot read other fields; set this field in a constructor instead";
    else if (std::string guess = suggestName(n.name); !guess.empty()) help = "did you mean " + q(guess) + "?";
    else help = "declare it before this line, e.g. `auto " + n.name + " = ...;`";
    diags_.error(n.loc, q(n.name) + " is not declared", help, spec::Variables);
}

// ---- Program, structs and functions ----------------------------------------

void Sema::check(Program& program) {
    program_ = &program;
    for (auto& imp : program.imports)
        diags_.error(imp.loc, "this compiler does not support modules yet, so '" + imp.path + "' cannot be imported",
                     "remove the import for now; built-ins like println need no import", spec::Status);

    for (auto& sd : program.structs)
        if (sd->defined) structs_[sd->name] = sd.get();

    for (auto& fn : program.funcs) {
        if (isBuiltinFunction(fn->name)) {
            diags_.error(fn->nameLoc, q(fn->name) + " is a built-in function and cannot be redefined",
                         "give your function a different name", spec::Printing);
            continue;
        }
        if (fn->isExtern && (fn->name == "main" || fn->name.rfind("cplus_", 0) == 0)) {
            diags_.error(fn->nameLoc,
                         fn->name == "main" ? "'main' is where a C+ program starts, so it cannot be a C function"
                                            : "names starting with 'cplus_' are reserved for the C+ runtime",
                         "give the function a different name", spec::CFunctions);
            continue;
        }
        if (auto s = structs_.find(fn->name); s != structs_.end()) {
            diags_.error(fn->nameLoc, q(fn->name) + " is already the name of a struct",
                         "give the function a different name", spec::Functions);
            diags_.note(s->second->nameLoc, "the struct is defined here");
            continue;
        }
        auto [it, inserted] = funcs_.emplace(fn->name, fn.get());
        if (!inserted) {
            diags_.error(fn->nameLoc, "a function named " + q(fn->name) + " is already defined",
                         "rename one of them; overloading (several functions with one name) is planned but not "
                         "supported yet",
                         spec::Functions);
            diags_.note(it->second->nameLoc, "the first definition is here");
        }
    }

    auto mainIt = funcs_.find("main");
    if (mainIt == funcs_.end()) {
        diags_.error(Loc{}, "this program has no main function",
                     "every program starts at `int main() { ... return 0; }`", spec::MainFunction);
    } else {
        FuncDecl* m = mainIt->second;
        if (!m->retType->is(K::Int))
            diags_.error(m->retTypeLoc, "main must return int, not " + q(m->retType),
                         "write `int main()`; the returned number becomes the exit code", spec::MainFunction);
        if (!m->params.empty())
            diags_.error(m->params[0]->loc, "this compiler does not support command-line arguments to main yet",
                         "write `int main()`", spec::Status);
    }

    checkStructs(program);
    for (auto& sd : program.structs)
        if (sd->defined) checkMembers(*sd);
    for (auto& fn : program.funcs) checkFunction(*fn);
    for (auto& sd : program.structs)
        if (sd->defined)
            for (auto& m : sd->methods) checkFunction(*m);
}

void Sema::checkMembers(StructDecl& sd) {
    std::unordered_map<std::string, FuncDecl*> seen;
    for (auto& m : sd.methods) {
        if (m->role != FuncDecl::Role::Method) continue;
        if (int f = sd.fieldIndex(m->name); f >= 0) {
            diags_.error(m->nameLoc, q(sd.name) + " already has a field named " + q(m->name),
                         "a method and a field cannot share a name", spec::Methods);
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

bool Sema::canAccess(const StructDecl& sd, Access access) const {
    return access == Access::Public || owner() == &sd;
}

void Sema::reportAccess(const StructDecl& sd, const std::string& member, bool isMethod, Loc loc) {
    diags_.error(loc, q(member) + " is private in " + q(sd.name),
                 std::string("it can only be used inside the methods of ") + q(sd.name) + ". Put the " +
                     (isMethod ? "method" : "field") + " under `public:` to use it here",
                 spec::Classes);
}

// What in `t` has no equivalent in C, or "" when C can use it: C knows numbers, chars,
// bools, pointers to those, and structs of those. `seen` stops at a struct already looked
// at, such as one that points to itself.
static std::string whatCLacks(const Type* t, std::set<const StructDecl*>& seen) {
    if (t->is(K::String)) return "a string";
    if (t->isVector()) return "a vector";
    if (t->isClass()) return "a class reference";
    if (t->isOptional()) return "an optional";
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
    "C knows numbers, chars, bools, pointers, and structs of those. For text, use a `char*` field and read it with "
    "`string(p)`. To pass a C+ value through C without C looking inside, pass its address as a `void*`";

void Sema::checkStructs(Program& program) {
    // A struct cannot contain itself, directly or through other structs.
    std::unordered_map<const StructDecl*, int> state;  // 1 = being visited, 2 = done
    std::function<void(const StructDecl*)> visit = [&](const StructDecl* sd) {
        state[sd] = 1;
        for (auto& f : sd->fields) {
            // An optional struct holds the struct itself, so it counts too.
            const Type* ft = f.type->isOptional() ? f.type->elem : f.type;
            if (!ft->isStruct()) continue;
            const StructDecl* inner = ft->decl;
            if (state[inner] == 1)
                diags_.error(f.loc,
                             "struct " + q(sd->name) + " contains itself through field " + q(f.name) +
                                 ", so it would be infinitely large",
                             "a struct can hold other structs, but not itself, not even as an optional. For a "
                             "linked structure, make it a class: a `" + std::string(sd->name) + "? next;` field in a "
                             "class refers to another object instead of holding it",
                             spec::Structs);
            else if (state[inner] == 0)
                visit(inner);
        }
        state[sd] = 2;
    };
    for (auto& sd : program.structs)
        if (sd->defined && state[sd.get()] == 0) visit(sd.get());

    // Field defaults are checked with no local variables in scope.
    for (auto& sd : program.structs) {
        if (!sd->defined) continue;
        for (auto& f : sd->fields) {
            if (f.type->is(K::Void) || f.type->is(K::Auto))
                diags_.error(f.typeLoc, "a field cannot have type " + q(f.type), "", spec::Structs);
            checkStorable(f.type, f.typeLoc);
            if (sd->externC) {
                std::set<const StructDecl*> seen{sd.get()};
                std::string lacks = whatCLacks(f.type, seen);
                if (!lacks.empty())
                    diags_.error(f.typeLoc, "a C struct cannot have a field of type " + q(f.type) +
                                                ", because C has no equivalent of " + lacks,
                                 kCTypesHelp, spec::CStructs);
            }
            if (!f.defaultValue) continue;
            scopes_.emplace_back();
            Flow flow;
            defaultsOf_ = sd.get();
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
        if (t->isClass())
            help = "C cannot hold a C+ object; pass the fields it needs as separate arguments";
        else if (t->isVector())
            message = "this compiler does not support passing vectors to or from C functions yet",
            help = "C takes an array as a pointer to its first element. Pass the elements one at a time, or memory from "
                   "C's malloc that holds them";
        else if (t->isOptional())
            help = "in C, only text and pointers can be null. " + std::string(isReturn ? "Declare the result as" : "Pass") +
                   " a plain " + q(t->elem) + " instead";
        diags_.error(loc, message, help, spec::CFunctions);
    };
    check(fn.retType, fn.retTypeLoc, true);
    for (auto& p : fn.params) {
        p->type = p->declType;
        check(p->declType, p->typeLoc, false);
    }
}

void Sema::checkFunction(FuncDecl& fn) {
    if (fn.isExtern) return checkExternFunction(fn);
    fn_ = &fn;
    Flow flow;
    bool isCtor = fn.role == FuncDecl::Role::Constructor;
    if (fn.owner) {
        fn.thisDecl = std::make_unique<VarDecl>(fn.nameLoc, "this", &fn.owner->type, fn.nameLoc);
        fn.thisDecl->type = &fn.owner->type;
        if (isCtor) markConstructed(flow, fn.thisDecl.get(), &fn.owner->type, "");
        else markAssigned(flow, fn.thisDecl.get(), "");
    }
    // Parameters and the body's top level share one scope, as in C++.
    scopes_.emplace_back();
    checkStorable(fn.retType, fn.retTypeLoc);
    for (auto& p : fn.params) {
        checkStorable(p->declType, p->typeLoc);
        if (p->declType->is(K::Void))
            diags_.error(p->typeLoc, "parameters cannot have type void", "", spec::Functions);
        p->type = p->declType;
        declare(*p);
        markAssigned(flow, p.get(), "");
    }
    if (isCtor) {
        // Fields are initialized in the order they are declared, whatever the order of
        // the list, as in C++.
        const StructDecl& sd = *fn.owner;
        for (auto& init : fn.memberInits) init.fieldIndex = sd.fieldIndex(init.field);
        std::stable_sort(fn.memberInits.begin(), fn.memberInits.end(),
                         [](auto& a, auto& b) { return a.fieldIndex < b.fieldIndex; });
        std::vector<bool> seen(sd.fields.size(), false);
        for (auto& init : fn.memberInits) {
            if (init.fieldIndex < 0) {
                std::string guess = suggestField(sd, init.field);
                diags_.error(init.loc, q(sd.name) + " has no field named " + q(init.field),
                             guess.empty() ? listFields(sd) : "did you mean " + q(guess) + "?", spec::Classes);
                checkLooseValue(*init.value, flow);
                continue;
            }
            const FieldDecl& f = sd.fields[init.fieldIndex];
            if (seen[init.fieldIndex])
                diags_.error(init.loc, "field " + q(f.name) + " is initialized more than once", "", spec::Classes);
            seen[init.fieldIndex] = true;
            checkValue(*init.value, flow, f.type, "for field " + q(f.name));
            markAssigned(flow, fn.thisDecl.get(), f.name);
        }
    }
    for (auto& s : fn.body->stmts) checkStmt(*s, flow);
    if (isCtor) requireFullyBuilt(flow, fn.body->endLoc);
    scopes_.pop_back();

    if (flow.reachable && !fn.retType->is(K::Void) && fn.name != "main") {
        diags_.error(fn.body->endLoc, "function " + q(fn.name) + " can reach its end without returning a value",
                     "add `return <" + std::string(fn.retType->name) + " value>;` before this closing brace",
                     spec::Functions);
    }
    fn_ = nullptr;
}

// ---- Statements ------------------------------------------------------------

void Sema::checkVarDecl(VarDecl& v, Flow& flow) {
    checkStorable(v.declType, v.typeLoc);
    if (v.init) {
        if (v.declType->is(K::Auto)) {
            const Type* t = checkExpr(*v.init, flow);
            if (t->is(K::Void))
                diags_.error(v.init->loc, "cannot store the result of a void function in " + q(v.name), "",
                             spec::Variables);
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

// Help for a bool? used as a condition.
static std::string boolOptionalHelp(const std::string& name) {
    return "it could mean true, or that it has a value: write `" + name + " == true`, or `" + name +
           " != null` to check that it holds a bool";
}

void Sema::checkCondition(Expr& e, Flow& flow, const char* what) {
    const Type* t = checkExpr(e, flow);
    if (usableAsCondition(t)) return;
    std::string help;
    if (t->is(K::Void)) help = "this expression produces no value";
    else if (t->isOptional()) help = boolOptionalHelp(spelled(e, "b"));
    else if (t->is(K::Char)) help = "a char is not a condition; compare it, e.g. `c != '\\0'` or `c == 'y'`";
    else if (t->isVector() || t->is(K::String))
        help = std::string("to check whether it has ") + (t->isVector() ? "elements" : "any text") + ", write `!" +
               (e.kind == Expr::Kind::Name ? static_cast<NameExpr&>(e).name
                                           : std::string(t->isVector() ? "items" : "text")) +
               ".empty()`";
    diags_.error(e.loc, std::string("the ") + what + " must be a bool, a number, a pointer or an optional, but this is " + q(t),
                 help, spec::Conditions);
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
        case Stmt::Kind::Expr: checkExpr(*static_cast<ExprStmt&>(s).expr, flow); return;
        case Stmt::Kind::Block: {
            scopes_.emplace_back();
            for (auto& st : static_cast<BlockStmt&>(s).stmts) checkStmt(*st, flow);
            scopes_.pop_back();
            return;
        }
        case Stmt::Kind::If: {
            auto& st = static_cast<IfStmt&>(s);
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
            loops_.emplace_back();
            checkLoopBody(*st.body, flow);  // the body always runs at least once
            for (auto& c : loops_.back().continues) flow = merge(flow, c);
            checkCondition(*st.cond, flow, "do-while condition");
            if (isConstantTrue(st.cond.get())) flow.reachable = false;
            for (auto& b : loops_.back().breaks) flow = merge(flow, b);
            loops_.pop_back();
            return;
        }
        case Stmt::Kind::For: {
            auto& st = static_cast<ForStmt&>(s);
            scopes_.emplace_back();
            if (st.init) checkStmt(*st.init, flow);
            if (st.cond) checkCondition(*st.cond, flow, "for loop condition");
            loops_.emplace_back();
            Flow body = flow;
            checkLoopBody(*st.body, body);
            if (!st.steps.empty()) {
                Flow stepFlow = body;
                for (auto& c : loops_.back().continues) stepFlow = merge(stepFlow, c);
                // If the step can never run, stepFlow is unreachable: types are still
                // checked, but definite assignment is not.
                for (auto& step : st.steps) checkExpr(*step, stepFlow);
            }
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
            const Type* ret = fn_->retType;
            if (st.value) {
                if (ret->is(K::Void)) {
                    const Type* t = checkExpr(*st.value, flow);
                    diags_.error(st.value->loc, "function " + q(fn_->name) + " returns void, so it cannot return a value",
                                 "change the return type to " + q(t) + ", or write just `return;`", spec::Functions);
                } else {
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
                diags_.error(s.loc, std::string(isBreak ? "'break'" : "'continue'") + " can only be used inside a loop",
                             "", spec::ControlFlow);
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
    if (rangeType->isVector()) {
        elem = rangeType->elem;
    } else if (rangeType->is(K::String)) {
        elem = Type::charTy();  // its bytes, as in C++
    } else if (!rangeType->isError()) {
        std::string n = spelled(*st.range, "items");
        diags_.error(st.range->loc, "a range-based for loop goes over a vector or a string, but this is " + q(rangeType),
                     rangeType->isOptional() ? "it may be null; unwrap it first: `if (auto v = " + n +
                                                   ") for (auto x : v) ...`"
                                             : "write the vector or string to loop over after the ':'",
                     spec::RangeFor);
        rangeType = Type::error();
    }
    st.rangeVar = std::make_unique<VarDecl>(st.range->loc, "range", rangeType, st.range->loc);
    st.rangeVar->type = rangeType;

    VarDecl& v = *st.var;
    if (v.declType->is(K::Auto)) {
        v.type = elem;
    } else {
        v.type = v.declType;
        if (!canConvert(elem, v.type))
            diags_.error(v.typeLoc, "the elements of " + q(rangeType) + " are " + q(elem) + ", which cannot be stored in " +
                                        q(v.type) + " " + q(v.name),
                         "write `for (auto " + v.name + " : ...)` or `for (" + std::string(elem->name) + " " + v.name +
                             " : ...)`",
                         spec::RangeFor);
    }
    scopes_.emplace_back();
    loops_.emplace_back();
    Flow body = flow;
    declare(v);
    markAssigned(body, &v, "");
    checkLoopBody(*st.body, body);
    Flow after = flow;
    for (auto& b : loops_.back().breaks) after = merge(after, b);
    loops_.pop_back();
    scopes_.pop_back();
    flow = after;
}

// ---- Expressions -----------------------------------------------------------

// Help for a value of type `from` where a `to` is expected, when one of them is a pointer.
static std::string pointerHelp(const Expr& e, const Type* from, const Type* to) {
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
                   q(to->elem) + " values, cast it: `(" + to->name + ")" + n + "`";
        }
        if (e.kind == Expr::Kind::IntLit && static_cast<const IntLitExpr&>(e).value == 0) return "a pointer to nothing is `null`";
        if (from == to->elem && isPlaceKind(e)) return "write `&" + n + "` for the address of " + q(n);
        if (from->is(K::String) && to->elem->is(K::Char))
            return "a string is not a pointer to its bytes. Declare a C function's text parameter as `string`, and C+ "
                   "passes a copy that ends in a zero byte";
        return "";
    }
    if (to->is(K::Bool)) return "compare it with null: `" + n + " != null`";
    if (to->is(K::String) && from->elem->is(K::Char))
        return "write `string(" + n + ")` to copy the text it points to, up to its zero byte";
    if (canConvert(from->elem, to)) return "write `*" + n + "` for the value it points to";
    if (to->isInteger()) return "a pointer does not convert to a number";
    return "";
}

void Sema::requireConvertible(const Expr& e, const Type* to, const std::string& context) {
    const Type* from = e.type;
    if (canConvert(from, to)) return;
    const Type* target = to->isOptional() ? to->elem : to;
    if (target->is(K::Float) && isDoubleConstant(e)) {  // `float f = 1.5;`
        if (std::fabs(static_cast<const FloatLitExpr&>(e).value) > FLT_MAX)
            diags_.error(e.loc, "this number is too large for a float",
                         "the largest float is about 3.4e38; use a double instead", spec::FloatingPointTypes);
        return;
    }
    std::string help;
    if (from->isPointer() || to->isPointer()) {
        help = pointerHelp(e, from, to);
    } else if (from->is(K::Long) && to->is(K::Int)) {
        help = e.kind == Expr::Kind::IntLit ? "this number is larger than an int can hold; use long instead"
                                            : "a long may not fit in an int. Use long here, or write `int(x)`, which "
                                              "stops the program if the value does not fit";
    } else if (target->is(K::Char) && (from->isInteger() || from->is(K::String) || from->isFloating())) {
        if (e.kind == Expr::Kind::StringLit && static_cast<const StringLitExpr&>(e).value.size() == 1)
            help = "'" + static_cast<const StringLitExpr&>(e).value + "' in single quotes is a char; \"" +
                   static_cast<const StringLitExpr&>(e).value + "\" is a string";
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
        help = "this expression produces no value";
    } else if (from->is(K::Bool) && to->isNumber()) {
        help = std::string("a bool does not convert to a number by itself; write `") + to->name + "(flag)` for 0 "
               "or 1";
    } else if (from->isOptional() && !to->isOptional() && canConvert(from->elem, to)) {
        help = unwrapHelp(spelled(e));
    } else if (from->isOptional() && to->isOptional()) {
        help = "an optional only converts to an optional of the same type. Unwrap it first: `if (auto v = " +
               spelled(e) + ") ...` gives the " + std::string(from->elem->name) + " inside";
    } else if (from->isNull()) {
        help = "only an optional or a pointer can be null; make the type `" + std::string(to->name) + "?`";
    }
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
    if (type->isError() || !type->hasFields()) {
        if (!type->isError())
            diags_.error(list.loc, "a { } list builds a struct, a class object or a vector, but " + q(type) +
                                       " is expected here",
                         "write the value without braces", spec::Structs);
        list.type = Type::error();
        for (auto& el : list.elements) checkLooseValue(*el.value, flow);
        return;
    }
    std::vector<ConstructArg> args;
    for (auto& el : list.elements) args.push_back({el.designator, el.loc, el.value.get(), &el.fieldIndex});
    checkConstruction(*type->decl, std::move(args), flow, list.loc);
}

void Sema::checkConstruction(const StructDecl& sd, std::vector<ConstructArg> args, Flow& flow, Loc loc) {
    // With a constructor, the values are its arguments, in order.
    if (const FuncDecl* ctor = sd.constructor) {
        if (!canAccess(sd, ctor->access)) reportAccess(sd, sd.name + "(...)", true, loc);
        for (auto& a : args)
            if (!a.designator.empty()) {
                diags_.error(a.loc, q(sd.name) + " has a constructor, so its values are passed in order",
                             "write `" + signature(*ctor).substr(sd.name.size() + 1) + "` without field names",
                             spec::Classes);
                break;
            }
        if (args.size() != ctor->params.size()) {
            for (auto& a : args) checkLooseValue(*a.value, flow);
            diags_.error(loc, "the constructor of " + q(sd.name) + " takes " + std::to_string(ctor->params.size()) +
                                  " argument" + (ctor->params.size() == 1 ? "" : "s") + " but " +
                                  std::to_string(args.size()) + (args.size() == 1 ? " was" : " were") + " given",
                         "its signature is `" + signature(*ctor).substr(sd.name.size() + 1) + "`", spec::Classes);
            return;
        }
        for (size_t i = 0; i < args.size(); ++i) {
            if (args[i].fieldIndex) *args[i].fieldIndex = (int)i;
            checkValue(*args[i].value, flow, ctor->params[i]->declType,
                       "for parameter " + q(ctor->params[i]->name) + " of the " + q(sd.name) + " constructor");
        }
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
                             listFields(sd), spec::Structs);
            }
        } else {
            idx = sd.fieldIndex(a.designator);
            if (idx < 0) {
                std::string guess = suggestField(sd, a.designator);
                diags_.error(a.loc, q(sd.name) + " has no field named " + q(a.designator),
                             guess.empty() ? listFields(sd) : "did you mean " + q(guess) + "?", spec::Structs);
            } else if (seen[idx]) {
                diags_.error(a.loc, "field " + q(a.designator) + " is given more than once", "", spec::Structs);
                idx = -1;
            }
        }
        if (a.fieldIndex) *a.fieldIndex = idx;
        if (idx >= 0) {
            seen[idx] = true;
            const FieldDecl& f = sd.fields[idx];
            if (!canAccess(sd, f.access)) reportAccess(sd, f.name, false, a.loc);
            checkValue(*a.value, flow, f.type, "for field " + q(f.name) + " of " + q(sd.name));
        } else {
            checkLooseValue(*a.value, flow);
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
        if (!n.decl) {
            // Inside a method, a field can be used by its name alone, as in C++.
            if (StructDecl* o = owner()) {
                int idx = o->fieldIndex(n.name);
                if (idx >= 0) {
                    n.implicitField = idx;
                    var = thisVar();
                    path = n.name;
                    return e.type = o->fields[idx].type;
                }
            }
            reportUnknownName(n);
            return e.type = Type::error();
        }
        var = n.decl;
        return e.type = n.decl->type;
    }
    if (e.kind == Expr::Kind::This) {
        if (!owner()) {
            diags_.error(e.loc, "'this' can only be used inside a method or constructor", "", spec::Methods);
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
        // An element of a vector always holds a value, so it is not tracked.
        auto& ix = static_cast<IndexExpr&>(e);
        const Type* baseType = isPlaceKind(*ix.base) ? resolvePlace(*ix.base, flow, var, path) : checkExpr(*ix.base, flow);
        if (var && !baseType->isError()) requireAssigned(flow, var, path, baseType, ix.base->loc);
        var = nullptr;
        path.clear();
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
                                                       : "",
                             spec::Pointers);
            return e.type = baseType->elem;
        }
        if (baseType->isOptional()) {
            std::string n = spelled(*ix.base, "items");
            diags_.error(e.loc, q(baseType) + " may be null, so it cannot be indexed directly",
                         "unwrap it first: `if (auto v = " + n + ") ... v[i]`", spec::Optionals);
            return e.type;
        }
        bool str = baseType->is(K::String);
        if (!str && !baseType->isVector()) {
            diags_.error(e.loc, "'[ ]' picks an element of a vector or a char of a string, but this is " + q(baseType),
                         baseType->hasFields() ? "read a field with '.', e.g. `p.x`" : "", spec::Vectors);
            return e.type;
        }
        const char* what = str ? "a string" : "a vector";
        if (!indexType->isError() && !indexType->isInteger())
            diags_.error(ix.index->loc, std::string("an index into ") + what + " must be an int or long, but this is " +
                                            q(indexType),
                         indexType->is(K::Char) ? "convert the char to its code first: `int(c)`"
                         : indexType->isFloating() ? "convert it first: `int(x)`"
                                                   : "",
                         str ? spec::Strings : spec::Vectors);
        else if (ix.index->kind == Expr::Kind::IntLit && static_cast<IntLitExpr&>(*ix.index).value < 0)
            diags_.error(ix.index->loc, std::string("an index into ") + what + " cannot be negative",
                         str ? "positions are numbered from 0; the last char is `s[s.size() - 1]` or `s.back()`"
                             : "elements are numbered from 0; the last one is `v[v.size() - 1]` or `v.back()`",
                         str ? spec::Strings : spec::Vectors);
        return e.type = str ? Type::charTy() : baseType->elem;
    }
    if (e.kind != Expr::Kind::Member) return checkExpr(e, flow);

    auto& m = static_cast<MemberExpr&>(e);
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
    if (baseType->isOptional()) {
        std::string n = spelled(*m.base);
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
                     "write `" + n + "->" + m.field + "`", spec::Pointers);
        var = nullptr;
        return e.type;
    }
    if (!baseType->hasFields()) {
        std::string help;
        bool str = baseType->is(K::String);
        if (str || baseType->isVector())
            help = m.field == "size" || m.field == "length" || m.field == "empty" || m.field == "front" ||
                           m.field == "back"
                       ? q(m.field) + " is a method; call it with `" + m.field + "()`"
                   : str ? "strings have methods like size(), substr(pos, n) and find(text)"
                         : "vectors have methods like size(), push_back(x) and back(), and elements are read with `v[i]`";
        diags_.error(e.loc, q(baseType) + " has no fields, so '." + m.field + "' cannot be used", help,
                     str ? spec::Strings : baseType->isVector() ? spec::Vectors : spec::Structs);
        var = nullptr;
        return e.type;
    }
    const StructDecl& sd = *baseType->decl;
    m.fieldIndex = sd.fieldIndex(m.field);
    if (m.fieldIndex < 0) {
        std::string help;
        if (sd.findMethod(m.field)) help = q(m.field) + " is a method; call it with `" + m.field + "()`";
        else if (std::string guess = suggestField(sd, m.field); !guess.empty()) help = "did you mean " + q(guess) + "?";
        else help = listFields(sd);
        diags_.error(e.loc, q(sd.name) + " has no field named " + q(m.field), help, spec::Structs);
        var = nullptr;
        return e.type;
    }
    if (!canAccess(sd, sd.fields[m.fieldIndex].access)) reportAccess(sd, m.field, false, e.loc);
    if (var) path = join(path, m.field);
    return e.type = sd.fields[m.fieldIndex].type;
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
// when the vector grows or the string is copied before a change.
static bool insideContainer(const Expr& e) {
    switch (e.kind) {
        case Expr::Kind::Member: {
            const Expr& base = *static_cast<const MemberExpr&>(e).base;
            return !(base.type && base.type->isClass()) && insideContainer(base);
        }
        case Expr::Kind::Index:
            return !static_cast<const IndexExpr&>(e).base->type->isPointer();
        default: return false;
    }
}

// Whether a place lies in a class object that only a temporary refers to, as in
// `makeUser().name`. The object is freed when the statement ends.
static bool inTemporaryObject(const Expr& e) {
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

bool Sema::checkChangeable(const Expr& target, const char* action, const VarDecl* var, const std::string& path) {
    const Expr* root = nullptr;
    switch (storageOf(target, root)) {
        // A field of a class object can always be changed: the object lives on its
        // own, not inside a temporary. `const` on a variable only stops the variable
        // itself from being reassigned, as with `const` in JavaScript.
        case Storage::Object: return true;
        case Storage::Temporary:
            diags_.error(target.loc, std::string("only a variable or one of its fields can be ") + action,
                         "this value is a temporary copy, so changing it would have no effect", spec::Variables);
            return false;
        case Storage::Pointer: {
            const Expr& ptr = followedPointer(*root);
            if (!ptr.type->constElem) return true;
            diags_.error(target.loc,
                         q(spelled(ptr, "the pointer")) + " is a " + q(ptr.type) + ", so what it points to cannot be " +
                             action,
                         "a pointer to const only reads. Declare it as `" + std::string(ptr.type->elem->name) +
                             "*` if it needs to change what it points to",
                         spec::Pointers);
            return false;
        }
        case Storage::Variable: break;
    }
    const VarDecl* rootVar = rootVariable(*root);
    if (!rootVar) return false;  // an unknown name, already reported
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
                         "remove `const` from its declaration if it needs to change", spec::Variables);
        else
            diags_.error(target.loc, q(rootVar->name) + " is const, so " + what + " cannot be " + action,
                         "remove `const` from its declaration if it needs to change", spec::Variables);
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
            help = "write `" + n + "?." + u.arrowMember + "`, which is null when " + n + " is, or unwrap it first: `if (auto v = " +
                   n + ") ... v." + u.arrowMember + "`";
        else if (t->hasFields())
            help = std::string(t->isClass() ? "a class reference is used directly, so " : "") +
                   "fields and methods are reached with '.': write `" + n + "." + u.arrowMember + "`";
        diags_.error(u.loc, "'->' follows a pointer, but " + q(n) + " is " + q(t), help, spec::Pointers);
        return;
    }
    if (t->isOptional())
        help = "the value inside an optional is `" + n + ".value()`, `" + n + " ?? fallback`, or `v` in `if (auto v = " + n +
               ")`";
    else if (t->isClass())
        help = "a class reference is used directly: write `" + n + "` without '*'";
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
    while (inner->isVector() || inner->isOptional()) inner = inner->elem;
    if (!inner->isOpaque()) return true;
    diags_.error(loc, q(inner) + " is a C type known only by name, so C+ can only hold a pointer to it",
                 "write `" + std::string(inner->name) + "*`", spec::CTypes);
    return false;
}

// `&x`: the address of a variable, a field, or what a pointer points to.
const Type* Sema::checkAddressOf(UnaryExpr& u, Flow& flow) {
    Expr& target = *u.operand;
    if (target.kind == Expr::Kind::This) {
        checkExpr(target, flow);
        diags_.error(u.loc, "the address of 'this' cannot be taken",
                     "'this' is the object a method works on, not a variable", spec::Pointers);
        return Type::error();
    }
    if (!isPlaceKind(target)) {
        const Type* t = checkExpr(target, flow);
        if (t->isError()) return t;
        diags_.error(u.loc, "'&' takes the address of a variable, a field, or what a pointer points to, but this is a "
                            "temporary value",
                     target.kind == Expr::Kind::OptChain ? "'?.' gives a copy of the value"
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
                     (path.empty() ? "give it a value first, e.g. `" + std::string(t->name) + " " + name + " = " + zeroOf(t) +
                                         ";`"
                                   : "assign it first") +
                         ". C+ cannot tell whether code that gets the pointer writes through it before reading",
                     spec::Pointers);
        markAssigned(flow, var, path);
    }
    if (throughOptionalChain(target)) {
        diags_.error(u.loc, "'&' cannot take the address of a value reached through '?.'",
                     "'?.' works on a copy of the value; unwrap the optional into a variable first", spec::Pointers);
        return Type::error();
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
                diags_.error(otherSide.loc, "null can only be compared with == and !=", "", spec::Pointers);
            b.operandType = common;
            return Type::boolTy();
        }
        std::string help;
        if (otherSide.kind == Expr::Kind::IntLit && static_cast<const IntLitExpr&>(otherSide).value == 0)
            help = "a pointer to nothing is `null`: write `" + spelled(ptrSide, "p") + " " + sym + " null`";
        else if (other->isPointer())
            help = "pointers to different types cannot be compared; convert one first, e.g. `(void*)" +
                   spelled(otherSide, "q") + "`";
        else
            help = "a pointer is compared with another pointer to the same type, or with null";
        diags_.error(otherSide.loc, "'" + sym + "' compares " + q(ptr) + " with a pointer or null, but this is " + q(other),
                     help, spec::Pointers);
        return Type::boolTy();
    }
    if (b.op == BinOp::Sub && l->isPointer() && r->isPointer()) {
        // How many elements apart two pointers into the same memory are.
        if (l->elem != r->elem)
            diags_.error(b.loc, "'-' between two pointers needs them to point to the same type, but they are " + q(l) +
                                    " and " + q(r),
                         "", spec::Pointers);
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
    std::string help = "pointers work with `p + n`, `p - n`, `p - q` and comparisons. The value a pointer points to is `*" +
                       spelled(ptrSide, "p") + "`";
    if ((b.op == BinOp::Add || b.op == BinOp::Sub) && !other->isPointer())
        diags_.error(otherSide.loc, "'" + sym + "' moves a pointer by an int or a long, but this is " + q(other), help,
                     spec::Pointers);
    else
        diags_.error(b.loc, "'" + sym + "' cannot be used with " + q(ptr), help, spec::Pointers);
    return Type::error();
}

// Checks that `target` can be changed. `var` and `path` are set to the variable and
// field path to track for definite assignment; `var` stays null for a place inside a
// class object or a vector, which always holds a value.
bool Sema::checkAssignable(Expr& target, Flow& flow, const char* action, const VarDecl*& var, std::string& path) {
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
        std::string help = target.kind == Expr::Kind::This ? "'this' always refers to the current object" : "";
        if (target.kind == Expr::Kind::MethodCall && target.type && !target.type->isError() &&
            static_cast<MethodCallExpr&>(target).vecMethod != VecMethod::None)
            help = "front() and back() return a copy of the element; to change it, write `v[0] = ...` or "
                   "`v[v.size() - 1] = ...`";
        diags_.error(target.loc, std::string("only a variable or one of its fields can be ") + action, help,
                     spec::Variables);
        return false;
    }
    const Type* t = resolvePlace(target, flow, var, path);
    if (t->isError()) return false;
    if (throughOptionalChain(target)) {
        diags_.error(target.loc, std::string("a value reached through '?.' cannot be ") + action, kChainOnlyReads,
                     spec::Optionals);
        var = nullptr;
        return false;
    }
    if (!checkChangeable(target, action, var, path)) {
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
                    diags_.error(u.operand->loc, "'!' needs a bool, a number, a pointer or an optional, but this is " + q(t),
                                 t->isOptional()  ? boolOptionalHelp(spelled(*u.operand, "b"))
                                 : t->is(K::Char) ? "compare the char instead, e.g. `c == '\\0'`"
                                                  : "",
                                 spec::Conditions);
                result = Type::boolTy();
            } else {
                const char* sym = u.op == UnOp::Neg ? "-" : u.op == UnOp::Plus ? "+" : "~";
                t = promoted(t);  // `-c` and `+c` are ints, as in C++
                if (u.op == UnOp::BitNot ? !t->isInteger() : !t->isNumber()) {
                    diags_.error(u.operand->loc,
                                 std::string("'") + sym + "' needs " + (u.op == UnOp::BitNot ? "an int or a long" : "a number") +
                                     ", but this is " + q(t),
                                 t->isFloating() ? "'~' flips the bits of an integer" : "", spec::Operators);
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
                const Type* r = checkExpr(*b.rhs, rhsFlow);
                for (auto [side, t] : {std::pair{b.lhs.get(), l}, std::pair{b.rhs.get(), r}})
                    if (!usableAsCondition(t))
                        diags_.error(side->loc, std::string("both sides of '") + sym + "' must be a bool, a number, a pointer or an optional, but this is " + q(t),
                                     "", spec::Conditions);
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
            if (equality && l == r && !l->isNumber()) {
                b.operandType = l;
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
                             "both sides of '" + std::string(sym) + "' must hold the same type of value",
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
                if (b.op == BinOp::Add)
                    diags_.error(other->loc, "'+' joins a string with a string or a char, but this is " + q(ot),
                                 textHelp(ot), spec::Strings);
                else
                    diags_.error(other->loc, std::string("'") + sym + "' compares two strings, but this is " + q(ot),
                                 ot->isNumber()    ? "turn the number into a string first with `to_string(n)`"
                                 : ot->is(K::Char) ? "compare chars with chars, e.g. `s[0] == 'a'`, or strings with "
                                                     "strings: `s == \"a\"`"
                                                   : "",
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
                if (bad->isOptional()) help = unwrapHelp(spelled(*side));
                else if (bad->isNull()) help = "null can only be compared with == and !=";
                else if (bad->is(K::String)) help = "strings work with + to join them, and with == != < <= > >= to compare them";
                else if (equality) help = "both sides of '" + std::string(sym) + "' must have the same type, but they are " + q(l) + " and " + q(r);
                else if (comparison && (bad->hasFields() || bad->isVector())) help = q(bad) + " values can only be compared with == and !=";
                else if (bad->is(K::Bool) && (b.op == BinOp::BitAnd || b.op == BinOp::BitOr)) help = std::string("use '") + (b.op == BinOp::BitAnd ? "&&" : "||") + "' for logic";
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
            bool ok = checkAssignable(*a.target, flow, a.op ? "changed" : "assigned to", var, path);
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
                                     textHelp(vt, "+="), spec::Strings);
                } else if (t->is(K::Char)) {
                    // `c += 1` and `c -= 1` step through the characters; the result must be a char.
                    if (*a.op != BinOp::Add && *a.op != BinOp::Sub)
                        diags_.error(a.target->loc, opName + " cannot be used with 'char'",
                                     "a char steps with `c += n` and `c -= n`; for other arithmetic, convert it to its "
                                     "code first: `int(c)`",
                                     spec::Char);
                    else if (!vt->isError() && !vt->isInteger() && !vt->is(K::Char))
                        diags_.error(a.value->loc, opName + " on a char needs an int or a long, but this is " + q(vt),
                                     vt->isFloating() ? "convert it first: `int(x)`" : "", spec::Char);
                } else if (t->isPointer()) {
                    // `p += n` and `p -= n` move the pointer by n elements.
                    if (*a.op != BinOp::Add && *a.op != BinOp::Sub)
                        diags_.error(a.target->loc, opName + " cannot be used with a pointer",
                                     "a pointer moves with `p += n` and `p -= n`", spec::Pointers);
                    else if (!pointsToKnownType(t))
                        reportUnknownPointee(a.loc, t, opName);
                    else if (!vt->isError() && !vt->isInteger() && !vt->is(K::Char))
                        diags_.error(a.value->loc, opName + " on a pointer needs an int or a long, but this is " + q(vt), "",
                                     spec::Pointers);
                } else if (t->is(K::String))
                    diags_.error(a.target->loc, opName + " cannot be used with 'string'",
                                 "the only compound assignment on strings is +=, which appends", spec::Strings);
                else if (!t->isError() && !t->isNumber())
                    diags_.error(a.target->loc, opName + " needs a number, but this is " + q(t), "", spec::Operators);
                else if (!a.value->type->isError() && !a.value->type->isArithmetic())
                    diags_.error(a.value->loc, opName + " needs a number, but this is " + q(a.value->type), "",
                                 spec::Operators);
                else if ((*a.op == BinOp::Rem || *a.op == BinOp::Shl || *a.op == BinOp::Shr || *a.op == BinOp::BitAnd ||
                          *a.op == BinOp::BitOr || *a.op == BinOp::BitXor) &&
                         (t->isFloating() || a.value->type->isFloating()))
                    diags_.error(t->isFloating() ? a.target->loc : a.value->loc,
                                 opName + " needs an int or a long, but this is " +
                                     q(t->isFloating() ? t : a.value->type),
                                 *a.op == BinOp::Rem ? "for floating-point numbers, write `x = fmod(x, y)`" : "",
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
            flow = merge(a, b);
            if (x->isError() || y->isError()) break;
            if (x == y) result = x;
            else if (x->isArithmetic() && y->isArithmetic()) result = commonNumber(x, y);
            else if (const Type* ptr = commonPointer(x, y)) result = ptr;  // `c ? p : null` is a pointer
            else if (const Type* opt = optionalOfBranches(x, y)) result = opt;
            else
                diags_.error(t.loc, "the two branches of ?: have different types, " + q(x) + " and " + q(y),
                             "make both branches the same type", spec::Operators);
            break;
        }
        case Expr::Kind::IncDec: {
            auto& i = static_cast<IncDecExpr&>(e);
            const char* sym = i.increment ? "++" : "--";
            const VarDecl* var = nullptr;
            std::string path;
            bool ok = checkAssignable(*i.target, flow, i.increment ? "incremented" : "decremented", var, path);
            const Type* t = i.target->type ? i.target->type : Type::error();
            if (ok) {
                if (var) requireAssigned(flow, var, path, t, i.target->loc);
                if (t->isPointer()) {
                    if (!pointsToKnownType(t)) reportUnknownPointee(i.loc, t, sym);
                } else if (!t->isNumber() && !t->is(K::Char)) {
                    diags_.error(i.target->loc,
                                 std::string("'") + sym + "' needs a number, a char or a pointer, but this is " + q(t), "",
                                 spec::Operators);
                }
            }
            result = t;
            break;
        }
        case Expr::Kind::Call: result = checkCall(static_cast<CallExpr&>(e), flow); break;
        case Expr::Kind::New: result = checkNew(static_cast<NewExpr&>(e), flow); break;
        case Expr::Kind::Delete: result = checkDelete(static_cast<DeleteExpr&>(e), flow); break;
        case Expr::Kind::Sizeof: {
            auto& z = static_cast<SizeofExpr&>(e);
            if (z.operand) {
                Flow unevaluated = flow;  // `sizeof x` does not read x
                unevaluated.reachable = false;
                z.target = checkExpr(*z.operand, unevaluated);
            }
            const Type* t = z.target;
            result = Type::longTy();
            if (t->isError()) break;
            if (t->is(K::Void) || t->isNull() || t->isOpaque() || t->is(K::Auto))
                diags_.error(e.loc, "sizeof needs a type with a size, but this is " + q(t),
                             t->isOpaque() ? q(t) + " is a C type known only by name, so only C knows its size" : "",
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
    {"ispunct", CharFn::IsPunct}, {"isxdigit", CharFn::IsXDigit}, {"toupper", CharFn::ToUpper},
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
                               : t->is(K::String) ? "this compiler cannot read a number from a string yet; C's `atof` "
                                                    "can be declared with extern \"C\" and called"
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
    if (call.vectorType) return checkVectorConstruction(call, flow);
    if (call.callee == "println" || call.callee == "print" || call.callee == "format") return checkPrint(call, flow);
    if (call.callee == "to_string") return checkToString(call, flow);
    if (call.callee == "string") return checkStringFill(call, flow);
    Loc nameLoc = call.loc;
    nameLoc.len = (uint32_t)call.callee.size();

    // Inside a method, `name(...)` calls another method on `this`, as in C++.
    if (StructDecl* o = owner()) {
        if (FuncDecl* m = o->findMethod(call.callee)) {
            call.method = m;
            requireAssigned(flow, thisVar(), "", &o->type, call.loc);
            if (!m->isConst && fn_->isConst)
                diags_.error(nameLoc, "const method " + q(fn_->name) + " cannot call " + q(m->name) + ", which may change fields",
                             "mark " + q(m->name) + " as `const` if it does not change any field", spec::Methods);
            checkArgs(call.args, *m, flow, call.loc, q(m->name));
            return m->retType;
        }
    }

    // `Point(1, 2)` builds a struct or class object.
    if (auto s = structs_.find(call.callee); s != structs_.end()) {
        const StructDecl& sd = *s->second;
        call.constructs = &sd;
        std::vector<ConstructArg> args;
        for (auto& a : call.args) args.push_back({"", a->loc, a.get(), nullptr});
        checkConstruction(sd, std::move(args), flow, call.loc);
        return &sd.type;
    }

    auto it = funcs_.find(call.callee);
    if (it == funcs_.end() && findMath(call.callee)) return checkMath(call, flow);
    if (it == funcs_.end() && findCharFn(call.callee)) return checkCharFn(call, flow);
    if (it == funcs_.end()) {
        for (auto& a : call.args) checkLooseValue(*a, flow);
        std::string help;
        const std::string& name = call.callee;
        std::string stem = name.size() > 3 ? name.substr(0, name.size() - 1) : "";
        if (name == "printf" || name == "puts" || name == "putchar")
            help = "C+ prints with println: `printf(\"%d\\n\", x)` becomes `println(\"{}\", x)`";
        else if (std::string reader = cNumberReader(name); !reader.empty())
            help = (name.starts_with("sto") ? "this compiler cannot read a number from a string yet. Until then, "
                                              "declare the C function: `extern \"C\" "
                                            : "a C function must be declared before it is called: `extern \"C\" ") +
                   reader + ";`";
        else if ((name.back() == 'f' || name.back() == 'l') && isMathFunction(stem))
            help = name.back() == 'f' ? "write `" + stem + "`, which returns a float when given floats, as in C++"
                                      : "C+ has no long double; write `" + stem + "`";
        else if (std::string guess = suggestName(name); !guess.empty())
            help = "did you mean " + q(guess) + "?";
        else
            help = "define it in this file, in any order, or declare a C function with `extern \"C\"`";
        diags_.error(nameLoc, "there is no function named " + q(call.callee), help, spec::Functions);
        return Type::error();
    }
    FuncDecl* fn = it->second;
    call.target = fn;
    checkArgs(call.args, *fn, flow, call.loc, q(fn->name));
    return fn->retType;
}

const Type* Sema::checkMethodCall(MethodCallExpr& call, Flow& flow) {
    const VarDecl* var = nullptr;
    std::string path;
    Expr& recv = *call.receiver;
    bool isPlace = isPlaceKind(recv);
    const Type* t = isPlace ? resolvePlace(recv, flow, var, path) : checkExpr(recv, flow);
    if (var && !t->isError()) requireAssigned(flow, var, path, t, recv.loc);
    if (t->isError()) {
        for (auto& a : call.args) checkLooseValue(*a, flow);
        return Type::error();
    }
    if (t->isOptional()) return checkOptionalMethod(call, flow, t);
    if (t->isPointer()) {
        for (auto& a : call.args) checkLooseValue(*a, flow);
        std::string n = spelled(recv, "p");
        diags_.error(call.loc, q(n) + " is a pointer, so the methods of what it points to are called with '->'",
                     "write `" + n + "->" + call.name + "(...)`", spec::Pointers);
        return Type::error();
    }
    if (t->isVector()) return checkVectorMethod(call, flow, t);
    if (t->is(K::String)) return checkStringMethod(call, flow);
    if (!t->hasFields()) {
        for (auto& a : call.args) checkLooseValue(*a, flow);
        diags_.error(call.loc, q(t) + " has no methods, so '." + call.name + "()' cannot be used", "", spec::Methods);
        return Type::error();
    }
    const StructDecl& sd = *t->decl;
    FuncDecl* m = sd.findMethod(call.name);
    if (!m) {
        for (auto& a : call.args) checkLooseValue(*a, flow);
        std::string help;
        if (sd.fieldIndex(call.name) >= 0) help = q(call.name) + " is a field; read it without parentheses";
        else {
            std::vector<std::string> names;
            for (auto& mm : sd.methods)
                if (mm->role == FuncDecl::Role::Method) names.push_back(mm->name);
            if (std::string guess = closest(call.name, names); !guess.empty()) help = "did you mean " + q(guess) + "?";
        }
        diags_.error(call.loc, q(sd.name) + " has no method named " + q(call.name), help, spec::Methods);
        return Type::error();
    }
    call.method = m;
    if (!canAccess(sd, m->access)) reportAccess(sd, m->name, true, call.loc);
    if (!m->isConst && t->isStruct() && inOptionalCopy(recv)) {
        diags_.error(call.loc, q(m->name) + " may change the struct, but '?.' works on a copy of it",
                     "unwrap the optional, change the copy and store it back: `if (auto v = opt) { v." + m->name +
                         "(...); opt = v; }`, or mark " + q(m->name) + " `const` if it does not change any field",
                     spec::Optionals);
    } else if (!m->isConst) {
        // A method that may change fields changes the struct value it is called on,
        // wherever that value is stored.
        const VarDecl* holder = nullptr;
        const Expr* root = nullptr;
        Storage storage = isPlace ? storageOf(recv, root) : Storage::Temporary;
        if (t->isStruct() && storage == Storage::Variable) holder = rootVariable(*root);
        else if (var && var == thisVar() && path.empty()) holder = var;  // `this` of a class
        if (t->isStruct() && storage == Storage::Pointer && followedPointer(*root).type->constElem)
            diags_.error(call.loc, "cannot call " + q(m->name) + " through a " + q(followedPointer(*root).type) +
                                       ", because it may change fields",
                         "mark " + q(m->name) + " as `const` if it does not change any field, or use a pointer that is "
                         "not const",
                         spec::Pointers);
        else if (holder && holder == thisVar() && fn_->isConst)
            diags_.error(call.loc, "const method " + q(fn_->name) + " cannot call " + q(m->name) + ", which may change fields",
                         "mark " + q(m->name) + " as `const` if it does not change any field", spec::Methods);
        else if (t->isStruct() && holder && holder->isConst)
            diags_.error(call.loc, "cannot call " + q(m->name) + " on const " + q(holder->name) + ", because it may change fields",
                         "mark " + q(m->name) + " as `const` if it does not change any field", spec::Methods);
    }
    checkArgs(call.args, *m, flow, call.loc, q(m->name));
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
    if (inOptionalCopy(recv)) {
        diags_.error(recv.loc, std::string(method) + "() changes the " + what + ", but '?.' works on a copy of it",
                     std::string("unwrap the optional, change the copy and store it back: `if (auto v = opt) { v.") +
                         method + "(...); opt = v; }`",
                     spec::Optionals);
        return;
    }
    if (!isPlaceKind(recv)) {
        diags_.error(recv.loc, std::string(method) + "() changes the " + what + ", so it must be called on a variable or a field",
                     std::string("this ") + what + " is a temporary copy, so changing it would have no effect", anchor);
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
    checkChangeable(recv, "changed", var, path);
}

const Type* Sema::checkVectorMethod(MethodCallExpr& call, Flow& flow, const Type* vecType) {
    const VectorMethodInfo* info = nullptr;
    for (auto& m : kVectorMethods)
        if (call.name == m.name) info = &m;
    if (!info) {
        for (auto& a : call.args) checkLooseValue(*a, flow);
        std::string help;
        if (call.name == "emplace_back") help = "write `push_back(value)`";
        else if (call.name == "at") help = "write `v[i]`; indexing is always checked in C+";
        else if (call.name == "length" || call.name == "count") help = "did you mean 'size'?";
        else if (call.name == "insert" || call.name == "erase" || call.name == "resize" || call.name == "reserve" ||
                 call.name == "begin" || call.name == "end") {
            diags_.error(call.loc, "this compiler does not support " + call.name + "() on vectors yet",
                         "the vector methods today are size, empty, push_back, pop_back, front, back and clear",
                         spec::Status);
            return Type::error();
        } else {
            std::vector<std::string> names;
            for (auto& m : kVectorMethods) names.push_back(m.name);
            std::string guess = closest(call.name, names);
            help = guess.empty() ? "the vector methods are size, empty, push_back, pop_back, front, back and clear"
                                 : "did you mean " + q(guess) + "?";
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

// Whether a value of type `t` can be made without being given one: what `vector<T>(n)`
// fills its elements with.
static bool hasDefault(const Type* t) {
    if (t->isStruct()) return !needsValue(t) && (!t->decl->constructor || t->decl->constructor->params.empty());
    return !t->isClass();
}

// `vector<T>()`, `vector<T>(n)` or `vector<T>(n, value)`.
const Type* Sema::checkVectorConstruction(CallExpr& call, Flow& flow) {
    const Type* vt = call.vectorType;
    std::string name = vt->name;
    if (call.args.size() > 2) {
        for (auto& a : call.args) checkLooseValue(*a, flow);
        diags_.error(call.loc, name + "(...) takes a size and an optional value, but " + std::to_string(call.args.size()) +
                                   " values were given",
                     "to list the elements, use braces: `" + name + "{1, 2, 3}`", spec::Vectors);
        return vt;
    }
    if (!call.args.empty()) checkValue(*call.args[0], flow, Type::longTy(), "as the size of " + q(vt));
    if (call.args.size() == 2) {
        checkValue(*call.args[1], flow, vt->elem, "as the element value of " + q(vt));
    } else if (call.args.size() == 1 && !hasDefault(vt->elem)) {
        const Type* elem = vt->elem;
        std::string why = elem->isClass() ? "a " + q(elem) + " reference cannot be empty"
                          : needsValue(elem) ? q(elem) + " holds a class reference that cannot be empty"
                                             : "the constructor of " + q(elem) + " needs arguments";
        diags_.error(call.loc, name + "(n) cannot make its elements on its own, because " + why,
                     "give the value to fill it with, as in `" + name + "(n, value)`, or start empty and use push_back",
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
            diags_.error(n.count->loc, "an array cannot have a negative number of elements", "", spec::NewDelete);
    }
    if (n.init) {
        checkValue(*n.init, flow, t, "for the value of `new " + name + "`");
    } else if (!hasDefault(t)) {
        std::string why = needsValue(t) ? q(t) + " holds a class reference that cannot be empty"
                                        : "the constructor of " + q(t) + " needs arguments";
        diags_.error(n.loc,
                     (n.count ? "`new " + name + "[n]` cannot make its elements" : "`new " + name + "` cannot make its value") +
                         " on its own, because " + why,
                     n.count ? "use a vector, which can start empty and grow with push_back: `vector<" + name + "> v;`"
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
                     t->isVector() || t->is(K::String) ? "a " + std::string(t->isVector() ? "vector" : "string") +
                                                             " frees its memory by itself"
                                                       : "",
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
                                 "a placeholder can give the precision of a floating-point value and f, e or g, such "
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
                     "use one {} for each value after the format string", spec::Printing);
        return result;
    }
    for (size_t i = 1; i < call.args.size(); ++i) {
        const Type* t = call.args[i]->type;
        const FormatSpec& spec = specs[i - 1];
        if (t->is(K::Void))
            diags_.error(call.args[i]->loc, "this expression produces no value, so it cannot be printed", "",
                         spec::Printing);
        else if (t->isPointer() && t->elem->is(K::Char) && !spec.type && spec.precision < 0) {
            std::string n = spelled(*call.args[i], "p");
            diags_.error(call.args[i]->loc,
                         "C++ prints a " + q(t) + " as the text it points to, but C+ cannot tell where that text ends",
                         "write `string(" + n + ")` to print the text, which must end with a zero byte, or `(void*)" + n +
                             "` to print the address",
                         spec::Printing);
        }
        else if ((spec.type || spec.precision >= 0) && !t->isFloating() && !t->isError())
            diags_.error(call.args[i]->loc, specTexts[i - 1] + " formats a float or a double, but this is " + q(t),
                         t->isInteger() ? "write {} for an integer, or convert it first: `double(n)`"
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
                     t->is(K::String) ? "it is already a string"
                     : t->is(K::Char) ? "in C++, to_string(c) gives the code of the char, such as \"97\". Write "
                                        "`string(1, c)` for the char itself, or `to_string(int(c))` for its code"
                                      : textHelp(t),
                     spec::Strings);
    return Type::stringTy();
}

// `int(x)`, `static_cast<int>(x)` and `(int)x` convert between the number types and bool,
// as in C++. A conversion that cannot keep the value stops the program (see Codegen).
const Type* Sema::checkCast(CastExpr& c, Flow& flow) {
    const Type* from = checkExpr(*c.operand, flow);
    const Type* to = c.target;
    checkStorable(to, c.loc);
    if (to->isPointer() || (from->isPointer() && !to->is(K::Bool) && !to->is(K::String)))
        return checkPointerCast(c, from, to);
    std::string cast = std::string(to->name) + "(x)";
    bool castable = to->isNumber() || to->is(K::Char) || to->is(K::Bool);
    if (from->isError()) return castable ? to : Type::error();
    if (to->is(K::String) && from->is(K::String)) return to;  // `string("text")`, as in C++: a copy
    if (to->is(K::String) && from->isPointer()) {  // `string(p)` copies the text a `char*` points to
        if (!from->elem->is(K::Char))
            diags_.error(c.operand->loc, "string(p) copies the text a char* points to, but this is " + q(from), "",
                         spec::Pointers);
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
                     to->isOptional() && canConvert(from, to) ? "no cast is needed: a value converts to an optional "
                                                                "by itself"
                                                              : "a cast can only convert to int, long, float, double, "
                                                                "char or bool",
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
                     : to->isFloating()   ? "until then, C's `atof` or `strtod` can be declared with extern \"C\" "
                                            "and called"
                                          : "until then, C's `atoi` or `strtol` can be declared with extern \"C\" "
                                            "and called",
                     spec::Status);
    else
        diags_.error(c.operand->loc, "'" + cast + "' converts a number, a char or a bool, but this is " + q(from),
                     from->isOptional() ? unwrapHelp(spelled(*c.operand))
                                        : "only a number, a char or a bool can be cast; convert one of its fields "
                                          "or elements instead, e.g. `" + std::string(to->name) + "(v[0])`",
                     spec::Casts);
    return to;
}

// A cast to a pointer type, or from a pointer to anything but bool and string.
const Type* Sema::checkPointerCast(CastExpr& c, const Type* from, const Type* to) {
    if (from->isError()) return to;
    if (!to->isPointer()) {
        diags_.error(c.loc, "a cast cannot turn a pointer into " + q(to),
                     "for a bool, compare it with null: `p != null`. The distance between two pointers into the same "
                     "memory is `p - q`",
                     spec::Casts);
        return to;
    }
    if (from->isNull()) return to;
    if (!from->isPointer()) {
        diags_.error(c.operand->loc, "a cast to " + q(to) + " converts another pointer, but this is " + q(from),
                     from->isInteger()       ? "C+ does not turn numbers into pointers; a pointer to nothing is `null`"
                     : isPlaceKind(*c.operand) ? "write `&" + spelled(*c.operand, "x") + "` for its address"
                                               : "",
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
    "the string methods today are size, length, empty, substr, find, contains, starts_with, ends_with, clear, "
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
            diags_.error(call.loc, notYet, kStringMethodList, spec::Status);
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
                                          q(t) + " holds " + q(t->elem),
                         "", spec::ImplicitConversions);
        return;
    }
    if (declared->is(K::Auto)) v.type = t;
    else requireConvertible(*v.init, declared, "to initialize " + q(v.name));
    if (!usableAsCondition(v.type))
        diags_.error(v.init->loc, std::string("the variable declared in the ") + stmt + " condition must be an "
                                      "optional, a bool or a number, but " + q(v.name) + " is " + q(v.type),
                     v.type->isOptional() ? boolOptionalHelp(v.name)
                                          : "an optional runs the branch when it holds a value, and a bool or a "
                                            "number when it is true or not zero",
                     spec::Optionals);
}

// `base?.rest`: the rest runs only when `base` holds a value, and its result becomes an
// optional, unless it already is one or is void.
const Type* Sema::checkOptChain(OptChainExpr& c, Flow& flow) {
    const Type* base = checkExpr(*c.base, flow);
    const Type* inner = base;
    if (base->isOptional()) {
        inner = base->elem;
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
    const Type* body = checkExpr(*c.body, bodyFlow);
    if (base->isError() || body->isError()) return Type::error();
    if (!base->isOptional() || body->is(K::Void) || body->isOptional()) return body;
    return Type::optionalOf(body);
}

// `opt ?? fallback`. With a fallback that cannot be null, the result cannot be either.
const Type* Sema::checkCoalesce(CoalesceExpr& c, Flow& flow) {
    const Type* l = checkExpr(*c.lhs, flow);
    Flow rhsFlow = flow;  // the fallback may not run
    if (l->isError() || !l->isOptional()) {
        checkLooseValue(*c.rhs, rhsFlow);
        if (l->isError()) return Type::error();
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
    if (r->isError()) return inner;
    if (r == l || r->isNull()) return l;  // the fallback may itself be null
    if (canConvert(r, inner)) return inner;
    if (r->isInteger() && inner->isInteger()) return commonInteger(r, inner);
    diags_.error(c.rhs->loc, "the fallback after '\?\?' must be " + q(inner) + ", but this is " + q(r),
                 "both sides of '\?\?' hold the same type of value", spec::Optionals);
    return inner;
}

// The methods of std::optional: has_value(), value() and value_or(fallback). Anything
// else is a method of the value inside, which needs `?.` or unwrapping.
const Type* Sema::checkOptionalMethod(MethodCallExpr& call, Flow& flow, const Type* opt) {
    const Type* inner = opt->elem;
    std::string n = spelled(*call.receiver);
    size_t want = 0;
    if (call.name == "has_value") call.optMethod = OptMethod::HasValue;
    else if (call.name == "value") call.optMethod = OptMethod::Value;
    else if (call.name == "value_or") call.optMethod = OptMethod::ValueOr, want = 1;
    else {
        for (auto& a : call.args) checkLooseValue(*a, flow);
        diags_.error(call.loc, q(opt) + " may be null, so " + q(call.name) + " cannot be called on it directly",
                     "write `" + n + "?." + call.name + "(...)`, which does nothing when " + n +
                         " is null, or unwrap it: `if (auto v = " + n + ") v." + call.name + "(...);`",
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
