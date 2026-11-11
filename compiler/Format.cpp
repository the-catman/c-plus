// `cplus fmt`: one style for all C+ code, with nothing to choose. It goes by the tokens and the comments, and keeps the
// line breaks a file has, but for the ones the style itself decides: a statement block's braces, one statement to a line,
// `} else {`. The tokens are what the program is, and what is written must lex to the same tokens and comments again,
// or nothing is written. The rules are in "Formatting" in docs/SPEC.md.

#include "Format.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "AST.h"
#include "Lexer.h"
#include "Modules.h"
#include "Parser.h"

namespace cplus {
namespace {

enum class Role : uint8_t {
    None,
    Block,          // `{` or `}` of a body: of a function, a type, `extern "C"` or a statement
    Init,           // `{` or `}` of a list of values: `{1, 2}`
    TemplateOpen,   // `<` of `vector<int>`
    TemplateClose,  // `>` or `>>` that ends one
    Pointer,        // `*` of a type: `int* p`
    Optional,       // `?` of a type: `int? n`
    Prefix,         // `-x`, `!x`, `*p`, `&x`, `++i`
    Postfix,        // `i++`
    Binary,         // everything else between two values: `a + b`, `a ? b : c`
    CastClose,      // `)` of `(int)x`
    AccessColon,    // `:` of `public:`
    CaseColon,      // `:` of `case 1:` or `default:`
};

const std::set<std::string> kTemplates = {"vector", "array", "hashmap", "Task", "unique", "local", "static_cast", "reinterpret_cast", "dynamic_cast",
                                          "const_cast", "same_as", "derived_from", "Comparable", "Number", "Integer", "Floating", "Hashable"};
const std::set<std::string> kTypes = {"vector", "array", "hashmap", "Task", "unique", "local", "Error", "Mutex", "LockGuard"};

bool isTypeKeyword(Tok k) {
    return k == Tok::KwInt || k == Tok::KwLong || k == Tok::KwChar || k == Tok::KwBool || k == Tok::KwDouble || k == Tok::KwFloat ||
           k == Tok::KwVoid || k == Tok::KwString || k == Tok::KwAuto;
}

bool isAccess(Tok k) { return k == Tok::KwPublic || k == Tok::KwPrivate || k == Tok::KwProtected; }

// Whether two tokens written together would read as one, or as other tokens.
bool wouldJoin(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return false;
    const char x = a.back(), y = b.front();
    auto word = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };
    if (word(x) && (word(y) || y == '"' || y == '\'')) return true;
    if (x == '.' && std::isdigit((unsigned char)y)) return true;
    if (std::isdigit((unsigned char)a.front()) && y == '.') return true;  // a number, which the `.` would go on
    static const char* const pairs[] = {"++", "--", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "==", "!=", "<=", ">=", "&&",
                                        "||", "<<", ">>", "??", "?.", "::", "->", "//", "/*", "*/", ".."};
    const char two[3] = {x, y, 0};
    for (const char* p : pairs)
        if (std::strcmp(two, p) == 0) return true;
    return false;
}

// How many columns text takes: its characters, after its last line break.
int widthOf(const std::string& text) {
    size_t start = text.rfind('\n');
    start = start == std::string::npos ? 0 : start + 1;
    int n = 0;
    for (size_t i = start; i < text.size(); ++i)
        if (((unsigned char)text[i] & 0xC0) != 0x80) ++n;
    return n;
}

struct Line {
    int indent = 0;
    std::string code;     // from the indent on
    std::string comment;  // a `//` comment, alone or after the code, where those of the lines next to it line up
    int pad = 2;          // the spaces between the code and the comment
    bool follows = false;  // a comment alone, under the one after the code of the line before, which goes on there
    bool blank = false;
};

class Formatter {
public:
    Formatter(const std::string& src, std::vector<Token> toks, std::vector<Comment> comments)
        : src_(src), toks_(std::move(toks)), comments_(std::move(comments)) {
        if (!toks_.empty() && toks_.back().kind == Tok::Eof) toks_.pop_back();
        const size_t n = toks_.size();
        role_.assign(n, Role::None);
        match_.assign(n, kNone);
        decl_.assign(n, false);
        container_.assign(n, false);
        type_.assign(n, false);
        do_.assign(n, false);
        kept_.assign(n, false);
        tail_.assign(n, false);
        enum_.assign(n, false);
    }

    void knowModuleNames(std::set<std::string> types, std::set<std::string> templates) {
        moduleTypes_ = std::move(types);
        moduleTemplates_ = std::move(templates);
    }

    std::string run() {
        findNames();
        findRoles();
        emit();
        return write();
    }

private:
    static constexpr size_t kNone = (size_t)-1;

    const std::string& src_;
    std::vector<Token> toks_;
    std::vector<Comment> comments_;
    std::vector<Role> role_;
    std::vector<size_t> match_;  // of a bracket: where the other one of the pair is
    std::vector<bool> decl_;       // `{` of the body of a function or a type, or of `extern "C"`, and not of a statement
    std::vector<bool> container_;  // `{` of a type or of `extern "C"`, which holds declarations
    std::vector<bool> type_;       // `{` of a type, whose `public:` lines up with it
    std::vector<bool> do_;         // `{` of a `do`
    std::vector<bool> kept_;       // `{` of a declaration's body, on one line with its `}`, which stays on one line
    std::vector<bool> tail_;       // `(` of the `while` that ends a `do`
    std::vector<bool> enum_;       // `{` of an enum, whose enumerators each start a line, or share one
    std::set<std::string> types_, templates_;
    std::set<std::string> moduleTypes_, moduleTemplates_;  // of the modules the file imports, with their paths: `geo::Box`
    std::vector<Line> lines_;

