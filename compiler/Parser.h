#pragma once

#include <unordered_set>
#include <vector>

#include "AST.h"
#include "Lexer.h"

namespace cplus {

class Parser {
public:
    Parser(std::vector<Token> tokens, Diagnostics& diags);
    Program parseProgram();

private:
    struct ParseError {};  // thrown after reporting, caught at a recovery point

    // Token helpers
    const Token& peek(size_t ahead = 0) const;
    const Token& prev() const;
    const Token& advance();
    bool check(Tok k) const { return peek().kind == k; }
    bool match(Tok k);
    const Token& expect(Tok k, const std::string& context);
    Loc afterPrev() const;
    [[noreturn]] void fail(Loc loc, const std::string& message, const std::string& help = "",
                           const std::string& spec = "");
    [[noreturn]] void notYet(Loc loc, const std::string& feature, const std::string& help = "");
    [[noreturn]] void failStd(Loc loc, const Token& name);
    const Type* parseBaseType(Loc& loc);
    ExprPtr parseCoalesce();
    ExprPtr parseMemberAccess(ExprPtr e, const char* after);
    CondDecl parseCondDecl(const char* stmt);
    void syncTopLevel();
    void syncStatement();

    // Grammar
    void parseTopLevel(Program& prog);
    // `[[allow(new_delete)]]`, the one attribute C+ has. Returns where it is written.
    Loc parseAttribute();
    bool atAttribute() const { return check(Tok::LBracket) && peek(1).kind == Tok::LBracket; }
    ExprPtr parseNew();
    void parseRecord();  // struct or class
    void parseExtern(Program& prog);
    void parseExternItem(Program& prog);  // a C function, or `struct sqlite3;`
    void parseOpaqueStruct();
    std::unique_ptr<FuncDecl> parseExternFunction();
    const Type* parseCType(Loc& loc, bool isReturn);
    void parseMember(StructDecl& sd, Access& access);
    void parseParams(FuncDecl& fn);
    void parseMemberInits(FuncDecl& ctor);
    bool isTypeName(const Token& t) const { return t.kind == Tok::Ident && typeNames_.count(t.text); }
    bool isTypeStart(size_t ahead = 0) const;
    bool isTypeAt(size_t ahead) const;  // whether a type, rather than an expression, starts there
    // A type, including pointers: `const char*`, `int**`. `isConst` receives a `const`
    // that makes the variable itself const; without it, such a `const` is an error.
    const Type* parseType(Loc& loc, bool* isConst = nullptr);
    // After the first variable of `int* a, * b;`, the stars of the next one, which must
    // repeat the first one's.
    void parseLaterStars(const Type* ty, const std::string& firstName, const char* what);
    void expectCloseAngle(const std::string& context);
    std::unique_ptr<FuncDecl> parseFunction(const Type* retType, Loc retLoc);
    std::unique_ptr<BlockStmt> parseBlock();
    StmtPtr parseStmt();
    StmtPtr parseDeclaration();  // one or more variables, without the trailing ';'
    StmtPtr parseDeclarators(bool isConst, const Type* ty, Loc typeLoc);  // the part after the type
    StmtPtr parseFor();
    void checkUnknownType();
    void checkUnknownPointerType();  // `FILE* f = ...` where FILE is not declared
    bool isOptionalDeclRest(size_t i) const;

    ExprPtr parseExpr();
    ExprPtr parseAssign();
    ExprPtr parseTernary();
    ExprPtr parseBinary(int minPrec);
    ExprPtr parseUnary();
    ExprPtr parsePostfix(ExprPtr e);
    ExprPtr parsePrimary();
    ExprPtr parseInitList(Loc start, const Type* explicitType);  // after the '{'

    std::vector<Token> toks_;
    Diagnostics& diags_;
    size_t pos_ = 0;
    Program* prog_ = nullptr;
    // Struct names found by a scan before parsing, so types can be used before
    // their definition (there are no headers or forward declarations).
    std::unordered_set<std::string> typeNames_;
    bool declSeen_ = false;  // a declaration came before; `[[allow(new_delete)]];` must be first
    bool fileAttr_ = false;  // the item just parsed was `[[allow(new_delete)]];`
};

}  // namespace cplus
