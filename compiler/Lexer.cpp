#include "Lexer.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "SpecRefs.h"

namespace cplus {

const char* tokSpelling(Tok kind) {
    switch (kind) {
        case Tok::Eof: return "end of file";
        case Tok::Ident: return "identifier";
        case Tok::IntLit: return "number";
        case Tok::FloatLit: return "number";
        case Tok::StringLit: return "string";
        case Tok::CharLit: return "character";
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
        case Tok::FloatLit: return "number " + t.text;
        case Tok::StringLit: return "a string";
        case Tok::CharLit: return "the character " + t.text;
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
    if (std::isdigit((unsigned char)c) || (c == '.' && std::isdigit((unsigned char)peek(1))))
        return atFloat() ? lexFloat() : lexNumber();
    if (std::isalpha((unsigned char)c) || c == '_') return lexIdentOrKeyword();
    if (c == '"') return lexString();
    if (c == '\'') return lexChar();

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
    // C++'s prefixed literals: L'a', u8"text", u"text" and U'a'. C+ text is UTF-8 already.
    if ((text == "L" || text == "u" || text == "U" || text == "u8") && (peek() == '\'' || peek() == '"')) {
        bool isChar = peek() == '\'';
        diags_.error(loc, "C+ has no " + text + (isChar ? "'...' character literals" : "\"...\" string literals"),
                     isChar ? "a char is one byte; write a plain character literal such as 'a', or a string for "
                              "text that needs more bytes in UTF-8"
                            : "strings hold UTF-8 text already; leave out the " + text,
                     isChar ? spec::CharacterLiterals : spec::StringLiterals);
        return isChar ? lexChar() : lexString();
    }
    auto it = keywords.find(text);
    return Token{it == keywords.end() ? Tok::Ident : it->second, loc, text, 0};
}

// Whether the decimal number here has a fraction or an exponent: `1.5`, `.5`, `2.`,
// `1e10`. A '.' followed by a name is a member access instead.
bool Lexer::atFloat() const {
    size_t i = 0;
    if (peek() == '0' && std::strchr("xXbBoO", peek(1)) && peek(1) != '\0') return false;  // 0x, 0b, 0o
    while (std::isdigit((unsigned char)peek(i)) || (peek(i) == '\'' && std::isdigit((unsigned char)peek(i + 1)))) ++i;
    if (peek(i) == '.') {
        char after = peek(i + 1);
        char next = peek(i + 2);
        if ((after == 'f' || after == 'F') && !(std::isalnum((unsigned char)next) || next == '_')) return true;  // 1.f
        return std::isdigit((unsigned char)after) || !(std::isalpha((unsigned char)after) || after == '_' || after == '.');
    }
    if (peek(i) == 'e' || peek(i) == 'E') {  // `1e5`, and `1e` or `1e+` with the digits missing
        char after = peek(i + 1);
        return std::isdigit((unsigned char)after) || after == '+' || after == '-' ||
               !(std::isalpha((unsigned char)after) || after == '_');
    }
    return false;
}

// `1.5`, `.5`, `1e-9`, `2.5f`. Digits may be separated with ', as in C++.
Token Lexer::lexFloat() {
    Loc loc = locHere();
    size_t start = pos_;
    std::string clean;  // the number without separators, for strtod
    auto digits = [&] {
        while (std::isdigit((unsigned char)peek()) ||
               (peek() == '\'' && pos_ > start && std::isdigit((unsigned char)src_[pos_ - 1]) &&
                std::isdigit((unsigned char)peek(1)))) {
            if (peek() != '\'') clean += peek();
            advance();
        }
    };
    digits();
    if (peek() == '.') {
        clean += advance();
        digits();
    }
    if (peek() == 'e' || peek() == 'E') {
        clean += advance();
        if (peek() == '+' || peek() == '-') clean += advance();
        size_t before = pos_;
        digits();
        if (pos_ == before) {
            Loc expLoc = loc;
            expLoc.len = (uint32_t)(pos_ - start);
            diags_.error(expLoc, "the exponent of '" + src_.substr(start, pos_ - start) + "' has no digits",
                         "write the power of ten after the e, e.g. `1e3` or `2.5e-4`", spec::FloatingPointTypes);
            clean += '0';
        }
    }
    bool isFloat = false;
    if (std::isalpha((unsigned char)peek()) || peek() == '_') {
        Loc suffixLoc = locHere();
        size_t s = pos_;
        while (std::isalnum((unsigned char)peek()) || peek() == '_') advance();
        std::string suffix = src_.substr(s, pos_ - s);
        suffixLoc.len = (uint32_t)suffix.size();
        if (suffix == "f" || suffix == "F") isFloat = true;
        else if (suffix == "l" || suffix == "L")
            diags_.error(suffixLoc, "C+ has no long double type",
                         "remove the L: a number with a fraction is a double, which has 64 bits",
                         spec::FloatingPointTypes);
        else
            diags_.error(suffixLoc, "'" + suffix + "' is not a suffix for a floating-point number",
                         "the only suffix is f, which makes the number a float, e.g. `1.5f`",
                         spec::FloatingPointTypes);
    }
    if (peek() == '.' && std::isdigit((unsigned char)peek(1))) {  // `1.2.3`
        while (peek() == '.' || std::isdigit((unsigned char)peek())) advance();
        Loc all = loc;
        all.len = (uint32_t)(pos_ - start);
        diags_.error(all, "a number can have only one decimal point",
                     "write one, e.g. `1.5`. Text such as a version number goes in quotes: `\"" +
                         src_.substr(start, pos_ - start) + "\"`",
                     spec::FloatingPointTypes);
    }
    std::string text = src_.substr(start, pos_ - start);
    loc.len = (uint32_t)text.size();
    double value = isFloat ? (double)std::strtof(clean.c_str(), nullptr) : std::strtod(clean.c_str(), nullptr);
    if (std::isinf(value))
        diags_.error(loc, "the number " + text + " is too large for a " + (isFloat ? "float" : "double"),
                     isFloat ? "the largest float is about 3.4e38; leave out the f to make it a double"
                             : "the largest double is about 1.8e308",
                     spec::FloatingPointTypes);
    Token t{Tok::FloatLit, loc, text, 0};
    t.floatValue = value;
    t.floatSuffix = isFloat;
    return t;
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

    if (peek() == '.' && std::isxdigit((unsigned char)peek(1))) {  // `0x1.8p3`, a C++ hexadecimal float
        while (std::isalnum((unsigned char)peek()) || peek() == '.' ||
               ((peek() == '+' || peek() == '-') && (src_[pos_ - 1] == 'p' || src_[pos_ - 1] == 'P')))
            advance();
        loc.len = (uint32_t)(pos_ - start);
        diags_.error(loc, "a number with a fraction is written in decimal",
                     "hexadecimal, octal and binary numbers have no fractions; write e.g. 1.5 or 1e-3",
                     spec::FloatingPointTypes);
    } else if (base == 16 && (peek() == 'p' || peek() == 'P')) {  // `0x1p3`, a C++ hexadecimal float
        advance();
        if (peek() == '+' || peek() == '-') advance();
        while (std::isalnum((unsigned char)peek())) advance();
        loc.len = (uint32_t)(pos_ - start);
        diags_.error(loc, "C+ has no hexadecimal floating-point numbers",
                     "write the number in decimal, e.g. 1.5 or 1e-3", spec::FloatingPointTypes);
    } else if (peek() == '_' && std::isdigit((unsigned char)peek(1))) {  // `1_000`
        while (std::isalnum((unsigned char)peek()) || peek() == '_' ||
               (peek() == '.' && std::isdigit((unsigned char)peek(1))))
            advance();
        std::string written = src_.substr(start, pos_ - start);
        loc.len = (uint32_t)written.size();
        std::string fixed = written;
        std::replace(fixed.begin(), fixed.end(), '_', '\'');
        diags_.error(loc, "digits are separated with ', as in C++, not with _", "write `" + fixed + "`",
                     spec::IntegerLiterals);
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
            diags_.error(suffixLoc, "a float needs a decimal point or an exponent, as in C++",
                         "write `" + text + ".0f`, or leave out the f, since an int converts to a float",
                         spec::FloatingPointTypes);
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
        lexEscape(value, spec::StringLiterals);
    }
    loc.len = loc.line == line_ ? col_ - loc.col : 1;
    return Token{Tok::StringLit, loc, value, 0};
}

// The escape after a backslash, which has been read: `\n`, `\x41`, ... Its byte is
// added to `value`. Returns false, after reporting it, for an unknown escape.
bool Lexer::lexEscape(std::string& value, const char* anchor) {
    Loc escLoc{line_, col_ - 1, 2};
    char e = pos_ < src_.size() && peek() != '\n' ? advance() : '\0';
    switch (e) {
        case 'n': value += '\n'; return true;
        case 't': value += '\t'; return true;
        case 'r': value += '\r'; return true;
        case '0': value += '\0'; return true;
        case '\\': value += '\\'; return true;
        case '"': value += '"'; return true;
        case '\'': value += '\''; return true;
        case 'x': {
            int v = 0, n = 0;
            while (n < 2 && std::isxdigit((unsigned char)peek())) {
                char h = advance();
                v = v * 16 + (std::isdigit((unsigned char)h) ? h - '0' : (std::tolower(h) - 'a' + 10));
                ++n;
            }
            if (n == 0) {
                diags_.error(escLoc, "\\x must be followed by hex digits", "write e.g. \\x41 for 'A'", anchor);
                return false;
            }
            value += (char)v;
            return true;
        }
        default:
            diags_.error(escLoc, std::string("unknown escape sequence '\\") + e + "'",
                         "valid escapes are \\n \\t \\r \\0 \\\\ \\\" \\' and \\xHH", anchor);
            return false;
    }
}

// The number of bytes of the UTF-8 sequence starting with `lead`, or 0 if it does not
// start one.
static size_t utf8Length(unsigned char lead) {
    return lead >= 0xF0 && lead < 0xF8 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 0;
}

// `'a'`, `'\n'`, `'\x41'`: one byte, as a C++ char. Text of more bytes is a string.
Token Lexer::lexChar() {
    Loc loc = locHere();
    size_t start = pos_;
    advance();  // opening quote
    std::string value;
    bool ok = true;
    while (true) {
        if (pos_ >= src_.size() || peek() == '\n') {
            // Take only the quote and the character after it, so that what follows, such
            // as the ';' in `char c = 'x;`, is still read as code.
            size_t k = 1;
            auto at = [&](size_t i) { return start + i < src_.size() ? src_[start + i] : '\n'; };
            if (at(1) != '\n') k += at(1) == '\\' && at(2) != '\n' ? 2 : 1;
            col_ = loc.col + (uint32_t)k;
            pos_ = start + k;
            loc.len = (uint32_t)k;
            diags_.error(loc, "this character literal is never closed",
                         "end it with ' on the same line, as in 'a'. Text goes in double quotes: \"text\"",
                         spec::CharacterLiterals);
            return Token{Tok::CharLit, loc, src_.substr(start, k), k > 1 ? (uint64_t)(unsigned char)at(k - 1) : 0};
        }
        char c = advance();
        if (c == '\'') break;
        if (c == '\\') ok = lexEscape(value, spec::CharacterLiterals) && ok;
        else value += c;
    }
    std::string text = src_.substr(start, pos_ - start);
    loc.len = (uint32_t)text.size();
    if (ok && value.empty()) {
        diags_.error(loc, "a character literal cannot be empty",
                     "write '\\0' for the zero byte, or \"\" for an empty string", spec::CharacterLiterals);
    } else if (ok && value.size() > 1) {
        std::string inner = text.substr(1, text.size() - 2);
        size_t n = utf8Length((unsigned char)value[0]);
        bool oneCodePoint = n == value.size();
        for (size_t i = 1; oneCodePoint && i < n; ++i) oneCodePoint = ((unsigned char)value[i] & 0xC0) == 0x80;
        if (oneCodePoint)
            diags_.error(loc, "'" + inner + "' takes " + std::to_string(n) + " bytes in UTF-8, but a char holds one byte",
                         "write it as a string, \"" + inner + "\", which holds UTF-8 text of any length",
                         spec::CharacterLiterals);
        else
            diags_.error(loc, "a character literal holds one character, but this has " + std::to_string(value.size()),
                         "write text in double quotes: \"" + inner + "\"", spec::CharacterLiterals);
    }
    Token t{Tok::CharLit, loc, text, value.empty() ? 0 : (uint64_t)(unsigned char)value[0]};
    return t;
}

}  // namespace cplus