    std::string text(size_t i) const { return src_.substr(toks_[i].begin, toks_[i].end - toks_[i].begin); }
    Tok kind(size_t i) const { return i < toks_.size() ? toks_[i].kind : Tok::Eof; }
    bool isIdent(size_t i, const char* name) const { return kind(i) == Tok::Ident && toks_[i].text == name; }
    // `case` or the `default` of `default:`, which start a section of a switch.
    bool isLabel(size_t i) const { return kind(i) == Tok::KwCase || (kind(i) == Tok::KwDefault && kind(i + 1) == Tok::Colon); }

    // ---- What the names are ------------------------------------------------

    // The program's types and generics, as all of them are in the file or come with the language.
    void findNames() {
        for (size_t i = 0; i + 1 < toks_.size(); ++i) {
            const Tok k = kind(i);
            if ((k == Tok::KwStruct || k == Tok::KwClass || k == Tok::KwInterface || k == Tok::KwTypename) && kind(i + 1) == Tok::Ident)
                types_.insert(toks_[i + 1].text);
            if (k != Tok::KwTemplate || kind(i + 1) != Tok::Less) continue;
            size_t j = i + 1;
            for (int depth = 0; j < toks_.size(); ++j) {
                depth += kind(j) == Tok::Less ? 1 : kind(j) == Tok::Greater ? -1 : kind(j) == Tok::Shr ? -2 : 0;
                if (depth <= 0 || kind(j) == Tok::Semi || kind(j) == Tok::LBrace) break;
            }
            ++j;  // what the template is
            if ((kind(j) == Tok::KwStruct || kind(j) == Tok::KwClass || kind(j) == Tok::KwInterface) && kind(j + 1) == Tok::Ident) {
                templates_.insert(toks_[j + 1].text);
            } else if (isIdent(j, "concept") && kind(j + 1) == Tok::Ident) {
                templates_.insert(toks_[j + 1].text);
            } else {
                for (; j + 1 < toks_.size() && kind(j) != Tok::Semi && kind(j) != Tok::LBrace; ++j)
                    if (kind(j) == Tok::Ident && kind(j + 1) == Tok::LParen) {
                        templates_.insert(toks_[j].text);
                        break;
                    }
            }
        }
        findTypesInUse();
    }

    // A type that is not defined in the file, such as one from a C header, which is seen to be one where it is used:
    // `FILE* f = ...;`, `tm t;`, `vector<FILE*>`, `(FILE*)p`, and `void f(uv_loop_t* loop)` among declarations.
    void findTypesInUse() {
        std::vector<bool> braces;  // for each `{` that is open, whether declarations are what it holds
        bool typeAhead = false;    // `struct`, `class` or `interface`, whose body has not begun
        for (size_t i = 0; i + 1 < toks_.size(); ++i) {
            const Tok k = kind(i);
            const Tok before = i ? kind(i - 1) : Tok::Eof;
            if (k == Tok::KwStruct || k == Tok::KwClass || k == Tok::KwInterface) typeAhead = true;
            if (k == Tok::Semi) typeAhead = false;
            if (k == Tok::LBrace) {
                braces.push_back(typeAhead || (before == Tok::StringLit && i >= 2 && kind(i - 2) == Tok::KwExtern));
                typeAhead = false;
            }
            if (k == Tok::RBrace && !braces.empty()) braces.pop_back();
            if (k != Tok::Ident || types_.count(toks_[i].text) || before == Tok::Dot || before == Tok::Arrow || before == Tok::QuestionDot) continue;
            size_t j = i + 1;
            while (kind(j) == Tok::Star) ++j;
            if (kind(j) == Tok::Amp) ++j;  // `const Point& p`
            const bool stars = j > i + 1;
            const Tok after = kind(j);
            if (stars && (after == Tok::RParen || after == Tok::Greater || after == Tok::Shr || after == Tok::Comma)) {
                types_.insert(toks_[i].text);  // nothing that multiplies ends there
                continue;
            }
            const Tok end = kind(j + 1);
            if (after != Tok::Ident || !(end == Tok::Assign || end == Tok::Semi || end == Tok::Comma || end == Tok::RParen || end == Tok::Colon ||
                                         end == Tok::LBracket))
                continue;
            const bool startsStatement = i == 0 || before == Tok::Semi || before == Tok::LBrace || before == Tok::RBrace || before == Tok::Colon ||
                                         (before == Tok::LParen && i >= 2 && kind(i - 2) == Tok::KwFor);
            const bool amongDeclarations = braces.empty() || braces.back();
            if (!stars || startsStatement || ((before == Tok::LParen || before == Tok::Comma) && amongDeclarations && inParameters(i)))
                types_.insert(toks_[i].text);
        }
    }

    // Whether the token is in the parameters of a function being declared: `void f(` or `Name(`, and not `x = f(`.
    bool inParameters(size_t i) const {
        int depth = 0;
        size_t j = i;
        while (j-- > 0) {
            const Tok k = kind(j);
            if (k == Tok::RParen) ++depth;
            else if (k == Tok::LParen && depth-- == 0) break;
            else if (k == Tok::Semi || k == Tok::LBrace || k == Tok::RBrace) return false;
        }
        if (j == kNone || j == 0 || kind(j - 1) != Tok::Ident) return false;
        const Tok k = j >= 2 ? kind(j - 2) : Tok::Eof;
        return isTypeKeyword(k) || k == Tok::Ident || k == Tok::Star || k == Tok::Greater || k == Tok::Question || k == Tok::Semi || k == Tok::LBrace ||
               k == Tok::RBrace || k == Tok::Colon || k == Tok::Eof || j == 1;
    }

