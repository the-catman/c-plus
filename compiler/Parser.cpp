#include "Parser.h"

#include "SpecRefs.h"

namespace cplus {

Parser::Parser(std::vector<Token> tokens, Diagnostics& diags) : toks_(std::move(tokens)), diags_(diags) {}

// ---- Token helpers ---------------------------------------------------------

const Token& Parser::peek(size_t ahead) const { return toks_[std::min(pos_ + ahead, toks_.size() - 1)]; }
const Token& Parser::prev() const { return toks_[pos_ ? pos_ - 1 : 0]; }

const Token& Parser::advance() {
    const Token& t = toks_[pos_];
    if (pos_ + 1 < toks_.size()) ++pos_;
    return t;
}

bool Parser::match(Tok k) {
    if (!check(k)) return false;
    advance();
    return true;
}

Loc Parser::afterPrev() const {
    Loc l = prev().loc;
    l.col += l.len;
    l.len = 1;
    return l;
}

void Parser::fail(Loc loc, const std::string& message, const std::string& help, const std::string& spec) {
    diags_.error(loc, message, help, spec);
    throw ParseError{};
}

void Parser::notYet(Loc loc, const std::string& feature, const std::string& help) {
    fail(loc, "this compiler does not support " + feature + " yet", help, spec::Status);
}

const Token& Parser::expect(Tok k, const std::string& context) {
    if (check(k)) return advance();
    std::string message = std::string("expected '") + tokSpelling(k) + "' " + context + ", found " +
                          describeToken(peek());
    // A missing ';' or ')' is best shown right after the previous token.
    bool pointAfter = (k == Tok::Semi || k == Tok::RParen) && pos_ > 0 && peek().loc.line != prev().loc.line;
    fail(pointAfter ? afterPrev() : peek().loc, message);
}

void Parser::syncTopLevel() {
    // Skip to the end of the current declaration: a ';' or a closing '}' at depth 0.
    int depth = 0;
    while (!check(Tok::Eof)) {
        Tok k = advance().kind;
        if (k == Tok::LBrace) ++depth;
        else if (k == Tok::RBrace && --depth <= 0) return;
        else if (k == Tok::Semi && depth == 0) return;
    }
}

void Parser::syncStatement() {
    int depth = 0;
    while (!check(Tok::Eof)) {
        if (check(Tok::RBrace) && depth == 0) return;  // let the enclosing block close
        Tok k = advance().kind;
        if (k == Tok::LBrace) ++depth;
        else if (k == Tok::RBrace && --depth == 0) return;
        else if (k == Tok::Semi && depth == 0) return;
    }
}

// ---- Declarations ----------------------------------------------------------

Program Parser::parseProgram() {
    Program prog;
    prog_ = &prog;
    for (size_t i = 0; i + 1 < toks_.size(); ++i) {
        bool isRecord = toks_[i].kind == Tok::KwStruct || toks_[i].kind == Tok::KwClass;
        if (isRecord && toks_[i + 1].kind == Tok::Ident && typeNames_.insert(toks_[i + 1].text).second)
            prog.structs.push_back(std::make_unique<StructDecl>(toks_[i + 1].text, toks_[i].kind == Tok::KwClass));
    }
    while (!check(Tok::Eof)) {
        try {
            parseTopLevel(prog);
        } catch (ParseError&) {
            syncTopLevel();
        }
    }
    return prog;
}

void Parser::parseTopLevel(Program& prog) {
    const Token& t = peek();
    switch (t.kind) {
        case Tok::KwImport: {
            Loc loc = advance().loc;
            std::string path = expect(Tok::Ident, "after 'import'").text;
            while (match(Tok::Dot)) path += "." + expect(Tok::Ident, "after '.' in the module name").text;
            if (check(Tok::Less) || check(Tok::StringLit))
                fail(peek().loc, "module names are written without quotes or angle brackets",
                     "write `import http;` or `import net.server;`", spec::Modules);
            expect(Tok::Semi, "after the import");
            loc.len = prev().loc.col + prev().loc.len - loc.col;
            prog.imports.push_back(ImportDecl{path, loc});
            return;
        }
        case Tok::KwStruct:
        case Tok::KwClass: parseRecord(); return;
        case Tok::KwTemplate: notYet(t.loc, "templates (generics)");
        case Tok::KwEnum: notYet(t.loc, "enums");
        case Tok::KwInterface: notYet(t.loc, "interfaces");
        case Tok::KwExtern: notYet(t.loc, "extern declarations for C libraries");
        case Tok::KwNamespace:
            fail(t.loc, "C+ has no namespaces; each file is a module", "remove the namespace block", spec::Modules);
        case Tok::Ident:
            if (peek(1).kind == Tok::ColonColon && isTypeName(t))
                fail(t.loc, "methods and constructors are defined inside their " +
                                std::string(prog_->findStruct(t.text)->keyword()),
                     "move this definition into the body of '" + t.text + "'; C+ has no headers, so there is no "
                     "separate declaration to match",
                     spec::Classes);
            if (t.text == "using")
                fail(t.loc, "C+ has no 'using' declarations",
                     "built-ins like println, string and vector need no std:: prefix, so `using namespace std;` "
                     "can simply be removed",
                     spec::Modules);
            break;
        default: break;
    }
    if (!isTypeStart()) {
        checkUnknownType();
        fail(t.loc, "expected a function or an import, found " + describeToken(t),
             "top-level code looks like `int main() { ... }` or `import http;`", spec::Functions);
    }
    if (check(Tok::KwConst)) notYet(t.loc, "global constants");
    Loc retLoc;
    const Type* ret = parseType(retLoc);
    if (ret->is(Type::Kind::Auto)) notYet(retLoc, "'auto' return types", "write the return type, e.g. `int`");
    if (check(Tok::Ident) && peek(1).kind == Tok::ColonColon && isTypeName(peek()))
        fail(peek().loc, "methods are defined inside their " + std::string(prog_->findStruct(peek().text)->keyword()),
             "move this definition into the body of '" + peek().text + "'; C+ has no headers, so there is no "
             "separate declaration to match",
             spec::Classes);
    if (check(Tok::Ident) && peek(1).kind != Tok::LParen) notYet(peek().loc, "global variables");
    prog.funcs.push_back(parseFunction(ret, retLoc));
}

bool Parser::isTypeStart(size_t ahead) const {
    switch (peek(ahead).kind) {
        case Tok::Ident:  // a struct name followed by a variable name: `Point p`
            return isTypeName(peek(ahead)) && peek(ahead + 1).kind == Tok::Ident;
        case Tok::KwInt: case Tok::KwLong: case Tok::KwBool: case Tok::KwString:
        case Tok::KwVoid: case Tok::KwAuto: case Tok::KwConst:
        case Tok::KwDouble: case Tok::KwFloat: case Tok::KwChar:
            return true;
        default: return false;
    }
}

// Reports `Foo x` where Foo is not a known type (two identifiers in a row).
void Parser::checkUnknownType() {
    if (!(check(Tok::Ident) && peek(1).kind == Tok::Ident)) return;
    const Token& t = peek();
    std::string help;
    if (t.text == "size_t" || t.text == "int32_t" || t.text == "int64_t" || t.text == "uint32_t" ||
        t.text == "uint64_t" || t.text == "unsigned")
        help = "use int (32-bit) or long (64-bit); both are signed and the same size on every platform";
    else
        help = "the types available today are int, long, bool, string, void and structs you define";
    fail(t.loc, "unknown type '" + t.text + "'", help, spec::IntegerTypes);
}

const Type* Parser::parseType(Loc& loc) {
    const Token& t = advance();
    loc = t.loc;
    switch (t.kind) {
        case Tok::KwInt: return Type::intTy();
        case Tok::KwLong:
            if (check(Tok::KwLong) || check(Tok::KwInt))
                fail(t.loc, check(Tok::KwLong) ? "'long long' is not a C+ type" : "'long int' is not a C+ type",
                     "write just `long`; it is always 64 bits", spec::IntegerTypes);
            return Type::longTy();
        case Tok::KwBool: return Type::boolTy();
        case Tok::KwString: return Type::stringTy();
        case Tok::KwVoid: return Type::voidTy();
        case Tok::KwAuto: return Type::autoTy();
        case Tok::KwDouble: case Tok::KwFloat: notYet(t.loc, "floating-point types", "use int or long for now");
        case Tok::KwChar: notYet(t.loc, "char types", "use string for text");
        case Tok::Ident:
            if (isTypeName(t)) return &prog_->findStruct(t.text)->type;
            fail(t.loc, "unknown type '" + t.text + "'", "", spec::IntegerTypes);
        default: fail(t.loc, "expected a type, found " + describeToken(t), "", spec::IntegerTypes);
    }
}

void Parser::parseParams(FuncDecl& fn) {
    expect(Tok::LParen, "after the function name");
    if (check(Tok::KwVoid) && peek(1).kind == Tok::RParen) advance();  // C-style f(void)
    if (!check(Tok::RParen)) {
        do {
            if (check(Tok::KwConst)) advance();  // const parameters are accepted; they are copies anyway
            if (!isTypeStart()) {
                checkUnknownType();
                fail(peek().loc, "expected a parameter type, found " + describeToken(peek()),
                     "parameters look like `int count`", spec::Functions);
            }
            Loc typeLoc;
            const Type* ty = parseType(typeLoc);
            if (ty->is(Type::Kind::Auto)) notYet(typeLoc, "'auto' parameters", "write the parameter type");
            if (check(Tok::Amp) || check(Tok::Star))
                notYet(peek().loc, std::string(check(Tok::Amp) ? "reference" : "pointer") + " parameters",
                       "pass the value itself: structs are copied and class objects are shared automatically");
            const Token& pname = expect(Tok::Ident, "for the parameter name");
            auto param = std::make_unique<VarDecl>(pname.loc, pname.text, ty, typeLoc);
            param->isParam = true;
            if (check(Tok::Assign)) notYet(peek().loc, "default argument values");
            fn.params.push_back(std::move(param));
        } while (match(Tok::Comma));
    }
    expect(Tok::RParen, "to close the parameter list");
}

std::unique_ptr<FuncDecl> Parser::parseFunction(const Type* retType, Loc retLoc) {
    auto fn = std::make_unique<FuncDecl>();
    fn->retType = retType;
    fn->retTypeLoc = retLoc;
    const Token& name = expect(Tok::Ident, "for the function name after the return type");
    fn->name = name.text;
    fn->nameLoc = name.loc;
    parseParams(*fn);
    if (check(Tok::Semi))
        fail(peek().loc, "C+ has no forward declarations",
             "remove this line; functions can be called before or after they are defined", spec::Functions);
    if (!check(Tok::LBrace)) expect(Tok::LBrace, "to start the function body");
    fn->body = parseBlock();
    return fn;
}

// ---- Structs and classes --------------------------------------------------

void Parser::parseRecord() {
    bool isClass = advance().kind == Tok::KwClass;
    const char* kw = isClass ? "class" : "struct";
    const Token& name = expect(Tok::Ident, std::string("for the ") + kw + " name");
    if (check(Tok::Semi))
        fail(peek().loc, "C+ has no forward declarations",
             std::string("remove this line; a ") + kw + " can be used before or after its definition",
             spec::Structs);
    if (check(Tok::Colon)) notYet(peek().loc, "inheritance");
    StructDecl* sd = prog_->findStruct(name.text);  // created by the scan in parseProgram
    std::unique_ptr<StructDecl> duplicate;
    if (sd->defined || sd->isClass != isClass) {
        diags_.error(name.loc, "the name '" + name.text + "' is already used by another struct or class",
                     "rename one of them", spec::Structs);
        if (sd->defined) diags_.note(sd->nameLoc, "the first definition is here");
        duplicate = std::make_unique<StructDecl>(name.text, isClass);  // parse into a scratch copy to keep going
        sd = duplicate.get();
    }
    sd->nameLoc = name.loc;
    expect(Tok::LBrace, "to start the body of " + std::string(kw) + " '" + name.text + "'");
    Access access = isClass ? Access::Private : Access::Public;  // as in C++
    while (!check(Tok::RBrace) && !check(Tok::Eof)) {
        try {
            parseMember(*sd, access);
        } catch (ParseError&) {
            syncStatement();
        }
    }
    expect(Tok::RBrace, "to close " + std::string(kw) + " '" + name.text + "'");
    sd->defined = true;
    if (!match(Tok::Semi))  // report without skipping ahead, so the next declaration still parses
        diags_.error(afterPrev(), "expected ';' after the definition of " + std::string(kw) + " '" + name.text +
                                      "', found " + describeToken(peek()),
                     std::string("a ") + kw + " definition ends with `};`, as in C++", spec::Structs);
}

void Parser::parseMemberInits(FuncDecl& ctor) {
    do {
        const Token& field = expect(Tok::Ident, "for the field name in the initializer list");
        FuncDecl::MemberInit init{field.text, field.loc, nullptr, -1};
        if (match(Tok::LBrace)) {
            init.value = parseInitList(field.loc, nullptr);  // `pos{1, 2}` takes the field's type
        } else {
            expect(Tok::LParen, "after '" + field.text + "' in the initializer list");
            init.value = parseExpr();
            if (check(Tok::Comma))
                fail(peek().loc, "a field is initialized from one value",
                     "to build a struct field from several values, write `" + field.text + "{a, b}`",
                     spec::Classes);
            expect(Tok::RParen, "to close the initializer of '" + field.text + "'");
        }
        ctor.memberInits.push_back(std::move(init));
    } while (match(Tok::Comma));
}

void Parser::parseMember(StructDecl& sd, Access& access) {
    const Token& t = peek();
    switch (t.kind) {
        case Tok::KwPublic: case Tok::KwPrivate: case Tok::KwProtected:
            advance();
            expect(Tok::Colon, "after '" + std::string(tokSpelling(t.kind)) + "'");
            access = t.kind == Tok::KwPublic ? Access::Public : t.kind == Tok::KwPrivate ? Access::Private : Access::Protected;
            return;
        case Tok::KwStruct: case Tok::KwClass:
            notYet(t.loc, "nested type definitions", "define it at the top level instead");
        case Tok::KwStatic: notYet(t.loc, "static members");
        case Tok::KwVirtual: notYet(t.loc, "virtual functions", "they arrive together with inheritance");
        case Tok::KwOperator: notYet(t.loc, "operator overloading");
        case Tok::KwTemplate: notYet(t.loc, "templates (generics)");
        case Tok::KwConst: notYet(t.loc, "const fields");
        default: break;
    }

    // Destructor: ~Name() { ... }
    if (t.kind == Tok::Tilde) {
        Loc tildeLoc = advance().loc;
        const Token& name = expect(Tok::Ident, "after '~'");
        if (name.text != sd.name)
            fail(name.loc, "a destructor is named after its type: '~" + sd.name + "'", "", spec::Classes);
        if (!sd.isClass)
            notYet(tildeLoc, "struct destructors", "destructors work for classes; a struct is a plain value");
        auto dtor = std::make_unique<FuncDecl>();
        dtor->name = "~" + sd.name;
        dtor->nameLoc = tildeLoc;
        dtor->retType = Type::voidTy();
        dtor->role = FuncDecl::Role::Destructor;
        dtor->owner = &sd;
        dtor->access = Access::Public;
        expect(Tok::LParen, "after the destructor name");
        expect(Tok::RParen, "a destructor takes no parameters");
        dtor->body = parseBlock();
        if (sd.destructor) {
            diags_.error(tildeLoc, "'" + sd.name + "' already has a destructor", "", spec::Classes);
            return;
        }
        sd.destructor = dtor.get();
        sd.methods.push_back(std::move(dtor));
        return;
    }

    // Constructor: Name(params) : field(value), ... { ... }
    if (t.kind == Tok::Ident && t.text == sd.name && peek(1).kind == Tok::LParen) {
        auto ctor = std::make_unique<FuncDecl>();
        ctor->name = sd.name;
        ctor->nameLoc = advance().loc;
        ctor->retType = Type::voidTy();
        ctor->role = FuncDecl::Role::Constructor;
        ctor->owner = &sd;
        ctor->access = access;
        parseParams(*ctor);
        if (match(Tok::Colon)) parseMemberInits(*ctor);
        if (check(Tok::Semi))
            fail(peek().loc, "write the constructor body here", "C+ has no separate definitions; add `{ ... }`",
                 spec::Classes);
        ctor->body = parseBlock();
        if (sd.constructor) {
            diags_.error(ctor->nameLoc, "'" + sd.name + "' already has a constructor",
                         "several constructors need overloading, which is planned; for now give the "
                         "parameters' fields defaults and use one constructor",
                         spec::Classes);
            return;
        }
        sd.constructor = ctor.get();
        sd.methods.push_back(std::move(ctor));
        return;
    }

    if (!isTypeStart()) {
        checkUnknownType();
        fail(t.loc, "expected a field or method, found " + describeToken(t),
             "fields look like `int x = 0;` and methods like `int area() { ... }`", spec::Structs);
    }
    Loc typeLoc;
    const Type* ty = parseType(typeLoc);
    if (ty->is(Type::Kind::Auto))
        fail(typeLoc, "fields and methods need a written type", "auto is only for local variables", spec::Structs);
    if (check(Tok::KwOperator)) notYet(peek().loc, "operator overloading");

    // Method: type name(params) [const] { ... }
    if (check(Tok::Ident) && peek(1).kind == Tok::LParen) {
        auto method = std::make_unique<FuncDecl>();
        method->retType = ty;
        method->retTypeLoc = typeLoc;
        const Token& name = advance();
        method->name = name.text;
        method->nameLoc = name.loc;
        method->role = FuncDecl::Role::Method;
        method->owner = &sd;
        method->access = access;
        parseParams(*method);
        method->isConst = match(Tok::KwConst);
        if (check(Tok::KwOverride)) notYet(peek().loc, "override", "it arrives together with inheritance");
        if (check(Tok::Semi))
            fail(peek().loc, "write the method body here",
                 "C+ has no separate definitions; replace ';' with `{ ... }`", spec::Classes);
        method->body = parseBlock();
        sd.methods.push_back(std::move(method));
        return;
    }

    if (ty->is(Type::Kind::Void))
        fail(typeLoc, "a field cannot have type void", "", spec::Structs);
    do {
        if (check(Tok::Star) || check(Tok::Amp))
            notYet(peek().loc, std::string(check(Tok::Star) ? "pointer" : "reference") + " types");
        const Token& name = expect(Tok::Ident, "for the field name");
        FieldDecl field{name.text, name.loc, ty, typeLoc, nullptr, access};
        if (match(Tok::Assign)) field.defaultValue = parseAssign();
        else if (check(Tok::LBrace))
            notYet(peek().loc, "brace initializers for fields",
                   "write `" + std::string(ty->name) + " " + name.text + " = value;`");
        if (check(Tok::LBracket)) notYet(peek().loc, "arrays");
        int existing = sd.fieldIndex(name.text);
        if (existing >= 0) {
            diags_.error(name.loc, std::string(sd.keyword()) + " '" + sd.name + "' already has a field named '" +
                                       name.text + "'",
                         "rename one of them", spec::Structs);
            diags_.note(sd.fields[existing].loc, "the first one is here");
        } else {
            sd.fields.push_back(std::move(field));
        }
    } while (match(Tok::Comma));
    expect(Tok::Semi, "after the field declaration");
}

// ---- Statements ------------------------------------------------------------

std::unique_ptr<BlockStmt> Parser::parseBlock() {
    auto block = std::make_unique<BlockStmt>(expect(Tok::LBrace, "to start a block").loc);
    while (!check(Tok::RBrace) && !check(Tok::Eof)) {
        Loc start = peek().loc;
        try {
            block->stmts.push_back(parseStmt());
        } catch (ParseError&) {
            syncStatement();
            block->stmts.push_back(std::make_unique<ErrorStmt>(start));
        }
    }
    block->endLoc = peek().loc;
    expect(Tok::RBrace, "to close the block");
    return block;
}

StmtPtr Parser::parseDeclaration() {
    bool isConst = match(Tok::KwConst);
    Loc typeLoc;
    const Type* ty = parseType(typeLoc);
    if (ty->is(Type::Kind::Void))
        fail(typeLoc, "variables cannot have type void", "pick a type such as int, or use auto", spec::Variables);
    std::vector<std::unique_ptr<VarDecl>> decls;
    do {
        if (check(Tok::Star) || check(Tok::Amp))
            notYet(peek().loc, std::string(check(Tok::Star) ? "pointer" : "reference") + " types");
        const Token& name =
            expect(Tok::Ident, decls.empty() ? "for the variable name" : "for the next variable name after ','");
        auto var = std::make_unique<VarDecl>(name.loc, name.text, ty, typeLoc);
        var->isConst = isConst;
        if (match(Tok::Assign)) {
            var->init = parseAssign();
        } else if (check(Tok::LBrace) || check(Tok::LParen)) {
            notYet(peek().loc, "brace and constructor initializers",
                   "write `" + std::string(ty->name) + " " + name.text + " = value;`");
        }
        if (check(Tok::LBracket)) notYet(peek().loc, "arrays");
        if (check(Tok::Colon)) notYet(peek().loc, "range-based for loops", "they arrive together with arrays");
        decls.push_back(std::move(var));
    } while (match(Tok::Comma));
    if (decls.size() == 1) return std::move(decls[0]);
    auto group = std::make_unique<DeclGroupStmt>(decls[0]->loc);
    group->decls = std::move(decls);
    return group;
}

StmtPtr Parser::parseStmt() {
    const Token& t = peek();
    switch (t.kind) {
        case Tok::LBrace: return parseBlock();
        case Tok::Semi: advance(); return std::make_unique<BlockStmt>(t.loc);
        case Tok::KwIf: {
            advance();
            expect(Tok::LParen, "after 'if'");
            ExprPtr cond = parseExpr();
            expect(Tok::RParen, "after the if condition");
            StmtPtr thenS = parseStmt();
            StmtPtr elseS = match(Tok::KwElse) ? parseStmt() : nullptr;
            return std::make_unique<IfStmt>(t.loc, std::move(cond), std::move(thenS), std::move(elseS));
        }
        case Tok::KwWhile: {
            advance();
            expect(Tok::LParen, "after 'while'");
            ExprPtr cond = parseExpr();
            expect(Tok::RParen, "after the while condition");
            return std::make_unique<WhileStmt>(t.loc, std::move(cond), parseStmt());
        }
        case Tok::KwDo: {
            advance();
            StmtPtr body = parseStmt();
            expect(Tok::KwWhile, "after the body of a do loop");
            expect(Tok::LParen, "after 'while'");
            ExprPtr cond = parseExpr();
            expect(Tok::RParen, "after the while condition");
            expect(Tok::Semi, "after a do-while loop");
            return std::make_unique<DoWhileStmt>(t.loc, std::move(body), std::move(cond));
        }
        case Tok::KwFor: return parseFor();
        case Tok::KwReturn: {
            advance();
            ExprPtr value = check(Tok::Semi) ? nullptr : parseExpr();
            expect(Tok::Semi, "after the return statement");
            return std::make_unique<ReturnStmt>(t.loc, std::move(value));
        }
        case Tok::KwBreak: advance(); expect(Tok::Semi, "after 'break'"); return std::make_unique<BreakStmt>(t.loc);
        case Tok::KwContinue:
            advance();
            expect(Tok::Semi, "after 'continue'");
            return std::make_unique<ContinueStmt>(t.loc);
        case Tok::KwSwitch: notYet(t.loc, "switch statements", "use if / else if for now");
        case Tok::KwTry: case Tok::KwThrow: notYet(t.loc, "errors (try, throw, catch)");
        case Tok::KwDelete: notYet(t.loc, "manual memory (new and delete)");
        default: break;
    }
    if (isTypeStart()) {
        StmtPtr decl = parseDeclaration();
        expect(Tok::Semi, "after the variable declaration");
        return decl;
    }
    checkUnknownType();
    ExprPtr e = parseExpr();
    Loc loc = e->loc;
    expect(Tok::Semi, "after the expression");
    return std::make_unique<ExprStmt>(loc, std::move(e));
}

StmtPtr Parser::parseFor() {
    auto loop = std::make_unique<ForStmt>(advance().loc);
    expect(Tok::LParen, "after 'for'");
    if (match(Tok::Semi)) {
        // no init
    } else if (isTypeStart()) {
        loop->init = parseDeclaration();
        expect(Tok::Semi, "after the loop's first clause");
    } else {
        ExprPtr e = parseExpr();
        Loc l = e->loc;
        loop->init = std::make_unique<ExprStmt>(l, std::move(e));
        expect(Tok::Semi, "after the loop's first clause");
    }
    if (!check(Tok::Semi)) loop->cond = parseExpr();
    expect(Tok::Semi, "after the loop condition");
    if (!check(Tok::RParen)) {
        do loop->steps.push_back(parseExpr());  // `i++, j--`
        while (match(Tok::Comma));
    }
    expect(Tok::RParen, "to close the for header");
    loop->body = parseStmt();
    return loop;
}

// ---- Expressions -----------------------------------------------------------

ExprPtr Parser::parseExpr() { return parseAssign(); }

static std::optional<std::optional<BinOp>> assignOp(Tok k) {
    switch (k) {
        case Tok::Assign: return std::optional<BinOp>{};
        case Tok::PlusAssign: return BinOp::Add;
        case Tok::MinusAssign: return BinOp::Sub;
        case Tok::StarAssign: return BinOp::Mul;
        case Tok::SlashAssign: return BinOp::Div;
        case Tok::PercentAssign: return BinOp::Rem;
        case Tok::AmpAssign: return BinOp::BitAnd;
        case Tok::PipeAssign: return BinOp::BitOr;
        case Tok::CaretAssign: return BinOp::BitXor;
        case Tok::ShlAssign: return BinOp::Shl;
        case Tok::ShrAssign: return BinOp::Shr;
        default: return std::nullopt;
    }
}

ExprPtr Parser::parseAssign() {
    ExprPtr lhs = parseTernary();
    if (auto op = assignOp(peek().kind)) {
        Loc opLoc = advance().loc;
        ExprPtr rhs = parseAssign();
        return std::make_unique<AssignExpr>(opLoc, *op, std::move(lhs), std::move(rhs));
    }
    return lhs;
}

ExprPtr Parser::parseTernary() {
    ExprPtr cond = parseBinary(1);
    if (!check(Tok::Question)) return cond;
    Loc loc = advance().loc;
    ExprPtr a = parseExpr();
    expect(Tok::Colon, "between the two branches of ?:");
    ExprPtr b = parseAssign();
    return std::make_unique<TernaryExpr>(loc, std::move(cond), std::move(a), std::move(b));
}

static int precedence(Tok k, BinOp& op) {
    switch (k) {
        case Tok::PipePipe: op = BinOp::LogOr; return 1;
        case Tok::AmpAmp: op = BinOp::LogAnd; return 2;
        case Tok::Pipe: op = BinOp::BitOr; return 3;
        case Tok::Caret: op = BinOp::BitXor; return 4;
        case Tok::Amp: op = BinOp::BitAnd; return 5;
        case Tok::EqEq: op = BinOp::Eq; return 6;
        case Tok::NotEq: op = BinOp::Ne; return 6;
        case Tok::Less: op = BinOp::Lt; return 7;
        case Tok::LessEq: op = BinOp::Le; return 7;
        case Tok::Greater: op = BinOp::Gt; return 7;
        case Tok::GreaterEq: op = BinOp::Ge; return 7;
        case Tok::Shl: op = BinOp::Shl; return 8;
        case Tok::Shr: op = BinOp::Shr; return 8;
        case Tok::Plus: op = BinOp::Add; return 9;
        case Tok::Minus: op = BinOp::Sub; return 9;
        case Tok::Star: op = BinOp::Mul; return 10;
        case Tok::Slash: op = BinOp::Div; return 10;
        case Tok::Percent: op = BinOp::Rem; return 10;
        default: return 0;
    }
}

ExprPtr Parser::parseBinary(int minPrec) {
    ExprPtr lhs = parseUnary();
    while (true) {
        BinOp op;
        int prec = precedence(peek().kind, op);
        if (prec == 0 || prec < minPrec) return lhs;
        if (check(Tok::QuestionQuestion)) notYet(peek().loc, "optionals");
        Loc opLoc = advance().loc;
        ExprPtr rhs = parseBinary(prec + 1);
        lhs = std::make_unique<BinaryExpr>(opLoc, op, std::move(lhs), std::move(rhs));
    }
}

ExprPtr Parser::parseUnary() {
    const Token& t = peek();
    switch (t.kind) {
        case Tok::Minus: {
            advance();
            // Fold "-<literal>" so that -2147483648 is an int and -9223372036854775808 a long.
            if (check(Tok::IntLit)) {
                const Token& lit = advance();
                Loc loc = t.loc;
                loc.len = lit.loc.col + lit.loc.len - t.loc.col;
                if (lit.intValue > (uint64_t)INT64_MAX + 1)
                    fail(loc, "the number -" + lit.text + " is too small for any integer type",
                         "the smallest long is -9223372036854775808", spec::IntegerLiterals);
                int64_t v = lit.intValue == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)lit.intValue;
                return parsePostfix(std::make_unique<IntLitExpr>(loc, v, lit.longSuffix));
            }
            return std::make_unique<UnaryExpr>(t.loc, UnOp::Neg, parseUnary());
        }
        case Tok::Plus: advance(); return std::make_unique<UnaryExpr>(t.loc, UnOp::Plus, parseUnary());
        case Tok::Bang: advance(); return std::make_unique<UnaryExpr>(t.loc, UnOp::Not, parseUnary());
        case Tok::Tilde: advance(); return std::make_unique<UnaryExpr>(t.loc, UnOp::BitNot, parseUnary());
        case Tok::PlusPlus:
        case Tok::MinusMinus: {
            advance();
            return std::make_unique<IncDecExpr>(t.loc, t.kind == Tok::PlusPlus, true, parseUnary());
        }
        case Tok::Star: case Tok::Amp: notYet(t.loc, "pointers");
        case Tok::KwNew:
            if (isTypeName(peek(1))) {
                if (prog_->findStruct(peek(1).text)->isClass)
                    fail(t.loc, "class objects are created without 'new'",
                         "write `" + peek(1).text + "(...)`; the object is reference-counted and freed automatically",
                         spec::Classes);
                fail(t.loc, "struct values are created without 'new'",
                     "write `" + peek(1).text + "(...)` or `" + peek(1).text + "{...}`; a struct is a plain value",
                     spec::Structs);
            }
            notYet(t.loc, "manual memory with 'new'");
        case Tok::KwSizeof: notYet(t.loc, "sizeof expressions");
        case Tok::KwAwait: notYet(t.loc, "async and await");
        case Tok::KwSpawn: notYet(t.loc, "threads (spawn)");
        default: return parsePostfix(parsePrimary());
    }
}

