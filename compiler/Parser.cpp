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
        fileAttr_ = false;
        try {
            parseTopLevel(prog);
        } catch (ParseError&) {
            syncTopLevel();
        }
        if (!fileAttr_) declSeen_ = true;
    }
    return prog;
}

// `[[allow(new_delete)]]`. C++'s own attributes, such as [[nodiscard]], are not part of C+.
Loc Parser::parseAttribute() {
    Loc loc = advance().loc;
    advance();
    const Token& name = peek();
    if (name.kind != Tok::Ident || name.text != "allow")
        fail(name.loc,
             name.kind == Tok::Ident ? "C+ has no attribute '" + name.text + "'"
                                     : "expected an attribute name after '[[', found " + describeToken(name),
             "the one attribute C+ has is `[[allow(new_delete)]]`, which turns off the warning for new and delete",
             spec::NewDelete);
    advance();
    expect(Tok::LParen, "after 'allow'");
    const Token& what = peek();
    if (what.kind != Tok::Ident || what.text != "new_delete")
        fail(what.loc,
             what.kind == Tok::Ident ? "'" + what.text + "' is not a warning that can be allowed"
                                     : "expected the name of a warning inside allow(...)",
             "the warning that can be turned off is the one for new and delete: `[[allow(new_delete)]]`",
             spec::NewDelete);
    advance();
    expect(Tok::RParen, "after 'new_delete'");
    expect(Tok::RBracket, "to close the attribute `[[allow(new_delete)]]`");
    expect(Tok::RBracket, "to close the attribute `[[allow(new_delete)]]`");
    if (prev().loc.line == loc.line) loc.len = prev().loc.col + 1 - loc.col;
    return loc;
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
        case Tok::LBracket: {
            if (!atAttribute()) break;
            Loc attr = parseAttribute();
            if (match(Tok::Semi)) {  // `[[allow(new_delete)]];` for the whole file
                if (declSeen_)
                    diags_.error(attr, "`[[allow(new_delete)]];` applies to the whole file, so it is written at the "
                                       "top, before the first declaration",
                                 "move it to the top of the file, or write `[[allow(new_delete)]]`, without the ';', "
                                 "just before a function that uses new and delete",
                                 spec::NewDelete);
                prog.allowNewDelete = true;
                fileAttr_ = true;
                return;
            }
            if (check(Tok::KwStruct) || check(Tok::KwClass)) {
                diags_.error(attr, "[[allow(new_delete)]] is written before a function or a method, not a " +
                                       std::string(tokSpelling(peek().kind)),
                             "write it before each method that uses new and delete, or write `[[allow(new_delete)]];` "
                             "at the top of the file to allow them everywhere",
                             spec::NewDelete);
                parseRecord();
                return;
            }
            if (check(Tok::KwExtern) || check(Tok::KwImport) || atAttribute() || check(Tok::Eof))
                fail(attr, "[[allow(new_delete)]] is written before a function",
                     "write it just before a function that uses new and delete, or end it with ';' at the top of "
                     "the file to allow them in the whole file",
                     spec::NewDelete);
            size_t before = prog.funcs.size();
            parseTopLevel(prog);
            if (prog.funcs.size() > before) prog.funcs.back()->allowNewDelete = true;
            return;
        }
        case Tok::KwTemplate: notYet(t.loc, "templates (generics)");
        case Tok::KwEnum: notYet(t.loc, "enums");
        case Tok::KwInterface: notYet(t.loc, "interfaces");
        case Tok::KwExtern: parseExtern(prog); return;
        case Tok::KwNamespace:
            fail(t.loc, "C+ has no namespaces; each file is a module", "remove the namespace block", spec::Modules);
        case Tok::Ident:
            if (peek(1).kind == Tok::ColonColon && isTypeName(t))
                fail(t.loc, "methods and constructors are defined inside their " +
                                std::string(prog_->findStruct(t.text)->keyword()),
                     "move this definition into the body of '" + t.text + "'; C+ has no headers, so there is no "
                     "separate declaration to match",
                     spec::Classes);
            if (t.text == "std" && peek(1).kind == Tok::ColonColon) failStd(t.loc, peek(2));
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
    Loc retLoc;
    bool isConst = false;
    const Type* ret = parseType(retLoc, &isConst);
    if (isConst) notYet(t.loc, "global constants");
    if (ret->is(Type::Kind::Auto)) notYet(retLoc, "'auto' return types", "write the return type, e.g. `int`");
    if (check(Tok::Ident) && peek(1).kind == Tok::ColonColon && isTypeName(peek()))
        fail(peek().loc, "methods are defined inside their " + std::string(prog_->findStruct(peek().text)->keyword()),
             "move this definition into the body of '" + peek().text + "'; C+ has no headers, so there is no "
             "separate declaration to match",
             spec::Classes);
    if (check(Tok::Ident) && peek(1).kind != Tok::LParen) notYet(peek().loc, "global variables");
    prog.funcs.push_back(parseFunction(ret, retLoc));
}

// `extern "C" int abs(int n);` or a block of such declarations: `extern "C" { ... }`.
void Parser::parseExtern(Program& prog) {
    Loc loc = advance().loc;
    if (!check(Tok::StringLit))
        fail(loc, "a C function is declared with `extern \"C\"`",
             "write e.g. `extern \"C\" int abs(int n);`; C+ code needs no extern to call C+ functions in the same "
             "file",
             spec::CFunctions);
    const Token& lang = advance();
    if (lang.text != "C")
        fail(lang.loc, "only C functions can be declared with extern, not \"" + lang.text + "\"",
             lang.text == "C++" ? "C++ functions cannot be called directly. Wrap them in functions declared "
                                  "`extern \"C\"` in C++, and declare those here with `extern \"C\"`"
                                : "write `extern \"C\"`",
             spec::CFunctions);
    if (!match(Tok::LBrace)) {
        parseExternItem(prog);
        return;
    }
    while (!check(Tok::RBrace) && !check(Tok::Eof)) {
        try {
            parseExternItem(prog);
        } catch (ParseError&) {
            syncStatement();
        }
    }
    expect(Tok::RBrace, "to close the extern \"C\" block");
    match(Tok::Semi);
}

void Parser::parseExternItem(Program& prog) {
    if (check(Tok::KwStruct) || check(Tok::KwClass) || (check(Tok::Ident) && peek().text == "typedef"))
        return parseOpaqueStruct();
    prog.funcs.push_back(parseExternFunction());
}