    bool opensTemplate(size_t i) const {
        if (i == 0) return false;
        const size_t p = i - 1;
        if (kind(p) == Tok::KwTemplate || kind(p) == Tok::KwWeak) return true;
        if (kind(p) != Tok::Ident) return false;
        const std::string& name = toks_[p].text;
        if (templates_.count(name) || kTemplates.count(name)) return true;
        const bool qualified = p >= 2 && kind(p - 1) == Tok::ColonColon && kind(p - 2) == Tok::Ident;
        if (qualified && moduleTemplates_.count(pathTo(p))) return true;
        return qualified && ((name == "parse" && toks_[p - 2].text == "json") || (name == "type" && toks_[p - 2].text == "reflect"));
    }

    // `net::server::Config`, of the name at `i` and what is before it.
    std::string pathTo(size_t i) const {
        std::string path = toks_[i].text;
        for (size_t j = i; j >= 2 && kind(j - 1) == Tok::ColonColon && kind(j - 2) == Tok::Ident; j -= 2) path = toks_[j - 2].text + "::" + path;
        return path;
    }

    // What can come before the `*` or `?` of a type.
    bool isType(size_t i) const {
        if (i == kNone || i >= toks_.size()) return false;
        const Tok k = kind(i);
        if (isTypeKeyword(k)) return true;
        if (k == Tok::Ident) {
            const std::string& name = toks_[i].text;
            if (i >= 2 && kind(i - 1) == Tok::ColonColon && kind(i - 2) == Tok::Ident && std::isupper((unsigned char)toks_[i - 2].text[0]))
                return false;  // `Color::Red`, an enumerator
            if (types_.count(name) || kTypes.count(name)) return true;
            if (i >= 2 && kind(i - 1) == Tok::ColonColon && moduleTypes_.count(pathTo(i))) return true;
            return i >= 1 && kind(i - 1) == Tok::ColonColon && std::isupper((unsigned char)name[0]);  // `http::Response`
        }
        return role_[i] == Role::TemplateClose || (k == Tok::Star && role_[i] == Role::Pointer) || (k == Tok::Question && role_[i] == Role::Optional);
    }

    // What a value ends with, so that a `-` after it is between two values.
    bool endsValue(size_t i) const {
        if (i == kNone || i >= toks_.size()) return false;
        switch (kind(i)) {
        case Tok::Ident: case Tok::IntLit: case Tok::FloatLit: case Tok::StringLit: case Tok::CharLit:
        case Tok::KwThis: case Tok::KwTrue: case Tok::KwFalse: case Tok::KwNull: case Tok::RBracket:
            return true;
        case Tok::RParen:
            return role_[i] != Role::CastClose;
        case Tok::RBrace:
            return role_[i] == Role::Init;
        case Tok::Greater: case Tok::Shr:
            return role_[i] == Role::TemplateClose;
        case Tok::PlusPlus: case Tok::MinusMinus:
            return role_[i] == Role::Postfix;
        default:
            return false;
        }
    }

    bool startsValue(size_t i) const {
        switch (kind(i)) {
        case Tok::Ident: case Tok::IntLit: case Tok::FloatLit: case Tok::StringLit: case Tok::CharLit: case Tok::KwThis: case Tok::KwTrue:
        case Tok::KwFalse: case Tok::KwNull: case Tok::LParen: case Tok::Minus: case Tok::Plus: case Tok::Bang: case Tok::Tilde: case Tok::Star:
        case Tok::Amp: case Tok::PlusPlus: case Tok::MinusMinus: case Tok::KwNew: case Tok::KwSizeof:
            return true;
        default:
            return false;
        }
    }

    // `(int)x`: the parentheses hold only a type, and a value comes after them.
    bool isCast(size_t open, size_t close) const {
        if (open + 1 >= close || !startsValue(close + 1)) return false;
        if (open > 0) {
            const Tok before = kind(open - 1);
            if (endsValue(open - 1) || isTypeKeyword(before) || before == Tok::KwSizeof || before == Tok::KwIf || before == Tok::KwWhile ||
                before == Tok::KwFor || before == Tok::KwSwitch || before == Tok::KwCatch || before == Tok::KwConstexpr)
                return false;
        }
        size_t i = open + 1;
        if (kind(i) == Tok::KwConst) ++i;
        if (isTypeKeyword(kind(i))) {
            ++i;
        } else {
            while (kind(i) == Tok::Ident && kind(i + 1) == Tok::ColonColon) i += 2;
            if (!isType(i)) return false;
            ++i;
        }
        while (i < close && (role_[i] == Role::Pointer || role_[i] == Role::Optional)) ++i;
        return i == close;
    }

    // ---- What each token is ------------------------------------------------

