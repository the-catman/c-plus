#include "CHeaders.h"

#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <unordered_map>

#include "AST.h"
#include "Lexer.h"

#ifdef CPLUS_HAVE_CLANG
#include <clang-c/Index.h>
#endif

namespace cplus {

namespace {

// What to say for a header that cannot be found, as one written for C++ often is.
std::string notFoundHelp(const HeaderImport& h, const HeaderSearch& search) {
    std::string stem = h.name;
    if (stem.size() > 2 && stem.compare(stem.size() - 2, 2, ".h") == 0) stem.resize(stem.size() - 2);
    if (isBuiltinModule(stem)) return "`" + stem + "` is a module that comes with C+, which is imported by its name alone: `import " + stem + ";`";
    static const std::set<std::string> cWrapped = {"cassert", "cctype", "cerrno", "cfloat", "climits", "cmath", "csignal", "cstddef", "cstdint", "cstdio", "cstdlib", "cstring", "ctime"};
    if (cWrapped.count(h.name))
        return "C+ reads C headers, and " + h.shown() + " is C++'s: write `import <" + h.name.substr(1) + ".h>;`, the C header it wraps" +
               (h.name == "cmath" ? ", or nothing for `sqrt` and the rest of math, which C+ has built in" : "");
    static const std::set<std::string> cplusplus = {"algorithm", "array", "chrono", "fstream", "functional", "iostream", "map", "memory", "mutex", "optional",
                                                     "set", "sstream", "string", "thread", "unordered_map", "unordered_set", "utility", "vector"};
    if (cplusplus.count(h.name))
        return h.name == "iostream" ? "remove the import: C+ prints with `println`, which needs nothing imported, and reads C headers, not C++ ones"
                                    : "remove the import: C+ reads C headers, not C++ ones, and what it has of " + h.shown() + " is built in";
    std::string where = h.angled ? "It is looked for in the system's folders"
                                 : "It is looked for in the folder of the program" + std::string(search.sourceDir.empty() ? "" : ", " + search.sourceDir) +
                                       ", then in the system's folders";
    return "check its name, and that the library's development files are installed. " + where +
           ", and in a folder named with -I, as in `cplus build app.cplus -I include`, or in the environment variable CPATH";
}

}  // namespace

// What a compiler built without libclang says, which the tests reach by pretending.
HeaderTranslation withoutClang(const std::vector<HeaderImport>& headers) {
    HeaderTranslation out;
    for (const HeaderImport& h : headers)
        out.errors.push_back({h.loc, "this compiler was built without libclang, which it reads C headers with, so it cannot import " + h.shown(),
                              "declare what the program uses from the header yourself, in `extern \"C\" { ... }`, with C+ types; or build cplus again with "
                              "libclang's development files installed"});
    return out;
}

#ifndef CPLUS_HAVE_CLANG

HeaderTranslation translateHeaders(const std::vector<HeaderImport>& headers, const HeaderSearch&, const NamesInProgram&) {
    (void)notFoundHelp;
    return withoutClang(headers);
}

#else

namespace {

std::string text(CXString s) {
    const char* c = clang_getCString(s);
    std::string out = c ? c : "";
    clang_disposeString(s);
    return out;
}

std::string spelling(CXCursor c) { return text(clang_getCursorSpelling(c)); }
std::string spelling(CXType t) { return text(clang_getTypeSpelling(t)); }

std::string usrOf(CXCursor c) { return text(clang_getCursorUSR(c)); }

// A type as an error shows it, with its article: "a `short`", "an `int[]`". clang names a struct with no name by where it is.
std::string aType(CXType t) {
    std::string shown = spelling(t);
    if (shown.find("(unnamed") != std::string::npos || shown.find("(anonymous") != std::string::npos)
        shown = shown.rfind("union", 0) == 0 ? "union { ... }" : "struct { ... }";
    const bool vowel = !shown.empty() && std::strchr("aeiou", shown[0]) && shown.rfind("union", 0) != 0 && shown.rfind("uint", 0) != 0;  // "a union"
    return (vowel ? "an `" : "a `") + shown + "`";
}

template <typename F>
void eachChild(CXCursor parent, F f) {
    clang_visitChildren(
        parent,
        [](CXCursor child, CXCursor, CXClientData data) {
            (*static_cast<F*>(data))(child);
            return CXChildVisit_Continue;
        },
        &f);
}

bool isIdentifier(const std::string& s) {
    if (s.empty() || !(std::isalpha((unsigned char)s[0]) || s[0] == '_')) return false;
    for (char c : s)
        if (!std::isalnum((unsigned char)c) && c != '_') return false;
    return true;
}

// C keeps these for itself and its compilers: `__x` and `_X`.
bool isReserved(const std::string& s) { return s.size() > 1 && s[0] == '_' && (s[1] == '_' || std::isupper((unsigned char)s[1])); }

bool isCPlusKeyword(const std::string& w) {
    static const std::set<std::string> words = {
#define X(name, spelled) spelled,
        CPLUS_KEYWORDS(X)
#undef X
    };
    return words.count(w) > 0;
}

// What C+ has itself, under the same name, which a header's would only hide.
bool isLanguageName(const std::string& n) {
    static const std::set<std::string> names = {"print",  "println", "format", "to_string", "move",  "min",       "max",   "delay", "vector", "array",
                                                "hashmap", "Task",   "unique", "local",     "Mutex", "LockGuard", "Error", "main",  "NULL",
                                                "readLine", "stoi", "stol", "stod", "stof"};
    return names.count(n) || isMathFunction(n) || isCharFunction(n);
}

// That, or a name kept for C's compiler or for the C+ runtime.
bool isBuiltinName(const std::string& n) { return isLanguageName(n) || n.rfind("cplus_", 0) == 0 || n.rfind("__", 0) == 0; }

// A name for C+ that is not one of its words: C's `new` or `class` is `new_` or `class_`.
std::string unclashed(std::string name) {
    while (isCPlusKeyword(name) || isLanguageName(name)) name += "_";
    return name;
}

// The shortest text that reads back as the same double.
std::string shortest(double v, int most) {
    char buf[40];
    for (int p = 1; p <= most; ++p) {
        std::snprintf(buf, sizeof buf, "%.*g", p, v);
        if ((most > 9 ? std::strtod(buf, nullptr) : (double)std::strtof(buf, nullptr)) == (most > 9 ? v : (double)(float)v)) break;
    }
    std::string s = buf;
    if (s.find_first_of(".en") == std::string::npos) s += ".0";
    return s;
}

std::string quoted(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') out += '\\', out += (char)c;
        else if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else if (c == '\r') out += "\\r";
        else if (c < 0x20 || c >= 0x7f) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02x", c);
            out += buf;
        } else out += (char)c;
    }
    return out + "\"";
}

