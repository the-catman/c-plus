// What each name in a program means, for hover and go to definition in `cplus lsp`. It walks what Sema has checked, which
// knows the declaration behind each name. A generic is checked once for each set of arguments, and only those copies have
// meanings, so a generic that is never used has none, and one that is used shows its first use.

#include "Index.h"

#include <set>
#include <utility>

namespace cplus {

namespace {

class Indexer {
public:
    explicit Indexer(const SourceFile& file) : file_(file) {}

    std::vector<Reference> refs;

    void program(const Program& p) {
        for (auto& s : p.structs)
            if (s) record(*s);
        for (auto& c : p.constants) var(c.get());
        for (auto& f : p.funcs)
            if (f) func(*f);
        for (auto& t : p.templates) {  // what could not be made from a generic is null
            for (auto& [name, s] : t->records)
                if (s) record(*s);
            for (auto& [name, f] : t->functions)
                if (f) func(*f);
        }
    }

private:
    const SourceFile& file_;
    std::set<std::pair<uint32_t, uint32_t>> places_;
    std::set<const void*> walked_;

    void add(Loc at, std::string what, Loc declared) {
        if (at.line == 0 || what.empty() || !places_.insert({at.line, at.col}).second) return;
        refs.push_back({at, std::move(what), declared});
    }

    // ---- What hovering shows ------------------------------------------------

    static std::string code(const std::string& text) { return "```cplus\n" + text + "\n```"; }

    // The `//` lines just above a declaration, which say what it is for, when it is the first on its line: the comment above
    // `struct P { int x; };` is about P, and the one above `int f(int a)` about f.
    std::string commentAbove(Loc declared) const {
        std::string_view before = file_.line(declared.line).substr(0, declared.col ? declared.col - 1 : 0);
        if (before.find_first_of("{;(") != std::string_view::npos) return "";
        std::vector<std::string> lines;
        for (uint32_t n = declared.line; n > 1; --n) {
            std::string_view line = file_.line(n - 1);
            const size_t start = line.find_first_not_of(" \t");
            if (start == std::string_view::npos || line.compare(start, 2, "//") != 0) break;
            std::string_view text = line.substr(start + 2);
            if (!text.empty() && text[0] == ' ') text.remove_prefix(1);
            lines.emplace_back(text);
        }
        std::string out;
        for (auto it = lines.rbegin(); it != lines.rend(); ++it) out += (out.empty() ? "" : "\n") + *it;
        return out;
    }

    std::string with(std::string text, Loc declared, const std::string& note = "") const {
        std::string out = code(text);
        if (!note.empty()) out += "\n\n" + note;
        if (declared.line) {
            std::string comment = commentAbove(declared);
            if (!comment.empty()) out += "\n\n" + comment;
        }
        return out;
    }

    static const Type* typeOf(const VarDecl& v) { return v.type ? v.type : v.declType; }

    std::string describe(const VarDecl& v) const {
        const Type* t = typeOf(v);
        if (!t) return "";
        return with(std::string(v.isConstexpr ? "constexpr " : v.isConst ? "const " : "") + t->name + (v.isRef ? "& " : " ") + v.name, v.loc);
    }

    std::string describe(const FuncDecl& f) const {
        if (f.name.rfind("__", 0) == 0) return "";  // a part of the language's own code, which a program cannot name
        std::string params;
        for (auto& p : f.params) {
            const Type* t = typeOf(*p);
            params += (params.empty() ? "" : ", ") + std::string(p->isRef && p->isConst ? "const " : "") + (t ? t->name : "?") + (p->isRef ? "& " : " ") + p->name;
        }
        std::string owner = f.owner ? std::string(f.owner->type.name) : "";
        std::string text;
        if (f.isExtern) text += "extern \"C\" ";
        if (f.isAsync) text += "async ";
        if (f.isVirtual && !f.isOverride) text += "virtual ";
        if (f.isConstexpr) text += "constexpr ";
        if (f.role == FuncDecl::Role::Constructor) text += owner + "(" + params + ")";
        else if (f.role == FuncDecl::Role::Destructor) text += "~" + owner + "()";
        else text += std::string(f.bodyRet() ? f.bodyRet()->name : "void") + " " + (owner.empty() ? "" : owner + "::") + f.name + "(" + params + ")";
        if (f.isConst) text += " const";
        if (f.isOverride) text += " override";
        if (f.isThrows) text += " throws";
        return with(text, f.nameLoc);
    }

    std::string describe(const StructDecl& s) const {
        std::string text = std::string(s.keyword()) + " " + s.type.name;
        for (size_t i = 0; i < s.baseNames.size(); ++i) text += (i == 0 ? " : public " : ", public ") + s.baseNames[i].name;
        return with(text, s.nameLoc);
    }

    std::string describe(const FieldDecl& f, const StructDecl& owner) const {
        return with(std::string(f.type ? f.type->name : "?") + " " + f.name, f.loc, std::string("A field of `") + owner.type.name + "`.");
    }

    // ---- Declarations -------------------------------------------------------