// `struct sqlite3;` inside extern "C": a C type whose fields C+ never sees. C+ code can
// hold pointers to it, `sqlite3*`, and pass them to C functions.
void Parser::parseOpaqueStruct() {
    const Token& kw = advance();
    if (kw.kind == Tok::Ident)
        fail(kw.loc, "C+ has no typedef",
             "to use a C type through pointers, declare it by name, e.g. `struct sqlite3;`. C+ code then writes "
             "`sqlite3*`, without 'struct'",
             spec::CTypes);
    if (kw.kind == Tok::KwClass)
        fail(kw.loc, "C has no classes", "declare a C type with `struct`, e.g. `struct sqlite3;`", spec::CTypes);
    const Token& name = expect(Tok::Ident, "for the name of the C struct");
    if (check(Tok::LBrace))
        notYet(peek().loc, "C structs with fields",
               "declare it without fields, `struct " + name.text + ";`, to pass pointers to it to C functions");
    if (!check(Tok::Semi))
        fail(kw.loc, "C+ writes a C type without the word 'struct'",
             "declare the type once inside extern \"C\", `struct " + name.text + ";`, and then write `" + name.text +
                 "*`",
             spec::CTypes);
    advance();
    StructDecl* sd = prog_->findStruct(name.text);  // created by the scan in parseProgram
    if (sd->defined || sd->opaque || sd->isClass) {
        diags_.error(name.loc, "the name '" + name.text + "' is already used by another struct or class",
                     "rename one of them", spec::Structs);
        if (sd->defined || sd->opaque) diags_.note(sd->nameLoc, "the first one is here");
        return;
    }
    sd->opaque = true;
    sd->nameLoc = name.loc;
    sd->type.kind = Type::Kind::Opaque;
}

// `int abs(int n);` inside `extern "C"`. Parameter names are optional, as in C.
std::unique_ptr<FuncDecl> Parser::parseExternFunction() {
    if (check(Tok::KwExtern))
        fail(peek().loc, "extern \"C\" blocks cannot be nested", "remove this `extern \"C\"`", spec::CFunctions);
    if (check(Tok::KwStatic) || (check(Tok::Ident) && peek().text == "inline"))
        fail(peek().loc, "'" + peek().text + "' does not belong in an extern \"C\" declaration",
             "remove it; C+ calls the function that the C library exports", spec::CFunctions);
    auto fn = std::make_unique<FuncDecl>();
    fn->isExtern = true;
    fn->retType = parseCType(fn->retTypeLoc, true);
    const Token& name = expect(Tok::Ident, "for the name of the C function");
    fn->name = name.text;
    fn->nameLoc = name.loc;
    if (!check(Tok::LParen))
        fail(name.loc, "this compiler does not support C variables, such as errno, yet",
             "only C functions can be declared with extern \"C\"", spec::CFunctions);
    advance();
    if (check(Tok::KwVoid) && peek(1).kind == Tok::RParen) advance();  // `int rand(void)`
    if (!check(Tok::RParen)) {
        do {
            if (check(Tok::Dot) && peek(1).kind == Tok::Dot && peek(2).kind == Tok::Dot)
                fail(peek().loc, "this compiler does not support C functions with variable arguments, such as "
                                 "printf, yet",
                     fn->name == "printf" || fn->name == "fprintf" || fn->name == "sprintf" || fn->name == "snprintf"
                         ? "print with `println(\"{}\", x)` and build strings with `format(\"{}\", x)`"
                         : "declare a C function with fixed parameters instead",
                     spec::CFunctions);
            Loc typeLoc;
            const Type* ty = parseCType(typeLoc, false);
            Loc nameLoc = typeLoc;
            std::string pname;
            if (check(Tok::Ident)) {
                const Token& t = advance();
                pname = t.text;
                nameLoc = t.loc;
            }
            if (check(Tok::LBracket))
                fail(peek().loc, "a C array parameter is written as a pointer",
                     "write `" + std::string(ty->name) + "* " + (pname.empty() ? "values" : pname) +
                         "`: C passes an array as a pointer to its first element",
                     spec::CFunctions);
            if (check(Tok::Assign)) notYet(peek().loc, "default argument values");
            auto param = std::make_unique<VarDecl>(nameLoc, pname, ty, typeLoc);
            param->isParam = true;
            fn->params.push_back(std::move(param));
        } while (match(Tok::Comma));
    }
    expect(Tok::RParen, "to close the parameter list");
    if (check(Tok::LBrace))
        fail(peek().loc, "a function declared extern \"C\" is defined in C, so it has no body here",
             "end the declaration with ';'. A function written in C+ needs no extern \"C\"",
             spec::CFunctions);
    expect(Tok::Semi, "after the declaration of a C function");
    return fn;
}

// A type in a C declaration. C+ types are written there, and the common C spellings get
// an error that names the C+ type to write instead.
const Type* Parser::parseCType(Loc& loc, bool isReturn) {
    const Token& t = peek();
    bool isConst = check(Tok::KwConst);
    const Token& base = isConst ? peek(1) : t;
    std::string spelled = (isConst ? "const " : "") + base.text;
    if (isConst && base.kind == Tok::KwChar && peek(2).kind == Tok::Star && peek(3).kind != Tok::Star)
        fail(t.loc, "C+ passes text to C as `string`, not as `" + spelled + "*`",
             isReturn ? "write `string?`, which is null when the C function returns NULL, or `string` if it never "
                        "does. The text is copied into a C+ string"
                      : "write `string`. C+ passes a copy of the text that ends in a zero byte, as C expects; write "
                        "`string?` if the function also accepts NULL. To pass a pointer to bytes already in memory, such "
                        "as a buffer the function fills, write `char*`",
             spec::CFunctions);
    if (base.kind == Tok::Ident) {
        const std::string& n = base.text;
        std::string use, why;
        if (n == "size_t" || n == "ssize_t" || n == "ptrdiff_t" || n == "intptr_t" || n == "int64_t" ||
            n == "off_t" || n == "time_t")
            use = "long", why = "it is 64 bits, like '" + n + "'";
        else if (n == "uint64_t" || n == "uintptr_t")
            use = "long", why = "it passes the same 64 bits; C+ has no unsigned types yet";
        else if (n == "int32_t" || n == "pid_t" || n == "uid_t" || n == "gid_t")
            use = "int", why = "it is 32 bits, like '" + n + "'";
        else if (n == "uint32_t" || n == "unsigned")
            use = "int", why = "it passes the same 32 bits; C+ has no unsigned types yet";
        if (n == "short" || n == "int8_t" || n == "int16_t" || n == "uint8_t" || n == "uint16_t")
            notYet(base.loc, "8-bit and 16-bit integer types such as '" + n + "'",
                   "a C function that takes one can be declared only once they exist");
        if (!use.empty()) {
            if (n == "unsigned" && peek(isConst ? 2 : 1).kind == Tok::KwLong)
                use = "long", why = "it passes the same 64 bits; C+ has no unsigned types yet";
            fail(t.loc, "'" + n + "' is a C type; write `" + use + "` here", why, spec::CFunctions);
        }
    }
    if (base.kind == Tok::KwStruct) {
        const Token& name = peek(isConst ? 2 : 1);
        if (name.kind == Tok::Ident && peek(isConst ? 3 : 2).kind == Tok::Star)
            fail(base.loc, "C+ writes a C type without the word 'struct'",
                 "declare the type once inside extern \"C\", `struct " + name.text + ";`, and then write `" +
                     name.text + "*`",
                 spec::CTypes);
        notYet(t.loc, "passing C structs by value",
               "pass a pointer to the struct, if the C library takes one, or its fields as separate arguments");
    }
    bool varConst = false;  // `const int n` in C means nothing to the caller
    const Type* ty = parseType(loc, &varConst);
    if (ty->is(Type::Kind::Auto)) fail(loc, "a C function needs written types, not 'auto'", "", spec::CFunctions);
    if (check(Tok::Amp)) fail(peek().loc, "C has no references", "pass a pointer instead, e.g. `int* out`", spec::CFunctions);
    return ty;
}