ExprPtr Parser::parsePostfix(ExprPtr e) {
    while (true) {
        const Token& t = peek();
        if (t.kind == Tok::PlusPlus || t.kind == Tok::MinusMinus) {
            advance();
            e = std::make_unique<IncDecExpr>(t.loc, t.kind == Tok::PlusPlus, false, std::move(e));
        } else if (t.kind == Tok::LParen) {
            fail(t.loc, "only functions can be called, by writing their name followed by (...)", "",
                 spec::Functions);
        } else if (t.kind == Tok::Dot) {
            advance();
            const Token& name = expect(Tok::Ident, "for the field or method name after '.'");
            if (match(Tok::LParen)) {
                std::vector<ExprPtr> args;
                if (!check(Tok::RParen)) {
                    do args.push_back(parseExpr());
                    while (match(Tok::Comma));
                }
                expect(Tok::RParen, "to close the argument list of '" + name.text + "'");
                e = std::make_unique<MethodCallExpr>(name.loc, std::move(e), name.text, std::move(args));
            } else {
                e = std::make_unique<MemberExpr>(name.loc, std::move(e), name.text);
            }
        } else if (t.kind == Tok::Arrow) {
            if (e->kind == Expr::Kind::This)
                fail(t.loc, "'this' is a reference in C+, not a pointer",
                     "write `this." + (peek(1).kind == Tok::Ident ? peek(1).text : std::string("field")) + "`",
                     spec::Classes);
            std::string member = peek(1).kind == Tok::Ident ? peek(1).text : "name";
            std::string base = e->kind == Expr::Kind::Name ? static_cast<NameExpr&>(*e).name : "value";
            notYet(t.loc, "pointers and '->'",
                   "fields and methods are accessed with '.': write `" + base + "." + member + "`");
        } else if (t.kind == Tok::LBracket) {
            notYet(t.loc, "arrays and indexing");
        } else if (t.kind == Tok::QuestionDot) {
            notYet(t.loc, "optionals");
        } else {
            return e;
        }
    }
}

