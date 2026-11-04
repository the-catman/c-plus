#include "Lexer.h"

#include <cctype>
#include <unordered_map>

#include "SpecRefs.h"

namespace cplus {

const char* tokSpelling(Tok kind) {
    switch (kind) {
        case Tok::Eof: return "end of file";
        case Tok::Ident: return "identifier";
        case Tok::IntLit: return "number";
        case Tok::StringLit: return "string";
#define X(name, text) \
    case Tok::Kw##name: return text;
            CPLUS_KEYWORDS(X)
#undef X
#define X(name, text) \
    case Tok::name: return text;
            CPLUS_PUNCTUATORS(X)
#undef X
    }
    return "?";
}

std::string describeToken(const Token& t) {
    switch (t.kind) {
        case Tok::Eof: return "end of file";
        case Tok::Ident: return "identifier '" + t.text + "'";
        case Tok::IntLit: return "number " + t.text;
        case Tok::StringLit: return "a string";
        default: break;
    }
    if (t.kind >= Tok::KwAsync && t.kind <= Tok::KwWhile) return std::string("keyword '") + tokSpelling(t.kind) + "'";
    return std::string("'") + tokSpelling(t.kind) + "'";
}

Lexer::Lexer(const SourceFile& file, Diagnostics& diags) : src_(file.text), diags_(diags) {}

char Lexer::peek(size_t ahead) const { return pos_ + ahead < src_.size() ? src_[pos_ + ahead] : '\0'; }

char Lexer::advance() {
    char c = src_[pos_++];
    if (c == '\n') {
        ++line_;
        col_ = 1;
    } else {
        ++col_;
    }
    return c;
}

std::vector<Token> Lexer::lexAll() {
    std::vector<Token> out;
    while (true) {
        out.push_back(next());
        if (out.back().kind == Tok::Eof) return out;
    }
}

void Lexer::skipSpaceAndComments() {
    while (pos_ < src_.size()) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
            advance();
        } else if (c == '/' && peek(1) == '/') {
            while (pos_ < src_.size() && peek() != '\n') advance();
        } else if (c == '/' && peek(1) == '*') {
            Loc start = locHere(2);
            advance();
            advance();
            while (pos_ < src_.size() && !(peek() == '*' && peek(1) == '/')) advance();
            if (pos_ >= src_.size()) {
                diags_.error(start, "this /* comment is never closed", "add */ where the comment should end",
                             spec::Comments);
                return;
            }
            advance();
            advance();
        } else if (c == '#') {
            Loc start = locHere();
            size_t end = src_.find('\n', pos_);
            std::string directive = src_.substr(pos_, end == std::string::npos ? std::string::npos : end - pos_);
            start.len = (uint32_t)directive.size();
            std::string help = "C+ has no preprocessor.";
            if (directive.rfind("#include", 0) == 0)
                help += " Built-ins like println, string and vector need no include; use `import name;` for modules";
            else if (directive.rfind("#define", 0) == 0)
                help += " Use a `const` variable or a function instead of #define";
            diags_.error(start, "'#' directives are not part of C+", help, spec::Preprocessor);
            while (pos_ < src_.size() && peek() != '\n') advance();
        } else {
            return;
        }
    }
}

Token Lexer::next() {
    skipSpaceAndComments();
    if (pos_ >= src_.size()) return Token{Tok::Eof, locHere(), "", 0};

    char c = peek();
    if (std::isdigit((unsigned char)c)) return lexNumber();
    if (std::isalpha((unsigned char)c) || c == '_') return lexIdentOrKeyword();
    if (c == '"') return lexString();
    if (c == '\'') {
        Loc loc = locHere();
        diags_.error(loc, "character literals like 'a' are not supported yet",
                     "use a string such as \"a\" for now", spec::Status);
        advance();
        while (pos_ < src_.size() && peek() != '\'' && peek() != '\n') advance();
        if (peek() == '\'') advance();
        return next();
    }

    static const struct {
        const char* text;
        Tok kind;
    } puncts[] = {
#define X(name, text) {text, Tok::name},
        CPLUS_PUNCTUATORS(X)
#undef X
    };
    for (const auto& p : puncts) {
        size_t n = std::char_traits<char>::length(p.text);
        if (src_.compare(pos_, n, p.text) == 0) {
            Token t{p.kind, locHere((uint32_t)n), p.text, 0};
            for (size_t i = 0; i < n; ++i) advance();
            return t;
        }
    }

    Loc loc = locHere();
    std::string shown = (unsigned char)c >= 0x80 ? "a non-ASCII character" : std::string("'") + c + "'";
    diags_.error(loc, "unexpected character " + shown + " in source",
                 "identifiers and operators use ASCII; non-ASCII text belongs inside a string");
    advance();
    while (pos_ < src_.size() && ((unsigned char)peek() & 0xC0) == 0x80) advance();  // rest of a UTF-8 sequence
    return next();
}

Token Lexer::lexIdentOrKeyword() {
    static const std::unordered_map<std::string, Tok> keywords = {
#define X(name, text) {text, Tok::Kw##name},
        CPLUS_KEYWORDS(X)
#undef X
    };
    Loc loc = locHere();
    size_t start = pos_;
    while (std::isalnum((unsigned char)peek()) || peek() == '_') advance();
    std::string text = src_.substr(start, pos_ - start);
    loc.len = (uint32_t)text.size();
    auto it = keywords.find(text);
    return Token{it == keywords.end() ? Tok::Ident : it->second, loc, text, 0};
}