// `std::name`. C+ has no namespaces, and its built-ins need no prefix.
void Parser::failStd(Loc loc, const Token& name) {
    std::string n = name.kind == Tok::Ident ? name.text : name.kind == Tok::KwString ? "string" : "";
    if (n == "string" || n == "vector" || n == "to_string" || n == "format" || n == "println" || n == "print")
        fail(loc, "C+ has no std:: namespace", "built-ins need no prefix: write `" + n + "` instead of `std::" + n + "`",
             spec::Modules);
    if (isMathFunction(n) || isCharFunction(n))
        fail(loc, "C+ has no std:: namespace", "it is built in: write `" + n + "(...)` instead of `std::" + n + "(...)`",
             isMathFunction(n) ? spec::Math : spec::Char);
    if (std::string reader = cNumberReader(n); !reader.empty())
        fail(loc, "C+ has no std:: namespace",
             "this compiler cannot read a number from a string yet. Until then, declare the C function: `extern \"C\" " +
                 reader + ";`",
             spec::Status);
    if (n == "numbers")
        fail(loc, "C+ has no std:: namespace", "math constants are planned. Until then, `acos(-1)` is pi", spec::Math);
    fail(loc, "C+ has no std:: namespace",
         "built-ins need no prefix: `std::cout << x << std::endl;` becomes `println(\"{}\", x);`", spec::Printing);
}

bool Parser::isTypeStart(size_t ahead) const {
    switch (peek(ahead).kind) {
        case Tok::Ident:  // a struct name followed by a variable name: `Point p`, `User? u`, or `vector<...>`
            if (peek(ahead + 1).kind == Tok::Less)
                return peek(ahead).text == "vector" || peek(ahead).text == "array" || peek(ahead).text == "hashmap";
            if (!isTypeName(peek(ahead))) return false;
            if (peek(ahead + 1).kind == Tok::Question || peek(ahead + 1).kind == Tok::QuestionQuestion) ++ahead;
            while (peek(ahead + 1).kind == Tok::Star || (peek(ahead + 1).kind == Tok::KwConst &&
                                                          peek(ahead).kind == Tok::Star))
                ++ahead;  // `Point* p`, `Node** head`, `Point* const p`
            return peek(ahead + 1).kind == Tok::Ident;
        case Tok::KwString: return peek(ahead + 1).kind != Tok::ColonColon;  // not `string::npos`
        case Tok::KwInt: case Tok::KwLong: case Tok::KwBool:
        case Tok::KwVoid: case Tok::KwAuto: case Tok::KwConst:
        case Tok::KwDouble: case Tok::KwFloat: case Tok::KwChar:
            return true;
        default: return false;
    }
}

bool Parser::isTypeAt(size_t ahead) const {
    const Token& t = peek(ahead);
    switch (t.kind) {
        case Tok::KwInt: case Tok::KwLong: case Tok::KwBool: case Tok::KwString: case Tok::KwVoid:
        case Tok::KwDouble: case Tok::KwFloat: case Tok::KwChar: case Tok::KwConst:
            return true;
        case Tok::Ident: return isTypeName(t) || (t.text == "vector" && peek(ahead + 1).kind == Tok::Less);
        default: return false;
    }
}

// `FILE* f = fopen(...)` where FILE was never declared: a name, stars, another name, and
// then what ends a declaration. As an expression it would be a useless multiplication.
void Parser::checkUnknownPointerType() {
    if (!check(Tok::Ident) || isTypeName(peek()) || peek(1).kind != Tok::Star) return;
    size_t i = 1;
    while (peek(i).kind == Tok::Star) ++i;
    if (peek(i).kind != Tok::Ident) return;
    Tok after = peek(i + 1).kind;
    if (after != Tok::Assign && after != Tok::Semi && after != Tok::Comma && after != Tok::RParen) return;
    const Token& t = peek();
    if (t.text == "size_t" || t.text == "int32_t" || t.text == "int64_t" || t.text == "uint32_t" ||
        t.text == "uint64_t" || t.text == "unsigned")
        fail(t.loc, "unknown type '" + t.text + "'",
             "use int (32-bit) or long (64-bit): `long* " + peek(i).text + "`", spec::IntegerTypes);
    fail(t.loc, "unknown type '" + t.text + "'",
         "if '" + t.text + "' is a C type used through pointers, declare it first, at the top level: `extern \"C\" "
         "struct " + t.text + ";`",
         spec::CTypes);
}

// Reports `Foo x` where Foo is not a known type (two identifiers in a row).
// Whether the tokens from `i` on are the rest of `Name? name = ...;` rather than of
// `c ? y = 1 : 2;`, which has a ':' before the statement ends.
bool Parser::isOptionalDeclRest(size_t i) const {
    Tok after = peek(i).kind;
    if (after == Tok::Semi || after == Tok::Comma || after == Tok::RParen) return true;
    if (after != Tok::Assign) return false;
    int depth = 0;
    for (size_t j = i + 1;; ++j) {
        Tok k = peek(j).kind;
        if (k == Tok::Eof || (depth == 0 && (k == Tok::Semi || k == Tok::RBrace))) return true;
        if (k == Tok::LParen || k == Tok::LBracket || k == Tok::LBrace) ++depth;
        else if (k == Tok::RParen || k == Tok::RBracket || k == Tok::RBrace) --depth;
        else if (depth == 0 && k == Tok::Colon) return false;
        if (depth < 0) return true;
    }
}

void Parser::checkUnknownType() {
    checkUnknownPointerType();
    bool optional = check(Tok::Ident) && peek(1).kind == Tok::Question && peek(2).kind == Tok::Ident &&
                    isOptionalDeclRest(3);
    if (!(check(Tok::Ident) && (peek(1).kind == Tok::Ident || optional))) return;
    const Token& t = peek();
    std::string help;
    if (t.text == "vector")
        fail(t.loc, "vector needs an element type", "write `vector<int> " + peek(1).text + ";`", spec::Vectors);
    if (t.text == "size_t" || t.text == "int32_t" || t.text == "int64_t" || t.text == "uint32_t" ||
        t.text == "uint64_t" || t.text == "unsigned")
        help = "use int (32-bit) or long (64-bit); both are signed and the same size on every platform";
    else
        help = "the types available today are int, long, bool, string, void, vector<T>, and structs and classes you "
               "define";
    fail(t.loc, "unknown type '" + t.text + "'", help, spec::IntegerTypes);
}