// A whole number, as a constant of the C+ type of C's size: the same bits, as for what passes to C.
std::string wholeConstant(const std::string& name, long long v, long long size) {
    if (size <= 4) {
        const int i = (int)(unsigned)(unsigned long long)v;
        return "const int " + name + " = " + (i == INT_MIN ? std::string("(-2147483647 - 1)") : std::to_string(i)) + ";";
    }
    if (v == LLONG_MIN) return "const long " + name + " = (-9223372036854775807L - 1);";
    return "const long " + name + " = " + std::to_string(v) + (v < INT_MIN || v > INT_MAX ? "L" : "") + ";";
}

enum class Use { Param, Result, Field, Pointee };

class Translator {
public:
    Translator(CXTranslationUnit tu, const std::string& mainName, const std::vector<HeaderImport>& headers, const NamesInProgram& names,
               HeaderTranslation& out)
        : tu_(tu), mainName_(mainName), headers_(headers), names_(names), out_(out) {}

    void collect();
    // The object-like macros of the program's names whose values may be constants, to be worked out in a second reading.
    std::vector<std::string> macrosToEvaluate() const;
    void evaluated(CXTranslationUnit probes);
    void resolve();
    std::string source() const;
    size_t importOfFile(const std::string& file) const {
        auto it = fileImport_.find(file);
        return it == fileImport_.end() || it->second >= headers_.size() ? 0 : it->second;
    }

private:
    struct Record {
        CXCursor decl;
        std::string tag, typedefName;
        std::string name;  // what C+ calls it, if anything
        bool isUnion = false;
        enum class State { Unknown, Deciding, Complete, Opaque } state = State::Unknown;
        bool queued = false;
        std::string why;   // why C+ cannot hold it
        std::string text;  // its declaration in C+
    };
    struct Macro {
        CXCursor cursor;
        bool functionLike = false;
        std::vector<std::string> body;  // the tokens of its value
    };
    struct Value {  // of a macro, worked out
        enum class Kind { None, Whole, Real, Text, Pointer } kind = Kind::None;
        long long whole = 0;
        double real = 0;
        std::string str;
        CXTypeKind type = CXType_Invalid;
        long long size = 0;
    };

    CXTranslationUnit tu_;
    std::string mainName_;
    const std::vector<HeaderImport>& headers_;
    const NamesInProgram& names_;
    HeaderTranslation& out_;

    std::unordered_map<std::string, Record> records_;  // by USR
    std::vector<std::string> recordOrder_;
    std::map<std::string, std::string> recordNamed_;  // C+ name to USR
    std::map<std::string, std::string> aliasOf_;      // a tag or typedef that is not what C+ calls the struct, to what it does
    std::vector<std::pair<std::string, std::string>> otherTypedefs_;  // a second typedef of a struct, and the struct's USR
    bool named_ = false;  // whether the structs collected have been named, so one found later is named when it is found
    std::map<std::string, CXCursor> functions_, variables_, typedefs_, enums_;
    std::map<std::string, std::pair<CXCursor, CXCursor>> enumConstants_;  // the constant and its enum
    std::map<std::string, Macro> macros_;
    std::map<std::string, Value> values_;
    std::map<std::string, size_t> fileImport_;  // a header's file, to the import that brought it in

    std::vector<std::string> declarations_, constants_;
    std::vector<std::string> decided_;  // records, in the order they were written
    std::deque<std::string> pending_;

    bool inMainFile(CXCursor c) const;
    std::string from(CXCursor c) const;  // `<stdio.h>`: the import a declaration came through
    void collectOne(CXCursor c);
    Record& recordOf(CXCursor decl);
    void nameRecords();
    void name(const std::string& usr);
    void take(const std::string& name);
    void function(const std::string& name, CXCursor c);
    void enumConstant(const std::string& name);
    bool macroConstant(const std::string& name);
    void notImported(const std::string& name, std::string message, std::string help) {
        out_.notImported[name] = NotImported{std::move(message), std::move(help)};
    }
    std::string type(CXType t, Use use, std::string& why);
    std::string recordName(CXCursor decl, bool byValue, std::string& why);
    void decide(Record& r);
};

bool Translator::inMainFile(CXCursor c) const {
    CXFile file;
    clang_getExpansionLocation(clang_getCursorLocation(c), &file, nullptr, nullptr, nullptr);
    return file && text(clang_getFileName(file)) == mainName_;
}