Token Lexer::lexNumber() {
    Loc loc = locHere();
    size_t start = pos_;
    int base = 10;
    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
        base = 16;
        advance();
        advance();
    } else if (peek() == '0' && (peek(1) == 'b' || peek(1) == 'B')) {
        base = 2;
        advance();
        advance();
    } else if (peek() == '0' && (peek(1) == 'o' || peek(1) == 'O')) {
        base = 8;
        advance();
        advance();
    }

    uint64_t value = 0;
    bool overflow = false;
    int digits = 0;
    bool leadingZero = base == 10 && peek() == '0' && std::isdigit((unsigned char)peek(1));
    while (true) {
        char c = peek();
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else if (c == '\'' && digits > 0 && std::isxdigit((unsigned char)peek(1))) { advance(); continue; }  // 1'000'000
        else break;
        if (d >= base) break;
        if (__builtin_mul_overflow(value, (uint64_t)base, &value) || __builtin_add_overflow(value, (uint64_t)d, &value))
            overflow = true;
        ++digits;
        advance();
    }
    if (std::isdigit((unsigned char)peek())) {
        Loc digitLoc = locHere();
        char bad = peek();
        while (std::isdigit((unsigned char)peek())) advance();
        const char* baseName = base == 2 ? "a binary" : base == 8 ? "an octal" : "a decimal";
        diags_.error(digitLoc, std::string("'") + bad + "' is not a valid digit in " + baseName + " number",
                     base == 2 ? "binary numbers use only 0 and 1" : "octal numbers use only the digits 0 to 7",
                     spec::IntegerLiterals);
    }
    std::string text = src_.substr(start, pos_ - start);
    loc.len = (uint32_t)text.size();

    if (digits == 0) {
        diags_.error(loc, "'" + text + "' has no digits after the prefix", "write a number such as 0xFF, 0o17 or 0b101",
                     spec::IntegerLiterals);
    } else if (leadingZero) {
        std::string digitsOnly = text.substr(text.find_first_not_of("0'"));
        diags_.error(loc, "numbers cannot start with 0 in C+",
                     "C and C++ silently read '" + text + "' as octal. Write 0o" + digitsOnly +
                         " for octal, or " + digitsOnly + " for decimal",
                     spec::IntegerLiterals);
    } else if (overflow) {
        diags_.error(loc, "the number " + text + " is too large for any integer type",
                     "the largest integer type is long, which holds up to 9223372036854775807",
                     spec::IntegerLiterals);
    }

    if (peek() == '.' && std::isdigit((unsigned char)peek(1))) {
        while (std::isdigit((unsigned char)peek()) || peek() == '.') advance();
        loc.len = (uint32_t)(pos_ - start);
        diags_.error(loc, "floating-point numbers are not supported yet", "use int or long for now", spec::Status);
    } else if (std::isalpha((unsigned char)peek()) || peek() == '_') {
        Loc suffixLoc = locHere();
        size_t s = pos_;
        while (std::isalnum((unsigned char)peek()) || peek() == '_') advance();
        std::string suffix = src_.substr(s, pos_ - s);
        suffixLoc.len = (uint32_t)suffix.size();
        std::string upper;
        for (char c : suffix) upper += (char)std::toupper((unsigned char)c);
        if (upper == "L" || upper == "LL") {
            Token t{Tok::IntLit, loc, src_.substr(start, pos_ - start), value};
            t.loc.len = (uint32_t)t.text.size();
            t.longSuffix = true;
            return t;
        }
        if (upper == "U" || upper == "UL" || upper == "ULL" || upper == "LU" || upper == "LLU")
            diags_.error(suffixLoc, "this compiler does not support unsigned integer types yet",
                         "use a plain number, or L for a long", spec::Status);
        else if (upper == "F")
            diags_.error(suffixLoc, "this compiler does not support floating-point numbers yet",
                         "use int or long for now", spec::Status);
        else
            diags_.error(suffixLoc, "'" + suffix + "' is not a number suffix",
                         "the only suffix is L, which makes a number a long, e.g. `1L << 40`",
                         spec::IntegerLiterals);
    }
    return Token{Tok::IntLit, loc, text, value};
}

Token Lexer::lexString() {
    Loc loc = locHere();
    advance();  // opening quote
    std::string value;
    while (true) {
        if (pos_ >= src_.size() || peek() == '\n') {
            diags_.error(loc, "this string is never closed", "add a closing \" on the same line",
                         spec::StringLiterals);
            break;
        }
        char c = advance();
        if (c == '"') break;
        if (c != '\\') {
            value += c;
            continue;
        }
        Loc escLoc{line_, col_ - 1, 2};
        char e = pos_ < src_.size() ? advance() : '\0';
        switch (e) {
            case 'n': value += '\n'; break;
            case 't': value += '\t'; break;
            case 'r': value += '\r'; break;
            case '0': value += '\0'; break;
            case '\\': value += '\\'; break;
            case '"': value += '"'; break;
            case '\'': value += '\''; break;
            case 'x': {
                int v = 0, n = 0;
                while (n < 2 && std::isxdigit((unsigned char)peek())) {
                    char h = advance();
                    v = v * 16 + (std::isdigit((unsigned char)h) ? h - '0' : (std::tolower(h) - 'a' + 10));
                    ++n;
                }
                if (n == 0)
                    diags_.error(escLoc, "\\x must be followed by hex digits", "write e.g. \\x41 for 'A'",
                                 spec::StringLiterals);
                value += (char)v;
                break;
            }
            default:
                diags_.error(escLoc, std::string("unknown escape sequence '\\") + e + "'",
                             "valid escapes are \\n \\t \\r \\0 \\\\ \\\" \\' and \\xHH", spec::StringLiterals);
        }
    }
    loc.len = loc.line == line_ ? col_ - loc.col : 1;
    return Token{Tok::StringLit, loc, value, 0};
}

}  // namespace cplus