// A type, with an optional `?` after it (`User?` is a User or null) and stars for a
// pointer: `int*`, `const char*`, `Node**`. As in C++, a leading `const` before a pointer
// type makes what it points to const, and a `const` after the last star makes the
// variable itself const.
const Type* Parser::parseType(Loc& loc, bool* isConst) {
    Loc start = peek().loc;
    Loc constLoc = start;
    bool leadingConst = match(Tok::KwConst);
    const Type* t = parseBaseType(loc);
    if (check(Tok::QuestionQuestion))
        fail(peek().loc, "an optional cannot hold another optional",
             "write one '?': `" + std::string(t->name) + "?` already means " + t->name + " or null", spec::Optionals);
    if (check(Tok::Question)) {
        Loc q = advance().loc;
        if (t->is(Type::Kind::Void) || t->is(Type::Kind::Auto))
            fail(loc, std::string("'") + t->name + "' cannot be optional",
                 t->is(Type::Kind::Auto) ? "write the type, e.g. `User? u = find(name);`, or `auto u = find(name);` "
                                           "when find returns an optional"
                                         : "a function that may return nothing returns an optional of a real type, "
                                           "e.g. `User?`",
                 spec::Optionals);
        if (check(Tok::Question))
            fail(peek().loc, "an optional cannot hold another optional",
                 "write one '?': `" + std::string(t->name) + "?` already means " + t->name + " or null", spec::Optionals);
        loc.len = q.col + 1 - loc.col;
        t = Type::optionalOf(t);
    }
    bool varConst = leadingConst;
    if (check(Tok::Star)) {
        if (t->is(Type::Kind::Auto))
            fail(peek().loc, "'auto' finds out by itself that a value is a pointer",
                 "write `auto p = &x;` without the '*'", spec::Variables);
        bool pointeeConst = leadingConst;  // `const char*`
        varConst = false;
        while (match(Tok::Star)) {
            t = Type::pointerTo(t, pointeeConst);
            pointeeConst = false;
            if (check(Tok::KwConst)) {
                constLoc = advance().loc;
                if (check(Tok::Star)) pointeeConst = true;  // `int* const*`
                else varConst = true;                       // `int* const p`
            }
        }
        if (check(Tok::Question) || check(Tok::QuestionQuestion))
            fail(peek().loc, "a pointer cannot be optional",
                 "a pointer can already be null: write `" + std::string(t->name) + "` and compare it with null",
                 spec::Pointers);
    }
    if (leadingConst || t->isPointer()) {  // the location covers the whole type
        loc = start;
        if (prev().loc.line == start.line) loc.len = prev().loc.col + prev().loc.len - start.col;
    }
    if (varConst) {
        if (!isConst)
            fail(constLoc, "'const' cannot be used here",
                 "const belongs to a variable, as in `const int n = 5;`, or to what a pointer points to, as in "
                 "`const char*`",
                 spec::Variables);
        *isConst = true;
    }
    return t;
}

const Type* Parser::parseBaseType(Loc& loc) {
    const Token& t = advance();
    loc = t.loc;
    switch (t.kind) {
        case Tok::KwInt: return Type::intTy();
        case Tok::KwLong:
            if (check(Tok::KwDouble))
                fail(t.loc, "C+ has no long double type", "write `double`, which has 64 bits", spec::FloatingPointTypes);
            if (check(Tok::KwLong) || check(Tok::KwInt))
                fail(t.loc, check(Tok::KwLong) ? "'long long' is not a C+ type" : "'long int' is not a C+ type",
                     "write just `long`; it is always 64 bits", spec::IntegerTypes);
            return Type::longTy();
        case Tok::KwBool: return Type::boolTy();
        case Tok::KwString: return Type::stringTy();
        case Tok::KwVoid: return Type::voidTy();
        case Tok::KwAuto: return Type::autoTy();
        case Tok::KwDouble: return Type::doubleTy();
        case Tok::KwFloat: return Type::floatTy();
        case Tok::KwChar: return Type::charTy();
        case Tok::Ident:
            if (isTypeName(t)) return &prog_->findStruct(t.text)->type;
            if (t.text == "vector" && check(Tok::Less)) {
                Loc start = t.loc;
                advance();
                if (check(Tok::Greater) || check(Tok::Shr))
                    fail(peek().loc, "vector needs an element type", "write e.g. `vector<int>`", spec::Vectors);
                Loc elemLoc;
                const Type* elem = parseType(elemLoc);
                if (elem->is(Type::Kind::Void) || elem->is(Type::Kind::Auto))
                    fail(elemLoc, std::string("a vector cannot hold '") + elem->name + "' elements",
                         "write the element type, e.g. `vector<int>`", spec::Vectors);
                if (check(Tok::Comma))
                    fail(peek().loc, "vector takes one type: the type of its elements", "write e.g. `vector<int>`",
                         spec::Vectors);
                expectCloseAngle("to close vector<...>");
                loc = start;
                loc.len = prev().loc.col + prev().loc.len - start.col;
                return Type::vectorOf(elem);
            }
            if ((t.text == "array" || t.text == "hashmap") && check(Tok::Less))
                notYet(t.loc, t.text == "array" ? "fixed-size arrays (array<T, N>)" : "hash maps (hashmap<K, V>)",
                       "use vector<T> for now");
            fail(t.loc, "unknown type '" + t.text + "'", "", spec::IntegerTypes);
        default: fail(t.loc, "expected a type, found " + describeToken(t), "", spec::IntegerTypes);
    }
}

// Expects the '>' that closes `vector<...>`. In `vector<vector<int>>` the lexer sees
// '>>', so the first '>' of it is taken and the token left as the second one.
void Parser::expectCloseAngle(const std::string& context) {
    if (check(Tok::Shr)) {
        Token& t = toks_[pos_];
        t.kind = Tok::Greater;
        t.text = ">";
        t.loc.col += 1;
        t.loc.len = 1;
        return;
    }
    expect(Tok::Greater, context);
}