std::string Translator::from(CXCursor c) const {
    CXFile file;
    clang_getExpansionLocation(clang_getCursorLocation(c), &file, nullptr, nullptr, nullptr);
    return headers_[file ? importOfFile(text(clang_getFileName(file))) : 0].shown();
}

void Translator::collect() {
    clang_getInclusions(
        tu_,
        [](CXFile included, CXSourceLocation* stack, unsigned depth, CXClientData data) {
            if (depth == 0) return;  // the file of #include lines itself
            unsigned line = 0;
            clang_getSpellingLocation(stack[depth - 1], nullptr, &line, nullptr, nullptr);
            static_cast<Translator*>(data)->fileImport_.emplace(text(clang_getFileName(included)), line ? line - 1 : 0);
        },
        this);
    eachChild(clang_getTranslationUnitCursor(tu_), [&](CXCursor c) { collectOne(c); });
    nameRecords();
}

Translator::Record& Translator::recordOf(CXCursor decl) {
    std::string usr = usrOf(decl);
    auto [it, added] = records_.try_emplace(usr);
    if (added) {
        Record& r = it->second;
        r.decl = decl;
        r.isUnion = clang_getCursorKind(decl) == CXCursor_UnionDecl;
        std::string tag = spelling(decl);
        if (isIdentifier(tag)) r.tag = tag;  // not `struct (unnamed at ...)`
        recordOrder_.push_back(usr);
        if (named_) name(usr);
    }
    return it->second;
}

void Translator::collectOne(CXCursor c) {
    const CXCursorKind kind = clang_getCursorKind(c);
    if (kind == CXCursor_MacroDefinition) {
        CXFile file;
        clang_getExpansionLocation(clang_getCursorLocation(c), &file, nullptr, nullptr, nullptr);
        if (!file) return;  // one the compiler defines, such as `linux`
        Macro m{c, clang_Cursor_isMacroFunctionLike(c) != 0, {}};
        CXToken* tokens = nullptr;
        unsigned count = 0;
        clang_tokenize(tu_, clang_getCursorExtent(c), &tokens, &count);
        for (unsigned i = 1; i < count; ++i) m.body.push_back(text(clang_getTokenSpelling(tu_, tokens[i])));
        clang_disposeTokens(tu_, tokens, count);
        macros_.insert_or_assign(spelling(c), std::move(m));  // the last definition is the one that counts
        return;
    }
    if (inMainFile(c)) return;
    std::string name = spelling(c);
    switch (kind) {
        case CXCursor_FunctionDecl: functions_.emplace(name, c); break;
        case CXCursor_VarDecl: variables_.emplace(name, c); break;
        case CXCursor_TypedefDecl: {
            CXType under = clang_getCanonicalType(clang_getTypedefDeclUnderlyingType(c));
            if (under.kind == CXType_Record) {
                Record& r = recordOf(clang_getTypeDeclaration(under));
                // glibc has `typedef struct _IO_FILE __FILE;` before `typedef struct _IO_FILE FILE;`, and C+ takes the name a program can use.
                if (r.typedefName.empty() || (isReserved(r.typedefName) && !isReserved(name))) std::swap(r.typedefName, name);
                if (!name.empty() && name != r.typedefName) otherTypedefs_.emplace_back(name, usrOf(clang_getTypeDeclaration(under)));
            } else typedefs_.emplace(name, c);
            break;
        }
        case CXCursor_StructDecl:
        case CXCursor_UnionDecl:
            recordOf(c);
            eachChild(c, [&](CXCursor inner) {  // C puts a struct declared inside another in the file's scope
                CXCursorKind k = clang_getCursorKind(inner);
                if (k == CXCursor_StructDecl || k == CXCursor_UnionDecl || k == CXCursor_EnumDecl) collectOne(inner);
            });
            break;
        case CXCursor_EnumDecl:
            if (isIdentifier(name)) enums_.emplace(name, c);
            eachChild(c, [&](CXCursor constant) {
                if (clang_getCursorKind(constant) == CXCursor_EnumConstantDecl) enumConstants_.emplace(spelling(constant), std::make_pair(constant, c));
            });
            break;
        default: break;
    }
}

// A struct is called by its typedef's name if it has one, as in `typedef struct _IO_FILE FILE;`, and by its tag if not.
void Translator::nameRecords() {
    for (const std::string& usr : recordOrder_) name(usr);
    named_ = true;
    for (const std::string& usr : recordOrder_) {
        const Record& r = records_[usr];
        if (!r.name.empty() && !r.tag.empty() && r.tag != r.name && !recordNamed_.count(r.tag)) aliasOf_.emplace(r.tag, r.name);
    }
    for (const auto& [typedefName, usr] : otherTypedefs_)
        if (!records_[usr].name.empty() && !recordNamed_.count(typedefName)) aliasOf_.emplace(typedefName, records_[usr].name);
}

void Translator::name(const std::string& usr) {
    Record& r = records_[usr];
    std::string name = !r.typedefName.empty() && !(isReserved(r.typedefName) && !r.tag.empty() && !isReserved(r.tag)) ? r.typedefName : r.tag;
    if (name.empty() || isCPlusKeyword(name) || isBuiltinName(name) || recordNamed_.count(name)) return;
    r.name = name;
    recordNamed_[name] = usr;
}

