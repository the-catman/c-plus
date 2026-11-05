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
    void syncTopLevel();
    void syncStatement();

    // Grammar
    void parseTopLevel(Program& prog);
    void parseRecord();  // struct or class
    void parseMember(StructDecl& sd, Access& access);
    void parseParams(FuncDecl& fn);
    void parseMemberInits(FuncDecl& ctor);
    bool isTypeName(const Token& t) const { return t.kind == Tok::Ident && typeNames_.count(t.text); }
    bool isTypeStart(size_t ahead = 0) const;
    const Type* parseType(Loc& loc);
    void expectCloseAngle(const std::string& context);
    std::unique_ptr<FuncDecl> parseFunction(const Type* retType, Loc retLoc);
    std::unique_ptr<BlockStmt> parseBlock();
    StmtPtr parseStmt();
    StmtPtr parseDeclaration();  // one or more variables, without the trailing ';'
    StmtPtr parseDeclarators(bool isConst, const Type* ty, Loc typeLoc);  // the part after the type
    StmtPtr parseFor();
    void checkUnknownType();

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
};

}  // namespace cplus
