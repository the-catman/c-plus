#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Diagnostics.h"

namespace cplus {

// Every reserved word. Some are reserved for features that are planned but not
// implemented yet, so that programs written today keep working later.
#define CPLUS_KEYWORDS(X)                                                                         \
    X(Async, "async") X(Auto, "auto") X(Await, "await") X(Bool, "bool") X(Break, "break")         \
    X(Case, "case") X(Catch, "catch") X(Char, "char") X(Class, "class") X(Const, "const")         \
    X(Constexpr, "constexpr") X(Continue, "continue") X(Default, "default") X(Delete, "delete")   \
    X(Do, "do") X(Double, "double") X(Else, "else") X(Enum, "enum") X(Extern, "extern")           \
    X(False, "false") X(Float, "float") X(For, "for") X(If, "if") X(Import, "import")             \
    X(Int, "int") X(Interface, "interface") X(Long, "long") X(Namespace, "namespace")             \
    X(New, "new") X(Null, "null") X(Operator, "operator") X(Override, "override")                 \
    X(Private, "private") X(Protected, "protected") X(Public, "public") X(Return, "return")       \
    X(Sizeof, "sizeof") X(Spawn, "spawn") X(Static, "static") X(String, "string")                 \
    X(Struct, "struct") X(Switch, "switch") X(Template, "template") X(This, "this")               \
    X(Throw, "throw") X(Throws, "throws") X(True, "true") X(Try, "try") X(Typename, "typename")    \
    X(Virtual, "virtual") X(Void, "void") X(Weak, "weak") X(While, "while")

// Punctuators, longest first so the lexer can match greedily in this order.
#define CPLUS_PUNCTUATORS(X)                                                                      \
    X(ShlAssign, "<<=") X(ShrAssign, ">>=") X(ColonColon, "::") X(Arrow, "->")                   \
    X(PlusPlus, "++") X(MinusMinus, "--") X(PlusAssign, "+=") X(MinusAssign, "-=")               \
    X(StarAssign, "*=") X(SlashAssign, "/=") X(PercentAssign, "%=") X(AmpAssign, "&=")           \
    X(PipeAssign, "|=") X(CaretAssign, "^=") X(EqEq, "==") X(NotEq, "!=") X(LessEq, "<=")        \
    X(GreaterEq, ">=") X(AmpAmp, "&&") X(PipePipe, "||") X(Shl, "<<") X(Shr, ">>")               \
    X(QuestionQuestion, "??") X(QuestionDot, "?.") X(LParen, "(") X(RParen, ")")                 \
    X(LBrace, "{") X(RBrace, "}") X(LBracket, "[") X(RBracket, "]") X(Semi, ";")                 \
    X(Comma, ",") X(Dot, ".") X(Colon, ":") X(Question, "?") X(Plus, "+") X(Minus, "-")          \
    X(Star, "*") X(Slash, "/") X(Percent, "%") X(Assign, "=") X(Less, "<") X(Greater, ">")       \
    X(Bang, "!") X(Amp, "&") X(Pipe, "|") X(Caret, "^") X(Tilde, "~")

enum class Tok {
    Eof,
    Ident,
    IntLit,
    StringLit,
#define X(name, text) Kw##name,
    CPLUS_KEYWORDS(X)
#undef X
#define X(name, text) name,
    CPLUS_PUNCTUATORS(X)
#undef X
};

struct Token {
    Tok kind = Tok::Eof;
    Loc loc;
    std::string text;       // identifier name, string literal contents, or source spelling
    uint64_t intValue = 0;  // for IntLit
    bool longSuffix = false;  // IntLit written with L or LL, e.g. 1L
};

const char* tokSpelling(Tok kind);         // "while", "(", or a description like "identifier"
std::string describeToken(const Token& t);  // for "found ..." in error messages

class Lexer {
public:
    Lexer(const SourceFile& file, Diagnostics& diags);
    std::vector<Token> lexAll();

private:
    Token next();
    void skipSpaceAndComments();
    Token lexNumber();
    Token lexString();
    Token lexIdentOrKeyword();
    char peek(size_t ahead = 0) const;
    char advance();
    Loc locHere(uint32_t len = 1) const { return Loc{line_, col_, len}; }

    const std::string& src_;
    Diagnostics& diags_;
    size_t pos_ = 0;
    uint32_t line_ = 1;
    uint32_t col_ = 1;
};

}  // namespace cplus