std::vector<std::string> Translator::macrosToEvaluate() const {
    std::vector<std::string> out;
    for (const std::string& n : names_.used) {
        auto it = macros_.find(n);
        if (it == macros_.end() || it->second.functionLike || it->second.body.empty() || names_.declared.count(n)) continue;
        // A value that could spill out of the declaration it is worked out in, and take the next ones with it.
        int depth = 0;
        bool fits = true;
        static const std::set<std::string> statements = {"{", "}", ";", "=", "[", "]", "#", "##", "do", "while", "if", "else", "for", "return",
                                                         "goto", "break", "continue", "switch", "case", "default", "typedef", "static", "extern"};
        for (const std::string& t : it->second.body) {
            if (statements.count(t)) fits = false;
            depth += t == "(" ? 1 : t == ")" ? -1 : 0;
            if (depth < 0) fits = false;
        }
        if (fits && depth == 0) out.push_back(n);
    }
    return out;
}

// From the second reading, where each macro is the value of a declaration: `static const __auto_type __cplus_n_X = (X);`,
// `static const char __cplus_s_X[] = X;` for text, and `static const long __cplus_p_X = (long)(X);` for a pointer.
void Translator::evaluated(CXTranslationUnit probes) {
    std::map<std::string, CXCursor> found;
    eachChild(clang_getTranslationUnitCursor(probes), [&](CXCursor c) {
        if (clang_getCursorKind(c) != CXCursor_VarDecl) return;
        std::string name = spelling(c);
        if (name.rfind("__cplus_", 0) == 0) found.emplace(name, c);
    });
    auto evaluate = [&](const std::string& var, CXEvalResult& result) {
        auto it = found.find(var);
        result = it == found.end() ? nullptr : clang_Cursor_Evaluate(it->second);
        return result ? clang_EvalResult_getKind(result) : CXEval_UnExposed;
    };
    for (auto& [name, macro] : macros_) {
        if (!found.count("__cplus_n_" + name)) continue;
        Value v;
        CXEvalResult r = nullptr;
        if (evaluate("__cplus_s_" + name, r) == CXEval_StrLiteral) {
            v.kind = Value::Kind::Text;
            v.str = clang_EvalResult_getAsStr(r);
        } else {
            if (r) clang_EvalResult_dispose(r), r = nullptr;
            CXType t = clang_getCanonicalType(clang_getCursorType(found["__cplus_n_" + name]));
            v.type = t.kind;
            v.size = clang_Type_getSizeOf(t);
            if (t.kind == CXType_Pointer) {
                if (evaluate("__cplus_p_" + name, r) == CXEval_Int) {
                    v.kind = Value::Kind::Pointer;
                    v.whole = clang_EvalResult_getAsLongLong(r);
                }
            } else {
                CXEvalResultKind k = evaluate("__cplus_n_" + name, r);
                if (k == CXEval_Int) {
                    v.kind = Value::Kind::Whole;
                    v.whole = clang_EvalResult_isUnsignedInt(r) ? (long long)clang_EvalResult_getAsUnsigned(r) : clang_EvalResult_getAsLongLong(r);
                } else if (k == CXEval_Float) {
                    v.kind = Value::Kind::Real;
                    v.real = clang_EvalResult_getAsDouble(r);
                }
            }
        }
        if (r) clang_EvalResult_dispose(r);
        if (v.kind != Value::Kind::None) values_[name] = v;
    }
}

void Translator::resolve() {
    for (const std::string& n : names_.used) {
        if (!isIdentifier(n) || isReserved(n) || isBuiltinName(n) || isCPlusKeyword(n)) continue;
        if (names_.declared.count(n) || names_.types.count(n)) continue;  // the program's own is used instead
        take(n);
    }
    while (!pending_.empty()) {
        Record& r = records_[pending_.front()];
        pending_.pop_front();
        if (r.state == Record::State::Unknown) decide(r);
    }
}