    void findRoles() {
        std::vector<size_t> open;
        bool typeAhead = false;  // a `struct`, `class` or `interface` whose body has not begun
        auto dropTemplates = [&] {  // a `<` that nothing closed was a comparison
            while (!open.empty() && role_[open.back()] == Role::TemplateOpen) {
                role_[open.back()] = Role::Binary;
                open.pop_back();
            }
        };
        auto innermostBlock = [&]() -> size_t {
            for (auto it = open.rbegin(); it != open.rend(); ++it)
                if (role_[*it] == Role::Block) return *it;
            return kNone;
        };
        bool enumAhead = false;      // `enum class`, whose body has not begun
        size_t caseDepth = kNone;    // after `case`: how many brackets are open around it, until its `:`
        int caseQuestions = 0;       // the `?` of a `a ? b : c` in the label, each of which takes a `:`
        for (size_t i = 0; i < toks_.size(); ++i) {
            const Tok k = kind(i);
            const size_t p = i == 0 ? kNone : i - 1;
            const Tok pk = p == kNone ? Tok::Eof : kind(p);
            switch (k) {
            case Tok::LParen:
            case Tok::LBracket:
                open.push_back(i);
                break;
            case Tok::RParen:
            case Tok::RBracket:
                dropTemplates();
                if (!open.empty() && kind(open.back()) == (k == Tok::RParen ? Tok::LParen : Tok::LBracket)) {
                    match_[i] = open.back();
                    match_[open.back()] = i;
                    open.pop_back();
                    if (k == Tok::RParen && isCast(match_[i], i)) role_[i] = Role::CastClose;
                }
                break;
            case Tok::LBrace: {
                const size_t block = innermostBlock();
                const bool inExpression = !open.empty() && role_[open.back()] != Role::Block;
                const bool amongDeclarations = block == kNone || container_[block];
                bool isBlock = false;
                if (inExpression) {
                    isBlock = false;
                } else if (typeAhead) {
                    isBlock = true;
                    container_[i] = type_[i] = true;
                } else if (pk == Tok::StringLit && p >= 1 && kind(p - 1) == Tok::KwExtern) {
                    isBlock = true;
                    container_[i] = true;
                } else if ((pk == Tok::RParen && role_[p] != Role::CastClose) || pk == Tok::KwElse || pk == Tok::KwDo || pk == Tok::KwTry ||
                           pk == Tok::KwConst || pk == Tok::KwOverride || pk == Tok::KwThrows || pk == Tok::RBracket ||
                           (pk == Tok::RBrace && role_[p] == Role::Init)) {
                    isBlock = true;
                    do_[i] = pk == Tok::KwDo;
                } else if (p == kNone || pk == Tok::Semi || (pk == Tok::LBrace && role_[p] == Role::Block) ||
                           (pk == Tok::RBrace && role_[p] == Role::Block) || role_[p] == Role::AccessColon) {
                    isBlock = true;  // a block of its own
                } else if (role_[p] == Role::CaseColon) {
                    isBlock = true;  // `case 1: {`
                }
                enum_[i] = isBlock && typeAhead && enumAhead;
                enumAhead = false;
                typeAhead = false;
                role_[i] = isBlock ? Role::Block : Role::Init;
                decl_[i] = isBlock && amongDeclarations;
                open.push_back(i);
                break;
            }
            case Tok::RBrace:
                dropTemplates();
                if (!open.empty() && kind(open.back()) == Tok::LBrace) {
                    const size_t o = open.back();
                    match_[i] = o;
                    match_[o] = i;
                    role_[i] = role_[o];
                    kept_[o] = decl_[o] && src_.find('\n', toks_[o].begin) >= toks_[i].begin;
                    open.pop_back();
                }
                break;
            case Tok::Less:
                if (opensTemplate(i)) {
                    role_[i] = Role::TemplateOpen;
                    open.push_back(i);
                } else {
                    role_[i] = Role::Binary;
                }
                break;
            case Tok::Greater:
            case Tok::Shr:
                if (!open.empty() && role_[open.back()] == Role::TemplateOpen) {
                    role_[i] = Role::TemplateClose;
                    match_[i] = open.back();
                    match_[open.back()] = i;
                    open.pop_back();
                    if (k == Tok::Shr && !open.empty() && role_[open.back()] == Role::TemplateOpen) open.pop_back();
                } else {
                    role_[i] = Role::Binary;
                }
                break;
            case Tok::Semi:
                dropTemplates();
                typeAhead = false;
                enumAhead = false;
                break;
            case Tok::KwCase:
                caseDepth = open.size();
                caseQuestions = 0;
                break;
            case Tok::Star:
                role_[i] = isType(p) ? Role::Pointer : endsValue(p) ? Role::Binary : Role::Prefix;
                break;
            case Tok::Question:
                role_[i] = isType(p) ? Role::Optional : Role::Binary;
                if (role_[i] == Role::Binary && caseDepth == open.size()) ++caseQuestions;
                break;
            case Tok::Amp:  // `const Point& p` is a reference, like the `*` of a pointer
                role_[i] = isType(p) ? Role::Pointer : endsValue(p) ? Role::Binary : Role::Prefix;
                break;
            case Tok::Minus:
            case Tok::Plus:
                role_[i] = endsValue(p) ? Role::Binary : Role::Prefix;
                break;
            case Tok::PlusPlus:
            case Tok::MinusMinus:
                role_[i] = endsValue(p) ? Role::Postfix : Role::Prefix;
                break;
            case Tok::Bang:
            case Tok::Tilde:
                role_[i] = Role::Prefix;
                break;
            case Tok::Colon:
                if (pk == Tok::KwDefault || (caseDepth == open.size() && caseQuestions == 0)) {
                    role_[i] = Role::CaseColon;
                    caseDepth = kNone;
                    break;
                }
                if (caseDepth == open.size()) --caseQuestions;
                role_[i] = isAccess(pk) ? Role::AccessColon : Role::Binary;
                break;
            case Tok::KwStruct:
            case Tok::KwClass:
            case Tok::KwInterface:
                if (kind(i + 1) == Tok::Ident && (open.empty() || role_[open.back()] == Role::Block)) typeAhead = true;
                enumAhead = typeAhead && pk == Tok::KwEnum;
                break;
            default:
                break;
            }
        }
    }

    // ---- Spaces between tokens on a line ------------------------------------

