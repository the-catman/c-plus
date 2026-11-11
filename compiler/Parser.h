#pragma once

#include <deque>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "AST.h"
#include "CHeaders.h"
#include "ConstEval.h"
#include "Lexer.h"

namespace cplus {

class Parser {
public:
    Parser(std::vector<Token> tokens, Diagnostics& diags);
    void parseProgram(Program& prog);
    void findHeadersIn(HeaderSearch search) { headerSearch_ = std::move(search); }  // for `import <name.h>;`
    // For `cplus fmt`, which formats a file whether or not its headers can be found: what one that cannot be would declare is
    // taken to be there.
    void quietHeaders() { quietHeaders_ = true; }
    bool gaveUp() const { return gaveUp_; }  // the program was too deeply nested to read to its end

    // Reads a generic again with its parameters standing for `args`, which makes an ordinary
    // function. The same arguments give the same one. Null after reporting an error.
    FuncDecl* instantiateFunction(TemplateDecl& t, const std::vector<TemplateArg>& args, Loc usedAt, int depth);
    const Type* taskType(const Type* result, bool canFail, Loc usedAt);  // what `spawn` gives for a function with this result
    // The same for a constraint: `concept Addable = requires(T a, T b) { a + b; };`.
    struct ConceptBody {
        std::unique_ptr<FuncDecl> params;
        std::vector<ExprPtr> requirements;
    };
    bool instantiateConcept(TemplateDecl& t, const TemplateArg& arg, ConceptBody& out);
    // Reads the branch of an `if constexpr` that runs.
    StmtPtr readBranch(const LazyBranches& lazy, bool thenBranch);

private:
    struct ParseError {};  // thrown after reporting, caught at a recovery point
    struct TooDeep {};     // thrown after reporting, which ends the parse

    // Counts how deeply the code being parsed is nested, while one parsing function runs.
    struct Nesting {
        Parser& parser;
        explicit Nesting(Parser& p) : parser(p) {
            if (++parser.nesting_ > kMaxNesting) parser.failTooDeep();
        }
        ~Nesting() { --parser.nesting_; }
    };
    static constexpr int kMaxNesting = 1000;
    static constexpr int kMaxChain = 10000;  // `a + b + c`, `a.b.c`: no nesting, but as deep to work through
    int nesting_ = 0;
    bool gaveUp_ = false;
    bool inExternStruct_ = false;  // reading the fields of a struct inside extern "C"
    [[noreturn]] void failTooDeep();

    // Token helpers
    const Token& peek(size_t ahead = 0) const;
    const Token& prev() const;
    const Token& advance();
    bool check(Tok k) const { return peek().kind == k; }
    bool match(Tok k);
    const Token& expect(Tok k, const std::string& context);
    const Token& expectNewName(const std::string& context);
    std::string operatorHelp(size_t ahead = 0) const;
    [[noreturn]] void failUnknownGeneric(size_t at);
    bool isValueArgument(const Token& k);
    [[noreturn]] void failMakePointer(const Token& t);
    void closeProgramBlocks();
    std::string writtenBetweenAngles(size_t at, size_t& end) const;
    Loc afterPrev() const;
    [[noreturn]] void fail(Loc loc, const std::string& message, const std::string& help, const std::string& spec);
    [[noreturn]] void notYet(Loc loc, const std::string& feature, const std::string& help);
    bool asyncPending_ = false;  // `async` has been read before a function at the top level
    Loc asyncAt_;
    void rejectOtherLanguages();  // at the start of something: a word of another language, which is not a name here
    [[noreturn]] void failStd(Loc loc, const Token& name);
    const Type* parseBaseType(Loc& loc);
    ExprPtr parseCoalesce();
    void addBuiltins();  // what is written in C+ itself, for a program that uses it. See Builtins.h
    void joinThrowingTasks();
    void joinModuleNames(const std::set<std::string>& modules);
    void addBuiltinSource(const SourceFile& source);
    void addHeaders();  // what the program uses from the C headers it imports. See CHeaders.h
    void rejectNotImported(const Token& name);  // a name a header has, which C+ cannot take
    HeaderSearch headerSearch_;
    bool quietHeaders_ = false;
    bool headerFailed_ = false;  // a header could not be read, so what it declares is not reported missing
    void parseThrowsMark(FuncDecl& fn);
    bool parseAsyncMark(Loc& at);  // `async` before a function's return type, if it is there
    StmtPtr parseTry();
    ExprPtr parseMemberAccess(ExprPtr e, const char* after, std::string qualifier = "");
    CondDecl parseCondDecl(const char* stmt);
    void syncTopLevel();
    void syncStatement();