void Translator::take(const std::string& n) {
    const std::string generic = "a declaration of the program's own, in `extern \"C\" { ... }`, is used instead of the header's: write one, if C+ has types that fit";
    if (auto r = recordNamed_.find(n); r != recordNamed_.end()) {
        Record& record = records_[r->second];
        if (!record.queued) record.queued = true, pending_.push_back(r->second);
        return;
    }
    auto macro = macros_.find(n);
    if (macro != macros_.end() && !macro->second.functionLike) {
        if (macroConstant(n)) return;
        const std::vector<std::string>& body = macro->second.body;
        const std::string header = from(macro->second.cursor);
        if (n == "errno") {
            notImported(n, "this compiler does not support C's `errno` yet",
                        "a C function's result says whether it failed, and `perror(\"what failed\")`, from <stdio.h>, prints why");
            return;
        }
        if (!functions_.count(n) && !variables_.count(n) && !enumConstants_.count(n)) {
            if (body.empty()) {
                notImported(n, "`" + n + "` from " + header + " is a C macro with no value, which C uses with `#ifdef`",
                            "C+ has no preprocessor: leave it out");
            } else {
                const bool alias = body.size() == 1 && isIdentifier(body[0]);
                notImported(n, "`" + n + "` from " + header + " is a C macro whose value C+ cannot work out while compiling: not a number, a string or a pointer",
                            alias && functions_.count(body[0]) ? "call `" + body[0] + "`, the C function it stands for"
                                                               : "write its value as a constant of the program's own: `const int " + n + " = ...;`");
            }
            return;
        }
    }
    if (auto f = functions_.find(n); f != functions_.end()) {
        function(n, f->second);
        return;
    }
    if (macro != macros_.end() && macro->second.functionLike) {
        notImported(n, "`" + n + "` from " + from(macro->second.cursor) + " is a C macro with arguments, which C+ cannot use",
                    n == "assert" ? "write `if (!ok) throw Error(\"...\");`, or stop the program with a C function"
                                  : "write what it does in C+, from what the header defines it as");
        return;
    }
    if (enumConstants_.count(n)) {
        enumConstant(n);
        return;
    }
    if (auto v = variables_.find(n); v != variables_.end()) {
        notImported(n, "this compiler does not support C variables, such as `" + n + "` from " + from(v->second) + ", yet",
                    n == "stdout" || n == "stderr" || n == "stdin" ? std::string("C+ prints with `println`, to the standard output")
                                                                   : "call a C function that gives what it holds, if the library has one");
        return;
    }
    if (auto t = typedefs_.find(n); t != typedefs_.end()) {
        CXType under = clang_getTypedefDeclUnderlyingType(t->second);
        CXType canonical = clang_getCanonicalType(under);
        std::string why;
        const std::string header = from(t->second);
        if (canonical.kind == CXType_Enum) {
            notImported(n, "`" + n + "` from " + header + " is C's name for an enum, which C+ writes as `int`", "write `int`, and its constants by name");
            return;
        }
        std::string written = type(under, Use::Param, why);
        if (written == "string?") written = "const char*";  // as a type of its own, and not one for passing text to C
        const bool toFunction = canonical.kind == CXType_Pointer && (clang_getCanonicalType(clang_getPointeeType(canonical)).kind == CXType_FunctionProto ||
                                                                      clang_getCanonicalType(clang_getPointeeType(canonical)).kind == CXType_FunctionNoProto);
        if (toFunction)
            notImported(n, "`" + n + "` from " + header + " is C's name for a pointer to a function, which C+ writes as `void*`, as it has no callbacks yet",
                        "write `void*`, and pass `null` where C allows it");
        else if (!written.empty())
            notImported(n, "`" + n + "` from " + header + " is C's name for `" + spelling(canonical) + "`, which C+ writes as `" + written + "`",
                        "write `" + written + "`");
        else
            notImported(n, "C+ has no type for `" + n + "` from " + header + ", which is C's name for `" + spelling(canonical) + "`" + why, generic);
        return;
    }
    if (auto a = aliasOf_.find(n); a != aliasOf_.end()) {
        notImported(n, "C+ calls `" + n + "` from " + from(records_[recordNamed_[a->second]].decl) + " by the name its typedef gives it, `" + a->second + "`",
                    "write `" + a->second + "`");
        return;
    }
    if (auto e = enums_.find(n); e != enums_.end())
        notImported(n, "`" + n + "` from " + from(e->second) + " is a C enum, which C+ writes as `int`", "write `int`, and its constants by name");
}

void Translator::function(const std::string& n, CXCursor c) {
    const std::string header = from(c);
    const std::string cannot = "C+ cannot call `" + n + "` from " + header + ", as ";
    const std::string generic = "a declaration of the program's own, in `extern \"C\" { ... }`, is used instead of the header's: write one, if C+ has types that fit";
    if (clang_Cursor_getStorageClass(c) == CX_SC_Static || clang_Cursor_isFunctionInlined(c)) {
        notImported(n, "this compiler does not support C functions whose code is in the header, such as `" + n + "` from " + header + ", yet",
                    "write what it does in C+, from its code in the header");
        return;
    }
    CXType fn = clang_getCursorType(c);
    if (fn.kind == CXType_FunctionProto && clang_isFunctionTypeVariadic(fn)) {  // and not C's `int f();`, which C+ calls with nothing
        const bool prints = n.find("printf") != std::string::npos;
        notImported(n, "this compiler does not support C functions that take a variable number of arguments, such as `" + n + "` from " + header + ", yet",
                    n == "sprintf" || n == "snprintf" || n == "asprintf" ? "`format(\"{}\", x)` returns the string"
                    : prints ? "C+ prints with println: `printf(\"%d\\n\", x)` becomes `println(\"{}\", x)`"
                             : "call a C function with a fixed list of arguments instead, if the library has one");
        return;
    }
    std::string why;
    // The result as the header writes it. For a function clang knows, such as strlen, the function's type has the
    // `unsigned long` of the compiler's own declaration, and not the header's `size_t`.
    std::pair<CXType, CXType> written{clang_getResultType(fn), clang_getResultType(fn)};
    clang_visitChildren(
        c,
        [](CXCursor child, CXCursor, CXClientData data) {
            auto& [result, canonical] = *static_cast<std::pair<CXType, CXType>*>(data);
            if (clang_isAttribute(child.kind)) return CXChildVisit_Continue;  // glibc's `__THROW` and the like
            if (child.kind != CXCursor_TypeRef) return CXChildVisit_Break;
            CXCursor named = clang_getCursorReferenced(child);
            if (named.kind == CXCursor_TypedefDecl && clang_equalTypes(clang_getCanonicalType(clang_getCursorType(named)), clang_getCanonicalType(canonical)))
                result = clang_getCursorType(named);
            return CXChildVisit_Break;
        },
        &written);
    const CXType resultType = written.first;
    const std::string result = type(resultType, Use::Result, why);
    if (result.empty()) {
        notImported(n, cannot + "its result is " + aType(resultType) + why, generic);
        return;
    }
    const int count = fn.kind == CXType_FunctionNoProto ? 0 : clang_getNumArgTypes(fn);
    std::string params;
    std::set<std::string> taken;
    for (int i = 0; i < count; ++i) {
        CXCursor p = clang_Cursor_getArgument(c, (unsigned)i);
        const std::string written = clang_Cursor_isNull(p) ? "" : spelling(p);
        CXType pt = clang_Cursor_isNull(p) ? clang_getArgType(fn, (unsigned)i) : clang_getCursorType(p);  // a size_t, as the header writes it
        const std::string t = type(pt, Use::Param, why);
        if (t.empty()) {
            notImported(n, cannot + "its parameter " + (written.empty() ? "number " + std::to_string(i + 1) : "`" + written + "`") + " is " + aType(pt) + why,
                        generic);
            return;
        }
        // glibc writes `__stream`. A name C+ reads as something else is left out, as it says nothing more there.
        std::string shown = written;
        while (!shown.empty() && shown[0] == '_') shown.erase(0, 1);
        if (!isIdentifier(shown) || isCPlusKeyword(shown) || isBuiltinName(shown) || recordNamed_.count(shown) || names_.types.count(shown) ||
            !taken.insert(shown).second)
            shown.clear();
        params += (i ? ", " : "") + t + (shown.empty() ? "" : " " + shown);
    }
    declarations_.push_back(result + " " + n + "(" + params + ");");
}