    bool spaceBetween(size_t a, size_t b) const {
        const Tok ak = kind(a), bk = kind(b);
        const Role ar = role_[a], br = role_[b];
        if (wouldJoin(text(a), text(b))) return true;
        if (bk == Tok::Comma || bk == Tok::Semi) return false;
        if (ak == Tok::Comma) return true;
        if (ak == Tok::Semi) return bk != Tok::RParen;
        if (ak == Tok::LParen || ak == Tok::LBracket || bk == Tok::RParen || bk == Tok::RBracket) return false;
        if ((ak == Tok::LBrace && ar == Role::Init) || (bk == Tok::RBrace && br == Role::Init)) return false;
        if (ak == Tok::Dot || ak == Tok::Arrow || ak == Tok::QuestionDot || ak == Tok::ColonColon) return false;
        if (bk == Tok::Dot || bk == Tok::Arrow || bk == Tok::QuestionDot || bk == Tok::ColonColon) return false;
        if (ak == Tok::LBrace) return !(bk == Tok::RBrace && match_[a] == b);  // `{}` is empty, and `{ x }` is not
        if ((bk == Tok::RBrace && br == Role::Block) || (ak == Tok::RBrace && ar == Role::Block)) return true;
        if (bk == Tok::LBrace) return br == Role::Block || !(ak == Tok::Ident || ar == Role::TemplateClose || isTypeKeyword(ak));
        if (ar == Role::TemplateOpen || br == Role::TemplateOpen || br == Role::TemplateClose) return false;
        if (br == Role::Pointer || br == Role::Optional || br == Role::Postfix || br == Role::AccessColon) return false;
        if (br == Role::CaseColon) return false;
        if (ar == Role::TemplateClose) return bk != Tok::LParen;
        if (ar == Role::Prefix || ar == Role::CastClose) return false;
        if (bk == Tok::LParen)
            return !(ak == Tok::Ident || ak == Tok::RParen || ak == Tok::RBracket || isTypeKeyword(ak) || ak == Tok::KwSizeof || ar == Role::Pointer ||
                     ar == Role::Optional);  // `new T?(x)`
        if (bk == Tok::LBracket) return !(endsValue(a) || isTypeKeyword(ak) || ak == Tok::KwDelete || ar == Role::Pointer);  // `new int*[n]`
        return true;
    }

    // ---- Lines -------------------------------------------------------------

    enum class Frame { Paren, Bracket, Init, Template, Block, Body };  // Body: of an `if` with no braces, on the lines after it

    struct Open {
        Frame frame;
        size_t at = kNone;
        int stmtIndent = 0;  // of the statement it is in
        int lineIndent = 0;  // of the line it is on
        int column = 0;      // just after it
        bool contentAfter = false;  // something came after it on its line
        bool kept = false;
        bool type = false;
        bool isDo = false;
        int indent = 0;  // of a Body
        bool isCase = false;  // a Body that is the statements after a `case 1:`, up to the next label or the switch's `}`
    };

    struct Item {
        bool comment;
        size_t index;
        size_t begin, end;
    };

    void emit() {
        std::vector<Item> items;
        for (size_t i = 0; i < toks_.size(); ++i) items.push_back({false, i, toks_[i].begin, toks_[i].end});
        for (size_t i = 0; i < comments_.size(); ++i) items.push_back({true, i, comments_[i].begin, comments_[i].end});
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.begin < b.begin; });

        std::vector<Open> opens;
        Line cur;
        bool lineOpen = false;
        int stmtIndent = 0;
        bool stmtStart = true;
        bool mustBreak = false;
        bool doTail = false;
        int keptDepth = 0;

        auto innerIndent = [&] {
            for (auto it = opens.rbegin(); it != opens.rend(); ++it) {
                if (it->frame == Frame::Block) return it->stmtIndent + 4;
                if (it->frame == Frame::Body) return it->indent;
            }
            return 0;
        };
        auto column = [&] { return cur.code.find('\n') == std::string::npos ? cur.indent + widthOf(cur.code) : widthOf(cur.code); };
        auto flush = [&] {
            if (lineOpen) lines_.push_back(cur);
            cur = Line();
            lineOpen = false;
        };
        // When a statement ends: the bodies it ends end with it, all of them unless the statement goes on.
        auto endBodies = [&](size_t next) {
            while (!opens.empty() && opens.back().frame == Frame::Body && !opens.back().isCase) {
                const bool fromDo = opens.back().isDo;
                opens.pop_back();
                if (fromDo) {
                    doTail = true;
                    return;
                }
                if (kind(next) == Tok::KwElse) return;
            }
        };