void Parser::parseParams(FuncDecl& fn) {
    expect(Tok::LParen, "after the function name");
    if (check(Tok::KwVoid) && peek(1).kind == Tok::RParen) advance();  // C-style f(void)
    if (!check(Tok::RParen)) {
        do {
            if (!isTypeStart()) {
                checkUnknownType();
                fail(peek().loc, "expected a parameter type, found " + describeToken(peek()),
                     "parameters look like `int count`", spec::Functions);
            }
            Loc typeLoc;
            bool isConst = false;  // const parameters are accepted; they are copies anyway
            const Type* ty = parseType(typeLoc, &isConst);
            if (ty->is(Type::Kind::Auto)) notYet(typeLoc, "'auto' parameters", "write the parameter type");
            if (check(Tok::Amp) || check(Tok::AmpAmp))
                notYet(peek().loc, "reference parameters",
                       "pass the value itself: structs are copied, class objects are shared, and a vector is only "
                       "copied if one of the copies changes. To let the function change the caller's variable, "
                       "pass a pointer: `int* count`, called as `f(&n)`");
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
             std::string("remove this line; a ") + kw + " can be used before or after its definition. A C type used "
             "only through pointers is declared with `extern \"C\" struct " + name.text + ";`",
             spec::Structs);
    if (check(Tok::Colon)) notYet(peek().loc, "inheritance");
    StructDecl* sd = prog_->findStruct(name.text);  // created by the scan in parseProgram
    std::unique_ptr<StructDecl> duplicate;
    if (sd->defined || sd->opaque || sd->isClass != isClass) {
        diags_.error(name.loc, "the name '" + name.text + "' is already used by another struct or class",
                     "rename one of them", spec::Structs);
        if (sd->defined || sd->opaque) diags_.note(sd->nameLoc, "the first definition is here");
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
    if (atAttribute()) {  // `[[allow(new_delete)]]` before a method, constructor or destructor
        Loc attr = parseAttribute();
        if (check(Tok::KwPublic) || check(Tok::KwPrivate) || check(Tok::KwProtected) || check(Tok::RBrace) ||
            atAttribute()) {
            diags_.error(attr, "[[allow(new_delete)]] is written before a method",
                         "write it just before a method, constructor or destructor that uses new and delete",
                         spec::NewDelete);
            if (!check(Tok::RBrace)) parseMember(sd, access);
            return;
        }
        size_t methods = sd.methods.size(), fields = sd.fields.size();
        parseMember(sd, access);
        if (sd.methods.size() > methods) sd.methods.back()->allowNewDelete = true;
        else if (sd.fields.size() > fields)
            diags_.error(attr, "[[allow(new_delete)]] is written before a method, not a field",
                         "for a field whose default value uses new, write `[[allow(new_delete)]];` at the top of the "
                         "file",
                         spec::NewDelete);
        return;
    }
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
    bool isConst = false;
    const Type* ty = parseType(typeLoc, &isConst);
    if (isConst) notYet(t.loc, "const fields");
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
    std::string firstName;
    do {
        if (!firstName.empty()) parseLaterStars(ty, firstName, "field");
        if (check(Tok::Amp)) notYet(peek().loc, "reference types", "a field holds a value, or a pointer: `int* p;`");
        const Token& name = expect(Tok::Ident, "for the field name");
        if (firstName.empty()) firstName = name.text;
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
    bool isConst = false;
    Loc typeLoc;
    const Type* ty = parseType(typeLoc, &isConst);
    return parseDeclarators(isConst, ty, typeLoc);
}

void Parser::parseLaterStars(const Type* ty, const std::string& firstName, const char* what) {
    size_t want = 0;
    const Type* base = ty;
    for (; base->isPointer(); base = base->elem) ++want;
    Loc starLoc = peek().loc;
    size_t got = 0;
    while (match(Tok::Star)) {
        ++got;
        if (check(Tok::KwConst))
            fail(peek().loc, std::string("a const pointer ") + what + " is declared on its own",
                 "declare it separately, e.g. `" + std::string(ty->name) + " const " +
                     (check(Tok::Ident) ? peek().text : std::string("p")) + " = ...;`",
                 spec::PointerDeclarations);
    }
    if (got == want) return;
    std::string name = check(Tok::Ident) ? peek().text : "b";
    if (got == 0)
        fail(peek().loc, "in C++, this makes '" + name + "' a plain " + base->name + ", not a pointer like '" +
                             firstName + "'",
             "C+ does not mix pointers and other " + std::string(what) + "s in one declaration. Write `" +
                 std::string(ty->name) + " " + firstName + ", " + std::string(want, '*') + " " + name +
                 ";` if both are pointers, or declare '" + name + "' on its own",
             spec::PointerDeclarations);
    if (want == 0)
        fail(starLoc, "in C++, this makes '" + name + "' a pointer but '" + firstName + "' a plain " + base->name,
             "C+ does not mix pointers and other " + std::string(what) + "s in one declaration. Declare them "
             "separately: `" + std::string(ty->name) + " " + firstName + ";` and `" + std::string(ty->name) +
                 std::string(got, '*') + " " + name + ";`",
             spec::PointerDeclarations);
    fail(starLoc, "'" + name + "' and '" + firstName + "' would have different types, which one declaration cannot mix",
         "declare them separately, e.g. `" + std::string(ty->name) + " " + firstName + ";` and `" +
             std::string(ty->name) + std::string(got > want ? got - want : 0, '*') + " " + name + ";`",
         spec::PointerDeclarations);
}

StmtPtr Parser::parseDeclarators(bool isConst, const Type* ty, Loc typeLoc) {
    if (ty->is(Type::Kind::Void))
        fail(typeLoc, "variables cannot have type void", "pick a type such as int, or use auto", spec::Variables);
    std::vector<std::unique_ptr<VarDecl>> decls;
    do {
        if (!decls.empty()) parseLaterStars(ty, decls[0]->name, "variable");
        if (check(Tok::Amp) || check(Tok::AmpAmp))
            notYet(peek().loc, "references",
                   "a variable holds its own value. To refer to another variable, use a pointer: `int* p = &x;`");
        const Token& name =
            expect(Tok::Ident, decls.empty() ? "for the variable name" : "for the next variable name after ','");
        auto var = std::make_unique<VarDecl>(name.loc, name.text, ty, typeLoc);
        var->isConst = isConst;
        if (match(Tok::Assign)) {
            var->init = parseAssign();
        } else if (check(Tok::LBrace) || check(Tok::LParen)) {
            std::string help = "write `" + std::string(ty->name) + " " + name.text + " = value;`";
            if (ty->isVector())
                help = "write `" + std::string(ty->name) + " " + name.text + " = {1, 2, 3};` or `auto " + name.text +
                       " = " + ty->name + "(count, value);`";
            notYet(peek().loc, "brace and constructor initializers", help);
        }
        if (check(Tok::LBracket))
            notYet(peek().loc, "C-style arrays", "use a vector: `vector<" + std::string(ty->name) + "> " + name.text + ";`");
        if (check(Tok::Colon))
            fail(peek().loc, "a range-based for loop is written inside 'for ( )'",
                 "write `for (" + std::string(ty->name) + " " + name.text + " : items) { ... }`", spec::RangeFor);
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
        case Tok::LBracket:
            if (atAttribute()) {
                Loc attr = parseAttribute();
                fail(attr, "[[allow(new_delete)]] is written before a function, not before a statement",
                     "move it to just before the function, as in `[[allow(new_delete)]] void f() { ... }`",
                     spec::NewDelete);
            }
            break;
        case Tok::KwIf: {
            advance();
            expect(Tok::LParen, "after 'if'");
            std::optional<CondDecl> decl;
            ExprPtr cond = isTypeStart() ? (decl = parseCondDecl("if"), nullptr) : parseExpr();
            expect(Tok::RParen, "after the if condition");
            StmtPtr thenS = parseStmt();
            StmtPtr elseS = match(Tok::KwElse) ? parseStmt() : nullptr;
            auto st = std::make_unique<IfStmt>(t.loc, std::move(cond), std::move(thenS), std::move(elseS));
            st->decl = std::move(decl);
            return st;
        }
        case Tok::KwWhile: {
            advance();
            expect(Tok::LParen, "after 'while'");
            std::optional<CondDecl> decl;
            ExprPtr cond = isTypeStart() ? (decl = parseCondDecl("while"), nullptr) : parseExpr();
            expect(Tok::RParen, "after the while condition");
            auto st = std::make_unique<WhileStmt>(t.loc, std::move(cond), parseStmt());
            st->decl = std::move(decl);
            return st;
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
        case Tok::KwExtern:
            fail(t.loc, "C functions are declared outside of functions",
                 "move the `extern \"C\"` declaration to the top level of the file; it can then be called anywhere "
                 "in the file",
                 spec::CFunctions);
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

// `auto u = find(name)` as the condition of an if or while, as in C++17.
CondDecl Parser::parseCondDecl(const char* stmt) {
    bool isConst = false;
    Loc typeLoc;
    const Type* ty = parseType(typeLoc, &isConst);
    const Token& name = expect(Tok::Ident, std::string("for the name of the variable declared in the ") + stmt +
                                               " condition");
    if (!check(Tok::Assign))
        fail(peek().loc, std::string("a variable declared in the ") + stmt + " condition needs a value",
             std::string("write e.g. `") + stmt + " (auto " + name.text + " = find(key))`", spec::Optionals);
    advance();
    auto var = std::make_unique<VarDecl>(name.loc, name.text, ty, typeLoc);
    var->isConst = isConst;
    var->init = parseExpr();
    return CondDecl{std::move(var)};
}

StmtPtr Parser::parseFor() {
    auto loop = std::make_unique<ForStmt>(advance().loc);
    expect(Tok::LParen, "after 'for'");
    if (match(Tok::Semi)) {
        // no init
    } else if (isTypeStart()) {
        bool isConst = false;
        Loc typeLoc;
        const Type* ty = parseType(typeLoc, &isConst);
        // `for (auto x : items)` loops over the elements of a vector.
        bool ref = check(Tok::Amp) || check(Tok::AmpAmp);
        if ((ref && peek(1).kind == Tok::Ident && peek(2).kind == Tok::Colon) ||
            (check(Tok::Ident) && peek(1).kind == Tok::Colon)) {
            if (ref) {
                std::string range = peek(3).kind == Tok::Ident ? peek(3).text : "items";
                std::string help = std::string("drop the '&': `for (") + (isConst ? "const " : "") + ty->name + " " +
                                   peek(1).text + " : " + range + ")` copies each element, which is cheap";
                if (!isConst)
                    help += ". To change the elements, loop over the indexes: `for (long i = 0; i < " + range +
                            ".size(); i++) " + range + "[i] = ...;`";
                notYet(peek().loc, "references in range-based for loops", help);
            }
            if (ty->is(Type::Kind::Void))
                fail(typeLoc, "a loop variable cannot have type void", "write `auto` or the element type",
                     spec::RangeFor);
            const Token& name = advance();
            auto var = std::make_unique<VarDecl>(name.loc, name.text, ty, typeLoc);
            var->isConst = isConst;
            advance();  // ':'
            ExprPtr range = parseExpr();
            expect(Tok::RParen, "to close the for header");
            StmtPtr body = parseStmt();
            return std::make_unique<ForEachStmt>(loop->loc, std::move(var), std::move(range), std::move(body));
        }
        loop->init = parseDeclarators(isConst, ty, typeLoc);
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

// `a ?? b` binds less tightly than `||` and more tightly than `?:`, as in JavaScript and
// C#, and groups to the right: `a ?? b ?? c` is `a ?? (b ?? c)`.
ExprPtr Parser::parseCoalesce() {
    ExprPtr lhs = parseBinary(1);
    if (!check(Tok::QuestionQuestion)) return lhs;
    Loc loc = advance().loc;
    ExprPtr rhs = parseCoalesce();
    return std::make_unique<CoalesceExpr>(loc, std::move(lhs), std::move(rhs));
}

ExprPtr Parser::parseTernary() {
    ExprPtr cond = parseCoalesce();
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
            if (check(Tok::FloatLit)) {  // so that `float f = -1.5;` sees a constant
                const Token& lit = advance();
                Loc loc = t.loc;
                loc.len = lit.loc.col + lit.loc.len - t.loc.col;
                return parsePostfix(std::make_unique<FloatLitExpr>(loc, -lit.floatValue, lit.floatSuffix));
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
        case Tok::Star: advance(); return std::make_unique<UnaryExpr>(t.loc, UnOp::Deref, parseUnary());
        case Tok::Amp: advance(); return std::make_unique<UnaryExpr>(t.loc, UnOp::AddrOf, parseUnary());
        case Tok::KwNew: return parseNew();
        case Tok::KwDelete: {
            advance();
            bool array = false;
            if (match(Tok::LBracket)) {
                if (!check(Tok::RBracket))
                    fail(peek().loc, "'delete[]' has nothing inside the brackets",
                         "write `delete[] p;`; the number of elements is kept with the memory", spec::NewDelete);
                advance();
                array = true;
            }
            ExprPtr operand = parseUnary();
            Loc loc = t.loc;
            if (prev().loc.line == loc.line) loc.len = prev().loc.col + prev().loc.len - loc.col;
            return std::make_unique<DeleteExpr>(loc, array, std::move(operand));
        }
        case Tok::KwSizeof: {
            advance();
            if (check(Tok::LParen) && isTypeAt(1)) {  // `sizeof(int)`
                advance();
                Loc typeLoc;
                const Type* ty = parseType(typeLoc);
                expect(Tok::RParen, "to close sizeof(...)");
                Loc loc = t.loc;
                if (prev().loc.line == loc.line) loc.len = prev().loc.col + 1 - loc.col;
                return std::make_unique<SizeofExpr>(loc, ty, nullptr);
            }
            return std::make_unique<SizeofExpr>(t.loc, nullptr, parseUnary());  // `sizeof x` or `sizeof(x)`
        }
        case Tok::KwAwait: notYet(t.loc, "async and await");
        case Tok::KwSpawn: notYet(t.loc, "threads (spawn)");
        default: return parsePostfix(parsePrimary());
    }
}

// `new T`, `new T(args)`, `new T{...}` or `new T[n]`, as in C++.
ExprPtr Parser::parseNew() {
    Loc loc = advance().loc;
    if (check(Tok::LParen))
        fail(peek().loc, "C+ has no placement new",
             "new allocates the memory itself: write the type right after it, as in `new int(5)` or "
             "`new Point{1, 2}`",
             spec::NewDelete);
    if (!isTypeAt(0) && !check(Tok::KwAuto)) {
        const Token& t = peek();
        fail(t.loc, t.kind == Tok::Ident ? "unknown type '" + t.text + "'" : "expected a type after 'new', found " + describeToken(t),
             "new is followed by the type of the value it makes, e.g. `new int(5)` or `new Point{1, 2}`",
             spec::NewDelete);
    }
    Loc typeLoc;
    const Type* ty = parseType(typeLoc);
    std::string name = ty->name;
    if (ty->isClass())
        fail(loc, "class objects are created without 'new'",
             "write `" + name + "(...)`; the object is reference-counted and freed automatically", spec::Classes);
    if (ty->is(Type::Kind::Auto))
        fail(typeLoc, "new needs the type of the value it makes", "write the type, e.g. `new int(5)`", spec::NewDelete);
    auto n = std::make_unique<NewExpr>(loc, ty, typeLoc);
    if (match(Tok::LBracket)) {
        if (check(Tok::RBracket))
            fail(peek().loc, "new[] needs the number of elements", "write e.g. `new " + name + "[10]`", spec::NewDelete);
        n->count = parseExpr();
        expect(Tok::RBracket, "to close the size in `new " + name + "[...]`");
        if (check(Tok::LBracket))
            notYet(peek().loc, "arrays of arrays from new",
                   "allocate one array of rows * columns elements and reach an element with `p[row * columns + "
                   "column]`");
        if (check(Tok::LParen) && peek(1).kind == Tok::RParen) {  // `new int[n]()`, the same as without ()
            advance();
            advance();
        } else if (check(Tok::LParen) || check(Tok::LBrace)) {
            notYet(peek().loc, "initial values for an array from new",
                   "the elements start with their default value, such as 0; set them afterward, e.g. in a loop: "
                   "`p[i] = ...;`");
        }
    } else if (check(Tok::LBrace)) {
        if (!ty->isStruct() && !ty->isVector())  // reported at the '{', so recovery skips the list
            notYet(peek().loc, "brace initializers for '" + name + "'", "write `new " + name + "(value)`");
        advance();
        n->init = parseInitList(typeLoc, ty);
    } else if (match(Tok::LParen)) {
        std::vector<ExprPtr> args;
        if (!check(Tok::RParen)) {
            do args.push_back(parseExpr());
            while (match(Tok::Comma));
        }
        expect(Tok::RParen, "to close `new " + name + "(...)`");
        if (ty->isStruct() || ty->isVector() || (ty->is(Type::Kind::String) && args.size() != 1)) {
            // `new Point(1, 2)` builds the value as `Point(1, 2)` does.
            if (!args.empty() || !ty->is(Type::Kind::String)) {
                auto call = std::make_unique<CallExpr>(typeLoc, name, std::move(args));
                if (ty->isVector()) call->vectorType = ty;
                n->init = std::move(call);
            }
        } else if (args.size() > 1) {
            fail(args[1]->loc, "`new " + name + "(...)` takes one value",
                 "write e.g. `new " + name + "(x)`, which starts with the value of x", spec::NewDelete);
        } else if (args.size() == 1) {
            n->init = std::move(args[0]);  // `new int(5)`; `new int()` is the default value, as in C++
        }
    }
    if (prev().loc.line == loc.line) n->loc.len = prev().loc.col + prev().loc.len - loc.col;
    return n;
}

// `.name` or `.name(args)` after `e`.
ExprPtr Parser::parseMemberAccess(ExprPtr e, const char* after) {
    const Token& name = expect(Tok::Ident, std::string("for the field or method name after '") + after + "'");
    if (!match(Tok::LParen)) return std::make_unique<MemberExpr>(name.loc, std::move(e), name.text);
    std::vector<ExprPtr> args;
    if (!check(Tok::RParen)) {
        do args.push_back(parseExpr());
        while (match(Tok::Comma));
    }
    expect(Tok::RParen, "to close the argument list of '" + name.text + "'");
    return std::make_unique<MethodCallExpr>(name.loc, std::move(e), name.text, std::move(args));
}

ExprPtr Parser::parsePostfix(ExprPtr e) {
    // Each `?.` starts a chain that takes in the rest of the postfix expression, so in
    // `a?.b.c` a null `a` skips `.b.c` as well, as in JavaScript.
    std::vector<std::unique_ptr<OptChainExpr>> chains;
    auto close = [&] {
        while (!chains.empty()) {
            chains.back()->body = std::move(e);
            e = std::move(chains.back());
            chains.pop_back();
        }
        return std::move(e);
    };
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
            e = parseMemberAccess(std::move(e), ".");
        } else if (t.kind == Tok::QuestionDot) {
            advance();
            if (!check(Tok::Ident))
                fail(peek().loc, "'?.' is followed by a field or method name",
                     check(Tok::LBracket) ? "to read an element of an optional vector, unwrap it first: "
                                            "`if (auto v = opt) ... v[i]`"
                                          : "write e.g. `user?.name`",
                     spec::Optionals);
            auto chain = std::make_unique<OptChainExpr>(t.loc, std::move(e));
            auto value = std::make_unique<OptValueExpr>(chain->base->loc, chain.get());
            chain->value = value.get();
            chains.push_back(std::move(chain));
            e = parseMemberAccess(std::move(value), "?.");
        } else if (t.kind == Tok::Arrow) {
            if (e->kind == Expr::Kind::This)
                fail(t.loc, "'this' is a reference in C+, not a pointer",
                     "write `this." + (peek(1).kind == Tok::Ident ? peek(1).text : std::string("field")) + "`",
                     spec::Classes);
            // `p->x` is `(*p).x`. Sema reports `->` on anything but a pointer.
            Loc arrowLoc = advance().loc;
            auto deref = std::make_unique<UnaryExpr>(arrowLoc, UnOp::Deref, std::move(e));
            deref->arrowMember = check(Tok::Ident) ? peek().text : "name";
            e = parseMemberAccess(std::move(deref), "->");
        } else if (t.kind == Tok::LBracket) {
            advance();
            if (check(Tok::RBracket)) fail(peek().loc, "expected an index inside [ ]", "write e.g. `items[0]`", spec::Vectors);
            ExprPtr index = parseExpr();
            if (check(Tok::Comma))
                fail(peek().loc, "an element is picked with one index",
                     "for a vector of vectors, write `grid[i][j]`", spec::Vectors);
            expect(Tok::RBracket, "to close the index");
            e = std::make_unique<IndexExpr>(t.loc, std::move(e), std::move(index));
        } else {
            return close();
        }
    }
}

// Whether the tokens from `i` on start a type that a cast can name: `int`, `double`, ...
static bool isCastType(Tok k) {
    return k == Tok::KwInt || k == Tok::KwLong || k == Tok::KwBool || k == Tok::KwFloat || k == Tok::KwDouble ||
           k == Tok::KwChar || k == Tok::KwString;
}

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
        case Tok::FloatLit: return std::make_unique<FloatLitExpr>(t.loc, t.floatValue, t.floatSuffix);
        case Tok::CharLit: return std::make_unique<CharLitExpr>(t.loc, (uint8_t)t.intValue);
        case Tok::KwInt: case Tok::KwLong: case Tok::KwBool: case Tok::KwString: case Tok::KwFloat: case Tok::KwDouble:
        case Tok::KwChar:
            if (t.kind == Tok::KwString && match(Tok::ColonColon)) {
                // `string::npos`, what find() returns when nothing is found. With no
                // unsigned types it is -1, a long.
                const Token& member = peek();
                if (member.kind != Tok::Ident || member.text != "npos")
                    fail(member.loc,
                         member.kind == Tok::Ident ? "'string' has no member named '" + member.text + "'"
                                                   : "expected a name after 'string::', found " + describeToken(member),
                         "the only one is `string::npos`, the value find() returns when nothing is found",
                         spec::Strings);
                advance();
                Loc loc = t.loc;
                if (member.loc.line == loc.line) loc.len = member.loc.col + member.loc.len - loc.col;
                return std::make_unique<IntLitExpr>(loc, -1, true);
            }
            if (match(Tok::LParen)) {  // `int(x)`
                --pos_;
                --pos_;
                Loc loc;
                const Type* ty = parseBaseType(loc);
                expect(Tok::LParen, "after the type");
                if (ty->is(Type::Kind::String)) {
                    // `string(count, c)` and `string()`, as in C++. `string(x)` is a cast, which Sema rejects.
                    std::vector<ExprPtr> args;
                    if (!check(Tok::RParen)) {
                        do args.push_back(parseExpr());
                        while (match(Tok::Comma));
                    }
                    expect(Tok::RParen, "to close the argument list of 'string'");
                    if (args.size() == 1) return std::make_unique<CastExpr>(loc, ty, std::move(args[0]));
                    return std::make_unique<CallExpr>(loc, "string", std::move(args));
                }
                if (check(Tok::RParen))  // `int()`, which is 0 in C++
                    fail(peek().loc, std::string("a conversion to '") + ty->name + "' takes one value",
                         "write `" + std::string(ty->name) + "(x)` to convert x. For a zero, write `" +
                             (ty->is(Type::Kind::Bool)   ? "false"
                              : ty->isFloating()         ? "0.0"
                              : ty->is(Type::Kind::Char) ? "'\\0'"
                                                         : "0") +
                             "`",
                         spec::Casts);
                ExprPtr operand = parseExpr();
                if (check(Tok::Comma))
                    fail(peek().loc, std::string("a conversion to '") + ty->name + "' takes one value",
                         "write `" + std::string(ty->name) + "(x)`", spec::Casts);
                expect(Tok::RParen, "to close the conversion");
                return std::make_unique<CastExpr>(loc, ty, std::move(operand));
            }
            fail(t.loc, "expected an expression, found " + describeToken(t),
                 "a type name can only start a declaration, e.g. `" + std::string(tokSpelling(t.kind)) + " x = ...;`");
        case Tok::KwNull: return std::make_unique<NullLitExpr>(t.loc);
        case Tok::KwThis: return std::make_unique<ThisExpr>(t.loc);
        case Tok::LParen: {
            // `(char*)p` or `(const Node*)p`: a type with stars, then ')'.
            size_t i = peek().kind == Tok::KwConst ? 1 : 0;
            bool baseType = isCastType(peek(i).kind) || peek(i).kind == Tok::KwVoid || isTypeName(peek(i));
            if (baseType && peek(i + 1).kind == Tok::Star) {
                for (i += 1; peek(i).kind == Tok::Star || peek(i).kind == Tok::KwConst; ++i) {}
                if (peek(i).kind == Tok::RParen) {
                    Loc typeLoc;
                    const Type* ty = parseType(typeLoc);
                    expect(Tok::RParen, "to close the cast");
                    Loc loc = t.loc;
                    if (prev().loc.line == loc.line) loc.len = prev().loc.col + 1 - t.loc.col;
                    return std::make_unique<CastExpr>(loc, ty, parseUnary());
                }
            }
            if (isCastType(peek().kind) && peek(1).kind == Tok::RParen) {  // `(int)x`, as in C
                Loc loc;
                const Type* ty = parseBaseType(loc);
                advance();
                loc = t.loc;
                loc.len = prev().loc.col + 1 - t.loc.col;
                return std::make_unique<CastExpr>(loc, ty, parseUnary());
            }
            ExprPtr e = parseExpr();
            expect(Tok::RParen, "to close the parenthesis");
            return e;
        }
        case Tok::LBrace: return parseInitList(t.loc, nullptr);
        case Tok::Ident: {
            if (t.text == "static_cast" && check(Tok::Less)) {  // `static_cast<int>(x)`
                advance();
                Loc typeLoc;
                const Type* ty = parseType(typeLoc);
                expectCloseAngle("to close static_cast<...>");
                expect(Tok::LParen, "after static_cast<" + std::string(ty->name) + ">");
                ExprPtr operand = parseExpr();
                expect(Tok::RParen, "to close the static_cast");
                Loc loc = t.loc;
                if (prev().loc.line == loc.line) loc.len = prev().loc.col + 1 - loc.col;
                return std::make_unique<CastExpr>(loc, ty, std::move(operand));
            }
            if (t.text == "reinterpret_cast" || t.text == "const_cast" || t.text == "dynamic_cast")
                fail(t.loc, "C+ has no " + t.text,
                     t.text == "dynamic_cast" ? "inheritance is planned; until then every object has exactly one type"
                     : t.text == "const_cast" ? "a const variable cannot be changed; copy it into a variable that is not const"
                                              : "a cast between pointer types is written `static_cast<char*>(p)` or "
                                                "`(char*)p`. To convert a number, write `int(x)` or "
                                                "`static_cast<int>(x)`",
                     spec::Casts);
            if (check(Tok::ColonColon)) {
                if (t.text == "std") failStd(t.loc, peek(1));
                notYet(t.loc, "module members like '" + t.text + "::" + peek(1).text + "'");
            }
            if (isTypeName(t) && match(Tok::LBrace)) return parseInitList(t.loc, &prog_->findStruct(t.text)->type);
            if (t.text == "vector" && check(Tok::Less)) {
                --pos_;  // back to `vector`, to read the whole type
                Loc loc;
                const Type* ty = parseBaseType(loc);  // a '?' after it is an operator
                if (match(Tok::LBrace)) return parseInitList(loc, ty);
                if (!match(Tok::LParen))
                    fail(peek().loc, "expected '{' or '(' after '" + std::string(ty->name) + "'",
                         "write `" + std::string(ty->name) + "{1, 2, 3}` to list the elements, or `" + ty->name +
                             "(count, value)`",
                         spec::Vectors);
                std::vector<ExprPtr> args;
                if (!check(Tok::RParen)) {
                    do args.push_back(parseExpr());
                    while (match(Tok::Comma));
                }
                expect(Tok::RParen, "to close the argument list of '" + std::string(ty->name) + "'");
                auto call = std::make_unique<CallExpr>(loc, ty->name, std::move(args));
                call->vectorType = ty;
                return call;
            }
            if (match(Tok::LParen)) {
                std::vector<ExprPtr> args;
                if (!check(Tok::RParen)) {
                    do args.push_back(parseExpr());
                    while (match(Tok::Comma));
                }
                expect(Tok::RParen, "to close the argument list of '" + t.text + "'");
                return std::make_unique<CallExpr>(t.loc, t.text, std::move(args));
            }
            if (check(Tok::Less) && (t.text == "array" || t.text == "hashmap")) {
                --pos_;
                Loc loc;
                parseBaseType(loc);  // reports that the container is not supported yet
            }
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