void Translator::enumConstant(const std::string& n) {
    auto [constant, owner] = enumConstants_[n];
    CXType whole = clang_getCanonicalType(clang_getEnumDeclIntegerType(owner));
    const bool isUnsigned = whole.kind == CXType_UInt || whole.kind == CXType_ULong || whole.kind == CXType_ULongLong || whole.kind == CXType_UChar ||
                            whole.kind == CXType_UShort || whole.kind == CXType_Char_U;
    long long v = isUnsigned ? (long long)clang_getEnumConstantDeclUnsignedValue(constant) : clang_getEnumConstantDeclValue(constant);
    constants_.push_back(wholeConstant(n, v, clang_Type_getSizeOf(whole)));
}

bool Translator::macroConstant(const std::string& n) {
    auto it = values_.find(n);
    if (it == values_.end()) return false;
    const Value& v = it->second;
    switch (v.kind) {
        case Value::Kind::Text: constants_.push_back("const string " + n + " = " + quoted(v.str) + ";"); return true;
        case Value::Kind::Pointer:
            constants_.push_back("void* const " + n + " = " + (v.whole == 0 ? std::string("null") : "reinterpret_cast<void*>(" + std::to_string(v.whole) +
                                                                                                    (v.whole < INT_MIN || v.whole > INT_MAX ? "L" : "") + ")") + ";");
            return true;
        case Value::Kind::Whole:
            if (v.type == CXType_Bool) constants_.push_back("const bool " + n + " = " + (v.whole ? "true" : "false") + ";");
            else if (v.type == CXType_UInt || v.type == CXType_ULong || v.type == CXType_ULongLong) {
                const unsigned long long bits = v.size <= 4 ? (unsigned long long)v.whole & 0xFFFFFFFFull : (unsigned long long)v.whole;
                constants_.push_back(std::string(v.size <= 4 ? "const uint32_t " : "const uint64_t ") + n + " = " + std::to_string(bits) + "u;");
            } else constants_.push_back(wholeConstant(n, v.whole, v.size));
            return true;
        case Value::Kind::Real: {
            const bool single = v.type == CXType_Float;
            std::string value = std::isnan(v.real) ? "0.0 / 0" : std::isinf(v.real) ? (v.real < 0 ? "-1.0 / 0" : "1.0 / 0") : shortest(v.real, single ? 9 : 17);
            if (single) value = std::isfinite(v.real) ? value + "f" : "float(" + value + ")";
            constants_.push_back(std::string(single ? "const float " : "const double ") + n + " = " + value + ";");
            return true;
        }
        case Value::Kind::None: break;
    }
    return false;
}

// What a C type is written as in C+, or nothing, with why after the type's name: ", a 16-bit number, ...".
std::string Translator::type(CXType written, Use use, std::string& why) {
    // A size is a long in C+, as `size()` gives one, though C's size_t is unsigned. A uint8_t that is a value, and not
    // what a pointer leads to, is one in C+ too, though C's is an `unsigned char`.
    for (CXType w = written; w.kind == CXType_Typedef || w.kind == CXType_Elaborated;) {
        if (w.kind == CXType_Elaborated) {
            w = clang_Type_getNamedType(w);
            continue;
        }
        const std::string name = text(clang_getTypedefName(w));
        if (name == "size_t" || name == "uintptr_t") return "long";
        if (name == "uint8_t" && use != Use::Pointee) return "uint8_t";
        w = clang_getTypedefDeclUnderlyingType(clang_getTypeDeclaration(w));
    }
    CXType t = clang_getCanonicalType(written);
    const long long size = clang_Type_getSizeOf(t);
    switch (t.kind) {
        case CXType_Void: return "void";
        case CXType_Bool: return "bool";
        case CXType_Char_S:
        case CXType_Char_U:
        case CXType_SChar:
        case CXType_UChar: return "char";
        case CXType_Short: why = ", a 16-bit number, which C+ has no type for yet"; return "";
        case CXType_UShort:
        case CXType_Char16:
        case CXType_UInt:
        case CXType_ULong:
        case CXType_ULongLong:
            if (size == 2) return "uint16_t";
            if (size == 4) return "uint32_t";
            if (size == 8) return "uint64_t";
            why = ", which C+ has no type for yet";
            return "";
        case CXType_Int:
        case CXType_Long:
        case CXType_LongLong:
        case CXType_WChar:
        case CXType_Char32:
        case CXType_Enum:
            if (size == 4) return "int";
            if (size == 8) return "long";
            if (size == 1) return "char";
            why = size == 2 ? ", a 16-bit number, which C+ has no type for yet" : ", which C+ has no type for yet";
            return "";
        case CXType_Int128:
        case CXType_UInt128: why = ", a 128-bit number, which C+ has no type for yet"; return "";
        case CXType_Float: return "float";
        case CXType_Double: return "double";
        case CXType_Pointer: {
            CXType pointee = clang_getCanonicalType(clang_getPointeeType(t));
            if (pointee.kind == CXType_FunctionProto || pointee.kind == CXType_FunctionNoProto) return "void*";  // no callbacks yet
            const bool isConst = clang_isConstQualifiedType(pointee);
            if (isConst && (pointee.kind == CXType_Char_S || pointee.kind == CXType_Char_U)) {
                if (use == Use::Param) return "string?";  // text, or null
                if (use == Use::Result) return "string";  // which stops the program if it is NULL, as following one does
            }
            // `const unsigned char*`, as for bytes, which C+ passes to and from C as `char*`: a `const char*` would be text.
            if (isConst && (pointee.kind == CXType_SChar || pointee.kind == CXType_UChar) && (use == Use::Param || use == Use::Result)) return "char*";
            std::string inner = type(pointee, Use::Pointee, why);
            why.clear();
            if (inner.empty()) return isConst ? "const void*" : "void*";  // what C+ cannot read can still be passed on
            // `char* const* argv` is `char**`: a `const` after a `*` says nothing of what C+ passes.
            return isConst && pointee.kind != CXType_Pointer ? "const " + inner + "*" : inner + "*";
        }
        case CXType_ConstantArray:
        case CXType_IncompleteArray: {
            std::string element = type(clang_getArrayElementType(t), use == Use::Param ? Use::Pointee : Use::Field, why);
            if (element.empty()) return "";
            if (use == Use::Param) return element + "*";  // as C passes it
            if (t.kind == CXType_IncompleteArray) {
                why = ", an array with no length, which C+ cannot hold";
                return "";
            }
            if (clang_getArraySize(t) < 1) {
                why = ", an array with no elements, which C+ cannot hold";
                return "";
            }
            return "array<" + element + ", " + std::to_string(clang_getArraySize(t)) + ">";
        }
        case CXType_Record: return recordName(clang_getTypeDeclaration(t), use != Use::Pointee, why);
        default: why = ", which C+ has no type for yet"; return "";
    }
}