        for (size_t n = 0; n < items.size(); ++n) {
            const Item& item = items[n];
            const Item* prev = n == 0 ? nullptr : &items[n - 1];
            const size_t newlines = prev ? (size_t)std::count(src_.begin() + prev->end, src_.begin() + item.begin, '\n') : 0;
            const bool prevLineComment = prev && prev->comment && src_.compare(prev->begin, 2, "//") == 0;
            const bool prevToken = prev && !prev->comment;
            const size_t t = item.comment ? kNone : item.index;
            const Tok k = item.comment ? Tok::Eof : kind(t);
            const Role r = item.comment ? Role::None : role_[t];

            // Whether it goes on a line of its own.
            bool join = false, forced = false;
            if (!item.comment) {
                const bool afterBlockClose = prevToken && kind(prev->index) == Tok::RBrace && role_[prev->index] == Role::Block;
                const bool aloneBlock = prevToken && (kind(prev->index) == Tok::Semi || role_[prev->index] == Role::AccessColon ||
                                                      role_[prev->index] == Role::Block);
                if (k == Tok::LBrace && r == Role::Block && !prevLineComment && prevToken && !aloneBlock) join = true;
                if ((k == Tok::KwElse || k == Tok::KwCatch) && afterBlockClose) join = true;
                if (k == Tok::KwWhile && afterBlockClose && do_[match_[prev->index]]) join = true;
                if (k == Tok::RBrace && r == Role::Block) {
                    const size_t o = match_[t];
                    const bool empty = o != kNone && prevToken && prev->index == o && newlines == 0;  // `{` and `}` on lines of their own stay so
                    if (empty || keptDepth > 0) join = true;
                    else forced = true;
                }
                forced = forced || mustBreak;
            }
            const bool breakBefore = prev && ((newlines > 0 || forced) && !join);
            // A label ends the section before it, and so does a comment on the lines just above one.
            if (!opens.empty() && opens.back().isCase && (item.comment ? breakBefore && stmtStart && labelAfter(items, n) : isLabel(t)))
                opens.pop_back();

            if (breakBefore) {
                const bool userBlank = newlines > 1;
                const bool afterOpen = prevToken && kind(prev->index) == Tok::LBrace && role_[prev->index] == Role::Block;
                const bool beforeClose = k == Tok::RBrace && r == Role::Block;
                flush();
                if (userBlank && !afterOpen && !beforeClose && !lines_.empty()) {
                    Line blank;
                    blank.blank = true;
                    lines_.push_back(blank);
                }
                // Its indent.
                int indent;
                if (k == Tok::RBrace && r == Role::Block) {
                    while (!opens.empty() && opens.back().frame == Frame::Body) opens.pop_back();
                    indent = opens.empty() ? 0 : opens.back().stmtIndent;
                } else if (!item.comment && isAccess(k) && role_.size() > t + 1 && role_[t + 1] == Role::AccessColon) {
                    indent = innerIndent() - 4;
                    for (auto it = opens.rbegin(); it != opens.rend(); ++it)
                        if (it->frame == Frame::Block) {
                            if (it->type) indent = it->stmtIndent;
                            break;
                        }
                } else if (stmtStart) {
                    indent = innerIndent();
                } else if (k == Tok::LBrace && r == Role::Block) {
                    indent = stmtIndent;  // under a line comment that its header ends with
                } else if (!opens.empty() && opens.back().frame != Frame::Block && opens.back().frame != Frame::Body) {
                    const Open& o = opens.back();
                    if (!item.comment && match_[o.at] == t) {
                        indent = o.lineIndent;
                    } else if (o.contentAfter) {
                        indent = o.column;
                    } else {
                        indent = o.lineIndent + 4;
                        if (indent == stmtIndent + 4 && inHeader(opens)) indent += 4;  // so it does not look like the body
                    }
                } else {
                    indent = stmtIndent + 4;
                }
                cur.indent = std::max(indent, 0);
                lineOpen = true;
            } else if (!lineOpen) {
                cur.indent = 0;
                lineOpen = true;
            } else if (item.comment) {
                // after code, below
            } else if (prevToken && spaceBetween(prev->index, t)) {
                cur.code += ' ';
            } else if (!prevToken) {
                cur.code += ' ';  // after a /* */ comment
            }

            if (item.comment) {
                std::string c = src_.substr(item.begin, item.end - item.begin);
                const bool line = c.compare(0, 2, "//") == 0;
                if (line && !c.empty() && c.back() == '\r') c.pop_back();  // what it says is left as it is, even a space at its end
                if (line && (cur.code.empty() || !breakBefore)) {
                    cur.comment = c;
                    const bool underLast = breakBefore && newlines == 1 && prevLineComment && !lines_.empty() && !lines_.back().comment.empty() &&
                                           (!lines_.back().code.empty() || lines_.back().follows);
                    cur.follows = underLast && sourceColumn(item.begin) == sourceColumn(prev->begin);
                } else {
                    if (!cur.code.empty()) cur.code += ' ';
                    cur.code += c;
                    if (breakBefore) mustBreak = false;  // what comes after `/* this */` on its line stays there
                }
                continue;
            }

            if (!opens.empty() && opens.back().at + 1 == t && !breakBefore && opens.back().frame != Frame::Block && opens.back().frame != Frame::Body)
                opens.back().contentAfter = true;
            if (stmtStart && !(k == Tok::RBrace && r == Role::Block)) {
                stmtIndent = cur.indent;  // after `for (...)` on the same line, too, its block closes under the `for`
                stmtStart = false;
            }
            if (!breakBefore && prevToken && role_[prev->index] == Role::CaseColon && k != Tok::LBrace && !opens.empty() && opens.back().isCase)
                stmtIndent = opens.back().indent;  // a statement on its label's line is one of the section's, but a `{` closes under the label
            cur.code += text(t);
            mustBreak = false;

            const size_t next = t + 1;
            switch (k) {
            case Tok::LParen:
            case Tok::LBracket:
                opens.push_back({k == Tok::LParen ? Frame::Paren : Frame::Bracket, t, stmtIndent, cur.indent, column()});
                break;
            case Tok::Less:
                if (r == Role::TemplateOpen) opens.push_back({Frame::Template, t, stmtIndent, cur.indent, column()});
                break;
            case Tok::LBrace:
                if (r == Role::Init) {
                    opens.push_back({Frame::Init, t, stmtIndent, cur.indent, column()});
                } else {
                    Open o{Frame::Block, t, stmtIndent, cur.indent, column()};
                    o.kept = kept_[t] || keptDepth > 0;
                    o.type = type_[t];
                    o.isDo = do_[t];
                    if (o.kept) ++keptDepth;
                    const bool empty = next < toks_.size() && match_[t] == next && !commentBetween(t, next);
                    mustBreak = !o.kept && !empty;
                    stmtStart = true;
                    opens.push_back(o);
                }
                break;
            case Tok::RParen:
            case Tok::RBracket:
            case Tok::Greater:
            case Tok::Shr:
            case Tok::RBrace: {
                if (k == Tok::RBrace && r == Role::Block) {
                    while (!opens.empty() && opens.back().frame != Frame::Block) opens.pop_back();
                    if (opens.empty()) break;
                    const Open o = opens.back();
                    opens.pop_back();
                    if (o.kept) --keptDepth;
                    stmtIndent = o.stmtIndent;
                    const Tok nk = kind(next);
                    const bool doWhile = o.isDo && nk == Tok::KwWhile;
                    if (doWhile && kind(next + 1) == Tok::LParen) tail_[next + 1] = true;
                    const bool goesOn = nk == Tok::KwElse || nk == Tok::KwCatch || doWhile || nk == Tok::Semi || nk == Tok::Comma || nk == Tok::RParen;
                    mustBreak = keptDepth == 0 && !goesOn && next < toks_.size();
                    if (!goesOn) {
                        stmtStart = true;
                        endBodies(next);
                    }
                    break;
                }
                if ((k == Tok::Greater || k == Tok::Shr) && r != Role::TemplateClose) break;
                const Frame want = k == Tok::RParen ? Frame::Paren : k == Tok::RBracket ? Frame::Bracket : k == Tok::RBrace ? Frame::Init : Frame::Template;
                int pops = k == Tok::Shr ? 2 : 1;
                while (pops > 0 && !opens.empty() && opens.back().frame == want) {
                    opens.pop_back();
                    --pops;
                }
                if (k == Tok::RParen && match_[t] != kNone) {
                    const size_t o = match_[t];
                    const Tok before = o > 0 ? kind(o - 1) : Tok::Eof;
                    const bool control = before == Tok::KwIf || before == Tok::KwFor || before == Tok::KwWhile || before == Tok::KwSwitch ||
                                         before == Tok::KwCatch || (before == Tok::KwConstexpr && o >= 2 && kind(o - 2) == Tok::KwIf);
                    if (control && !tail_[o] && kind(next) != Tok::LBrace && keptDepth == 0) {
                        Open body{Frame::Body};
                        body.indent = stmtIndent + 4;
                        opens.push_back(body);
                        stmtStart = true;
                    }
                }
                if (r == Role::TemplateClose && match_[t] != kNone && match_[t] > 0 && kind(match_[t] - 1) == Tok::KwTemplate)
                    stmtStart = true;  // what the template header is for begins the same as the header
                if (k == Tok::RBracket && match_[t] != kNone && kind(match_[t] + 1) == Tok::LBracket && match_[match_[t] + 1] + 1 == t)
                    stmtStart = true;  // the same for `[[allow(new_delete)]]`
                break;
            }
            case Tok::Semi:
                if (!opens.empty() && opens.back().frame != Frame::Block && opens.back().frame != Frame::Body) break;
                stmtStart = true;
                doTail = false;
                endBodies(next);
                mustBreak = keptDepth == 0 && next < toks_.size() && kind(next) != Tok::KwElse && !doTail;
                break;
            case Tok::KwElse:
            case Tok::KwDo:
                if (kind(next) != Tok::LBrace && !(k == Tok::KwElse && kind(next) == Tok::KwIf) && keptDepth == 0) {
                    Open body{Frame::Body};
                    body.indent = stmtIndent + 4;
                    body.isDo = k == Tok::KwDo;
                    opens.push_back(body);
                    stmtStart = true;
                }
                break;
            case Tok::KwWhile:
                if (doTail && kind(next) == Tok::LParen) tail_[next] = true;
                doTail = false;
                break;
            case Tok::Colon:
                if (r == Role::AccessColon) {
                    mustBreak = keptDepth == 0;
                    stmtStart = true;
                }
                if (r == Role::CaseColon && keptDepth == 0) {  // what comes after it is one level in, or on its line
                    Open section{Frame::Body};
                    section.indent = stmtIndent + 4;
                    section.isCase = true;
                    opens.push_back(section);
                    stmtStart = true;
                    mustBreak = statementsOnLine(next) > 1;  // one statement stays on the label's line, and more go under it
                }
                break;
            case Tok::Comma:  // in an enum, a line starts with an enumerator
                if (!opens.empty() && opens.back().frame == Frame::Block && enum_[opens.back().at]) stmtStart = true;
                break;
            default:
                break;
            }
        }
        flush();
        while (!lines_.empty() && lines_.back().blank) lines_.pop_back();
    }

    // How many statements end on the line of the token before `from`, from there on, outside brackets and braces.
    int statementsOnLine(size_t from) const {
        int count = 0, depth = 0;
        for (size_t j = from; j > 0 && j < toks_.size() && src_.find('\n', toks_[j - 1].end) >= toks_[j].begin; ++j) {
            const Tok k = kind(j);
            if (k == Tok::LParen || k == Tok::LBracket || k == Tok::LBrace) ++depth;
            if (k == Tok::RParen || k == Tok::RBracket || k == Tok::RBrace) --depth;
            if (k == Tok::Semi && depth == 0) ++count;
        }
        return count;
    }

    // Whether the first token after the comments that follow item n is a label.
    bool labelAfter(const std::vector<Item>& items, size_t n) const {
        for (size_t m = n + 1; m < items.size(); ++m)
            if (!items[m].comment) return isLabel(items[m].index);
        return false;
    }

    // Inside the parentheses of an `if`, `for`, `while` or `switch`.
    bool inHeader(const std::vector<Open>& opens) const {
        for (const Open& o : opens) {
            if (o.frame != Frame::Paren || o.at == 0) continue;
            const Tok before = kind(o.at - 1);
            if (before == Tok::KwIf || before == Tok::KwFor || before == Tok::KwWhile || before == Tok::KwSwitch ||
                (before == Tok::KwConstexpr && o.at >= 2 && kind(o.at - 2) == Tok::KwIf))
                return true;
        }
        return false;
    }

    // The column of a place in the file, counting characters.
    int sourceColumn(size_t at) const {
        const size_t start = src_.rfind('\n', at == 0 ? 0 : at - 1);
        return widthOf(src_.substr(start == std::string::npos ? 0 : start + 1, at - (start == std::string::npos ? 0 : start + 1)));
    }

    bool commentBetween(size_t a, size_t b) const {
        for (const Comment& c : comments_)
            if (c.begin >= toks_[a].end && c.end <= toks_[b].begin) return true;
        return false;
    }

    std::string write() {
        // `//` comments after code, on lines next to each other, line up.
        // A comment alone on its line, that was under such a comment, stays under it.
        auto aligned = [](const Line& l) {
            return !l.blank && !l.comment.empty() && ((!l.code.empty() && l.code.find('\n') == std::string::npos) || l.follows);
        };
        for (size_t i = 0; i < lines_.size();) {
            if (!aligned(lines_[i]) || lines_[i].follows) {
                lines_[i].follows = false;  // there is nothing for it to be under
                ++i;
                continue;
            }
            size_t end = i;
            int width = 0;
            while (end < lines_.size() && aligned(lines_[end])) {
                if (!lines_[end].follows) width = std::max(width, lines_[end].indent + widthOf(lines_[end].code));
                ++end;
            }
            for (; i < end; ++i) lines_[i].pad = width + 2 - (lines_[i].follows ? 0 : lines_[i].indent + widthOf(lines_[i].code));
        }
        std::string out;
        for (const Line& l : lines_) {
            if (l.follows) {
                out += std::string(l.pad, ' ') + l.comment;
            } else if (!l.blank) {
                std::string code = l.code;
                code.erase(code.find_last_not_of(' ') + 1);
                out += std::string(l.indent, ' ') + code;
                if (!l.comment.empty()) out += (code.empty() ? "" : std::string(l.pad, ' ')) + l.comment;
            }
            out += '\n';
        }
        return out;
    }
};