static const char* kCastHelp =
    "an int already converts to a long and a number to a bool without a cast, so the cast can usually be "
    "removed, e.g. `long total = count;`";

ExprPtr Parser::parsePrimary() {
    const Token& t = advance();
    switch (t.kind) {
        case Tok::IntLit:
            if (t.intValue > (uint64_t)INT64_MAX)
                fail(t.loc, "the number " + t.text + " is too large for any integer type",
                     "the largest long is 9223372036854775807", spec::IntegerLiterals);
            return std::make_unique<IntLitExpr>(t.loc, (int64_t)t.intValue, t.longSuffix);
        case Tok::StringLit: {
            Loc loc = t.loc;
            std::string value = t.text;
            while (check(Tok::StringLit)) {  // "adjacent" "strings" are joined, as in C
                const Token& next = advance();
                value += next.text;
                if (next.loc.line == loc.line) loc.len = next.loc.col + next.loc.len - loc.col;
            }
            return std::make_unique<StringLitExpr>(loc, std::move(value));
        }
        case Tok::KwTrue: return std::make_unique<BoolLitExpr>(t.loc, true);
        case Tok::KwFalse: return std::make_unique<BoolLitExpr>(t.loc, false);
        case Tok::KwInt: case Tok::KwLong: case Tok::KwBool: case Tok::KwString:
            if (check(Tok::LParen)) notYet(t.loc, "casts like '" + std::string(tokSpelling(t.kind)) + "(x)'", kCastHelp);
            fail(t.loc, "expected an expression, found " + describeToken(t),
                 "a type name can only start a declaration, e.g. `" + std::string(tokSpelling(t.kind)) + " x = ...;`");
        case Tok::KwNull: notYet(t.loc, "optionals and null");
        case Tok::KwThis: return std::make_unique<ThisExpr>(t.loc);
        case Tok::LParen: {
            if ((check(Tok::KwInt) || check(Tok::KwLong) || check(Tok::KwBool) || check(Tok::KwString)) &&
                peek(1).kind == Tok::RParen)
                notYet(t.loc, "casts like '(" + std::string(tokSpelling(peek().kind)) + ")x'", kCastHelp);
            ExprPtr e = parseExpr();
            expect(Tok::RParen, "to close the parenthesis");
            return e;
        }
        case Tok::LBrace: return parseInitList(t.loc, nullptr);
        case Tok::Ident: {
            if (t.text == "static_cast" || t.text == "reinterpret_cast" || t.text == "const_cast" ||
                t.text == "dynamic_cast")
                notYet(t.loc, "casts like '" + t.text + "<T>(x)'", kCastHelp);
            if (check(Tok::ColonColon)) {
                if (t.text == "std")
                    fail(t.loc, "C+ has no std:: namespace",
                         "built-ins need no prefix: `std::cout << x << std::endl;` becomes `println(\"{}\", x);`",
                         spec::Printing);
                notYet(t.loc, "module members like '" + t.text + "::" + peek(1).text + "'");
            }
            if (isTypeName(t) && match(Tok::LBrace)) return parseInitList(t.loc, &prog_->findStruct(t.text)->type);
            if (match(Tok::LParen)) {
                std::vector<ExprPtr> args;
                if (!check(Tok::RParen)) {
                    do args.push_back(parseExpr());
                    while (match(Tok::Comma));
                }
                expect(Tok::RParen, "to close the argument list of '" + t.text + "'");
                return std::make_unique<CallExpr>(t.loc, t.text, std::move(args));
            }
            if (check(Tok::Less) && (t.text == "vector" || t.text == "array" || t.text == "hashmap"))
                notYet(t.loc, "containers like " + t.text + "<T>");
            return std::make_unique<NameExpr>(t.loc, t.text);
        }
        default:
            fail(t.loc, "expected an expression, found " + describeToken(t));
    }
}

ExprPtr Parser::parseInitList(Loc start, const Type* explicitType) {
    auto list = std::make_unique<InitListExpr>(start);
    list->explicitType = explicitType;
    while (!check(Tok::RBrace) && !check(Tok::Eof)) {
        InitListExpr::Element el;
        el.loc = peek().loc;
        if (match(Tok::Dot)) {  // designated: `.x = 1`
            const Token& field = expect(Tok::Ident, "for the field name after '.'");
            el.designator = field.text;
            el.loc = field.loc;
            expect(Tok::Assign, "after the field name, as in `.x = 1`");
        }
        el.value = parseAssign();
        list->elements.push_back(std::move(el));
        if (!match(Tok::Comma)) break;  // a trailing comma is allowed, as in C++
    }
    expect(Tok::RBrace, "to close the { } list");
    return list;
}

}  // namespace cplus
