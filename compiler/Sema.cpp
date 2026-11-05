#include "Sema.h"

#include <algorithm>
#include <set>
#include <functional>

#include "SpecRefs.h"

namespace cplus {

using K = Type::Kind;

static bool canConvert(const Type* from, const Type* to) {
    if (from->isError() || to->isError()) return true;
    if (from == to) return true;
    if (from->is(K::Int) && to->is(K::Long)) return true;  // widening never loses information
    return from->isInteger() && to->is(K::Bool);           // as in C++: zero is false, anything else true
}

static const Type* commonInteger(const Type* a, const Type* b) {
    return (a->is(K::Long) || b->is(K::Long)) ? Type::longTy() : Type::intTy();
}

static std::string q(const std::string& s) { return "'" + s + "'"; }
static std::string q(const Type* t) { return q(t->name); }
static std::string join(const std::string& path, const std::string& field) {
    return path.empty() ? field : path + "." + field;
}
static std::string placeName(const VarDecl* v, const std::string& path) {
    return path.empty() ? v->name : v->name + "." + path;
}
static std::string zeroOf(const Type* t) {
    if (t->is(K::Bool)) return "false";
    if (t->is(K::String)) return "\"\"";
    if (t->isStruct()) return "{}";
    if (t->isClass()) return std::string(t->name) + "(...)";
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

static std::string cycleHelp(const Type* fieldType, const StructDecl& sd) {
    std::set<const StructDecl*> seen;
    if (!needsExisting(fieldType, &sd, seen)) return "";
    return ". A " + q(sd.name) + " can only be built from an existing one, so none can be built yet; references "
           "that can be empty (`" + std::string(fieldType->name) + "?`) are planned for this";
}

static std::string signature(const FuncDecl& fn) {
    std::string sig = (fn.owner ? fn.owner->name + "." : "") + fn.name + "(";
    for (size_t i = 0; i < fn.params.size(); ++i)
        sig += (i ? ", " : "") + std::string(fn.params[i]->declType->name) + " " + fn.params[i]->name;
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

// Expressions that name storage: a variable, `this`, a field, or an element.
static bool isPlaceKind(const Expr& e) {
    return e.kind == Expr::Kind::Name || e.kind == Expr::Kind::Member || e.kind == Expr::Kind::This ||
           e.kind == Expr::Kind::Index;
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
// value and fields their defaults, as in C++. Numbers and bools stay unassigned.
void Sema::markDefaults(Flow& flow, const VarDecl* var, const Type* type, const std::string& path) {
    if (tooDeep(path)) return;
    if (type->is(K::String) || type->isVector()) {
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
                     cycleHelp(missingType, *owner()),
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
    std::vector<std::string> candidates{"println", "print"};
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
    else if (n.name == "nullptr" || n.name == "NULL") help = "C+ uses optionals (`T?` and null) instead";
    else if (defaultsOf_ && defaultsOf_->fieldIndex(n.name) >= 0)
        help = "a field's default cannot read other fields; set this field in a constructor instead";
    else if (std::string guess = suggestName(n.name); !guess.empty()) help = "did you mean " + q(guess) + "?";
    diags_.error(n.loc, q(n.name) + " is not declared", help, spec::Variables);
}

// ---- Program, structs and functions ----------------------------------------

void Sema::check(Program& program) {
    for (auto& imp : program.imports)
        diags_.error(imp.loc, "this compiler does not support modules yet, so '" + imp.path + "' cannot be imported",
                     "remove the import for now; built-ins like println need no import", spec::Status);

    for (auto& sd : program.structs)
        if (sd->defined) structs_[sd->name] = sd.get();

    for (auto& fn : program.funcs) {
        if (fn->name == "println" || fn->name == "print") {
            diags_.error(fn->nameLoc, q(fn->name) + " is a built-in function and cannot be redefined",
                         "give your function a different name", spec::Printing);
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

void Sema::checkStructs(Program& program) {
    // A struct cannot contain itself, directly or through other structs.
    std::unordered_map<const StructDecl*, int> state;  // 1 = being visited, 2 = done
    std::function<void(const StructDecl*)> visit = [&](const StructDecl* sd) {
        state[sd] = 1;
        for (auto& f : sd->fields) {
            if (!f.type->isStruct()) continue;
            const StructDecl* inner = f.type->decl;
            if (state[inner] == 1)
                diags_.error(f.loc,
                             "struct " + q(sd->name) + " contains itself through field " + q(f.name) +
                                 ", so it would be infinitely large",
                             "a struct can hold other structs, but not itself. Classes and pointers, which are "
                             "planned, will allow linked structures",
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

void Sema::checkFunction(FuncDecl& fn) {
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
    for (auto& p : fn.params) {
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
    if (v.init) {
        if (v.declType->is(K::Auto)) {
            const Type* t = checkExpr(*v.init, flow);
            if (t->is(K::Void))
                diags_.error(v.init->loc, "cannot store the result of a void function in " + q(v.name), "",
                             spec::Variables);
            v.type = t->is(K::Void) ? Type::error() : t;
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

// As in C++, a number used as a condition is true when it is not zero.
static bool usableAsCondition(const Type* t) { return t->isError() || t->is(K::Bool) || t->isInteger(); }

void Sema::checkCondition(Expr& e, Flow& flow, const char* what) {
    const Type* t = checkExpr(e, flow);
    if (usableAsCondition(t)) return;
    std::string help;
    if (t->is(K::Void)) help = "this expression produces no value";
    else if (t->isVector())
        help = "to check whether it has elements, write `!" +
               (e.kind == Expr::Kind::Name ? static_cast<NameExpr&>(e).name : std::string("items")) + ".empty()`";
    diags_.error(e.loc, std::string("the ") + what + " must be a bool or a number, but this is " + q(t), help,
                 spec::Conditions);
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
            checkCondition(*st.cond, flow, "if condition");
            Flow thenFlow = flow;
            checkLoopBody(*st.thenStmt, thenFlow);  // a branch gets its own scope
            Flow elseFlow = flow;
            if (st.elseStmt) checkLoopBody(*st.elseStmt, elseFlow);
            flow = merge(thenFlow, elseFlow);
            return;
        }
        case Stmt::Kind::While: {
            auto& st = static_cast<WhileStmt&>(s);
            checkCondition(*st.cond, flow, "while condition");
            loops_.emplace_back();
            Flow body = flow;
            checkLoopBody(*st.body, body);
            Flow after = flow;
            if (isConstantTrue(st.cond.get())) after.reachable = false;  // only a break leaves `while (true)`
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
    } else if (!rangeType->isError()) {
        diags_.error(st.range->loc, "a range-based for loop goes over a vector, but this is " + q(rangeType),
                     rangeType->is(K::String) ? "looping over the characters of a string is planned"
                                              : "write the vector to loop over after the ':'",
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

void Sema::requireConvertible(const Expr& e, const Type* to, const std::string& context) {
    const Type* from = e.type;
    if (canConvert(from, to)) return;
    std::string help;
    if (from->is(K::Long) && to->is(K::Int)) {
        help = e.kind == Expr::Kind::IntLit ? "this number is larger than an int can hold; use long instead"
                                            : "a long may not fit in an int. Use long here; explicit casts are not "
                                              "supported yet";
    } else if (from->is(K::Void)) {
        help = "this expression produces no value";
    } else if (from->is(K::Bool) && to->isInteger()) {
        help = "a bool does not convert to a number; write `flag ? 1 : 0`";
    }
    diags_.error(e.loc, "expected " + q(to) + " " + context + ", but this is " + q(from), help,
                 spec::ImplicitConversions);
}

void Sema::checkValue(Expr& e, Flow& flow, const Type* expected, const std::string& context) {
    if (e.kind == Expr::Kind::InitList && !static_cast<InitListExpr&>(e).explicitType) {
        checkInitList(static_cast<InitListExpr&>(e), flow, expected);
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
                     "give it a value here, or give the field a default in " + q(sd.name) + cycleHelp(f.type, sd),
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
        if (!baseType->isVector()) {
            diags_.error(e.loc, "'[ ]' picks an element of a vector, but this is " + q(baseType),
                         baseType->is(K::String) ? "string operations are planned" : "", spec::Vectors);
            return e.type;
        }
        if (!indexType->isError() && !indexType->isInteger())
            diags_.error(ix.index->loc, "a vector index must be an int or long, but this is " + q(indexType), "",
                         spec::Vectors);
        else if (ix.index->kind == Expr::Kind::IntLit && static_cast<IntLitExpr&>(*ix.index).value < 0)
            diags_.error(ix.index->loc, "a vector index cannot be negative",
                         "elements are numbered from 0; the last one is `v[v.size() - 1]` or `v.back()`",
                         spec::Vectors);
        return e.type = baseType->elem;
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
    if (!baseType->hasFields()) {
        std::string help = baseType->is(K::String) ? "string operations are planned" : "";
        if (baseType->isVector())
            help = m.field == "size" || m.field == "empty" || m.field == "front" || m.field == "back"
                       ? q(m.field) + " is a method; call it with `" + m.field + "()`"
                       : "vectors have methods like size(), push_back(x) and back(), and elements are read with `v[i]`";
        diags_.error(e.loc, q(baseType) + " has no fields, so '." + m.field + "' cannot be used", help,
                     baseType->isVector() ? spec::Vectors : spec::Structs);
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
// object reached through a reference, or in a temporary such as a function's result.
enum class Storage { Variable, Object, Temporary };

// Follows fields and elements from the place `e` back to what holds them. For a
// variable, `root` is set to the Name or This expression it starts from.
static Storage storageOf(const Expr& e, const Expr*& root) {
    switch (e.kind) {
        case Expr::Kind::Name:
        case Expr::Kind::This: root = &e; return Storage::Variable;
        case Expr::Kind::Member: {
            const Expr& base = *static_cast<const MemberExpr&>(e).base;
            if (base.type && base.type->isClass() && base.kind != Expr::Kind::This) return Storage::Object;
            return storageOf(base, root);
        }
        case Expr::Kind::Index: return storageOf(*static_cast<const IndexExpr&>(e).base, root);
        default: return Storage::Temporary;
    }
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
        case Storage::Variable: break;
    }
    const VarDecl* rootVar = rootVariable(*root);
    if (!rootVar) return false;  // an unknown name, already reported
    bool element = containsIndex(target);
    if (rootVar == thisVar() && fn_->isConst) {
        std::string what = element ? "the elements of " + q(outerVectorName(target)) : q(var ? path : rootVar->name);
        diags_.error(target.loc, what + " cannot be " + action + " inside const method " + q(fn_->name),
                     "remove `const` after the parameter list of " + q(fn_->name) + " if it needs to change fields",
                     spec::Methods);
        return false;
    }
    if (rootVar->isConst) {
        std::string what = element ? "its elements" : "its fields";
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

// Checks that `target` can be changed. `var` and `path` are set to the variable and
// field path to track for definite assignment; `var` stays null for a place inside a
// class object or a vector, which always holds a value.
bool Sema::checkAssignable(Expr& target, Flow& flow, const char* action, const VarDecl*& var, std::string& path) {
    var = nullptr;
    if (target.kind != Expr::Kind::Name && target.kind != Expr::Kind::Member && target.kind != Expr::Kind::Index) {
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
        case Expr::Kind::BoolLit: result = Type::boolTy(); break;
        case Expr::Kind::StringLit: result = Type::stringTy(); break;
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
            const Type* t = checkExpr(*u.operand, flow);
            if (t->isError()) break;
            if (u.op == UnOp::Not) {
                if (!usableAsCondition(t))
                    diags_.error(u.operand->loc, "'!' needs a bool or a number, but this is " + q(t), "",
                                 spec::Conditions);
                result = Type::boolTy();
            } else {
                const char* sym = u.op == UnOp::Neg ? "-" : u.op == UnOp::Plus ? "+" : "~";
                if (!t->isInteger()) {
                    diags_.error(u.operand->loc, std::string("'") + sym + "' needs an int or long, but this is " + q(t),
                                 "", spec::Operators);
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
                        diags_.error(side->loc, std::string("both sides of '") + sym + "' must be a bool or a number, but this is " + q(t),
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
            if (equality && l == r && !l->isInteger()) {
                b.operandType = l;
                result = Type::boolTy();
                break;
            }
            if (!l->isInteger() || !r->isInteger()) {
                const Type* bad = !l->isInteger() ? l : r;
                const Expr* side = !l->isInteger() ? b.lhs.get() : b.rhs.get();
                std::string help;
                if (bad->is(K::String) && b.op == BinOp::Add) help = "joining strings is not supported yet; print the parts with println(\"{}{}\", a, b)";
                else if (equality) help = "both sides of '" + std::string(sym) + "' must have the same type, but they are " + q(l) + " and " + q(r);
                else if (comparison && bad->is(K::String)) help = "strings can be compared with == and != for now; ordering them is planned";
                else if (comparison && (bad->hasFields() || bad->isVector())) help = q(bad) + " values can only be compared with == and !=";
                else if (bad->is(K::Bool) && (b.op == BinOp::BitAnd || b.op == BinOp::BitOr)) help = std::string("use '") + (b.op == BinOp::BitAnd ? "&&" : "||") + "' for logic";
                diags_.error(side->loc, std::string("'") + sym + "' cannot be used with " + q(bad), help, spec::Operators);
                if (comparison) result = Type::boolTy();
                break;
            }
            if (b.op == BinOp::Shl || b.op == BinOp::Shr) {
                b.operandType = l;  // a shift keeps the type of its left side
                result = l;
                checkShiftAmount(l, b.lhs.get(), *b.rhs);
            } else {
                b.operandType = commonInteger(l, r);
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
                                                                      : "this";
            if (a.op) {
                if (ok && var) requireAssigned(flow, var, path, t, a.target->loc);  // `x += v` reads x first
                checkLooseValue(*a.value, flow);
                std::string opName = std::string("'") + binOpSpelling(*a.op) + "='";
                if (!t->isError() && !t->isInteger())
                    diags_.error(a.target->loc, opName + " needs an int or long, but this is " + q(t), "", spec::Operators);
                else if (!a.value->type->isError() && !a.value->type->isInteger())
                    diags_.error(a.value->loc, opName + " needs an int or long, but this is " + q(a.value->type), "",
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
            else if (x->isInteger() && y->isInteger()) result = commonInteger(x, y);
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
                if (!t->isInteger())
                    diags_.error(i.target->loc, std::string("'") + sym + "' needs an int or long, but this is " + q(t),
                                 "", spec::Operators);
            }
            result = t;
            break;
        }
        case Expr::Kind::Call: result = checkCall(static_cast<CallExpr&>(e), flow); break;
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
    for (size_t i = 0; i < args.size(); ++i)
        checkValue(*args[i], flow, fn.params[i]->declType, "for parameter " + q(fn.params[i]->name) + " of " + q(fn.name));
}

const Type* Sema::checkCall(CallExpr& call, Flow& flow) {
    if (call.vectorType) return checkVectorConstruction(call, flow);
    if (call.callee == "println" || call.callee == "print") return checkPrint(call, flow);
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
    if (it == funcs_.end()) {
        for (auto& a : call.args) checkLooseValue(*a, flow);
        std::string help;
        if (call.callee == "printf" || call.callee == "puts" || call.callee == "putchar")
            help = "C+ prints with println: `printf(\"%d\\n\", x)` becomes `println(\"{}\", x)`";
        else if (std::string guess = suggestName(call.callee); !guess.empty())
            help = "did you mean " + q(guess) + "?";
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
    if (t->isVector()) return checkVectorMethod(call, flow, t);
    if (!t->hasFields()) {
        for (auto& a : call.args) checkLooseValue(*a, flow);
        diags_.error(call.loc, q(t) + " has no methods, so '." + call.name + "()' cannot be used",
                     t->is(K::String) ? "string operations are planned" : "", spec::Methods);
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
    if (!m->isConst) {
        // A method that may change fields changes the struct value it is called on,
        // wherever that value is stored.
        const VarDecl* holder = nullptr;
        const Expr* root = nullptr;
        if (t->isStruct() && isPlace && storageOf(recv, root) == Storage::Variable) holder = rootVariable(*root);
        else if (var && var == thisVar() && path.empty()) holder = var;  // `this` of a class
        if (holder && holder == thisVar() && fn_->isConst)
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
    if (info->changes) {
        const Expr& recv = *call.receiver;
        if (!isPlaceKind(recv)) {
            diags_.error(recv.loc, std::string(info->name) + "() changes the vector, so it must be called on a variable "
                                                             "or a field",
                         "this vector is a temporary copy, so changing it would have no effect", spec::Vectors);
        } else {
            const VarDecl* var = nullptr;
            std::string path;
            const Expr* root = nullptr;
            if (storageOf(recv, root) == Storage::Variable && root && !containsIndex(recv)) {
                var = rootVariable(*root);
                if (recv.kind == Expr::Kind::Member || (recv.kind == Expr::Kind::Name &&
                                                        static_cast<const NameExpr&>(recv).implicitField >= 0))
                    path = outerVectorName(recv);
            }
            checkChangeable(recv, "changed", var, path);
        }
    }
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

const Type* Sema::checkPrint(CallExpr& call, Flow& flow) {
    call.builtin = call.callee == "println" ? Builtin::Println : Builtin::Print;
    for (auto& a : call.args) checkExpr(*a, flow);
    if (call.args.empty()) {
        if (call.builtin == Builtin::Print)
            diags_.error(call.loc, "print needs a format string", "write e.g. `print(\"{}\", value);`", spec::Printing);
        call.formatPieces = {""};
        return Type::voidTy();
    }
    Expr& fmtExpr = *call.args[0];
    if (fmtExpr.kind != Expr::Kind::StringLit) {
        diags_.error(fmtExpr.loc, "the first argument of " + call.callee + " must be a format string written in quotes",
                     "to print a value, write `" + call.callee + "(\"{}\", value);`", spec::Printing);
        return Type::voidTy();
    }
    const std::string& fmt = static_cast<StringLitExpr&>(fmtExpr).value;
    std::vector<std::string> pieces{""};
    for (size_t i = 0; i < fmt.size(); ++i) {
        char c = fmt[i];
        char n = i + 1 < fmt.size() ? fmt[i + 1] : '\0';
        if (c == '{' && n == '{') { pieces.back() += '{'; ++i; }
        else if (c == '}' && n == '}') { pieces.back() += '}'; ++i; }
        else if (c == '{' && n == '}') { pieces.emplace_back(); ++i; }
        else if (c == '{') {
            diags_.error(fmtExpr.loc, "only {} placeholders are supported in format strings so far",
                         "write {{ for a literal '{'; format options like {:x} are not supported yet", spec::Printing);
            return Type::voidTy();
        } else if (c == '}') {
            diags_.error(fmtExpr.loc, "this format string has a '}' with no matching '{'",
                         "write }} for a literal '}'", spec::Printing);
            return Type::voidTy();
        } else pieces.back() += c;
    }
    size_t placeholders = pieces.size() - 1, values = call.args.size() - 1;
    if (placeholders != values) {
        diags_.error(call.loc, "the format string has " + std::to_string(placeholders) + " {} placeholder" +
                                   (placeholders == 1 ? "" : "s") + " but " + std::to_string(values) + " value" +
                                   (values == 1 ? " was" : "s were") + " given",
                     "use one {} for each value after the format string", spec::Printing);
        return Type::voidTy();
    }
    for (size_t i = 1; i < call.args.size(); ++i) {
        const Type* t = call.args[i]->type;
        if (t->is(K::Void))
            diags_.error(call.args[i]->loc, "this expression produces no value, so it cannot be printed", "",
                         spec::Printing);
    }
    call.formatPieces = std::move(pieces);
    return Type::voidTy();
}

}  // namespace cplus