// The name of a struct, and its declaration among those written. Through a pointer, C+ needs only its name; by value, its fields.
std::string Translator::recordName(CXCursor decl, bool byValue, std::string& why) {
    Record& r = recordOf(decl);
    if (r.name.empty()) {
        why = r.isUnion ? ", a union with no name, which C+ cannot hold" : ", a struct with no name of its own, which C+ cannot hold";
        return "";
    }
    if (auto own = names_.types.find(r.name); own != names_.types.end()) {
        if (own->second) return r.name;  // the program declares it in `extern "C"`, which is taken to be this one
        why = ", and the program has a type of its own named `" + r.name + "`";
        return "";
    }
    if (!byValue) {
        if (!r.queued) r.queued = true, pending_.push_back(usrOf(decl));
        return r.name;
    }
    if (r.state == Record::State::Unknown) decide(r);
    if (r.state == Record::State::Complete) return r.name;
    why = r.isUnion ? ", a union, which C+ cannot hold yet, but a pointer to one works" : ", a struct that C+ cannot hold, as " + r.why;
    return "";
}

// Whether C+ can hold the struct, with the same layout as C's, or knows it by name only.
void Translator::decide(Record& r) {
    r.state = Record::State::Deciding;
    CXCursor def = clang_getCursorDefinition(r.decl);
    bool complete = !r.isUnion && !clang_Cursor_isNull(def);
    if (!r.isUnion && clang_Cursor_isNull(def)) r.why = "its header gives only its name";
    std::string body;
    if (complete) {
        long long offset = 0, align = 1;
        int fields = 0;
        std::set<std::string> taken;
        eachChild(def, [&](CXCursor f) {
            if (!complete) return;
            CXCursorKind k = clang_getCursorKind(f);
            if ((k == CXCursor_StructDecl || k == CXCursor_UnionDecl) && clang_Cursor_isAnonymousRecordDecl(f)) {
                r.why = "it has a struct or union inside it with no name";
                complete = false;
                return;
            }
            if (k != CXCursor_FieldDecl) return;
            const std::string name = spelling(f);
            CXType ft = clang_getCursorType(f);
            if (clang_Cursor_isBitField(f)) {
                r.why = "its field `" + name + "` is a bit field, which C+ cannot hold yet";
                complete = false;
                return;
            }
            std::string why;
            const std::string t = type(ft, Use::Field, why);
            if (t.empty()) {
                r.why = "its field `" + name + "` is " + aType(ft) + why;
                complete = false;
                return;
            }
            const long long fieldSize = clang_Type_getSizeOf(ft), fieldAlign = clang_Type_getAlignOf(ft);
            if (fieldSize < 0 || fieldAlign <= 0) {
                r.why = "its field `" + name + "` has no size that C+ can find";
                complete = false;
                return;
            }
            offset = (offset + fieldAlign - 1) / fieldAlign * fieldAlign;
            if (clang_Cursor_getOffsetOfField(f) != offset * 8) {
                r.why = "the header packs or aligns its fields in a way of its own";
                complete = false;
                return;
            }
            offset += fieldSize;
            align = std::max(align, fieldAlign);
            std::string field = unclashed(name);
            while (!taken.insert(field).second) field += "_";
            body += "        " + t + " " + field + ";\n";
            ++fields;
        });
        if (complete && fields == 0) {
            r.why = "it has no fields";
            complete = false;
        }
        if (complete && (offset + align - 1) / align * align != clang_Type_getSizeOf(clang_getCursorType(def))) {
            r.why = "the header packs or aligns its fields in a way of its own";
            complete = false;
        }
    }
    r.state = complete ? Record::State::Complete : Record::State::Opaque;
    r.text = complete ? "    struct " + r.name + " {\n" + body + "    };\n" : "    struct " + r.name + ";\n";
    decided_.push_back(r.name);
}