    void record(const StructDecl& s) {
        if (!walked_.insert(&s).second) return;
        add(s.nameLoc, describe(s), s.nameLoc);
        for (size_t i = 0; i < s.baseNames.size() && i < s.bases.size(); ++i)
            if (s.bases[i]) add(s.baseNames[i].loc, describe(*s.bases[i]), s.bases[i]->nameLoc);
        for (auto& f : s.fields) {
            add(f.loc, describe(f, s), f.loc);
            typeRef(f.typeLoc, f.type, false);
            expr(f.defaultValue.get());
        }
        for (auto& m : s.methods)
            if (m) func(*m);
    }

    void func(const FuncDecl& f) {
        if (!walked_.insert(&f).second) return;
        add(f.nameLoc, describe(f), f.nameLoc);
        typeRef(f.retTypeLoc, f.bodyRet(), false);
        for (auto& p : f.params) var(p.get());
        for (auto& init : f.memberInits) {
            if (f.owner && init.fieldIndex >= 0 && (size_t)init.fieldIndex < f.owner->fields.size())
                add(init.loc, describe(f.owner->fields[init.fieldIndex], *f.owner), f.owner->fields[init.fieldIndex].loc);
            expr(init.value.get());
        }
        for (auto& init : f.baseInits) {
            if (f.owner)
                for (const StructDecl* base : f.owner->bases)
                    if (base && (base->type.name == init.name || base->name == init.name)) add(init.loc, describe(*base), base->nameLoc);
            for (auto& a : init.args) expr(a.get());
        }
        if (f.body) stmt(f.body.get());
    }

    void var(const VarDecl* v) {
        if (!v) return;
        add(v->loc, describe(*v), v->loc);
        typeRef(v->typeLoc, typeOf(*v), v->declType && v->declType->is(Type::Kind::Auto));
        expr(v->init.get());
    }

    // A type as it is written: what it names, or for `auto`, what it turned out to be.
    void typeRef(Loc at, const Type* t, bool writtenAuto) {
        if (at.line == 0 || !t) return;
        const StructDecl* named = nullptr;
        for (const Type* part = t; part && !named; part = part->elem)
            if (part->decl && part->decl->nameLoc.line) named = part->decl;
        if (writtenAuto) add(at, code(t->name), named ? named->nameLoc : Loc{});
        else if (named) add(at, describe(*named), named->nameLoc);
    }

    // ---- Statements and expressions -----------------------------------------

    void stmt(const Stmt* s) {
        if (!s) return;
        switch (s->kind) {
        case Stmt::Kind::VarDecl: var(static_cast<const VarDecl*>(s)); break;
        case Stmt::Kind::DeclGroup:
            for (auto& d : static_cast<const DeclGroupStmt*>(s)->decls) var(d.get());
            break;
        case Stmt::Kind::Expr: expr(static_cast<const ExprStmt*>(s)->expr.get()); break;
        case Stmt::Kind::Block:
            for (auto& inner : static_cast<const BlockStmt*>(s)->stmts) stmt(inner.get());
            break;
        case Stmt::Kind::If: {
            auto* i = static_cast<const IfStmt*>(s);
            if (i->decl) var(i->decl->var.get());
            expr(i->cond.get());
            stmt(i->thenStmt.get());
            stmt(i->elseStmt.get());
            break;
        }
        case Stmt::Kind::While: {
            auto* w = static_cast<const WhileStmt*>(s);
            if (w->decl) var(w->decl->var.get());
            expr(w->cond.get());
            stmt(w->body.get());
            break;
        }
        case Stmt::Kind::DoWhile: {
            auto* d = static_cast<const DoWhileStmt*>(s);
            stmt(d->body.get());
            expr(d->cond.get());
            break;
        }
        case Stmt::Kind::For: {
            auto* f = static_cast<const ForStmt*>(s);
            stmt(f->init.get());
            expr(f->cond.get());
            for (auto& step : f->steps) expr(step.get());
            stmt(f->body.get());
            break;
        }
        case Stmt::Kind::ForEach: {
            auto* f = static_cast<const ForEachStmt*>(s);
            var(f->var.get());
            expr(f->range.get());
            stmt(f->body.get());
            break;
        }
        case Stmt::Kind::Return: expr(static_cast<const ReturnStmt*>(s)->value.get()); break;
        case Stmt::Kind::Try: {
            auto* t = static_cast<const TryStmt*>(s);
            stmt(t->body.get());
            for (auto& c : t->catches) {
                typeRef(c.typeLoc, c.type, false);
                var(c.var.get());
                stmt(c.body.get());
            }
            break;
        }
        case Stmt::Kind::Throw: expr(static_cast<const ThrowStmt*>(s)->value.get()); break;
        case Stmt::Kind::Break:
        case Stmt::Kind::Continue:
        case Stmt::Kind::Error: break;
        }
    }