    // Grammar
    void parseTopLevel(Program& prog);
    void parseConstants(Program& prog, const Type* type, Loc typeLoc, bool isConstexpr);
    // `[[allow(new_delete)]]`, the one attribute C+ has. Returns where it is written.
    Loc parseAttribute();
    bool atAttribute() const { return check(Tok::LBracket) && peek(1).kind == Tok::LBracket; }
    ExprPtr parseNew();
    void parseRecord(StructDecl* into = nullptr);  // struct or class; `into` when the type is made from a generic
    // Generics
    struct TemplateBinding {
        const Type* type = nullptr;  // a type parameter; null for a value parameter
        TemplateArg value;
        const Type* valueType = nullptr;
    };
    // While one generic is read: the old state comes back at the end.
    struct GenericScope {
        Parser& p;
        size_t pos;
        std::unordered_map<std::string, TemplateBinding> bindings;
        const TemplateDecl* current;
        std::vector<TemplateArg> currentArgs;
        const Type* injected;
        bool dryRun;
        size_t splitShr;
        std::vector<std::unordered_map<std::string, const VarDecl*>> localScopes;
        explicit GenericScope(Parser& parser);
        ~GenericScope();
    };
    void scanDeclarations(Program& prog);
    void scanTemplate(Program& prog, size_t& i);
    void parseTemplateHeader(TemplateDecl& t);
    size_t afterBraces(size_t i) const;  // from a '{': the token after the one that closes it
    void readTemplate(TemplateDecl& t);  // once, to report syntax errors and learn the shape of a function's parameters
    void bindTemplate(const TemplateDecl& t, const std::vector<TemplateArg>& args);
    void parseConcept(ConceptBody& out);
    std::string instanceName(const TemplateDecl& t, const std::vector<TemplateArg>& args) const;
    TemplateDecl* classTemplate(const std::string& name) const;
    TemplateDecl* functionTemplate(const std::string& name) const;
    const Type* boundType(const Token& t) const;  // a type parameter, or a generic class's own name inside it
    // At the '<' after a generic's name. `partial` when the rest can be worked out from a call.
    std::vector<TemplateArg> parseTemplateArgs(const TemplateDecl& t, Loc nameLoc, bool partial);
    TemplateArg parseTemplateValue(const TemplateParam& p);
    const Type* genericRecord(TemplateDecl& t, Loc nameLoc);  // at the '<': `Box<int>`
    const Type* instantiateRecord(TemplateDecl& t, const std::vector<TemplateArg>& args, Loc usedAt);
    ExprPtr parseBoundValue(const Token& t, const Type* bound);  // `T(x)`, `T{...}`, `T()`
    void rejectTemplateParamName(const Token& name);
    // Working out values while reading: array sizes and template arguments.
    void readConstants();  // the constants and constexpr functions of the file, wherever they are
    ConstHooks constHooks();
    ConstValue evaluate(const Expr& e, const std::string& what);  // reports what it cannot work out
    bool isConstraintName(const std::string& name) const;
    StmtPtr parseIf(bool isConstexpr, size_t ifPos, Loc loc);
    void parseExtern(Program& prog);
    void parseExternItem(Program& prog);  // a C function, or `struct sqlite3;`
    void parseOpaqueStruct();
    std::unique_ptr<FuncDecl> parseExternFunction();
    const Type* parseCType(Loc& loc, bool isReturn);
    void parseMember(StructDecl& sd, Access& access);
    void parseParams(FuncDecl& fn);
    void parseMemberInits(FuncDecl& ctor);
    bool isTypeName(const Token& t) const;
    bool isTypeStart(size_t ahead = 0) const;
    bool isTypeAt(size_t ahead) const;  // whether a type, rather than an expression, starts there
    // A type, including pointers: `const char*`, `int**`. `isConst` receives a `const`
    // that makes the variable itself const; without it, such a `const` is an error.
    const Type* parseType(Loc& loc, bool* isConst = nullptr);
    // After the first variable of `int* a, * b;`, the stars of the next one, which must
    // repeat the first one's.
    void parseLaterStars(const Type* ty, const std::string& firstName, const char* what);
    void expectCloseAngle(const std::string& context);
    enum class ArrayPlace { Variable, Parameter, Field };
    [[noreturn]] void failCStyleArray(const Type* elem, const std::string& name, ArrayPlace place = ArrayPlace::Variable);
    size_t afterAngles(size_t ahead) const;
    std::unique_ptr<FuncDecl> parseFunction(const Type* retType, Loc retLoc);
    std::unique_ptr<BlockStmt> parseBlock(const FuncDecl* function = nullptr);  // with the parameters of the function it is the body of
    StmtPtr parseStmt();
    StmtPtr parseDeclaration();  // one or more variables, without the trailing ';'
    StmtPtr parseDeclarators(bool isConst, const Type* ty, Loc typeLoc);  // the part after the type
    StmtPtr parseFor();
    bool isValueOfType() const;
    bool isDeclarationStart() const { return isTypeStart() && !isValueOfType(); }
    void checkUnknownType();
    void rejectStructKeyword();  // `struct tm t`, where C+ writes `tm t`
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
    std::unordered_map<size_t, TemplateDecl*> templateAt_;  // by the position of `template`
    std::unordered_map<size_t, size_t> skipTemplate_;       // the same for one with a bad header: where it ends
    std::deque<std::string> names_;                         // what the placeholders' names point to
    std::unordered_map<std::string, TemplateBinding> bindings_;  // the parameters of the generic being read
    const TemplateDecl* currentTemplate_ = nullptr;
    std::vector<TemplateArg> currentArgs_;
    std::vector<size_t> constantStarts_;  // where `const` and `constexpr` begin outside any function
    bool constantsRead_ = false;
    std::unordered_map<std::string, const VarDecl*> constants_;
    std::vector<StmtPtr> constantDecls_;  // what `constants_` points into
    std::unordered_map<std::string, std::unique_ptr<FuncDecl>> constexprFunctions_;
    // The constants and variables of the blocks being read, innermost last.
    std::vector<std::unordered_map<std::string, const VarDecl*>> localScopes_;
    struct ConstexprIf {
        size_t thenStart, elseStart, end;
    };
    std::unordered_map<size_t, ConstexprIf> constexprIfs_;  // from reading a generic's definition, by the `if`
    const Type* injected_ = nullptr;  // inside `Box<T>`: what `Box` alone means
    bool dryRun_ = false;  // reading a definition with parameters not yet known
    std::deque<Type> placeholders_;
    std::deque<TemplatePattern> patterns_;
    int genericDepth_ = 0;
    int instanceCount_ = 0;
    size_t splitShr_ = SIZE_MAX;  // the `>>` whose first half closed an angle bracket
    bool declSeen_ = false;  // a declaration came before; `[[allow(new_delete)]];` must be first
    bool fileAttr_ = false;  // the item just parsed was `[[allow(new_delete)]];`

    // Enums and switch
    void readEnums();  // the enums of the file, before anything else, as their values can be used anywhere
    void parseEnum();  // at `enum`: `enum class Color : long { Red = 1, Green };`
    int64_t enumeratorValue(const Expr& e, const StructDecl& en, int64_t fallback);
    // After the name of an enum in an expression: `Color::Red`, or `Color(n)`. Null for anything else.
    ExprPtr parseEnumUse(const Token& t, const StructDecl& en);
    StmtPtr parseSwitch();
    void parseCaseLabel(SwitchSection& section);  // `case value:` or `default:`, added to the section
    StmtPtr parseFallthrough();  // at `[[fallthrough]]`
    [[noreturn]] void failStrayLabel(const Token& t);  // `case` or `default` that is not directly in a switch
    std::vector<size_t> enumStarts_;  // where `enum` begins outside any function
    std::unordered_map<size_t, size_t> enumEnds_;  // the enums read before the rest, by where they start: where they end
    int switchDepth_ = 0;  // the switches whose sections are being read
};

}  // namespace cplus