std::string Translator::source() const {
    if (decided_.empty() && declarations_.empty() && constants_.empty()) return "";
    std::string out = "extern \"C\" {\n";
    for (const std::string& name : decided_) out += records_.at(recordNamed_.at(name)).text;
    for (const std::string& d : declarations_) out += "    " + d + "\n";
    out += "}\n";
    for (const std::string& c : constants_) out += c + "\n";
    return out;
}

}  // namespace

HeaderTranslation translateHeaders(const std::vector<HeaderImport>& headers, const HeaderSearch& search, const NamesInProgram& names) {
    if (std::getenv("CPLUS_NO_CLANG_FOR_TESTS")) return withoutClang(headers);
    HeaderTranslation out;
    if (headers.empty()) return out;
    // A file of #include lines, which is not on disk, in the program's folder: a header in quotes is looked for there first.
    const std::string mainName = (search.sourceDir.empty() ? "" : search.sourceDir + "/") + "cplus-import.c";
    std::string includes;
    for (const HeaderImport& h : headers) includes += "#include " + h.shown() + "\n";
    std::vector<std::string> storage = {"-x", "c", "-std=gnu17", "-w", "-ferror-limit=0"};
    for (const std::string& dir : search.includeDirs) storage.push_back("-I" + dir);
    std::vector<const char*> args;
    for (const std::string& a : storage) args.push_back(a.c_str());

    CXIndex index = clang_createIndex(0, 0);
    auto parse = [&](const std::string& source, unsigned options) {
        CXUnsavedFile file{mainName.c_str(), source.c_str(), (unsigned long)source.size()};
        CXTranslationUnit tu = nullptr;
        if (clang_parseTranslationUnit2(index, mainName.c_str(), args.data(), (int)args.size(), &file, 1, options, &tu) != CXError_Success) tu = nullptr;
        return tu;
    };
    CXTranslationUnit tu = parse(includes, CXTranslationUnit_DetailedPreprocessingRecord | CXTranslationUnit_SkipFunctionBodies | CXTranslationUnit_KeepGoing);
    if (tu && std::getenv("CPLUS_CLANG_FAILS_FOR_TESTS")) clang_disposeTranslationUnit(tu), tu = nullptr;  // so the tests reach what this says
    if (!tu) {
        for (const HeaderImport& h : headers)
            out.errors.push_back({h.loc, "libclang, which C+ reads C headers with, could not read " + h.shown(),
                                  "check that libclang works, by itself, on this computer; meanwhile, declare what the program uses in `extern \"C\" { ... }`"});
        clang_disposeIndex(index);
        return out;
    }

    Translator translator(tu, mainName, headers, names, out);
    translator.collect();

    // An error in a header, or a header not found, is said once for each import, at the first error.
    std::vector<bool> reported(headers.size(), false);
    for (unsigned i = 0, n = clang_getNumDiagnostics(tu); i < n; ++i) {
        CXDiagnostic d = clang_getDiagnostic(tu, i);
        const CXDiagnosticSeverity severity = clang_getDiagnosticSeverity(d);
        CXFile file;
        unsigned line = 0, column = 0;
        clang_getExpansionLocation(clang_getDiagnosticLocation(d), &file, &line, &column, nullptr);
        const std::string message = text(clang_getDiagnosticSpelling(d));
        clang_disposeDiagnostic(d);
        if (severity < CXDiagnostic_Error) continue;
        const std::string fileName = file ? text(clang_getFileName(file)) : "";
        size_t which = 0;
        if (fileName == mainName) {
            if (line < 1 || line > headers.size()) continue;
            which = line - 1;
        } else {
            which = translator.importOfFile(fileName);
        }
        if (reported[which]) continue;
        reported[which] = true;
        const HeaderImport& h = headers[which];
        const bool notFound = message.find("file not found") != std::string::npos;
        if (fileName == mainName && notFound) {
            out.errors.push_back({h.loc, "cannot find the C header " + h.shown(), notFoundHelp(h, search)});
        } else {
            const std::string where = fileName.empty() ? "" : fileName + ":" + std::to_string(line) + ": ";
            out.errors.push_back({h.loc, "C+ cannot read the C header " + h.shown() + ", which has an error: " + where + message,
                                  notFound ? "a header that it includes may be in a folder of its own: name the folder with -I, as in `cplus build app.cplus -I "
                                             "/usr/include/glib-2.0`. `pkg-config --cflags <library>` lists the folders a library needs"
                                           : "check that the header is the one meant: a C compiler reading it would say the same"});
        }
    }

    // The macros the program uses, worked out by the compiler as values of declarations, in a second reading.
    const std::vector<std::string> macros = translator.macrosToEvaluate();
    if (!macros.empty()) {
        std::string probes = includes;
        for (const std::string& m : macros)
            probes += "static const __auto_type __cplus_n_" + m + " = (" + m + ");\nstatic const char __cplus_s_" + m + "[] = " + m +
                      ";\nstatic const long __cplus_p_" + m + " = (long)(" + m + ");\n";
        if (CXTranslationUnit second = parse(probes, CXTranslationUnit_SkipFunctionBodies | CXTranslationUnit_KeepGoing)) {
            translator.evaluated(second);
            clang_disposeTranslationUnit(second);
        }
    }
    translator.resolve();
    out.source = translator.source();
    clang_disposeTranslationUnit(tu);
    clang_disposeIndex(index);
    return out;
}

#endif

}  // namespace cplus