    void expr(const Expr* e) {
        if (!e) return;
        switch (e->kind) {
        case Expr::Kind::Name: {
            auto* n = static_cast<const NameExpr*>(e);
            if (n->decl) {
                add(n->loc, describe(*n->decl), n->decl->loc);
            } else if (n->implicitField >= 0 && !n->path.empty()) {
                const StructDecl* owner = n->path.back();
                if ((size_t)n->implicitField < owner->fields.size())
                    add(n->loc, describe(owner->fields[n->implicitField], *owner), owner->fields[n->implicitField].loc);
            }
            break;
        }
        case Expr::Kind::Call: {
            auto* c = static_cast<const CallExpr*>(e);
            if (const FuncDecl* f = c->target ? c->target : c->method) add(c->loc, describe(*f), f->nameLoc);
            else if (c->constructs) add(c->loc, describe(*c->constructs), c->constructs->nameLoc);
            for (auto& a : c->args) expr(a.get());
            break;
        }
        case Expr::Kind::MethodCall: {
            auto* m = static_cast<const MethodCallExpr*>(e);
            if (m->method) add(m->loc, describe(*m->method), m->method->nameLoc);
            if (m->lowered) expr(m->lowered.get());
            expr(m->receiver.get());
            for (auto& a : m->args) expr(a.get());
            break;
        }
        case Expr::Kind::Member: {
            auto* m = static_cast<const MemberExpr*>(e);
            if (m->fieldOwner && m->fieldIndex >= 0 && (size_t)m->fieldIndex < m->fieldOwner->fields.size()) {
                const FieldDecl& f = m->fieldOwner->fields[m->fieldIndex];
                add(m->loc, describe(f, *m->fieldOwner), f.loc);
            }
            expr(m->base.get());
            break;
        }
        case Expr::Kind::Unary: expr(static_cast<const UnaryExpr*>(e)->operand.get()); break;
        case Expr::Kind::Binary: {
            auto* b = static_cast<const BinaryExpr*>(e);
            expr(b->lhs.get());
            expr(b->rhs.get());
            break;
        }
        case Expr::Kind::Assign: {
            auto* a = static_cast<const AssignExpr*>(e);
            expr(a->target.get());
            expr(a->value.get());
            break;
        }
        case Expr::Kind::Ternary: {
            auto* t = static_cast<const TernaryExpr*>(e);
            expr(t->cond.get());
            expr(t->thenExpr.get());
            expr(t->elseExpr.get());
            break;
        }
        case Expr::Kind::IncDec: expr(static_cast<const IncDecExpr*>(e)->target.get()); break;
        case Expr::Kind::Index: {
            auto* i = static_cast<const IndexExpr*>(e);
            expr(i->base.get());
            expr(i->index.get());
            break;
        }
        case Expr::Kind::InitList: {
            auto* l = static_cast<const InitListExpr*>(e);
            const Type* t = l->explicitType ? l->explicitType : l->type;
            if (t && t->isOptional()) t = t->elem;
            for (auto& el : l->elements) {
                if (!el.designator.empty() && t && t->hasFields() && t->decl) {
                    int i = t->decl->fieldIndex(el.designator);
                    if (i >= 0) add(el.loc, describe(t->decl->fields[i], *t->decl), t->decl->fields[i].loc);
                }
                expr(el.value.get());
            }
            break;
        }
        case Expr::Kind::OptChain: {
            auto* o = static_cast<const OptChainExpr*>(e);
            expr(o->base.get());
            expr(o->body.get());
            break;
        }
        case Expr::Kind::Coalesce: {
            auto* c = static_cast<const CoalesceExpr*>(e);
            expr(c->lhs.get());
            expr(c->rhs.get());
            break;
        }
        case Expr::Kind::Cast: expr(static_cast<const CastExpr*>(e)->operand.get()); break;
        case Expr::Kind::Sizeof: expr(static_cast<const SizeofExpr*>(e)->operand.get()); break;
        case Expr::Kind::New: {
            auto* n = static_cast<const NewExpr*>(e);
            typeRef(n->typeLoc, n->target, false);
            expr(n->count.get());
            expr(n->init.get());
            break;
        }
        case Expr::Kind::Delete: expr(static_cast<const DeleteExpr*>(e)->operand.get()); break;
        case Expr::Kind::Try: expr(static_cast<const TryExpr*>(e)->operand.get()); break;
        case Expr::Kind::Borrow: expr(static_cast<const BorrowExpr*>(e)->operand.get()); break;
        case Expr::Kind::Spawn: expr(static_cast<const SpawnExpr*>(e)->call.get()); break;
        case Expr::Kind::Await: expr(static_cast<const AwaitExpr*>(e)->task.get()); break;
        case Expr::Kind::IntLit:
        case Expr::Kind::FloatLit:
        case Expr::Kind::CharLit:
        case Expr::Kind::BoolLit:
        case Expr::Kind::StringLit:
        case Expr::Kind::NullLit:
        case Expr::Kind::This:
        case Expr::Kind::OptValue:
        case Expr::Kind::Constraint: break;
        }
    }
};

}  // namespace

std::vector<Reference> indexProgram(const Program& program, const SourceFile& file) {
    Indexer indexer(file);
    indexer.program(program);
    return std::move(indexer.refs);
}

}  // namespace cplus