std::vector<Token> lexWithComments(const SourceFile& file, Diagnostics& diags, std::vector<Comment>& comments) {
    Lexer lexer(file, diags);
    lexer.keepComments(&comments);
    return lexer.lexAll();
}

std::string trimmed(std::string s) {
    if (!s.empty() && s.back() == '\r') s.pop_back();
    return s;
}

}  // namespace

FormatResult formatSource(const SourceFile& file, Diagnostics& diags, std::string& out, const std::string& top) {
    std::vector<Comment> comments;
    std::vector<Token> toks = lexWithComments(file, diags, comments);
    if (diags.hasErrors()) return FormatResult::HasErrors;
    std::set<std::string> moduleTypes, moduleTemplates;  // `geo::Point`, `geo::Box`, as other files write them
    {
        Modules modules;
        Modules::Options options;
        options.quiet = true;
        const Modules::Place place = Modules::placeOf(file, top, options);
        options.root = place.root;
        diags.onlyGivenFile();
        modules.load(file, place.moduleName, diags, options);
        for (size_t f = 1; f < modules.files.size(); ++f)
            for (const auto& [name, member] : modules.files[f].members) {
                if (member.isTemplate) moduleTemplates.insert(modules.files[f].prefix + "::" + name);
                else if (member.isType) moduleTypes.insert(modules.files[f].prefix + "::" + name);
            }
        if (modules.incomplete) diags.mute();  // what the module that is not there has is not known, which is not the file's error
        Parser parser(modules.tokens(), diags);
        parser.useModules(&modules);
        parser.findHeadersIn(HeaderSearch{std::filesystem::path(file.path).parent_path().string(), {}});
        parser.quietHeaders();
        Program program;
        parser.parseProgram(program);
        if (modules.incomplete) diags.unmute();
        if (diags.hasErrors() || parser.gaveUp()) return FormatResult::HasErrors;
    }
    Formatter formatter(file.text, toks, comments);
    formatter.knowModuleNames(moduleTypes, moduleTemplates);
    out = formatter.run();
    if (std::getenv("CPLUS_FMT_BUG_FOR_TESTS")) out += "0\n";  // so the tests reach what a bug in the formatter does

    // What it wrote is the same program, with the same comments.
    SourceFile again(file.path, out);
    Diagnostics quiet(again);
    quiet.mute();
    std::vector<Comment> commentsAgain;
    std::vector<Token> toksAgain = lexWithComments(again, quiet, commentsAgain);
    if (quiet.mutedErrors() > 0 || toksAgain.size() != toks.size() || commentsAgain.size() != comments.size()) return FormatResult::WouldChange;
    for (size_t i = 0; i < toks.size(); ++i)
        if (toks[i].kind != toksAgain[i].kind ||
            file.text.compare(toks[i].begin, toks[i].end - toks[i].begin, out, toksAgain[i].begin, toksAgain[i].end - toksAgain[i].begin) != 0)
            return FormatResult::WouldChange;
    for (size_t i = 0; i < comments.size(); ++i)
        if (trimmed(file.text.substr(comments[i].begin, comments[i].end - comments[i].begin)) !=
            trimmed(out.substr(commentsAgain[i].begin, commentsAgain[i].end - commentsAgain[i].begin)))
            return FormatResult::WouldChange;
    return FormatResult::Formatted;
}

}  // namespace cplus
