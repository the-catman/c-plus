#include "Parser.h"

#include "Builtins.h"

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

void Parser::failTooDeep() {
    diags_.error(peek().loc, "this code is nested too deeply for the compiler",
                 "split it into smaller pieces, with local variables or functions", spec::NestingLimits);
    gaveUp_ = true;
    throw TooDeep{};
}

void Parser::notYet(Loc loc, const std::string& feature, const std::string& help) {
    fail(loc, "this compiler does not support " + feature + " yet", help, spec::Status);
}

// A name being declared. What a module has is only read, by the name that joinModuleNames makes.
const Token& Parser::expectNewName(const std::string& context) {
    if (check(Tok::KwOperator)) notYet(peek().loc, "operator overloading", operatorHelp());
    if (check(Tok::KwConst)) fail(peek().loc, "C+ writes `const` before the type", "write `const int x`, or `const char* s`", spec::Variables);
    if (check(Tok::Ident) && peek().loc.line && peek().text.find("::") != std::string::npos) {
        const std::string module = peek().text.substr(0, peek().text.find("::"));
        fail(peek().loc, "'" + peek().text + "' would add to the module '" + module + "', which a program cannot do",
             "leave out the `" + module + "::`, and use the name by itself", spec::Modules);
    }
    return expect(Tok::Ident, context);
}

// What to do instead of writing an operator, which the token after `operator` decides.
std::string Parser::operatorHelp(size_t ahead) const {
    Tok op = peek(ahead + 1).kind;
    if (op == Tok::EqEq || op == Tok::NotEq)
        return "for a struct, remove it: every struct already has `==` and `!=`, field by field. For a class, `==` means the same object, so write a method: `bool same(User other) const { ... }`";
    return "write a method for now, such as `Point add(Point other)`";
}

const Token& Parser::expect(Tok k, const std::string& givenContext) {
    if (check(k)) return advance();
    // "for the name of the base class" and "for the field name" both say that a name is wanted, which is said already.
    std::string context = givenContext;
    if (k == Tok::Ident) {
        const std::string longForm = "for the name of ", shortEnd = " name";
        if (context.rfind(longForm, 0) == 0) context = "for " + context.substr(longForm.size());
        else if (context.rfind("for the ", 0) == 0 && context.size() > shortEnd.size() && context.compare(context.size() - shortEnd.size(), shortEnd.size(), shortEnd) == 0)
            context = context.substr(0, context.size() - shortEnd.size());
    }
    const std::string wanted = k == Tok::Ident ? "a name" : std::string("'") + tokSpelling(k) + "'";
    std::string message = "expected " + wanted + " " + context + ", found " + describeToken(peek());
    if (k == Tok::Semi && check(Tok::Comma)) {
        std::string help = "write two statements, each with its own `;`";
        if (context == "after the return statement") help = "a function returns one value. To return several, define a struct with a field for each, make it the return type, and write `return {1, 2};`";
        else if (context == "after the loop's first clause") help = "assign one before the loop: `i = 0; for (j = 1; ...)`, or declare both: `for (int i = 0, j = 1; ...)`";
        else if (context == "after the loop condition") help = "join the conditions with `&&`, or test the second in the body of the loop";
        fail(peek().loc, "C+ has no comma operator", help, spec::Grammar);
    }
    // A missing ';' or ')' is best shown right after the previous token.
    bool pointAfter = (k == Tok::Semi || k == Tok::RParen) && pos_ > 0 && peek().loc.line != prev().loc.line;
    fail(pointAfter ? afterPrev() : peek().loc, message, "write " + wanted + " " + context, spec::Grammar);
}

void Parser::syncTopLevel() {
    // Skip to the end of the current declaration: a ';' or a closing '}' at depth 0.
    int depth = 0;
    while (!check(Tok::Eof)) {
        Tok k = advance().kind;
        if (k == Tok::LBrace) ++depth;
        else if (k == Tok::RBrace && --depth <= 0) {
            match(Tok::Semi);  // the end of `struct S { ... };`
            return;
        } else if (k == Tok::Semi && depth == 0) return;
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

// A generic with arguments that could be given to it, for advice.
// What C++ has and C+ does not yet, by the name of its type.
static bool isOtherContainer(const std::string& name) {
    for (const char* other : {"set", "unordered_set", "multiset", "multimap", "deque", "list", "forward_list", "stack", "queue", "priority_queue", "pair", "tuple", "atomic", "channel", "Channel"})
        if (name == other) return true;
    return false;
}

static const char* article(const char* word) { return word[0] == 'i' ? "an " : "a "; }

static bool constraintTakesInt(const std::string& c) {
    return c.empty() || c == "Comparable" || c == "Number" || c == "Integer" || c == "Hashable" || c == "__Hashable";
}

static std::string exampleArguments(const TemplateDecl& t) {
    if (t.name == "hashmap" && t.nameLoc.line == 0) return "hashmap<string, int>";
    std::string written = t.name + "<";
    for (size_t i = 0; i < t.params.size(); ++i) {
        const TemplateParam& p = t.params[i];
        written += std::string(i ? ", " : "") + (p.valueType ? "4" : constraintTakesInt(p.constraint) ? "int" : p.constraint == "Floating" ? "double" : "...");
    }
    return written + ">";
}

// What `...` in those stands for: a type only the program knows of.
static std::string constraintNote(const TemplateDecl& t) {
    for (const TemplateParam& p : t.params)
        if (!p.valueType && !constraintTakesInt(p.constraint) && p.constraint != "Floating")
            return ", with a type that has what '" + p.constraint + "' requires";
    return "";
}

// Code written in C+ that comes with the language. Its tokens have no place in the file.
void Parser::addBuiltinSource(const SourceFile& source) {
    Lexer lexer(source, diags_);
    std::vector<Token> tokens = lexer.lexAll();
    tokens.pop_back();  // its end of file
    for (Token& t : tokens) t.loc = Loc{};
    toks_.insert(toks_.end() - 1, tokens.begin(), tokens.end());  // after the program, so an attribute can still come first
}

// `json::parse`, for a module that is imported, becomes one identifier, which then is a name like any other.
void Parser::joinModuleNames(const std::set<std::string>& modules) {
    std::vector<Token> joined;
    joined.reserve(toks_.size());
    for (size_t i = 0; i < toks_.size(); ++i) {
        // `delete` is a word of the language, but HTTP has a method by that name.
        if (toks_[i].kind == Tok::Ident && modules.count(toks_[i].text) && i + 2 < toks_.size() && toks_[i + 1].kind == Tok::ColonColon &&
            (toks_[i + 2].kind == Tok::Ident || (toks_[i].text == "http" && toks_[i + 2].kind == Tok::KwDelete))) {
            Token name = toks_[i];
            name.text += "::" + toks_[i + 2].text;
            if (toks_[i + 2].loc.line == name.loc.line) name.loc.len = toks_[i + 2].loc.col + toks_[i + 2].loc.len - name.loc.col;
            joined.push_back(std::move(name));
            i += 2;
            continue;
        }
        joined.push_back(std::move(toks_[i]));
    }
    toks_ = std::move(joined);
}

// `Task<int, throws>` is the generic `__TaskThrows<int>`, as a task that can fail differs in what `wait()` can do. The empty
// struct of `Task<void>` is named `void`, which is how it shows, and which nothing in a program can spell by accident.
void Parser::joinThrowingTasks() {
    for (size_t i = 0; i + 1 < toks_.size(); ++i) {
        if (toks_[i].kind == Tok::Ident && toks_[i].text == "__Void") toks_[i].text = "void";
        if (toks_[i].kind != Tok::Ident || toks_[i].text != "Task" || toks_[i + 1].kind != Tok::Less) continue;
        int depth = 0;
        for (size_t j = i + 1; j + 1 < toks_.size(); ++j) {
            Tok k = toks_[j].kind;
            if (k == Tok::Less) ++depth;
            else if (k == Tok::Greater) --depth;
            else if (k == Tok::Shr) depth -= 2;
            else if (k == Tok::Semi || k == Tok::LBrace || k == Tok::RBrace || k == Tok::Eof) break;
            if (depth <= 0) break;
            if (depth == 1 && k == Tok::Comma && toks_[j + 1].kind == Tok::KwThrows) {
                toks_[i].text = "__TaskThrows";
                toks_.erase(toks_.begin() + (std::ptrdiff_t)j, toks_.begin() + (std::ptrdiff_t)j + 2);
                break;
            }
        }
    }
}

void Parser::addBuiltins() {
    bool usesError = false, definesError = false, usesMap = false, definesMap = false;
    bool usesSpawn = false, usesTask = false, definesTask = false;
    bool usesMutex = false, definesMutex = false, usesGuard = false, definesGuard = false;
    Loc spawnLoc, guardLoc;
    // A function or variable of the program with one of these names, as in `int Mutex(int x)`: a type comes before it.
    auto declaresName = [&](size_t i) {
        if (i == 0 || i + 1 >= toks_.size()) return false;
        Tok before = toks_[i - 1].kind, after = toks_[i + 1].kind;
        bool typeBefore = before == Tok::KwInt || before == Tok::KwLong || before == Tok::KwBool || before == Tok::KwString || before == Tok::KwVoid ||
                          before == Tok::KwAuto || before == Tok::KwDouble || before == Tok::KwFloat || before == Tok::KwChar || before == Tok::Ident ||
                          before == Tok::Star || before == Tok::Greater || before == Tok::Shr || before == Tok::Question;
        return typeBefore && (after == Tok::LParen || after == Tok::Assign || after == Tok::Semi);
    };
    std::set<std::string> modules;
    Loc errorModuleLoc;
    std::string errorModule;  // a module that throws, and has objects in maps
    for (size_t i = 0; i < toks_.size(); ++i) {
        Tok k = toks_[i].kind;
        if (k == Tok::KwImport && i + 2 < toks_.size() && toks_[i + 1].kind == Tok::Ident && toks_[i + 2].kind == Tok::Semi &&
            isBuiltinModule(toks_[i + 1].text)) {
            modules.insert(toks_[i + 1].text);
            if (toks_[i + 1].text == "json" || toks_[i + 1].text == "http") {
                errorModule = toks_[i + 1].text;
                errorModuleLoc = toks_[i + 1].loc;
            }
        }
        usesError = usesError || k == Tok::KwTry || k == Tok::KwThrow || k == Tok::KwThrows || k == Tok::KwCatch ||
                    (k == Tok::Ident && toks_[i].text == "Error");
        usesMap = usesMap || (k == Tok::Ident && toks_[i].text == "hashmap");
        if (k == Tok::KwSpawn && !usesSpawn) {
            usesSpawn = true;
            spawnLoc = toks_[i].loc;
        }
        usesTask = usesTask || (k == Tok::Ident && toks_[i].text == "Task");
        usesMutex = usesMutex || (k == Tok::Ident && toks_[i].text == "Mutex");
        if (k == Tok::Ident && toks_[i].text == "LockGuard" && !usesGuard) {
            usesGuard = true;
            guardLoc = toks_[i].loc;
        }
        if (k == Tok::Ident && declaresName(i)) {
            definesMap = definesMap || toks_[i].text == "hashmap";
            definesTask = definesTask || toks_[i].text == "Task";
            definesMutex = definesMutex || toks_[i].text == "Mutex";
            definesGuard = definesGuard || toks_[i].text == "LockGuard";
        }
        if ((k == Tok::KwClass || k == Tok::KwStruct || k == Tok::KwInterface) && i + 1 < toks_.size() && toks_[i + 1].kind == Tok::Ident) {
            definesError = definesError || toks_[i + 1].text == "Error";
            definesMap = definesMap || toks_[i + 1].text == "hashmap";
            definesTask = definesTask || toks_[i + 1].text == "Task";
            definesMutex = definesMutex || toks_[i + 1].text == "Mutex";
            definesGuard = definesGuard || toks_[i + 1].text == "LockGuard";
        }
    }
    if (!errorModule.empty()) {
        usesError = true;  // what it throws
        usesMap = true;    // what a json::Value holds an object in, and an http::Request its headers
        gaveUp_ = gaveUp_ || definesError || definesMap;  // what comes with the language would only report what follows from this
        if (definesError)
            diags_.error(errorModuleLoc, "'" + errorModule + "' uses the built-in class 'Error', but this program has its own",
                         "rename your class. One of your own for errors can derive from the built-in one: `class AppError : public Error { ... };`", spec::Errors);
        if (definesMap)
            diags_.error(errorModuleLoc, "'" + errorModule + "' uses the built-in type 'hashmap', but this program has its own",
                         "rename your type, e.g. to `MyMap`: this import needs the name `hashmap` for the built-in one", spec::HashMaps);
    }
    prog_->usesThreads = usesSpawn;
    if (usesGuard && definesMutex && !definesGuard)
        diags_.error(guardLoc, "'LockGuard' works with the built-in class 'Mutex', but this program has its own",
                     "rename your class, or write a guard of your own for it", spec::Mutex);
    if (usesSpawn && definesTask)
        diags_.error(spawnLoc, "`spawn` gives a built-in class 'Task', but this program has its own",
                     "rename your class, e.g. to `MyTask`: `spawn` needs the name `Task` for the built-in one", spec::Concurrency);
    bool builtinTask = (usesSpawn || usesTask) && !definesTask;
    usesError = usesError || builtinTask;  // what a task that fails holds
    usesMutex = usesMutex || usesGuard;
    prog_->linksThreads = usesSpawn || builtinTask || (usesMutex && !definesMutex);
    if (usesError && !definesError) {
        static const SourceFile builtin("<built-in>",
                                        "class Error {\n"
                                        "public:\n"
                                        "    string message;\n"
                                        "    Error(string m) : message(m) {}\n"
                                        "    virtual string what() const { return message; }\n"
                                        "};\n");
        addBuiltinSource(builtin);
    }
    if (usesMap && !definesMap) {
        static const SourceFile builtin("<built-in>", kHashmapSource);
        addBuiltinSource(builtin);
    }
    if (modules.count("json") && !definesError) {
        static const SourceFile builtin("<built-in>", kJsonSource);
        addBuiltinSource(builtin);
    }
    if (builtinTask) {
        static const SourceFile builtin("<built-in>", kTaskSource);
        addBuiltinSource(builtin);
        joinThrowingTasks();
    }
    if (usesMutex && !definesMutex) {
        static const SourceFile builtin("<built-in>", kMutexSource);
        addBuiltinSource(builtin);
    }
    if (usesGuard && !definesGuard && !definesMutex) {
        static const SourceFile builtin("<built-in>", kLockGuardSource);
        addBuiltinSource(builtin);
    }
    if (modules.count("http") && !definesError) {
        static const SourceFile builtin("<built-in>", kHttpSource);
        addBuiltinSource(builtin);
    }
    if (modules.count("reflect")) {
        static const SourceFile builtin("<built-in>", kReflectSource);
        addBuiltinSource(builtin);
    }
    if (!modules.empty()) joinModuleNames(modules);
}

void Parser::parseProgram(Program& prog) {
    prog_ = &prog;
    addBuiltins();
    try {
        scanDeclarations(prog);
        for (auto& t : prog.templates) readTemplate(*t);
    } catch (TooDeep&) {
        return;
    }
    pos_ = 0;
    while (!check(Tok::Eof)) {
        fileAttr_ = false;
        const size_t declaration = pos_;
        try {
            parseTopLevel(prog);
        } catch (TooDeep&) {
            return;
        } catch (ParseError&) {
            // A function whose header or body could not be read is still there, as far as what calls it can tell.
            for (size_t i = declaration; i < pos_ && i + 1 < toks_.size(); ++i)
                if (toks_[i].kind == Tok::Ident && toks_[i + 1].kind == Tok::LParen) {
                    prog.unreadFunctions.insert(toks_[i].text);
                    break;
                }
            syncTopLevel();
        }
        if (!fileAttr_) declSeen_ = true;
    }
}

// Types and generics can be used before they are defined, since there are no headers or forward
// declarations, so their names are found before anything else is read.
void Parser::scanDeclarations(Program& prog) {
    int depth = 0;
    for (size_t i = 0; i + 1 < toks_.size();) {
        Tok kind = toks_[i].kind;
        if (kind == Tok::LBrace) ++depth;
        else if (kind == Tok::RBrace) --depth;
        if (kind == Tok::KwTemplate && depth == 0) {
            scanTemplate(prog, i);
            continue;
        }
        if ((kind == Tok::KwConst || kind == Tok::KwConstexpr) && depth == 0) constantStarts_.push_back(i);
        bool isRecord = kind == Tok::KwStruct || kind == Tok::KwClass || kind == Tok::KwInterface;
        if (isRecord && toks_[i + 1].kind == Tok::Ident && typeNames_.insert(toks_[i + 1].text).second)
            prog.structs.push_back(std::make_unique<StructDecl>(toks_[i + 1].text, kind != Tok::KwStruct));
        ++i;
    }
}

size_t Parser::afterBraces(size_t i) const {
    int depth = 0;
    for (; i < toks_.size(); ++i) {
        if (toks_[i].kind == Tok::LBrace) ++depth;
        else if (toks_[i].kind == Tok::RBrace && --depth == 0) return i + 1;
    }
    return toks_.size() - 1;
}

void Parser::scanTemplate(Program& prog, size_t& i) {
    size_t first = i;
    auto t = std::make_unique<TemplateDecl>();
    t->allowNewDelete = i >= 2 && toks_[i - 1].kind == Tok::RBracket && toks_[i - 2].kind == Tok::RBracket;
    pos_ = i;
    try {
        parseTemplateHeader(*t);
    } catch (ParseError&) {
        // Reported. The declaration is passed over where it is reached.
        size_t end = pos_;
        while (end + 1 < toks_.size() && toks_[end].kind != Tok::LBrace && toks_[end].kind != Tok::Semi) ++end;
        if (toks_[end].kind == Tok::LBrace) end = afterBraces(end);
        else if (end + 1 < toks_.size()) ++end;
        if (end < toks_.size() && toks_[end].kind == Tok::Semi) ++end;
        skipTemplate_[first] = end;
        i = end;
        return;
    }
    t->start = pos_;
    const Token& head = peek();
    size_t end = pos_;
    if (head.kind == Tok::KwStruct || head.kind == Tok::KwClass || head.kind == Tok::KwInterface) {
        t->kind = TemplateDecl::Kind::Record;
        t->nameLoc = peek(1).loc;
        t->name = peek(1).text;
        while (end + 1 < toks_.size() && toks_[end].kind != Tok::LBrace && toks_[end].kind != Tok::Semi) ++end;
        end = toks_[end].kind == Tok::LBrace ? afterBraces(end) : end;
        if (end + 1 < toks_.size() && toks_[end].kind == Tok::Semi) ++end;
    } else if (head.kind == Tok::Ident && head.text == "concept") {
        t->kind = TemplateDecl::Kind::Concept;
        t->nameLoc = peek(1).loc;
        t->name = peek(1).text;
        while (end + 1 < toks_.size() && toks_[end].kind != Tok::Semi) {
            if (toks_[end].kind == Tok::LBrace) end = afterBraces(end);
            else ++end;
        }
        if (end + 1 < toks_.size()) ++end;
    } else {
        t->kind = TemplateDecl::Kind::Function;
        while (end + 1 < toks_.size() && toks_[end].kind != Tok::LParen && toks_[end].kind != Tok::LBrace &&
               toks_[end].kind != Tok::Semi)
            ++end;
        if (end > pos_ && toks_[end].kind == Tok::LParen && toks_[end - 1].kind == Tok::Ident) {
            t->nameLoc = toks_[end - 1].loc;
            t->name = toks_[end - 1].text;
        }
        while (end + 1 < toks_.size() && toks_[end].kind != Tok::LBrace && toks_[end].kind != Tok::Semi) ++end;
        end = toks_[end].kind == Tok::LBrace ? afterBraces(end) : (end + 1 < toks_.size() ? end + 1 : end);
    }
    t->end = end;
    i = end;
    if (TemplateDecl* other = prog.findTemplate(t->name); other && !t->name.empty()) {
        diags_.error(t->nameLoc, "the name '" + t->name + "' is already used by another generic", "rename one of them",
                     spec::Generics);
        diags_.note(other->nameLoc, "the first definition is here");
        skipTemplate_[first] = end;
        return;
    }
    templateAt_[first] = t.get();
    prog.templates.push_back(std::move(t));
}

// At `template`: reads `template<typename T, Comparable U, int N>`.
void Parser::parseTemplateHeader(TemplateDecl& t) {
    Loc start = advance().loc;
    if (!check(Tok::Less))
        fail(start, "expected '<' after 'template'", "write e.g. `template<typename T>`", spec::Generics);
    advance();
    if (check(Tok::Greater))
        fail(start, "C+ has no template specialization",
             "write one definition for every type. For a type that needs something different, use another name for it",
             spec::Generics);
    do {
        TemplateParam p;
        if (match(Tok::KwTypename) || match(Tok::KwClass)) {
            // a type, which can be anything
        } else if (check(Tok::KwInt) || check(Tok::KwLong) || check(Tok::KwBool) || check(Tok::KwChar)) {
            Loc loc;
            p.valueType = parseBaseType(loc);
        } else if (check(Tok::Ident)) {
            p.constraint = peek().text;
            p.constraintLoc = advance().loc;
        } else {
            fail(peek().loc, "expected a template parameter, found " + describeToken(peek()),
                 "write `typename T`, a constraint such as `Comparable T`, or a value such as `int N`", spec::Generics);
        }
        if (check(Tok::Dot))
            fail(peek().loc, "C+ has no templates with any number of parameters",
                 "write the parameters one by one, or pass a vector", spec::Generics);
        const Token& name = expectNewName("for the name of the template parameter");
        if (check(Tok::Assign)) notYet(peek().loc, "default template arguments", "remove `= ...`, and give the arguments wherever it is used");
        for (auto& other : t.params)
            if (other.name == name.text)
                fail(name.loc, "the template parameter '" + name.text + "' is written twice", "rename one of them", spec::Generics);
        p.name = name.text;
        p.loc = name.loc;
        if (!p.valueType) {
            Type placeholder{Type::Kind::TypeParam, nullptr};
            placeholder.length = (int64_t)t.params.size();
            placeholders_.push_back(placeholder);
            names_.push_back(p.name);
            placeholders_.back().name = names_.back().c_str();
            p.placeholder = &placeholders_.back();
        }
        t.params.push_back(std::move(p));
    } while (match(Tok::Comma));
    if (!check(Tok::Greater))
        fail(peek().loc, "expected '>' to close the template parameters, found " + describeToken(peek()), "write the parameters with commas between them: `template<typename T, int N>`", spec::Generics);
    advance();
    if (check(Tok::Ident) && peek().text == "requires")
        notYet(peek().loc, "requires clauses", "write the constraint on the parameter: `template<Comparable T>`");
    if (check(Tok::KwTemplate))
        fail(peek().loc, "a generic has one `template<...>`", "put all of its parameters in one list", spec::Generics);
}

Parser::GenericScope::GenericScope(Parser& parser)
    : p(parser), pos(parser.pos_), bindings(std::move(parser.bindings_)), current(parser.currentTemplate_),
      currentArgs(std::move(parser.currentArgs_)), injected(parser.injected_), dryRun(parser.dryRun_), splitShr(parser.splitShr_),
      localScopes(std::move(parser.localScopes_)) {
    p.bindings_.clear();
    p.currentArgs_.clear();
    p.localScopes_.clear();
    p.splitShr_ = SIZE_MAX;
}

Parser::GenericScope::~GenericScope() {
    p.pos_ = pos;
    p.bindings_ = std::move(bindings);
    p.currentTemplate_ = current;
    p.injected_ = injected;
    p.dryRun_ = dryRun;
    p.splitShr_ = splitShr;
    p.currentArgs_ = std::move(currentArgs);
    p.localScopes_ = std::move(localScopes);
}

// The file's constants and constexpr functions, wherever they are, for values worked out before
// the parser reaches them. They are read again at their place, where errors are reported.
void Parser::readConstants() {
    if (constantsRead_) return;
    constantsRead_ = true;
    GenericScope scope(*this);
    dryRun_ = true;
    currentTemplate_ = nullptr;
    injected_ = nullptr;
    diags_.mute();
    struct Unmute {
        Diagnostics& d;
        ~Unmute() { d.unmute(); }
    } unmute{diags_};
    for (size_t start : constantStarts_) {
        pos_ = start;
        try {
            bool isConstexpr = match(Tok::KwConstexpr);
            match(Tok::KwConst);
            Tok k = peek().kind;
            if (k != Tok::KwInt && k != Tok::KwLong && k != Tok::KwBool && k != Tok::KwChar && k != Tok::KwFloat &&
                k != Tok::KwDouble && k != Tok::KwString && k != Tok::KwAuto)
                continue;
            Loc loc;
            const Type* type = parseBaseType(loc);
            if (check(Tok::Ident) && peek(1).kind == Tok::LParen) {
                if (!isConstexpr) continue;
                auto fn = parseFunction(type, loc);
                fn->isConstexpr = true;
                std::string name = fn->name;
                constexprFunctions_[name] = std::move(fn);
                continue;
            }
            constantDecls_.push_back(parseDeclarators(true, type, loc));
            Stmt* decl = constantDecls_.back().get();
            if (decl->kind == Stmt::Kind::VarDecl) {
                auto* v = static_cast<VarDecl*>(decl);
                v->isTopLevel = true;
                constants_[v->name] = v;
            } else {
                for (auto& v : static_cast<DeclGroupStmt*>(decl)->decls) {
                    v->isTopLevel = true;
                    constants_[v->name] = v.get();
                }
            }
        } catch (ParseError&) {
        } catch (TooDeep&) {
        }
    }
}

ConstHooks Parser::constHooks() {
    ConstHooks hooks;
    hooks.constant = [this](const std::string& name) -> const VarDecl* {
        for (size_t i = localScopes_.size(); i-- > 0;)
            if (auto it = localScopes_[i].find(name); it != localScopes_[i].end()) return it->second;
        readConstants();
        auto it = constants_.find(name);
        return it == constants_.end() ? nullptr : it->second;
    };
    hooks.globalConstant = [this](const std::string& name) -> const VarDecl* {
        readConstants();
        auto it = constants_.find(name);
        return it == constants_.end() ? nullptr : it->second;
    };
    hooks.function = [this](const std::string& name) -> const FuncDecl* {
        readConstants();
        auto it = constexprFunctions_.find(name);
        return it == constexprFunctions_.end() ? nullptr : it->second.get();
    };
    hooks.generic = [this](const std::string& name) { return functionTemplate(name) != nullptr; };
    hooks.constraint = [](const ConstraintExpr& c) {
        std::optional<bool> holds = builtinConstraint(c.name, c.first, c.second);
        if (!holds) throw ConstError{c.loc, "'" + c.name + "' can only be asked where everything has been read, such as in `if constexpr`"};
        return *holds;
    };
    hooks.sizeOf = [](const Type* t, Loc loc) -> int64_t {
        switch (t->kind) {
            case Type::Kind::Bool: case Type::Kind::Char: return 1;
            case Type::Kind::Int: case Type::Kind::Float: return 4;
            case Type::Kind::Long: case Type::Kind::Double: case Type::Kind::Pointer: case Type::Kind::Class:
            case Type::Kind::Vector: case Type::Kind::Weak:
                return 8;
            case Type::Kind::String: return 24;
            default: throw ConstError{loc, "the size of '" + std::string(t->name) + "' is not known yet where this is read"};
        }
    };
    return hooks;
}

ConstValue Parser::evaluate(const Expr& e, const std::string& what) {
    ConstEval eval(constHooks());
    try {
        return eval.eval(e);
    } catch (ConstError& error) {
        diags_.error(e.loc, what + ", but " + error.message,
                     "use a literal, a `const` or `constexpr` value, or a call to a `constexpr` function. For a size only known "
                     "while the program runs, use a vector",
                     spec::Constexpr);
        if (error.loc.line && error.loc.line != e.loc.line) diags_.note(error.loc, "this is what cannot be worked out");
        throw ParseError{};
    }
}

bool Parser::isConstraintName(const std::string& name) const {
    if (classTemplate(name) || typeNames_.count(name)) return false;
    if (isBuiltinConstraintName(name)) return true;
    TemplateDecl* t = prog_->findTemplate(name);
    return t && t->kind == TemplateDecl::Kind::Concept;
}

StmtPtr Parser::readBranch(const LazyBranches& lazy, bool thenBranch) {
    size_t start = thenBranch ? lazy.thenStart : lazy.elseStart;
    if (!start) return nullptr;
    GenericScope scope(*this);
    dryRun_ = false;
    bindTemplate(*lazy.tmpl, lazy.args);
    injected_ = lazy.injected;
    localScopes_ = lazy.localScopes;
    pos_ = start;
    try {
        return parseStmt();
    } catch (ParseError&) {
    } catch (TooDeep&) {
    }
    return std::make_unique<ErrorStmt>(Loc{});
}

StmtPtr Parser::parseIf(bool isConstexpr, size_t ifPos, Loc loc) {
    expect(Tok::LParen, isConstexpr ? "after 'if constexpr'" : "after 'if'");
    std::optional<CondDecl> decl;
    if (isConstexpr && isDeclarationStart())
        fail(peek().loc, "the condition of `if constexpr` cannot declare a variable", "write `constexpr int x = ...;` before the `if`. For a value only known while the program runs, use a plain `if`", spec::Constexpr);
    ExprPtr cond = isDeclarationStart() ? (decl = parseCondDecl("if"), nullptr) : parseExpr();
    expect(Tok::RParen, "after the if condition");
    if (isConstexpr && currentTemplate_ && !dryRun_) {  // what runs is read once it is known which
        ConstexprIf place = constexprIfs_.at(ifPos);
        auto st = std::make_unique<IfStmt>(loc, std::move(cond), nullptr, nullptr);
        st->isConstexpr = true;
        st->lazy = std::make_shared<LazyBranches>();
        st->lazy->thenStart = place.thenStart;
        st->lazy->elseStart = place.elseStart;
        st->lazy->tmpl = currentTemplate_;
        st->lazy->args = currentArgs_;
        st->lazy->injected = injected_;
        st->lazy->localScopes = localScopes_;
        pos_ = place.end;
        return st;
    }
    size_t thenStart = pos_;
    StmtPtr thenS = parseStmt();
    size_t elseStart = 0;
    StmtPtr elseS;
    if (match(Tok::KwElse)) {
        elseStart = pos_;
        elseS = parseStmt();
    }
    if (isConstexpr && dryRun_) constexprIfs_[ifPos] = {thenStart, elseStart, pos_};
    auto st = std::make_unique<IfStmt>(loc, std::move(cond), std::move(thenS), std::move(elseS));
    st->decl = std::move(decl);
    st->isConstexpr = isConstexpr;
    return st;
}

// Reads a definition with its parameters unknown. Nothing is kept but a function's parameter
// types, from which a call works out the arguments. Using the definition reads it again.
void Parser::readTemplate(TemplateDecl& t) {
    if (t.failed) return;
    GenericScope scope(*this);
    dryRun_ = true;
    currentTemplate_ = &t;
    pos_ = t.start;
    std::vector<TemplateArg> args;
    for (size_t i = 0; i < t.params.size(); ++i) {
        TemplateBinding binding;
        binding.type = t.params[i].placeholder;
        binding.valueType = t.params[i].valueType;
        binding.value.valueParam = (int)i;
        bindings_[t.params[i].name] = binding;
        TemplateArg arg;
        arg.type = t.params[i].placeholder;
        arg.valueParam = (int)i;
        args.push_back(arg);
    }
    int errors = diags_.errorCount();
    try {
        if (t.kind == TemplateDecl::Kind::Record) {
            patterns_.push_back({&t, args, instanceName(t, args), Type{Type::Kind::Dependent, nullptr}});
            patterns_.back().type.name = patterns_.back().name.c_str();
            patterns_.back().type.pattern = &patterns_.back();
            injected_ = &patterns_.back().type;
            StructDecl scratch(t.name, peek().kind != Tok::KwStruct);
            parseRecord(&scratch);
        } else if (t.kind == TemplateDecl::Kind::Concept) {
            ConceptBody body;
            parseConcept(body);
        } else {
            match(Tok::KwConstexpr);
            Loc retLoc;
            bool isConst = false;
            const Type* ret = parseType(retLoc, &isConst);
            if (isConst) fail(retLoc, "a function cannot return a const value", "remove `const`", spec::Functions);
            if (ret->is(Type::Kind::Auto)) notYet(retLoc, "'auto' return types", "write the return type, e.g. `T`");
            t.pattern = parseFunction(ret, retLoc);
        }
    } catch (ParseError&) {
        t.failed = true;  // what stopped it has been reported
        return;
    }
    t.failed = diags_.errorCount() != errors;
}

void Parser::bindTemplate(const TemplateDecl& t, const std::vector<TemplateArg>& args) {
    currentTemplate_ = &t;
    currentArgs_ = args;
    for (size_t i = 0; i < t.params.size(); ++i) {
        TemplateBinding binding;
        binding.type = t.params[i].valueType ? nullptr : args[i].type;
        binding.value = args[i];
        binding.valueType = t.params[i].valueType;
        bindings_[t.params[i].name] = binding;
    }
}

std::string Parser::instanceName(const TemplateDecl& t, const std::vector<TemplateArg>& args) const {
    if (t.nameLoc.line == 0 && t.name == "__TaskThrows") return "Task<" + std::string(args[0].type->name) + ", throws>";
    std::string name = t.name + "<";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) name += ", ";
        const Type* valueType = t.params[i].valueType;
        if (!valueType) name += args[i].type->name;
        else if (args[i].valueParam >= 0) name += t.params[args[i].valueParam].name;
        else if (valueType->is(Type::Kind::Bool)) name += args[i].value ? "true" : "false";
        else if (valueType->is(Type::Kind::Char)) name += "'" + std::string(1, (char)args[i].value) + "'";
        else name += std::to_string(args[i].value);
    }
    return name + ">";
}

TemplateDecl* Parser::classTemplate(const std::string& name) const {
    TemplateDecl* t = prog_->findTemplate(name);
    return t && t->kind == TemplateDecl::Kind::Record ? t : nullptr;
}

TemplateDecl* Parser::functionTemplate(const std::string& name) const {
    TemplateDecl* t = prog_->findTemplate(name);
    return t && t->kind == TemplateDecl::Kind::Function ? t : nullptr;
}

const Type* Parser::boundType(const Token& t) const {
    if (t.kind != Tok::Ident) return nullptr;
    if (auto it = bindings_.find(t.text); it != bindings_.end()) return it->second.type;
    if (injected_ && currentTemplate_ && t.text == currentTemplate_->name) return injected_;
    return nullptr;
}

bool Parser::isTypeName(const Token& t) const { return t.kind == Tok::Ident && (typeNames_.count(t.text) || boundType(t)); }

void Parser::rejectTemplateParamName(const Token& name) {
    if (bindings_.count(name.text))
        fail(name.loc, "'" + name.text + "' is a template parameter here, so it cannot also be a variable",
             "give the variable another name", spec::Generics);
}

TemplateArg Parser::parseTemplateValue(const TemplateParam& p) {
    const Token& t = peek();
    Tok after = peek(1).kind;
    if (t.kind == Tok::Ident && (after == Tok::Comma || after == Tok::Greater || after == Tok::Shr)) {  // `N` alone
        auto it = bindings_.find(t.text);
        if (it != bindings_.end() && !it->second.type) {
            advance();
            return it->second.value;
        }
    }
    ExprPtr e = parseBinary(9);  // no `>` at its top, which closes the arguments; parentheses allow one
    TemplateArg arg;
    if (dryRun_) return arg;  // its parameters are not known yet
    ConstValue v = evaluate(*e, "a template argument must be known at compile time");
    bool wantsBool = p.valueType->is(Type::Kind::Bool), wantsChar = p.valueType->is(Type::Kind::Char);
    bool fits = wantsBool ? v.kind == ConstValue::Kind::Bool
                : wantsChar ? v.kind == ConstValue::Kind::Char
                            : v.kind == ConstValue::Kind::Int || v.kind == ConstValue::Kind::Long || v.kind == ConstValue::Kind::Char;
    if (p.valueType->is(Type::Kind::Int) && (v.i < INT32_MIN || v.i > INT32_MAX)) fits = false;
    if (!fits)
        fail(e->loc, "expected a value of type '" + std::string(p.valueType->name) + "' for the template parameter '" + p.name +
                         "', but this is a '" + v.typeName() + "'",
             "write a value of that type", spec::Generics);
    arg.value = v.i;
    return arg;
}

std::vector<TemplateArg> Parser::parseTemplateArgs(const TemplateDecl& t, Loc nameLoc, bool partial) {
    advance();  // the '<'
    std::vector<TemplateArg> args;
    if (!check(Tok::Greater) && !check(Tok::Shr)) {
        do {
            if (args.size() == t.params.size())
                fail(peek().loc, "'" + t.name + "' takes " + std::to_string(t.params.size()) + " template argument" +
                                     (t.params.size() == 1 ? "" : "s") + ", but more were given",
                     "write e.g. `" + exampleArguments(t) + "`" + constraintNote(t), spec::Generics);
            const TemplateParam& p = t.params[args.size()];
            if (p.valueType) {
                args.push_back(parseTemplateValue(p));
                continue;
            }
            Loc loc;
            TemplateArg arg;
            arg.type = parseType(loc);
            if (arg.type->is(Type::Kind::Auto))
                fail(loc, "'auto' cannot be given for a template parameter", "write the type", spec::Generics);
            args.push_back(arg);
        } while (match(Tok::Comma));
    }
    expectCloseAngle("to close the template arguments of '" + t.name + "'");
    if (!partial && args.size() != t.params.size())
        fail(nameLoc, "'" + t.name + "' takes " + std::to_string(t.params.size()) + " template argument" +
                          (t.params.size() == 1 ? "" : "s") + ", but " + std::to_string(args.size()) +
                          (args.size() == 1 ? " was" : " were") + " given",
             "write e.g. `" + exampleArguments(t) + "`" + constraintNote(t), spec::Generics);
    return args;
}

const Type* Parser::genericRecord(TemplateDecl& t, Loc nameLoc) {
    std::vector<TemplateArg> args = parseTemplateArgs(t, nameLoc, false);
    if (t.nameLoc.line == 0 && (t.name == "Task" || t.name == "__TaskThrows") && args[0].type->is(Type::Kind::Void))
        args[0].type = &prog_->findStruct("void")->type;
    if (t.nameLoc.line == 0 && t.name == "hashmap" && args[1].type->is(Type::Kind::Void))
        fail(nameLoc, "a hashmap cannot hold 'void' values", "for a key with nothing to say about it, write `hashmap<string, bool>`", spec::HashMaps);
    if (!dryRun_) return instantiateRecord(t, args, nameLoc);
    patterns_.push_back({&t, args, instanceName(t, args), Type{Type::Kind::Dependent, nullptr}});
    patterns_.back().type.name = patterns_.back().name.c_str();
    patterns_.back().type.pattern = &patterns_.back();
    return &patterns_.back().type;
}

const Type* Parser::taskType(const Type* result, bool canFail, Loc usedAt) {
    TemplateArg arg;
    arg.type = result->is(Type::Kind::Void) ? &prog_->findStruct("void")->type : result;
    return instantiateRecord(*classTemplate(canFail ? "__TaskThrows" : "Task"), {arg}, usedAt);
}

const Type* Parser::instantiateRecord(TemplateDecl& t, const std::vector<TemplateArg>& args, Loc usedAt) {
    std::string name = instanceName(t, args);
    if (auto it = t.records.find(name); it != t.records.end()) return &it->second->type;
    if (t.failed) throw ParseError{};  // reported where it is defined
    if (genericDepth_ >= 64 || name.size() > 2000 || instanceCount_ >= 20000)
        fail(usedAt, "the generic type '" + t.name + "' keeps needing larger versions of itself, which would never end",
             "a field or method uses this generic with a larger argument, like `G<vector<T>>` inside `G<T>`. Use `G<T>` there, or another type", spec::Generics);
    bool isClass = toks_[t.start].kind != Tok::KwStruct;
    prog_->structs.push_back(std::make_unique<StructDecl>(name, isClass));
    StructDecl* sd = prog_->structs.back().get();
    sd->templateOf = &t;
    sd->templateArgs = args;
    sd->templateName = t.name;
    sd->usedAt = usedAt;
    t.records[name] = sd;  // before its body, which can refer to it
    ++instanceCount_;
    GenericScope scope(*this);
    dryRun_ = false;
    bindTemplate(t, args);
    injected_ = &sd->type;
    pos_ = t.start;
    ++genericDepth_;
    diags_.pushContext(usedAt, "in '" + name + "', which is used here");
    struct Leave {
        Parser& p;
        ~Leave() {
            --p.genericDepth_;
            p.diags_.popContext();
        }
    } leave{*this};
    try {
        parseRecord(sd);
    } catch (ParseError&) {
        sd->defined = true;  // reported; what was read is still a type
    }
    return &sd->type;
}

FuncDecl* Parser::instantiateFunction(TemplateDecl& t, const std::vector<TemplateArg>& args, Loc usedAt, int depth) {
    std::string name = instanceName(t, args);
    if (auto it = t.functions.find(name); it != t.functions.end()) return it->second;
    if (t.failed) return nullptr;
    t.functions[name] = nullptr;
    GenericScope scope(*this);
    dryRun_ = false;
    injected_ = nullptr;
    bindTemplate(t, args);
    pos_ = t.start;
    diags_.pushContext(usedAt, "in '" + name + "', which is used here");
    struct Leave {
        Parser& p;
        ~Leave() { p.diags_.popContext(); }
    } leave{*this};
    try {
        bool isConstexpr = match(Tok::KwConstexpr);
        Loc retLoc;
        bool isConst = false;
        const Type* ret = parseType(retLoc, &isConst);
        auto fn = parseFunction(ret, retLoc);
        fn->isConstexpr = isConstexpr;
        fn->name = name;
        fn->allowNewDelete = t.allowNewDelete;
        fn->usedAt = usedAt;
        fn->genericDepth = depth;
        fn->requested = true;
        prog_->funcs.push_back(std::move(fn));
        return t.functions[name] = prog_->funcs.back().get();
    } catch (ParseError&) {
        return nullptr;
    } catch (TooDeep&) {
        return nullptr;
    }
}

// `concept Addable = requires(T a, T b) { a + b; };`
void Parser::parseConcept(ConceptBody& out) {
    advance();  // `concept`
    const Token& name = expectNewName("for the name of the constraint");
    expect(Tok::Assign, "after the name of the constraint");
    if (!check(Tok::Ident) || peek().text != "requires")
        notYet(peek().loc, "constraints that are not a `requires` expression", "write `template<typename T> concept " + name.text + " = requires(T a) { ... };`");
    advance();
    out.params = std::make_unique<FuncDecl>();
    out.params->name = "requires";
    if (check(Tok::LParen)) parseParams(*out.params);
    expect(Tok::LBrace, "to start what the constraint requires");
    while (!check(Tok::RBrace) && !check(Tok::Eof)) {
        if (check(Tok::LBrace) || check(Tok::KwTypename) || (check(Tok::Ident) && peek().text == "requires"))
            notYet(peek().loc, "requirements other than an expression", "write an expression that must be valid, e.g. `a + b;`");
        out.requirements.push_back(parseExpr());
        expect(Tok::Semi, "after the requirement");
    }
    expect(Tok::RBrace, "to close what the constraint requires");
    expect(Tok::Semi, "after the constraint");
}

bool Parser::instantiateConcept(TemplateDecl& t, const TemplateArg& arg, ConceptBody& out) {
    if (t.failed) return false;
    GenericScope scope(*this);
    dryRun_ = false;
    injected_ = nullptr;
    bindTemplate(t, {arg});
    pos_ = t.start;
    try {
        parseConcept(out);
        return true;
    } catch (ParseError&) {
        return false;
    } catch (TooDeep&) {
        return false;
    }
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
             "remove it. The one attribute C+ has is `[[allow(new_delete)]]`, which turns off the warning for new and delete",
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

void Parser::rejectOtherLanguages() {
    const Token& t = peek();
    if (t.kind == Tok::KwAsync) notYet(t.loc, "async and await", "to run something without waiting for it, write a plain function: `auto t = spawn f();`, then `t.wait()`");
    if (t.kind != Tok::Ident || isTypeName(t) || boundType(t) || classTemplate(t.text)) return;
    // Only when a name or a type comes next, which an expression cannot have: `let` can still be the name of a variable.
    Tok next = peek(1).kind;
    if (t.text == "union" && next == Tok::LBrace) notYet(t.loc, "unions", "use a struct with a field for each, or classes that derive from one");
    if (next != Tok::Ident && next != Tok::KwInt && next != Tok::KwLong && next != Tok::KwBool && next != Tok::KwString && next != Tok::KwDouble && next != Tok::KwFloat &&
        next != Tok::KwChar && next != Tok::KwVoid && next != Tok::KwAuto && next != Tok::KwConst && next != Tok::KwStruct && next != Tok::KwClass && next != Tok::KwNamespace)
        return;
    const std::string& w = t.text;
    if (w == "typedef") fail(t.loc, "C+ has no typedef", "write the type itself. A struct or a class already goes by its name", spec::Variables);
    if (w == "using") fail(t.loc, "C+ has no 'using' declarations", "built-ins like println, string and vector need no std:: prefix, so `using` has nothing to do", spec::Modules);
    if (w == "inline") fail(t.loc, "C+ has no 'inline'", "remove it: with -O, the compiler decides what to inline", spec::Functions);
    if (w == "goto") fail(t.loc, "C+ has no goto", "use a loop with `break` or `continue`, or a function with `return`", spec::ControlFlow);
    if (w == "register") fail(t.loc, "C+ has no 'register'", "remove it: the compiler decides", spec::Variables);
    if (w == "volatile") fail(t.loc, "C+ has no 'volatile'", "remove it. For what threads share, use a field of a class and a `Mutex`", spec::Mutex);
    if (w == "mutable") fail(t.loc, "C+ has no 'mutable'", "remove `mutable`, and take `const` off the method that changes it", spec::Methods);
    if (w == "explicit") fail(t.loc, "C+ has no 'explicit'", "remove it: a plain value never converts to a class by itself", spec::Classes);
    if (w == "friend") fail(t.loc, "C+ has no 'friend'", "make what it needs public, or give the class a method for it", spec::Classes);
    if (w == "union") notYet(t.loc, "unions", "use a struct with a field for each, or classes that derive from one");
    if (w == "let" || w == "var")
        fail(t.loc, "C+ declares a variable with its type, or `auto`",
             localScopes_.empty() ? "write `const auto x = 1;`: variables that change are not supported outside functions yet"
                                  : "write `auto x = 1;`, or the type in place of `auto`",
             spec::Variables);
    if ((w == "function" || w == "fn" || w == "func" || w == "def") && !localScopes_.empty())
        fail(t.loc, "C+ has no functions inside functions", "write `void " + peek(1).text + "() { ... }` at the top level of the file, and pass it what it needs", spec::Functions);
    if (w == "function" || w == "fn" || w == "func" || w == "def") fail(t.loc, "C+ writes a function with its return type", peek(2).kind == Tok::LParen && peek(3).kind == Tok::RParen
                 ? "write `" + std::string(peek(1).text == "main" ? "int " : "void ") + peek(1).text + "() { ... }`: a return type first, `void` if none"
                 : peek(1).text == "main" ? std::string("write `int main(int argc, char** argv) { ... }`: a return type first, and a type for each parameter")
                 : "write `int " + peek(1).text + "(int a) { ... }`: a return type first, `void` if none, and a type for each parameter", spec::Functions);
}

void Parser::parseTopLevel(Program& prog) {
    rejectOtherLanguages();
    const Token& t = peek();
    switch (t.kind) {
        case Tok::KwImport: {
            Loc loc = advance().loc;
            if (check(Tok::Less) || check(Tok::StringLit))
                fail(peek().loc, "module names are written without quotes or angle brackets",
                     "write the name alone: `import http;`", spec::Modules);
            std::string path = expect(Tok::Ident, "after 'import'").text;
            while (match(Tok::Dot)) path += "." + expect(Tok::Ident, "after '.' in the module name").text;
            expect(Tok::Semi, "after the import");
            loc.len = prev().loc.col + prev().loc.len - loc.col;
            prog.imports.push_back(ImportDecl{path, loc});
            return;
        }
        case Tok::KwStruct:
        case Tok::KwClass:
        case Tok::KwInterface: parseRecord(); return;
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
        case Tok::KwTemplate: {  // read when the file was first scanned
            if (auto it = templateAt_.find(pos_); it != templateAt_.end()) pos_ = it->second->end;
            else if (auto bad = skipTemplate_.find(pos_); bad != skipTemplate_.end()) pos_ = bad->second;
            else throw ParseError{};
            return;
        }
        case Tok::KwEnum: notYet(t.loc, "enums", "use constants for now: `const int RED = 0;`");
        case Tok::KwConstexpr: {
            advance();
            if (check(Tok::KwStruct) || check(Tok::KwClass))
                notYet(prev().loc, "constexpr structs", "leave out `constexpr`, and use `constexpr` functions and constants of numbers, `char`, `bool` or `string`");
            Loc typeLoc;
            bool isConst = false;
            const Type* type = parseType(typeLoc, &isConst);
            if (check(Tok::Ident) && peek(1).kind == Tok::LParen) {
                if (type->is(Type::Kind::Auto)) notYet(typeLoc, "'auto' return types", "write the return type, e.g. `int`");
                prog.funcs.push_back(parseFunction(type, typeLoc));
                prog.funcs.back()->isConstexpr = true;
                return;
            }
            return parseConstants(prog, type, typeLoc, true);
        }
        case Tok::KwStatic:
            fail(t.loc, "C+ has no 'static' outside a type", "remove it, which leaves a constant or a function as it was",
                 spec::Constants);
        case Tok::KwExtern: parseExtern(prog); return;
        case Tok::KwNamespace:
            fail(t.loc, "C+ has no namespaces; each file is a module", "remove the `namespace ... {` line and its closing `}`, and keep what is inside", spec::Modules);
        case Tok::Ident:
            if (peek(1).kind == Tok::ColonColon && isTypeName(t))
                fail(t.loc, "methods and constructors are defined inside their " +
                                std::string(prog_->findStruct(t.text)->keyword()),
                     "move this definition into the body of '" + t.text + "'; C+ has no headers, so there is no "
                     "separate declaration to match",
                     spec::Classes);
            if (t.text == "std" && peek(1).kind == Tok::ColonColon) failStd(t.loc, peek(2));
            break;
        default: break;
    }
    if (check(Tok::Ident) && peek(1).kind == Tok::KwOperator) notYet(peek(1).loc, "operator overloading", operatorHelp(1));
    if (check(Tok::Ident) && peek(1).kind == Tok::Amp && peek(2).kind == Tok::KwOperator) notYet(peek(2).loc, "operator overloading", operatorHelp(2));
    if (!isTypeStart()) {
        checkUnknownType();
        fail(t.loc, "expected a function or an import, found " + describeToken(t),
             t.kind == Tok::Semi ? "remove this ';': the `}` of a function needs none"
             : t.kind == Tok::RBrace ? "remove it, as this closes nothing, or check the braces before it"
                                     : "write what goes at the top of a file, such as `int main() { ... }` or `import http;`",
             spec::Functions);
    }
    Loc retLoc;
    bool isConst = false;
    const Type* ret = parseType(retLoc, &isConst);
    if (check(Tok::Ident) && peek(1).kind == Tok::ColonColon && isTypeName(peek()))
        fail(peek().loc, "methods are defined inside their " + std::string(prog_->findStruct(peek().text)->keyword()),
             "move this definition into the body of '" + peek().text + "'; C+ has no headers, so there is no "
             "separate declaration to match",
             spec::Classes);
    if (isConst && check(Tok::Ident) && peek(1).kind != Tok::LParen) return parseConstants(prog, ret, retLoc, false);
    if (ret->is(Type::Kind::Auto) && (!check(Tok::Ident) || peek(1).kind == Tok::LParen)) notYet(retLoc, "'auto' return types", "write the return type, e.g. `int`");
    if (check(Tok::Ident) && peek(1).kind != Tok::LParen)
        notYet(peek().loc, "global variables",
               ret->is(Type::Kind::Auto)
                   ? "for a value that never changes, write a constant: `const auto " + peek().text + " = ...;`"
               : peek(1).kind == Tok::LBracket
                   ? "pass the value to the functions that use it, or keep it in a class object. An array is written `array<" + std::string(ret->name) + ", N>`"
               : ret->isPointer() && ret->elem->is(Type::Kind::Char) && peek(1).kind == Tok::Assign && peek(2).kind == Tok::StringLit
                   ? "for text that never changes, write `const string " + peek().text + " = " + "...;`"
               : ret->isPointer()
                   ? "for a pointer that never changes, write a constant, with `const` after the '*': `" +
                         std::string(ret->name) + " const " + peek().text + " = null;`"
                   : ret->isNumber() || ret->is(Type::Kind::Bool) || ret->is(Type::Kind::Char) || ret->is(Type::Kind::String)
                       ? "for a value that never changes, write a constant: `const " + std::string(ret->name) + " " + peek().text + " = ..." +
                             ";`. For one that changes, pass it to the functions that use it, or keep it in a class object"
                       : "pass the value to the functions that use it, or keep it in a class object. A function that returns it "
                         "also works, if it never changes");
    prog.funcs.push_back(parseFunction(ret, retLoc));
}

// `const int LIMIT = 10, TWICE = LIMIT * 2;`, after the type.
void Parser::parseConstants(Program& prog, const Type* type, Loc typeLoc, bool isConstexpr) {
    StmtPtr decl = parseDeclarators(true, type, typeLoc);
    expect(Tok::Semi, "after the constant");
    auto add = [&](std::unique_ptr<VarDecl> constant) {
        constant->isTopLevel = true;
        constant->isConstexpr = isConstexpr;
        prog.constants.push_back(std::move(constant));
    };
    if (decl->kind == Stmt::Kind::VarDecl) add(std::unique_ptr<VarDecl>(static_cast<VarDecl*>(decl.release())));
    else for (auto& constant : static_cast<DeclGroupStmt&>(*decl).decls) add(std::move(constant));
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
             "for a number, write the number type. Write a C struct as `struct div_t { int quot; int rem; };`, or, for one used only through pointers, "
             "`struct sqlite3;`. C+ code then uses the name without 'struct'",
             spec::CTypes);
    if (kw.kind == Tok::KwClass)
        fail(kw.loc, "C has no classes", "declare a C type with `struct`, e.g. `struct sqlite3;`", spec::CTypes);
    const Token& name = expectNewName("for the name of the C struct");
    if (check(Tok::LBrace)) {  // an ordinary struct, except that its fields must all be C types
        pos_ -= 2;
        struct Inside {
            bool& flag;
            explicit Inside(bool& f) : flag(f) { flag = true; }
            ~Inside() { flag = false; }
        } inside(inExternStruct_);
        parseRecord();
        prog_->findStruct(name.text)->externC = true;
        return;
    }
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
    const Token& name = expectNewName("for the name of the C function");
    fn->name = name.text;
    fn->nameLoc = name.loc;
    if (!check(Tok::LParen))
        fail(name.loc, "this compiler does not support C variables, such as errno, yet",
             "write a small function in C that returns it, and declare that function", spec::CFunctions);
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
            if (check(Tok::LParen) && peek(1).kind == Tok::Star)
                notYet(peek().loc, "C callbacks", "a C function that takes a function pointer cannot be declared yet. Write a small wrapper for it in C, and declare that");
            Loc nameLoc = typeLoc;
            std::string pname;
            if (check(Tok::Ident)) {
                const Token& t = advance();
                pname = t.text;
                nameLoc = t.loc;
            }
            if (check(Tok::LBracket))
                fail(peek().loc, "a C array parameter is written as a pointer",
                     ty->is(Type::Kind::String)
                         ? "write `char** " + (pname.empty() ? std::string("values") : pname) + "`: C passes an array of text as a pointer to its first `char*`"
                         : "write `" + std::string(ty->name) + "* " + (pname.empty() ? "values" : pname) + "`: C passes an array as a pointer to its first element",
                     spec::CFunctions);
            if (check(Tok::Assign)) notYet(peek().loc, "default argument values", "remove `= ...`, and write the value at each call");
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
    if (isConst && base.kind == Tok::KwChar && peek(2).kind == Tok::Star && peek(3).kind == Tok::Ident && peek(4).kind == Tok::LBracket)
        fail(t.loc, "a C array parameter is written as a pointer", "write `char** " + peek(3).text + "`: C passes an array of text as a pointer to its first `char*`", spec::CFunctions);
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
        if (n == "int8_t" || n == "uint8_t")
            notYet(base.loc, "8-bit and 16-bit integer types such as '" + n + "'", "write `char`, one byte from 0 to 255, or `char*` for a buffer of bytes");
        if (n == "short" || n == "int16_t" || n == "uint16_t")
            notYet(base.loc, "8-bit and 16-bit integer types such as '" + n + "'", "write a small wrapper in C that takes an `int`, and declare that. A pointer to them can be a `void*`");
        if (n == "unsigned" && peek(isConst ? 2 : 1).kind == Tok::KwChar)
            fail(t.loc, "'unsigned char' is a C type; write `char` here", "write `char`, one byte from 0 to 255, or `char*` for a buffer of bytes", spec::CFunctions);
        if (n == "unsigned" && peek(isConst ? 2 : 1).kind == Tok::Ident && peek(isConst ? 2 : 1).text == "short")
            notYet(base.loc, "8-bit and 16-bit integer types such as 'unsigned short'", "write a small wrapper in C that takes an `int`, and declare that. A pointer to them can be a `void*`");
        if (!use.empty()) {
            if (n == "unsigned" && peek(isConst ? 2 : 1).kind == Tok::KwLong)
                use = "long", why = "it passes the same 64 bits; C+ has no unsigned types yet";
            fail(t.loc, "'" + n + "' is a C type; write `" + use + "` here", "write `" + use + "`: " + why, spec::CFunctions);
        }
    }
    if (base.kind == Tok::KwStruct) {
        const Token& name = peek(isConst ? 2 : 1);
        if (name.kind == Tok::Ident && peek(isConst ? 3 : 2).kind == Tok::Star)
            fail(base.loc, "C+ writes a C type without the word 'struct'",
                 "declare the type once inside extern \"C\", `struct " + name.text + ";`, and then write `" +
                     name.text + "*`",
                 spec::CTypes);
        if (name.kind != Tok::Ident)
            fail(base.loc, "C+ has no structs without a name", "give it a name, at the top level: `struct Point { int x; };`", spec::CStructs);
        fail(base.loc, "C+ writes a C type without the word 'struct'",
             "declare the struct once inside extern \"C\", and then write just `" + name.text + "`", spec::CStructs);
    }
    bool varConst = false;  // `const int n` in C means nothing to the caller
    const Type* ty = parseType(loc, &varConst);
    if (ty->is(Type::Kind::Auto)) fail(loc, "a C function needs written types, not 'auto'", "write the C type in the terms of C+: `int`, `long`, `string`, or a pointer such as `FILE*`", spec::CFunctions);
    if (check(Tok::Amp)) fail(peek().loc, "C has no references", "pass a pointer instead, e.g. `int* out`", spec::CFunctions);
    return ty;
}

// `std::name`. C+ has no namespaces, and its built-ins need no prefix.
static const char* cIntegerEquivalent(const std::string& n);

void Parser::failStd(Loc loc, const Token& name) {
    std::string n = name.text.empty() ? tokSpelling(name.kind) : name.text;
    if (n == "string" || n == "vector" || n == "array" || n == "to_string" || n == "format" || n == "println" || n == "print")
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
    if (n == "optional" || n == "nullopt")
        fail(loc, "C+ has no std:: namespace", n == "nullopt" ? "write `null`" : "write `T?`, such as `int?`, which is an int or `null`. A value converts to it by itself, so there is nothing to build", spec::Optionals);
    if (n == "function")
        fail(loc, "C+ has no std:: namespace", "write a function, and call it by name: C+ has no function values yet", spec::Functions);
    if (n == "stringstream" || n == "ostringstream" || n == "istringstream")
        fail(loc, "C+ has no std:: namespace", "build the text with `format(\"{}: {}\", a, b)`", spec::Printing);
    if (n == "hex" || n == "oct")
        fail(loc, "C+ has no std:: namespace", "this compiler does not support other bases in `println` yet (`{:x}` is planned). Print the number with `{}`", spec::Printing);
    if (n == "setprecision" || n == "fixed")
        fail(loc, "C+ has no std:: namespace", "choose the digits in the format: `println(\"{:.2f}\", x)`", spec::Printing);
    if (n == "make_pair")
        fail(loc, "C+ has no std:: namespace", "write a struct with a name for each part, and build it with `{a, b}`", spec::Structs);
    if (n == "uint8_t" || n == "int8_t" || n == "byte")
        fail(loc, "C+ has no std:: namespace", "write `char`, one byte from 0 to 255", spec::Char);
    if (n == "int16_t" || n == "uint16_t")
        fail(loc, "C+ has no std:: namespace", "write `int`, as this compiler has no 16-bit integers yet", spec::IntegerTypes);
    if (!cDeclarationOf(n).empty())
        fail(loc, "C+ has no std:: namespace", "a C function must be declared: `extern \"C\" " + cDeclarationOf(n) + ";`", spec::CFunctions);
    if (n == "exception" || n == "runtime_error" || n == "logic_error" || n == "invalid_argument" || n == "out_of_range")
        fail(loc, "C+ has no std:: namespace", "write `Error(\"why\")`, or derive a class from `Error`. In a `catch`, write `catch (Error e)`", spec::Errors);
    if (const char* use = cIntegerEquivalent(n))
        fail(loc, "C+ has no std:: namespace", "write `" + std::string(use) + "`, which has the same size", spec::IntegerTypes);
    if (n == "pair" || n == "tuple")
        fail(loc, "C+ has no std:: namespace", "write a struct with a name for each part", spec::Structs);
    if (n == "set" || n == "unordered_set" || n == "deque" || n == "list" || n == "stack" || n == "queue" || n == "multimap" || n == "multiset" || n == "priority_queue" || n == "forward_list")
        fail(loc, "C+ has no std:: namespace", "use a `vector<T>` or a `hashmap<K, V>` for now: `cplus explain hashmap` says what they do", spec::Vectors);
    if (n == "condition_variable")
        fail(loc, "C+ has no std:: namespace", "this compiler does not support condition variables yet. To wait for a thread, write `t.wait()`", spec::Concurrency);
    if (n == "sort" || n == "stable_sort" || n == "reverse" || n == "accumulate" || n == "min_element" || n == "max_element")
        fail(loc, "C+ has no std:: namespace", "write the loop: this compiler has no sorting yet, nor the other algorithms of C++", spec::Status);
    if (n == "swap")
        fail(loc, "C+ has no std:: namespace", "write it with a third variable: `auto t = a; a = b; b = t;`", spec::Variables);
    if (n == "getline" || n == "cin")
        fail(loc, "C+ has no std:: namespace", "declare C's: `extern \"C\" int getchar();`, as there are no input functions yet", spec::CFunctions);
    if (n == "weak_ptr")
        fail(loc, "C+ has no std:: namespace", "write `weak<Node>`, which works like std::weak_ptr", spec::Weak);
    if (n == "unique_ptr")
        fail(loc, "C+ has no std:: namespace", "write `unique<Node>`, which works like std::unique_ptr, but is never empty", spec::Unique);
    if (n == "make_unique")
        fail(loc, "C+ has no std:: namespace", "write `unique<Node>(...)`, which builds a new Node", spec::Unique);
    if (n == "move")
        fail(loc, "C+ has no std:: namespace", "built-ins need no prefix: write `move(x)`", spec::Unique);
    if (n == "unordered_map" || n == "map")
        fail(loc, "C+ has no std:: namespace", "write `hashmap<K, V>`, used like std::unordered_map", spec::HashMaps);
    if (n == "thread" || n == "jthread" || n == "async" || n == "future")
        fail(loc, "C+ has no std:: namespace", "write `auto t = spawn f(x);`, then `t.wait()`, which gives what `f` returned", spec::Concurrency);
    if (n == "mutex" || n == "lock_guard" || n == "scoped_lock" || n == "unique_lock")
        fail(loc, "C+ has no std:: namespace", "write `Mutex m = Mutex();`, then `LockGuard g = LockGuard(m);` for the block that needs it", spec::Concurrency);
    if (n == "atomic")
        fail(loc, "C+ has no std:: namespace", "this compiler does not support atomic values yet. Use a field of a class, and a `Mutex`", spec::Concurrency);
    if (n == "shared_ptr" || n == "make_shared")
        fail(loc, "C+ has no std:: namespace",
             "a class variable is already a counted reference: `class Node {...};`, then `Node n = Node();`", spec::Classes);
    if (n == "cerr" || n == "clog")
        fail(loc, "C+ has no std:: namespace",
             "`println` prints to standard output. For standard error, declare C's `write`: `extern \"C\" long write(int fd, string s, long n);`, then `write(2, s, s.size());`", spec::Printing);
    if (n == "cout" || n == "endl" || n == "printf")
        fail(loc, "C+ has no std:: namespace",
             "built-ins need no prefix: `std::cout << x << std::endl;` becomes `println(\"{}\", x);`", spec::Printing);
    fail(loc, "C+ has no std:: namespace", "leave out `std::`, and see what C+ has instead in the table of `cplus explain cheatsheet`", spec::Modules);
}

bool Parser::isTypeStart(size_t ahead) const {
    switch (peek(ahead).kind) {
        case Tok::Ident:  // a struct name followed by a variable name: `Point p`, `User? u`, or `vector<...>`
            if (peek(ahead + 1).kind == Tok::Less) {
                // `array` is also a common variable name: `array < n` is a comparison.
                if (peek(ahead).text == "array")
                    return isTypeAt(ahead + 2) || peek(ahead + 2).kind == Tok::KwAuto ||
                           peek(ahead + 2).kind == Tok::Greater ||
                           (peek(ahead + 2).kind == Tok::Ident && peek(ahead + 3).kind == Tok::Comma);
                // So is `unique`: `unique < n` is a comparison.
                // With a name that is no type, only a declaration has a name after the `>`, and it can then say so.
                if (peek(ahead).text == "unique" || peek(ahead).text == "local")
                    return isTypeAt(ahead + 2) || (peek(ahead + 2).kind == Tok::Ident && peek(ahead + 3).kind == Tok::Greater && (peek(ahead + 4).kind == Tok::Ident || peek(ahead + 4).kind == Tok::Question));
                if (peek(ahead).text == "unordered_map" || peek(ahead).text == "map" || isOtherContainer(peek(ahead).text)) return isTypeAt(ahead + 2);
                return peek(ahead).text == "vector" || classTemplate(peek(ahead).text);
            }
            // `Box b`: reports that its arguments are missing.
            if (classTemplate(peek(ahead).text) && !boundType(peek(ahead)) && peek(ahead + 1).kind == Tok::Ident) return true;
            if (!isTypeName(peek(ahead))) return false;
            if (peek(ahead + 1).kind == Tok::Question || peek(ahead + 1).kind == Tok::QuestionQuestion) ++ahead;
            while (peek(ahead + 1).kind == Tok::Star || (peek(ahead + 1).kind == Tok::KwConst &&
                                                          peek(ahead).kind == Tok::Star))
                ++ahead;  // `Point* p`, `Node** head`, `Point* const p`
            return peek(ahead + 1).kind == Tok::Ident;
        case Tok::KwString: return peek(ahead + 1).kind != Tok::ColonColon;  // not `string::npos`
        case Tok::KwInt: case Tok::KwLong: case Tok::KwBool:
        case Tok::KwVoid: case Tok::KwAuto: case Tok::KwConst: case Tok::KwWeak:
        case Tok::KwDouble: case Tok::KwFloat: case Tok::KwChar:
            return true;
        default: return false;
    }
}

// `int(x) > 3`, `string(a).size()`, `vector<int>{1} == v`: a type followed by '(' or '{'
// makes a value, so what starts this way is an expression. A declaration has a name there.
bool Parser::isValueOfType() const {
    size_t after = 1;
    switch (peek().kind) {
        case Tok::KwInt: case Tok::KwLong: case Tok::KwBool: case Tok::KwDouble: case Tok::KwFloat:
        case Tok::KwChar: case Tok::KwString:
            break;
        case Tok::Ident: case Tok::KwWeak:
            if (peek(1).kind != Tok::Less) return false;
            after = afterAngles(1);
            if (!after) return false;
            break;
        default: return false;
    }
    return peek(after).kind == Tok::LParen || peek(after).kind == Tok::LBrace || peek(after).kind == Tok::ColonColon;
}

bool Parser::isTypeAt(size_t ahead) const {
    const Token& t = peek(ahead);
    switch (t.kind) {
        case Tok::KwInt: case Tok::KwLong: case Tok::KwBool: case Tok::KwString: case Tok::KwVoid:
        case Tok::KwDouble: case Tok::KwFloat: case Tok::KwChar: case Tok::KwConst: case Tok::KwWeak:
            return true;
        case Tok::Ident:
            if (isTypeName(t)) return true;
            if (peek(ahead + 1).kind != Tok::Less) return false;
            return t.text == "vector" || ((t.text == "array" || t.text == "unique" || t.text == "local") && isTypeAt(ahead + 2)) || classTemplate(t.text);
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
    if (t.text == "uint8_t" || t.text == "int8_t")
        fail(t.loc, "unknown type '" + t.text + "'", "write `char* " + peek(i).text + "`, as a char is one byte", spec::IntegerTypes);
    if (t.text == "short" || t.text == "int16_t" || t.text == "uint16_t")
        fail(t.loc, "unknown type '" + t.text + "'", "write `void* " + peek(i).text + "`, as this compiler has no 16-bit integers yet", spec::IntegerTypes);
    if (t.text == "size_t" || t.text == "int32_t" || t.text == "int64_t" || t.text == "uint32_t" ||
        t.text == "uint64_t" || t.text == "unsigned")
        fail(t.loc, "unknown type '" + t.text + "'",
             "write `" + std::string(t.text == "unsigned" || t.text == "int32_t" || t.text == "uint32_t" ? "int" : "long") + "* " + peek(i).text + "`, which has the same size", spec::IntegerTypes);
    fail(t.loc, "unknown type '" + t.text + "'",
         "check the spelling, or define it. If '" + t.text + "' is a C type used through pointers, declare it first, at the top level: `extern \"C\" "
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

// `struct tm t;` or `struct tm* next;`, as C writes them. `struct Name {` is left to the caller.
void Parser::rejectStructKeyword() {
    const Token& name = peek(1);
    if (name.kind == Tok::Ident && peek(2).kind != Tok::LBrace)
        fail(peek().loc, "C+ writes the type of a struct or class without the word '" + peek().text + "'",
             "write just `" + name.text + "`, as in C++", spec::Structs);
}

// The C+ type with the size of a C integer type, or null.
static const char* cIntegerEquivalent(const std::string& n) {
    if (n == "size_t" || n == "ssize_t" || n == "ptrdiff_t" || n == "intptr_t" || n == "uintptr_t" || n == "int64_t" ||
        n == "uint64_t" || n == "off_t" || n == "time_t")
        return "long";
    if (n == "int32_t" || n == "uint32_t" || n == "pid_t" || n == "uid_t" || n == "gid_t") return "int";
    return nullptr;
}

// Whether a template argument is a value: a number, or a constant.
bool Parser::isValueArgument(const Token& k) {
    if (k.kind == Tok::IntLit) return true;
    return k.kind == Tok::Ident && !isTypeName(k) && !classTemplate(k.text) && constHooks().constant(k.text);
}

// The tokens from `at` as they would be written, up to the `>` of the `<` before them. `end` is left at that `>`.
std::string Parser::writtenBetweenAngles(size_t at, size_t& end) const {
    std::string out;
    int depth = 1;
    for (end = at; end < toks_.size() && end < at + 64; ++end) {
        const Token& k = toks_[end];
        if (k.kind == Tok::Less) ++depth;
        else if (k.kind == Tok::Greater && --depth == 0) break;
        else if (k.kind == Tok::Shr && (depth -= 2) <= 0) {
            if (depth == 0) out += ">";  // the first of the two closes what is inside
            break;
        }
        const std::string text = k.text.empty() ? tokSpelling(k.kind) : k.text;
        const bool isWord = k.kind == Tok::Ident || k.kind == Tok::KwConst || k.kind == Tok::KwInt || k.kind == Tok::KwLong || k.kind == Tok::KwString || k.kind == Tok::KwBool || k.kind == Tok::KwChar || k.kind == Tok::KwDouble || k.kind == Tok::KwFloat;
        if (isWord && !out.empty() && (std::isalnum((unsigned char)out.back()) || out.back() == '_')) out += " ";
        out += text;
        if (k.kind == Tok::Comma) out += " ";
    }
    return out;
}

// `Foo<int>`, at the position of its name, where Foo is nothing that has been defined.
void Parser::failUnknownGeneric(size_t at) {
    const Token& name = toks_[at];
    const std::string& n = name.text;
    std::string params;
    int count = 0, depth = 0;
    size_t i = at + 1;
    for (; i < toks_.size() && i < at + 64; ++i) {
        if (toks_[i].kind == Tok::Less && ++depth == 1) {
            params = isValueArgument(toks_[i + 1]) ? "int N" : "typename T";
            count = 1;
        } else if (toks_[i].kind == Tok::Less) {
            ++depth;
        } else if (toks_[i].kind == Tok::Greater && --depth == 0) {
            break;
        } else if (toks_[i].kind == Tok::Shr && (depth -= 2) <= 0) {
            break;
        } else if (toks_[i].kind == Tok::Comma && depth == 1) {
            ++count;
            params += ", " + std::string(isValueArgument(toks_[i + 1]) ? "int N" : "typename T") + std::to_string(count);
        }
    }
    const bool built = i + 1 < toks_.size() && toks_[i + 1].kind == Tok::LParen;
    size_t closing = 0;
    const std::string inside = writtenBetweenAngles(at + 2, closing);
    const bool isPointer = !inside.empty() && inside.back() == '*';
    fail(name.loc, "unknown type '" + n + "'",
         n == "optional" && isPointer ? "write `" + inside + "`, which can already be null"
         : n == "optional" && built ? "a value converts by itself: `return 3;`, or `null`. The type is written `" + inside + "?`, as in `string? s = null;`"
         : n == "optional" ? "write `" + inside + "?`, which holds a value of that type, or `null`"
         : n == "function" ? "write a function, and call it by name: C+ has no function values yet"
                           : "check the spelling, or define it first: `template<" + params + "> class " + n + " { ... };`",
         spec::Generics);
}

void Parser::checkUnknownType() {
    if (check(Tok::Ident) && peek(1).kind == Tok::ColonColon) {
        if (peek().text == "std") failStd(peek().loc, peek(2));
        if (isBuiltinModule(peek().text) && peek(2).kind == Tok::Ident)
            fail(peek().loc, "'" + peek().text + "::" + peek(2).text + "' is in the module '" + peek().text + "', which this program does not import",
                 "write `import " + peek().text + ";` at the top of the file", spec::Modules);
    }
    checkUnknownPointerType();
    if (check(Tok::Ident) && peek(1).kind == Tok::Less) {
        if (peek().text == "weak_ptr")
            fail(peek().loc, "C+ has no weak_ptr", "write `weak<Node>`, which works the same way", spec::Weak);
        if (peek().text == "unique_ptr")
            fail(peek().loc, "C+ has no unique_ptr", "write `unique<Node>`, which works the same way, but is never empty", spec::Unique);
        if (peek().text == "shared_ptr")
            fail(peek().loc, "C+ has no shared_ptr",
                 "a class variable is already a counted reference: `class Node {...};`, then `Node n = Node();`", spec::Classes);
    }
    // `Foo<int> x;`, which would otherwise be read as a comparison. A variable of that name means it is one.
    if (check(Tok::Ident) && peek(1).kind == Tok::Less && !classTemplate(peek().text) && !functionTemplate(peek().text) && !isOtherContainer(peek().text)) {
        bool isVariable = false;
        for (auto& scope : localScopes_) isVariable = isVariable || scope.count(peek().text);
        size_t at = 2;
        int depth = 1;
        while (depth > 0 && at < 64) {
            Tok k = peek(at).kind;
            if (k == Tok::Less) ++depth;
            else if (k == Tok::Greater) --depth;
            else if (k == Tok::Shr) depth -= 2;
            else if (k != Tok::Ident && k != Tok::Comma && k != Tok::Star && k != Tok::Question && k != Tok::IntLit && k != Tok::ColonColon && !isTypeAt(at)) break;
            ++at;
        }
        if (!isVariable && (peek().text == "function" || (depth <= 0 && (peek(at).kind == Tok::Ident || peek(at).kind == Tok::Star || peek(at).kind == Tok::Question))))
            failUnknownGeneric(pos_);
    }
    // C's spellings of integers, where a wrong guess would put a C struct's fields in the
    // wrong places.
    if (check(Tok::Ident)) {
        const std::string& n = peek().text;
        const Token& next = peek(1);
        bool sign = n == "unsigned" || n == "signed";
        if (n == "short" || n == "int16_t" || n == "uint16_t" || (sign && next.kind == Tok::Ident && next.text == "short"))
            notYet(peek().loc, std::string("16-bit integer types such as '") + (sign ? "unsigned " : "") + (sign && next.kind == Tok::Ident ? next.text : n) + "'",
                   inExternStruct_ ? "`int` would change its layout. Declare the struct by name, `extern \"C\" struct name;`, and use it through pointers"
                                   : "write `int`. A C struct with such a field can still be used through pointers, by declaring it by name: "
                                     "`extern \"C\" struct name;`");
        if ((n == "int8_t" || n == "uint8_t") && next.kind == Tok::Ident)
            notYet(peek().loc, "8-bit integer types such as '" + n + "'", "write `int` for a number, or `char` for a byte, as in `char c = char(n);`");
        if (sign && (next.kind == Tok::KwChar || next.kind == Tok::KwInt || next.kind == Tok::KwLong)) {
            std::string use = tokSpelling(next.kind);
            fail(peek().loc, "C+ has no '" + n + " " + use + "' type",
                 (next.kind == Tok::KwChar ? std::string("write `int` for a number, or `char` for a byte, as in `char c = char(n);`") : "write `" + use + "`, which has the same size") + "; C+ has no unsigned types yet", spec::IntegerTypes);
        }
    }
    bool optional = check(Tok::Ident) && peek(1).kind == Tok::Question && peek(2).kind == Tok::Ident &&
                    isOptionalDeclRest(3);
    if (!(check(Tok::Ident) && (peek(1).kind == Tok::Ident || optional))) return;
    const Token& t = peek();
    if (t.text == "condition_variable" && !isTypeName(t))
        notYet(t.loc, "condition variables", "to wait for a thread, write `t.wait()`. To wait for something else, check for it under a `Mutex`, in a loop");
    std::string help;
    if (t.text == "vector")
        fail(t.loc, "vector needs an element type", "write `vector<int> " + peek(1).text + ";`", spec::Vectors);
    if (const char* use = cIntegerEquivalent(t.text))
        help = "write `" + std::string(use) + "`, which has the same size as '" + t.text + "'";
    else if (t.text == "unsigned")
        notYet(t.loc, "unsigned integer types", "use int (32-bit) or long (64-bit); both are signed and the same size on every platform");
    else
        help = "write one of the types: int, long, double, float, bool, char, string, void, vector<T>, array<T, N>, hashmap<K, V>, T?, T*, or the "
               "structs and classes of the program";
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
    if (t->isWeak() && peek().loc.line && (check(Tok::Question) || check(Tok::QuestionQuestion)))
        fail(peek().loc, "a weak reference cannot be optional",
             "it is already empty when nothing is there: write `" + std::string(t->name) + "`", spec::Weak);
    if (check(Tok::QuestionQuestion))
        fail(peek().loc, "an optional cannot hold another optional",
             "write one '?': `" + std::string(t->name) + "?` already means " + t->name + " or null", spec::Optionals);
    if (check(Tok::Question) && peek().loc.line == 0 && (t->isOptional() || t->isPointer() || t->isWeak())) {
        advance();  // it can already be empty: see Builtins.h
    } else if (check(Tok::Question)) {
        Loc q = advance().loc;
        if (t->is(Type::Kind::Void) || t->is(Type::Kind::Auto))
            fail(loc, std::string("'") + t->name + "' cannot be optional",
                 t->is(Type::Kind::Auto) ? "write the type, e.g. `User? u = find(name);`, or `auto u = find(name);` "
                                           "when find returns an optional"
                                         : "a function that may return nothing returns an optional of a real type, "
                                           "e.g. `User?`",
                 spec::Optionals);
        if (check(Tok::Question) || t->isOptional())
            fail(t->isOptional() ? q : peek().loc, "an optional cannot hold another optional",
                 "write one '?': `" + std::string(t->isOptional() ? t->elem->name : t->name) + "?` already means " +
                     (t->isOptional() ? t->elem->name : t->name) + " or null",
                 spec::Optionals);
        if (t->isPointer())
            fail(q, "a pointer cannot be optional",
                 "a pointer can already be null: write `" + std::string(t->name) + "` and compare it with null", spec::Pointers);
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
        int stars = 0;
        while (match(Tok::Star)) {
            if (++stars > kMaxNesting) failTooDeep();
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
                 "remove it here: const belongs to a variable, as in `const int n = 5;`, or to what a pointer points to, as in "
                 "`const char*`",
                 spec::Variables);
        *isConst = true;
    }
    return t;
}

const Type* Parser::parseBaseType(Loc& loc) {
    Nesting nesting(*this);
    const Token& t = advance();
    loc = t.loc;
    switch (t.kind) {
        case Tok::KwWeak: {
            if (!check(Tok::Less))
                fail(t.loc, "weak is written with the class it refers to: `weak<Node> parent;`",
                     "it is a type of its own, with no `?`. Read it with `parent.lock()`, which gives a `Node?`", spec::Weak);
            advance();
            Loc inner;
            const Type* target = parseType(inner);
            expectCloseAngle("to close weak<...>");
            if (!target->isClass() && !dryRun_) {
                std::string name = target->name;
                fail(inner, "a weak reference refers to a class object, but '" + name + "' is not a class",
                     target->isOptional() && target->elem->isClass()
                         ? "it is already empty when the object is gone: write `weak<" + std::string(target->elem->name) + ">`"
                     : target->isStruct() ? "a struct is a value, so nothing refers to it. Make it a class, or use a pointer"
                                          : "make it a class: only objects of a class are counted, so only they can be gone",
                     spec::Weak);
            }
            loc.len = prev().loc.col + prev().loc.len - t.loc.col;
            return Type::weakOf(target);
        }
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
            if (const Type* bound = boundType(t); bound && !check(Tok::Less)) return bound;
            if (TemplateDecl* tmpl = classTemplate(t.text)) {
                if (!check(Tok::Less))
                    fail(t.loc, "'" + t.text + "' is generic, so its template arguments must be written",
                         "write e.g. `" + exampleArguments(*tmpl) + "`" + constraintNote(*tmpl), spec::Generics);
                const Type* made = genericRecord(*tmpl, t.loc);
                loc.len = prev().loc.col + prev().loc.len - t.loc.col;
                return made;
            }
            if (isTypeName(t)) return &prog_->findStruct(t.text)->type;
            if (t.text == "unique" && check(Tok::Less)) {
                advance();
                Loc inner;
                const Type* target = parseType(inner);
                expectCloseAngle("to close unique<...>");
                if (!target->isClass() && !dryRun_) {
                    std::string name = target->name;
                    fail(inner, "a unique reference owns a class object, but '" + name + "' is not a class",
                         target->isOptional() && target->elem->isClass()
                             ? "for one that can be empty, write `unique<" + std::string(target->elem->name) + ">?`"
                             : "use the value itself: a struct or a number already has one owner, the variable that holds it",
                         spec::Unique);
                }
                loc.len = prev().loc.col + prev().loc.len - t.loc.col;
                return Type::uniqueOf(target);
            }
            if (t.text == "local" && check(Tok::Less)) {
                advance();
                Loc inner;
                const Type* target = parseType(inner);
                expectCloseAngle("to close local<...>");
                if (!target->isClass() && !dryRun_) {
                    std::string name = target->name;
                    fail(inner, "a local reference refers to a class object, but '" + name + "' is not a class",
                         target->isOptional() && target->elem->isClass()
                             ? "for one that can be empty, write `local<" + std::string(target->elem->name) + ">?`"
                             : "use the value itself: a struct or a number has no count to save work on",
                         spec::Local);
                }
                loc.len = prev().loc.col + prev().loc.len - t.loc.col;
                return Type::localOf(target);
            }
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
            if (t.text == "array" && check(Tok::Less)) {
                Loc start = t.loc;
                advance();
                if (check(Tok::Greater) || check(Tok::Shr))
                    fail(peek().loc, "array needs an element type and a size", "write e.g. `array<int, 5>`", spec::Arrays);
                Loc elemLoc;
                const Type* elem = parseType(elemLoc);
                std::string name = elem->name;
                if (elem->is(Type::Kind::Void) || elem->is(Type::Kind::Auto))
                    fail(elemLoc, "an array cannot hold '" + name + "' elements",
                         "write the element type, e.g. `array<int, 5>`", spec::Arrays);
                if (!check(Tok::Comma))
                    fail(peek().loc, "array needs a size after its element type",
                         "write e.g. `array<" + name + ", 5>`. For a list that can grow, use `vector<" + name + ">`",
                         spec::Arrays);
                advance();
                const Token& size = peek();
                Tok afterSize = peek(1).kind;
                if (size.kind == Tok::Ident && (afterSize == Tok::Greater || afterSize == Tok::Shr)) {
                    auto bound = bindings_.find(size.text);
                    if (bound != bindings_.end() && bound->second.valueType && bound->second.valueType->isInteger()) {  // `array<T, N>`
                        advance();
                        expectCloseAngle("to close array<...>");
                        loc = start;
                        loc.len = prev().loc.col + prev().loc.len - start.col;
                        const TemplateArg& value = bound->second.value;
                        if (!dryRun_ && value.value < 0)
                            fail(loc, "the size of an array cannot be negative, but '" + size.text + "' is " + std::to_string(value.value), "write a whole number, zero or more", spec::Arrays);
                        // While the definition is read, the length says which parameter gives it.
                        return Type::arrayOf(elem, value.valueParam >= 0 ? -1 - value.valueParam : value.value);
                    }
                }
                if (size.kind == Tok::Greater || size.kind == Tok::Shr || size.kind == Tok::Comma || isTypeAt(0) ||
                    size.kind == Tok::StringLit)
                    fail(size.loc, "the size of an array is a whole number, zero or more",
                         "write e.g. `array<" + name + ", 5>`, or a constant. For a size known only while the program runs, use `vector<" +
                             name + ">`",
                         spec::Arrays);
                ExprPtr sizeExpr = parseBinary(9);  // no `>` at its top, which closes the type; parentheses allow one
                if (!check(Tok::Greater) && !check(Tok::Shr))
                    fail(peek().loc, "the size of an array is a single number",
                         "write e.g. `array<" + name + ", 5>`. For rows and columns, use an array of arrays: `array<array<" +
                             name + ", 3>, 2>`",
                         spec::Arrays);
                expectCloseAngle("to close array<...>");
                loc = start;
                loc.len = prev().loc.col + prev().loc.len - start.col;
                if (dryRun_) return Type::arrayOf(elem, 0);  // what it depends on is not known yet
                ConstValue length = evaluate(*sizeExpr, "the size of an array must be known at compile time");
                if (length.kind != ConstValue::Kind::Int && length.kind != ConstValue::Kind::Long)
                    fail(sizeExpr->loc, "the size of an array is a whole number, zero or more, but this is a '" + std::string(length.typeName()) + "'",
                         "write e.g. `array<" + name + ", 5>`", spec::Arrays);
                if (length.i < 0)
                    fail(sizeExpr->loc, "the size of an array cannot be negative, but this is " + std::to_string(length.i),
                         "write a whole number, zero or more", spec::Arrays);
                return Type::arrayOf(elem, length.i);
            }
            if (t.text == "std" && check(Tok::ColonColon)) failStd(t.loc, peek(1));
            if (isBuiltinModule(t.text) && check(Tok::ColonColon) && peek(1).kind == Tok::Ident)
                fail(t.loc, "'" + t.text + "::" + peek(1).text + "' is in the module '" + t.text + "', which this program does not import",
                     "write `import " + t.text + ";` at the top of the file", spec::Modules);
            if (t.text == "unordered_map")
                fail(t.loc, "unknown type '" + t.text + "'",
                     "write `hashmap<K, V>`, used like std::unordered_map. It keeps its entries in the order they were added",
                     spec::HashMaps);
            if (t.text == "map") notYet(t.loc, "sorted maps", "write `hashmap<K, V>`, which keeps its entries in the order they were added");
            if (isOtherContainer(t.text)) {
                if (t.text == "atomic") notYet(t.loc, "atomic values", "use a field of a class, and a `Mutex`");
                if (t.text == "channel" || t.text == "Channel") notYet(t.loc, "channels", "hand values over with `spawn` and `t.wait()`, or use a vector in a class, and a `Mutex`");
                if (t.text == "set" || t.text == "unordered_set" || t.text == "multiset")
                    notYet(t.loc, "sets", t.text == "multiset" ? "use a `hashmap<T, int>`, with `m[x]++` for how many of each"
                                                               : "use a `hashmap<T, bool>`, whose keys are the set, with `m.contains(x)`");
                if (t.text == "multimap") notYet(t.loc, "'multimap'", "use a `hashmap<K, vector<V>>`, with `m[k].push_back(v)`");
                if (t.text == "priority_queue") notYet(t.loc, "'priority_queue'", "use a `vector<T>`, and a loop to find the largest");
                if (t.text == "queue" || t.text == "deque" || t.text == "list" || t.text == "forward_list")
                    notYet(t.loc, "'" + t.text + "'", "use a `vector<T>` with `push_back`, and an index for the next one to take: `v[head]; head++;`");
                notYet(t.loc, "'" + t.text + "'", t.text == "pair" || t.text == "tuple" ? "write a struct with a name for each part" : "use a `vector<T>`, with `push_back`, `pop_back` and `back`");
            }
            fail(t.loc, "unknown type '" + t.text + "'", "write one of the types: int, long, double, float, bool, char, string, void, vector<T>, array<T, N>, hashmap<K, V>, T?, T*, or the structs and classes of the program", spec::IntegerTypes);
        default: fail(t.loc, "expected a type, found " + describeToken(t), "write one of the types: int, long, double, float, bool, char, string, void, vector<T>, array<T, N>, hashmap<K, V>, T?, T*, or the structs and classes of the program", spec::IntegerTypes);
    }
}

// How far ahead the token after the `<...>` that starts `ahead` tokens from here is, or 0
// if the brackets do not close.
size_t Parser::afterAngles(size_t ahead) const {
    int depth = 0;
    int parens = 0;  // inside parentheses, as in `Box<(a > b)>`, angle brackets count for nothing
    for (size_t i = ahead;; ++i) {
        switch (peek(i).kind) {
            case Tok::LParen: ++parens; break;
            case Tok::Less: if (!parens) ++depth; break;
            case Tok::Greater: if (!parens) --depth; break;
            case Tok::Shr: if (!parens) depth -= 2; break;
            case Tok::RParen:
                if (!parens) return 0;
                --parens;
                break;
            case Tok::Semi: case Tok::LBrace: case Tok::RBrace: case Tok::Eof: return 0;
            default: break;
        }
        if (depth == 0) return i + 1;
        if (depth < 0) return 0;
    }
}

// `int a[5]`, at the '['. C+ writes `array<int, 5> a`, which stays an array when it is
// passed around, where a C array turns into a pointer.
void Parser::failCStyleArray(const Type* elem, const std::string& name, ArrayPlace place) {
    Loc loc = peek().loc;
    // What is in the brackets, and what comes after them.
    std::string size = peek(2).kind == Tok::RBracket && (peek(1).kind == Tok::IntLit || peek(1).kind == Tok::Ident) ? peek(1).text : "";
    size_t after = 1;
    while (after < 32 && peek(after).kind != Tok::RBracket && peek(after).kind != Tok::Eof) ++after;
    ++after;
    std::string type = elem->name;
    // Every dimension, for an array of arrays of them.
    std::vector<std::string> sizes;
    bool anyName = false, anyMissing = false;
    size_t past = 0;
    while (peek(past).kind == Tok::LBracket && sizes.size() < 8) {
        size_t close = past + 1;
        std::string written;
        while (close < past + 32 && peek(close).kind != Tok::RBracket && peek(close).kind != Tok::Eof) {
            written += (written.empty() ? "" : " ") + (peek(close).text.empty() ? std::string(tokSpelling(peek(close).kind)) : peek(close).text);
            anyName = anyName || peek(close).kind == Tok::Ident;
            ++close;
        }
        anyMissing = anyMissing || written.empty();
        sizes.push_back(written.empty() ? "N" : written);
        past = close + 1;
    }
    std::string nested = type, nestedVectors = type, builtVectors;
    for (size_t i = sizes.size(); i-- > 0;) {
        nested = "array<" + nested + ", " + sizes[i] + ">";
        builtVectors = "vector<" + nestedVectors + ">(" + sizes[i] + (builtVectors.empty() ? "" : ", " + builtVectors) + ")";
        nestedVectors = "vector<" + nestedVectors + ">";
    }
    const std::string note = anyMissing ? ", with N the number of rows" : "";
    std::string help;
    if (sizes.size() > 1 && elem->is(Type::Kind::Char) && peek(past).kind == Tok::Assign && peek(past + 1).kind == Tok::LBrace)
        help = "write `array<string, " + sizes[0] + "> " + name + " = {...};`" + note;
    else if (place == ArrayPlace::Parameter && sizes.size() > 1)
        help = "write `" + nested + " " + name + "`, or `" + nestedVectors + " " + name + "`, which are copies. To change the caller's, write `" + nested + "* " + name + "`, called with `&x`, and use `(*" + name + ")[0][0]`" + note;
    else if (place != ArrayPlace::Field && sizes.size() > 1 && anyName)
        help = "write `" + nested + " " + name + "` if those are constants, or `auto " + name + " = " + builtVectors + ";` if they are only known while the program runs" + note;
    else if (sizes.size() > 1)
        help = "write an array of arrays: `" + nested + " " + name + "`" + note;
    else if (place == ArrayPlace::Parameter)
        help = "write `vector<" + type + "> " + name + "`, or, to change the caller's array, `" + type + "* " + name + "`, called with `x.data()`";
    else if (place == ArrayPlace::Field && (size.empty() || peek(1).kind == Tok::Ident))
        help = "write `vector<" + type + "> " + name + ";`, or `array<" + type + ", N> " + name + ";`, with N a constant";
    else if (elem->is(Type::Kind::Char) && peek(after).kind == Tok::Assign && peek(after + 1).kind == Tok::StringLit)
        help = "write `string " + name + " = ...;`, as text is a string";
    else if (size.empty() && peek(after).kind == Tok::Assign)
        help = "write `vector<" + type + "> " + name + " = {...};`, or `array<" + type + ", N> " + name + " = {...};`, with N the number of elements";
    else if (size.empty())
        help = "a fixed-size array is written with its size in the type: `array<" + type + ", 5> " + name + "`. For a size known only while the program runs, write `auto " + name + " = vector<" + type + ">(n);`";
    else if (peek(1).kind == Tok::Ident)
        help = "write `array<" + type + ", " + size + "> " + name + "` if " + size + " is a constant, or `auto " + name + " = vector<" + type + ">(" + size + ");` if it is only known while the program runs";
    else
        help = "write `array<" + type + ", " + size + "> " + name + "`";
    fail(loc, "C+ has no C-style arrays", help, spec::Arrays);
}

// Expects the '>' that closes `vector<...>` or `array<..., N>`. In `vector<vector<int>>` the lexer sees
// '>>', so the first '>' of it is taken and the token left as the second one.
void Parser::expectCloseAngle(const std::string& context) {
    if (check(Tok::Shr)) {  // closes two brackets, and the tokens stay as they are, since a generic is read again
        if (splitShr_ == pos_) {
            splitShr_ = SIZE_MAX;
            advance();
        } else {
            splitShr_ = pos_;
        }
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
                if (check(Tok::Dot))
                    fail(peek().loc, "a C+ function cannot take any number of arguments", "take a `vector<T>`, or write the parameters one by one", spec::Functions);
                fail(peek().loc, "expected a parameter type, found " + describeToken(peek()),
                     "write a type and a name, like `int count`", spec::Functions);
            }
            Loc typeLoc;
            bool isConst = false;
            const Type* ty = parseType(typeLoc, &isConst);
            if (ty->is(Type::Kind::Auto)) notYet(typeLoc, "'auto' parameters", "write the parameter type");
            if (check(Tok::Amp) || check(Tok::AmpAmp))
                notYet(peek().loc, "reference parameters",
                       "pass the value itself: structs are copied, class objects are shared, and a vector is only "
                       "copied if one of the copies changes. To let the function change the caller's variable, "
                       "pass a pointer: `int* count`, called as `f(&n)`");
            const Token& pname = expectNewName("for the parameter name");
            rejectTemplateParamName(pname);
            if (check(Tok::LBracket) && fn.name == "main" && fn.role == FuncDecl::Role::Free) {
                // Reported, and read as what it means, so that this is still the program's main.
                diags_.error(peek().loc, "C+ has no C-style arrays", "write `int main(int argc, char** argv)`", spec::MainFunction);
                while (match(Tok::LBracket)) {
                    while (!check(Tok::RBracket) && !check(Tok::Eof)) advance();
                    match(Tok::RBracket);
                    ty = Type::pointerTo(ty);
                }
            }
            if (check(Tok::LBracket)) failCStyleArray(ty, pname.text, ArrayPlace::Parameter);
            auto param = std::make_unique<VarDecl>(pname.loc, pname.text, ty, typeLoc);
            param->isParam = true;
            param->isConst = isConst;
            if (check(Tok::Assign)) notYet(peek().loc, "default argument values", "remove `= ...`, and write the value at each call");
            fn.params.push_back(std::move(param));
        } while (match(Tok::Comma));
    }
    expect(Tok::RParen, "to close the parameter list");
}

std::unique_ptr<FuncDecl> Parser::parseFunction(const Type* retType, Loc retLoc) {
    auto fn = std::make_unique<FuncDecl>();
    fn->retType = retType;
    fn->retTypeLoc = retLoc;
    const Token& name = expectNewName("for the function name after the return type");
    fn->name = name.text;
    fn->nameLoc = name.loc;
    parseParams(*fn);
    parseThrowsMark(*fn);
    if (check(Tok::Semi))
        fail(peek().loc, "C+ has no forward declarations",
             "remove this line; functions can be called before or after they are defined", spec::Functions);
    if (!check(Tok::LBrace)) expect(Tok::LBrace, "to start the function body");
    fn->body = parseBlock(fn.get());
    return fn;
}

// After a function's parameters, and after `const` on a method: `int f() const throws`.
void Parser::parseThrowsMark(FuncDecl& fn) {
    if (check(Tok::Ident) && peek().text == "noexcept")
        fail(peek().loc, "C+ has no noexcept", "remove it: a function that is not marked `throws` cannot fail", spec::Errors);
    if (check(Tok::KwThrow))
        fail(peek().loc, "a function that can fail is marked `throws`",
             peek(1).kind == Tok::LParen && peek(2).kind == Tok::RParen ? "remove it: a function that is not marked `throws` cannot fail"
                                                                        : "write `throws` after its parameters, with no types",
             spec::Errors);
    fn.isThrows = match(Tok::KwThrows);
    if (fn.isThrows && (check(Tok::LParen) || check(Tok::Ident)))
        fail(peek().loc, "`throws` takes no types", "write `throws` alone: a function does not say which errors it throws", spec::Errors);
    if (fn.isThrows && check(Tok::KwConst))
        fail(peek().loc, "`const` comes before `throws`", "write `const` before `throws`, on a method: `int f() const throws { ... }`. A function outside a type cannot be const", spec::Errors);
}

// ---- Structs and classes --------------------------------------------------

void Parser::parseRecord(StructDecl* into) {
    Tok keyword = advance().kind;
    bool isClass = keyword != Tok::KwStruct;
    bool isInterface = keyword == Tok::KwInterface;
    const char* kw = isInterface ? "interface" : isClass ? "class" : "struct";
    const Token& name = expectNewName(std::string("for the ") + kw + " name");
    if (check(Tok::Semi))
        fail(peek().loc, "C+ has no forward declarations",
             std::string("remove this line; ") + article(kw) + kw + " can be used before or after its definition. A C type used "
             "only through pointers is declared with `extern \"C\" struct " + name.text + ";`",
             spec::Structs);
    if (check(Tok::Ident) && peek().text == "final") notYet(peek().loc, "final classes", "leave out `final`");
    std::vector<StructDecl::BaseName> baseNames;
    if (match(Tok::Colon)) {
        if (!isClass)
            fail(prev().loc, "a struct cannot have a base class",
                 "a struct is a value, and copying one into a variable of its base type would lose part of it. Make "
                 "both of them classes, or hold the other struct in a field",
                 spec::Inheritance);
        do {
            bool isPublic = match(Tok::KwPublic);
            if (check(Tok::KwVirtual))
                fail(peek().loc, "C+ has no virtual inheritance",
                     "write `public " + std::string(peek(peek(1).kind == Tok::KwPublic || peek(1).kind == Tok::KwProtected || peek(1).kind == Tok::KwPrivate ? 2 : 1).text) + "`. A diamond holds the shared base class twice, and "
                     "`d.B::name` says which is meant",
                     spec::Inheritance);
            if (check(Tok::KwPrivate) || check(Tok::KwProtected))
                notYet(peek().loc, "private and protected base classes",
                       "derive with `: public Base`, or hold an object of the other class in a field");
            if (check(Tok::Ident) && peek().text == "std" && peek(1).kind == Tok::ColonColon) failStd(peek().loc, peek(2));
            const Token& base = expect(Tok::Ident, "for the name of the base class");
            if (!isPublic)
                fail(base.loc, std::string("write `public` before the base class: `") + kw + " " + name.text + " : public " +
                                   base.text + "`",
                     "write `public`: in C++ a class without it derives privately, which is not what it looks like",
                     spec::Inheritance);
            std::string baseName = base.text;
            if (classTemplate(base.text) || boundType(base)) {  // `public Source<T>`, or `public T`
                --pos_;
                Loc loc;
                baseName = parseBaseType(loc)->name;
            }
            baseNames.push_back({baseName, base.loc});
        } while (match(Tok::Comma));
    }
    StructDecl* sd = into ? into : prog_->findStruct(name.text);  // created by the scan in parseProgram
    std::unique_ptr<StructDecl> duplicate;
    if (!into && (sd->defined || sd->opaque || sd->isClass != isClass)) {
        diags_.error(name.loc, "the name '" + name.text + "' is already used by another struct or class",
                     "rename one of them", spec::Structs);
        if (sd->defined || sd->opaque) diags_.note(sd->nameLoc, "the first definition is here");
        duplicate = std::make_unique<StructDecl>(name.text, isClass);  // parse into a scratch copy to keep going
        sd = duplicate.get();
    }
    sd->nameLoc = name.loc;
    sd->isInterface = isInterface;
    sd->baseNames = std::move(baseNames);
    expect(Tok::LBrace, "to start the body of " + std::string(kw) + " '" + name.text + "'");
    Access access = isClass && !isInterface ? Access::Private : Access::Public;  // as in C++
    while (!check(Tok::RBrace) && !check(Tok::Eof)) {
        try {
            parseMember(*sd, access);
        } catch (ParseError&) {
            syncStatement();
            match(Tok::Semi);  // the end of what could not be read
        }
    }
    expect(Tok::RBrace, "to close " + std::string(kw) + " '" + name.text + "'");
    sd->defined = true;
    if (!match(Tok::Semi))  // report without skipping ahead, so the next declaration still parses
        diags_.error(afterPrev(), "expected ';' after the definition of " + std::string(kw) + " '" + name.text +
                                      "', found " + describeToken(peek()),
                     std::string(article(kw)) + kw + " definition ends with `};`, as in C++", spec::Structs);
}

void Parser::parseMemberInits(FuncDecl& ctor) {
    do {
        const Token& field = expect(Tok::Ident, "for the field name in the initializer list");
        std::string baseName;  // the whole name, also for `Source(x)` or `Source<int>(x)` when it is `Source<int>`
        for (auto& base : ctor.owner->baseNames)
            if (base.name == field.text || base.name.rfind(field.text + "<", 0) == 0) baseName = base.name;
        if (classTemplate(field.text) && check(Tok::Less)) {
            --pos_;
            Loc loc;
            baseName = parseBaseType(loc)->name;
        }
        if (!baseName.empty()) {  // `Animal(name, 3)`: what builds the base class
            FuncDecl::BaseInit base{baseName, field.loc, {}};
            Tok close = match(Tok::LBrace) ? Tok::RBrace : Tok::RParen;
            if (close == Tok::RParen) expect(Tok::LParen, "after '" + field.text + "' in the initializer list");
            if (!check(close)) {
                do base.args.push_back(parseExpr());
                while (match(Tok::Comma));
            }
            expect(close, "to close the values for the base class '" + field.text + "'");
            for (auto& earlier : ctor.baseInits)
                if (earlier.name == baseName)
                    diags_.error(field.loc, "the base class " + field.text + " is built more than once", "build it once, with all the values it takes", spec::Inheritance);
            ctor.baseInits.push_back(std::move(base));
            continue;
        }
        FuncDecl::MemberInit init{field.text, field.loc, nullptr, -1};
        if (match(Tok::LBrace)) {
            init.value = parseInitList(field.loc, nullptr);  // `pos{1, 2}` takes the field's type
        } else {
            expect(Tok::LParen, "after '" + field.text + "' in the initializer list");
            init.value = parseExpr();
            if (check(Tok::Comma))
                fail(peek().loc, "a field is initialized from one value",
                     "for a struct field, write `" + field.text + "{a, b}`. For a vector or a string, build the value: `" + field.text + "(vector<int>(3, 0))`, `" + field.text + "(string(3, 'a'))`",
                     spec::Classes);
            expect(Tok::RParen, "to close the initializer of '" + field.text + "'");
        }
        ctor.memberInits.push_back(std::move(init));
    } while (match(Tok::Comma));
}

void Parser::parseMember(StructDecl& sd, Access& access) {
    if (peek().loc.line == 0 && check(Tok::Ident) && peek().text == "__role") {  // only in what comes with the language
        advance();
        expect(Tok::LParen, "after __role");
        const std::string role = advance().text;
        expect(Tok::RParen, "after the role");
        expect(Tok::Semi, "after __role(...)");
        sd.role = role == "hashmap" ? StructDecl::Role::Hashmap : role == "json_value" ? StructDecl::Role::JsonValue : StructDecl::Role::Opaque;
        return;
    }
    rejectOtherLanguages();
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
    const std::string& ownName = sd.templateName.empty() ? sd.name : sd.templateName;  // of its constructor
    bool isVirtual = check(Tok::KwVirtual);
    if (isVirtual) {
        Loc loc = advance().loc;
        if (!sd.isClass)
            fail(loc, "only a class can have virtual methods",
                 "a struct is a value, so its type is always known. Make it a class to derive from it", spec::Inheritance);
        if (peek().kind == Tok::Ident && peek().text == ownName && peek(1).kind == Tok::LParen)
            fail(loc, "a constructor cannot be virtual", "remove `virtual`", spec::Inheritance);
    }
    const Token& t = peek();
    if (sd.isInterface && (t.kind == Tok::Tilde || (t.kind == Tok::Ident && t.text == ownName && peek(1).kind == Tok::LParen)))
        fail(t.loc, std::string("an interface cannot have a ") + (t.kind == Tok::Tilde ? "destructor" : "constructor"),
             t.kind == Tok::Tilde ? "remove it: a destructor always runs for what the object really is, which is a class"
                                  : "remove it: an interface only lists methods, and holds nothing to set up. Use a class for that",
             spec::Interfaces);
    switch (t.kind) {
        case Tok::KwPublic: case Tok::KwPrivate: case Tok::KwProtected:
            advance();
            expect(Tok::Colon, "after '" + std::string(tokSpelling(t.kind)) + "'");
            if (sd.isInterface && t.kind != Tok::KwPublic)
                fail(t.loc, "every method of an interface is public", "remove this line", spec::Interfaces);
            access = t.kind == Tok::KwPublic ? Access::Public : t.kind == Tok::KwPrivate ? Access::Private : Access::Protected;
            return;
        case Tok::KwStruct: case Tok::KwClass:
            rejectStructKeyword();
            notYet(t.loc, "nested type definitions", "define it at the top level instead");
        case Tok::KwStatic: notYet(t.loc, "static members", "use a constant or a function outside the type");
        case Tok::KwConstexpr:
            notYet(t.loc, "constexpr methods and fields", "write a `constexpr` function or constant outside the type");
        case Tok::KwOperator: notYet(t.loc, "operator overloading", operatorHelp());
        case Tok::KwTemplate:
            notYet(t.loc, "generic methods", "make the whole type generic, or write a generic function outside it");
        default: break;
    }

    // Destructor: ~Name() { ... }
    if (t.kind == Tok::Tilde) {
        Loc tildeLoc = advance().loc;
        const Token& name = expect(Tok::Ident, "after '~'");
        if (name.text != ownName)
            fail(name.loc, "a destructor is named after its type: '~" + ownName + "'", "write `~" + ownName + "()`", spec::Classes);
        if (!sd.isClass)
            notYet(tildeLoc, "struct destructors", "make it a class, which can have a destructor, or remove it: a struct is a plain value");
        auto dtor = std::make_unique<FuncDecl>();
        dtor->name = "~" + sd.name;
        dtor->nameLoc = tildeLoc;
        dtor->retType = Type::voidTy();
        dtor->role = FuncDecl::Role::Destructor;
        dtor->owner = &sd;
        dtor->access = Access::Public;
        expect(Tok::LParen, "after the destructor name");
        expect(Tok::RParen, "to close the destructor's parameters, as it takes none");
        if (check(Tok::Ident) && peek().text == "noexcept")
            fail(peek().loc, "C+ has no noexcept", "remove it: a destructor cannot fail", spec::Errors);
        if (check(Tok::KwThrows))
            fail(peek().loc, "a destructor cannot fail", "handle errors inside it, with `try { } catch (Error e) { }`", spec::Errors);
        // A destructor always runs for what the object really is, so `virtual` and `override`
        // change nothing. They are allowed, as C++ code is full of them.
        if (match(Tok::KwOverride) && sd.baseNames.empty())
            fail(prev().loc, "'" + sd.name + "' has no base class, so its destructor overrides nothing",
                 "remove `override`", spec::Inheritance);
        if (match(Tok::Assign)) {  // `~Animal() = default;`, the same as having none
            if (!check(Tok::KwDefault))
                fail(peek().loc, "a destructor can be set to `= default` only", "write `{ ... }`, or leave the destructor out",
                     spec::Classes);
            advance();
            expect(Tok::Semi, "after `= default`");
            return;
        }
        dtor->body = parseBlock();
        if (sd.destructor) {
            diags_.error(tildeLoc, "'" + sd.name + "' already has a destructor", "keep one, with all that it has to do", spec::Classes);
            return;
        }
        sd.destructor = dtor.get();
        sd.methods.push_back(std::move(dtor));
        return;
    }

    // Constructor: Name(params) : field(value), ... { ... }
    if (t.kind == Tok::Ident && t.text == ownName && peek(1).kind == Tok::LParen) {
        auto ctor = std::make_unique<FuncDecl>();
        ctor->name = sd.name;
        ctor->nameLoc = advance().loc;
        ctor->retType = Type::voidTy();
        ctor->role = FuncDecl::Role::Constructor;
        ctor->owner = &sd;
        ctor->access = access;
        parseParams(*ctor);
        if (check(Tok::KwThrows))
            notYet(peek().loc, "constructors that can fail",
                   "write a function that can fail and makes the object: `" + ownName + " make(int x) throws { ...; return " +
                       ownName + "(x); }`");
        if (match(Tok::Colon)) parseMemberInits(*ctor);
        if (check(Tok::Semi))
            fail(peek().loc, "write the constructor body here", "C+ has no separate definitions; add `{ ... }`",
                 spec::Classes);
        if (check(Tok::Assign))
            fail(peek().loc, "a constructor cannot be set to `= default` or `= delete`", "remove it. Without a constructor, a type is built from the defaults of its fields", spec::Classes);
        if (check(Tok::Ident) && peek().text == "noexcept")
            fail(peek().loc, "C+ has no noexcept", "remove it: a constructor cannot fail", spec::Errors);
        ctor->body = parseBlock(ctor.get());
        if (sd.constructor) {
            diags_.error(ctor->nameLoc, "'" + sd.name + "' already has a constructor",
                         "several constructors need overloading, which is planned. For now, keep one, and give fields default values for what it does not set",
                         spec::Classes);
            return;
        }
        sd.constructor = ctor.get();
        sd.methods.push_back(std::move(ctor));
        return;
    }

    if (peek(1).kind == Tok::KwOperator) notYet(peek(1).loc, "operator overloading", operatorHelp(1));
    if (peek(1).kind == Tok::Amp && peek(2).kind == Tok::KwOperator) notYet(peek(2).loc, "operator overloading", operatorHelp(2));
    if (!isTypeStart()) {
        checkUnknownType();
        fail(t.loc, "expected a field or method, found " + describeToken(t),
             t.kind == Tok::Semi ? "remove this ';': the `}` of a method needs none" : "write a field, like `int x = 0;`, or a method, like `int area() { ... }`", spec::Structs);
    }
    Loc typeLoc;
    bool isConst = false;
    const Type* ty = parseType(typeLoc, &isConst);
    if (isConst) notYet(t.loc, "const fields", "leave out `const`, keep it private, and give the type a method that returns it");
    if (ty->is(Type::Kind::Auto))
        fail(typeLoc, "fields and methods need a written type", "write the type, e.g. `int x = 0;`. `auto` is only for local variables", spec::Structs);
    if (check(Tok::KwOperator)) notYet(peek().loc, "operator overloading", operatorHelp());

    // Method: type name(params) [const] { ... }
    if (check(Tok::Ident) && peek(1).kind == Tok::LParen) {
        auto method = std::make_unique<FuncDecl>();
        method->retType = ty;
        method->retTypeLoc = typeLoc;
        const Token& name = expectNewName("for the method name");
        method->name = name.text;
        method->nameLoc = name.loc;
        method->role = FuncDecl::Role::Method;
        method->owner = &sd;
        method->access = access;
        method->isVirtual = isVirtual;
        parseParams(*method);
        method->isConst = match(Tok::KwConst);
        parseThrowsMark(*method);
        auto rejectFinal = [&] {
            if (check(Tok::Ident) && peek().text == "final") notYet(peek().loc, "final methods", "leave out `final`");
        };
        rejectFinal();
        method->isOverride = match(Tok::KwOverride);
        rejectFinal();
        if (sd.isInterface) {  // only lists it: `string speak() const;`
            method->isVirtual = true;
            method->isPure = true;
            if (match(Tok::Assign)) {  // as in C++
                if (peek().kind != Tok::IntLit || peek().intValue != 0)
                    fail(peek().loc, "a method of an interface cannot be set to a value", "remove what comes after `=`",
                         spec::Interfaces);
                advance();
            }
            if (check(Tok::LBrace))
                fail(peek().loc, "a method of an interface has no body", "replace `{ ... }` with `;`, and write the body in "
                     "the classes that derive from it. For a method with a body, use a class",
                     spec::Interfaces);
            expect(Tok::Semi, "after the method of an interface");
            sd.methods.push_back(std::move(method));
            return;
        }
        if (match(Tok::Assign)) {  // `= 0`: no body, and a class that derives must write one
            if (peek().kind != Tok::IntLit || peek().intValue != 0)
                fail(peek().loc, "a method can only be set to `= 0`, which means it has no body",
                     "write `{ ... }` for a body, or remove the method", spec::Inheritance);
            if (!method->isVirtual && !method->isOverride)
                fail(prev().loc, "only a virtual method can have no body", "write `virtual` before it", spec::Inheritance);
            advance();
            expect(Tok::Semi, "after `= 0`");
            method->isPure = true;
            sd.methods.push_back(std::move(method));
            return;
        }
        if (check(Tok::Semi))
            fail(peek().loc, "write the method body here",
                 "C+ has no separate definitions; replace ';' with `{ ... }`, or write `virtual ... = 0;` for a method "
                 "that deriving classes must write",
                 spec::Classes);
        method->body = parseBlock(method.get());
        sd.methods.push_back(std::move(method));
        return;
    }

    if (sd.isInterface)
        fail(check(Tok::Ident) ? peek().loc : typeLoc, "an interface cannot have fields",
             "an interface only lists methods. Use a class for fields", spec::Interfaces);
    if (ty->is(Type::Kind::Void))
        fail(typeLoc, "a field cannot have type void", "pick a type such as int, or leave the field out", spec::Structs);
    std::string firstName;
    do {
        if (!firstName.empty()) parseLaterStars(ty, firstName, "field");
        if (check(Tok::Amp)) notYet(peek().loc, "reference types", "a field holds a value, or a pointer: `int* p;`");
        const Token& name = expectNewName("for the field name");
        if (firstName.empty()) firstName = name.text;
        FieldDecl field{name.text, name.loc, ty, typeLoc, nullptr, access};
        if (match(Tok::Assign)) field.defaultValue = parseAssign();
        else if (check(Tok::LBrace))
            notYet(peek().loc, "brace initializers for fields",
                   "move what is in the braces after `=`: `" + std::string(ty->name) + " " + name.text + " = ...;`");
        if (check(Tok::LBracket)) failCStyleArray(ty, name.text, ArrayPlace::Field);
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

StmtPtr Parser::parseTry() {
    auto st = std::make_unique<TryStmt>(advance().loc);
    st->body = parseBlock();
    if (!check(Tok::KwCatch))
        fail(peek().loc, "a try block needs at least one catch",
             "write `catch (Error e) { ... }` after it. To pass errors on to the caller, leave out the block and write "
             "`try f();`",
             spec::Errors);
    while (check(Tok::KwCatch)) {
        CatchClause clause;
        clause.loc = advance().loc;
        if (!check(Tok::LParen))
            fail(peek().loc, "a catch says what it takes", "write `catch (Error e) { ... }`, or `catch (...) { ... }` for any error",
                 spec::Errors);
        advance();
        if (check(Tok::Dot) && peek(1).kind == Tok::Dot && peek(2).kind == Tok::Dot) {
            advance();
            advance();
            advance();
        } else {
            if (check(Tok::Ident) && !isTypeName(peek()) && !classTemplate(peek().text) && peek(1).kind == Tok::RParen)
                fail(peek().loc, "a catch says the type of error it takes", "write `catch (Error " + peek().text + ")`", spec::Errors);
            bool isConst = false;
            clause.type = parseType(clause.typeLoc, &isConst);
            if (check(Tok::Amp) || check(Tok::AmpAmp))
                fail(peek().loc, "a catch takes its error without a reference",
                     "write `catch (" + std::string(clause.type->name) + " e)`: a class variable already refers to the object, "
                     "so nothing is copied",
                     spec::Errors);
            if (check(Tok::Ident)) {
                const Token& name = expectNewName("for the name of the error");
                rejectTemplateParamName(name);
                clause.var = std::make_unique<VarDecl>(name.loc, name.text, clause.type, clause.typeLoc);
                clause.var->isConst = isConst;
            }
        }
        expect(Tok::RParen, "to close the catch");
        clause.body = parseBlock();
        st->catches.push_back(std::move(clause));
    }
    if (check(Tok::Ident) && peek().text == "finally")
        fail(peek().loc, "C+ has no finally", "put what must always happen in a destructor, which runs on every way out",
             spec::Errors);
    return st;
}

std::unique_ptr<BlockStmt> Parser::parseBlock(const FuncDecl* function) {
    auto block = std::make_unique<BlockStmt>(expect(Tok::LBrace, "to start a block").loc);
    localScopes_.emplace_back();
    if (function)  // not constants, whatever else has the name
        for (auto& param : function->params) localScopes_.back()[param->name] = param.get();
    struct Leave {
        std::vector<std::unordered_map<std::string, const VarDecl*>>& scopes;
        ~Leave() { scopes.pop_back(); }
    } leave{localScopes_};
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
             std::string(base->name) + std::string(got, '*') + " " + name + ";`",
         spec::PointerDeclarations);
}

StmtPtr Parser::parseDeclarators(bool isConst, const Type* ty, Loc typeLoc) {
    if (ty->is(Type::Kind::Void)) {
        if (peek().kind == Tok::Ident && peek(1).kind == Tok::LParen) {  // `void g() { ... }`
            int depth = 1;
            size_t at = 2;
            for (; depth > 0 && peek(at).kind != Tok::Eof; ++at) {
                if (peek(at).kind == Tok::LParen) ++depth;
                else if (peek(at).kind == Tok::RParen) --depth;
            }
            if (peek(at).kind == Tok::LBrace)
                fail(peek().loc, "C+ has no functions inside functions", "write it at the top level of the file, and pass it what it needs", spec::Functions);
        }
        fail(typeLoc, "variables cannot have type void", "pick a type such as int, or use auto", spec::Variables);
    }
    std::vector<std::unique_ptr<VarDecl>> decls;
    do {
        if (!decls.empty()) parseLaterStars(ty, decls[0]->name, "variable");
        if (check(Tok::LBracket) && peek(1).kind == Tok::RBracket)
            fail(peek().loc, "C+ has no C-style arrays", "write `vector<" + std::string(ty->name) + "> a = {...};`, or `array<" + std::string(ty->name) + ", N> a = {...};`, with N the number of elements", spec::Arrays);
        if (check(Tok::LBracket) || ((check(Tok::Amp) || check(Tok::AmpAmp)) && peek(1).kind == Tok::LBracket))
            notYet(peek().loc, "structured bindings",
                   "name the whole value, and reach into it: `for (auto e : m) ... e.first ... e.second`, or `auto p = f(); ... p.x`");
        if (check(Tok::Amp) || check(Tok::AmpAmp))
            notYet(peek().loc, "references",
                   "a variable holds its own value. To refer to another variable, use a pointer: `int* p = &x;`");
        const Token& name =
            expectNewName(decls.empty() ? "for the variable name" : "for the next variable name after ','");
        rejectTemplateParamName(name);
        auto var = std::make_unique<VarDecl>(name.loc, name.text, ty, typeLoc);
        var->isConst = isConst;
        if (!localScopes_.empty()) localScopes_.back()[name.text] = var.get();  // a name in an array's size may mean it
        if (match(Tok::Assign)) {
            var->init = parseAssign();
        } else if (check(Tok::LBrace) || check(Tok::LParen)) {
            if (check(Tok::LParen)) {  // `int g() { ... }`, which is a function here in C++ too, or would be
                size_t at = 1;
                for (int depth = 1; depth > 0 && at < 256 && peek(at).kind != Tok::Eof; ++at) {
                    if (peek(at).kind == Tok::LParen) ++depth;
                    else if (peek(at).kind == Tok::RParen) --depth;
                }
                if (peek(at).kind == Tok::LBrace)
                    fail(name.loc, "C+ has no functions inside functions", "write it at the top level of the file, and pass it what it needs", spec::Functions);
            }
            std::string help = "write `" + std::string(ty->name) + " " + name.text + " = ...;`, with the same value";
            if (ty->is(Type::Kind::String)) help = "write `string " + name.text + " = string(3, 'a');`, or `string " + name.text + " = \"text\";`";
            if (ty->isClass()) help = "write `" + std::string(ty->name) + " " + name.text + " = " + ty->name + "(...);`";
            if (ty->isStruct() && !isHashmap(ty)) help = "write `" + std::string(ty->name) + " " + name.text + " = {...};`, with the same values";
            if (isHashmap(ty)) help = "write `" + std::string(ty->name) + " " + name.text + " = {...};`";
            if (ty->isVector())
                help = "write `" + std::string(ty->name) + " " + name.text + " = " + (ty->elem->isNumber() ? "{1, 2, 3}" : "{...}") + ";` or `auto " + name.text +
                       " = " + ty->name + "(count, value);`";
            if (ty->isArray())
                help = "list the elements after '=': `" + std::string(ty->name) + " " + name.text + " = {...};`";
            notYet(peek().loc, "brace and constructor initializers", help);
        }
        if (check(Tok::LBracket)) failCStyleArray(ty, name.text);
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
    Nesting nesting(*this);
    rejectOtherLanguages();
    const Token& t = peek();
    switch (t.kind) {
        case Tok::LBrace: return parseBlock();
        case Tok::Semi: advance(); return std::make_unique<BlockStmt>(t.loc);
        case Tok::KwStruct: case Tok::KwClass:
            rejectStructKeyword();
            notYet(t.loc, "types defined inside a function", "define it at the top level of the file");
        case Tok::LBracket:
            if (atAttribute()) {
                Loc attr = parseAttribute();
                fail(attr, "[[allow(new_delete)]] is written before a function, not before a statement",
                     "move it to just before the function, as in `[[allow(new_delete)]] void f() { ... }`",
                     spec::NewDelete);
            }
            break;
        case Tok::KwIf: {
            size_t ifPos = pos_;
            advance();
            return parseIf(match(Tok::KwConstexpr), ifPos, t.loc);
        }
        case Tok::KwWhile: {
            advance();
            expect(Tok::LParen, "after 'while'");
            std::optional<CondDecl> decl;
            ExprPtr cond = isDeclarationStart() ? (decl = parseCondDecl("while"), nullptr) : parseExpr();
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
        case Tok::KwConstexpr: {
            advance();
            StmtPtr decl = parseDeclaration();
            expect(Tok::Semi, "after the variable declaration");
            auto mark = [](VarDecl& v) {
                v.isConst = true;
                v.isConstexpr = true;
            };
            if (decl->kind == Stmt::Kind::VarDecl) mark(static_cast<VarDecl&>(*decl));
            else for (auto& v : static_cast<DeclGroupStmt&>(*decl).decls) mark(*v);
            return decl;
        }
        case Tok::KwStatic:
            notYet(t.loc, "static local variables",
                   "keep the value in a class object, or pass it to the function and return the new value");
        case Tok::KwSwitch: notYet(t.loc, "switch statements", "use if / else if for now");
        case Tok::KwTry:
            if (peek(1).kind == Tok::LBrace) return parseTry();
            break;  // `try f();`, an expression
        case Tok::KwThrow: {
            advance();
            ExprPtr value = check(Tok::Semi) ? nullptr : parseExpr();
            expect(Tok::Semi, "after the throw");
            return std::make_unique<ThrowStmt>(t.loc, std::move(value));
        }
        case Tok::KwCatch:
            fail(t.loc, "a catch comes right after a try block", "write `try { ... } catch (Error e) { ... }`", spec::Errors);
        case Tok::KwExtern:
            fail(t.loc, "C functions are declared outside of functions",
                 "move the `extern \"C\"` declaration to the top level of the file; it can then be called anywhere "
                 "in the file",
                 spec::CFunctions);
        default: break;
    }
    if (isDeclarationStart()) {
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
    const Token& name = expectNewName(std::string("for the name of the variable declared in the ") + stmt +
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
    } else if (isDeclarationStart()) {
        bool isConst = false;
        Loc typeLoc;
        const Type* ty = parseType(typeLoc, &isConst);
        // `for (auto x : items)` loops over the elements of a vector or an array.
        bool ref = check(Tok::Amp) || check(Tok::AmpAmp);
        if ((ref && peek(1).kind == Tok::Ident && peek(2).kind == Tok::Colon) ||
            (check(Tok::Ident) && peek(1).kind == Tok::Colon)) {
            if (ref) {
                // All of what comes after the ':', if it is a plain path.
                std::string range;
                size_t at = 3;
                for (; at < 32 && (peek(at).kind == Tok::Ident || peek(at).kind == Tok::Dot); ++at) range += peek(at).text.empty() ? tokSpelling(peek(at).kind) : peek(at).text;
                if (range.empty() || peek(at).kind != Tok::RParen) range = "items";
                std::string help = std::string("drop the '&': `for (") + (isConst ? "const " : "") + ty->name + " " +
                                   peek(1).text + " : " + range + ")` copies each element, which is cheap";
                if (!isConst)
                    help += ". To change the elements of a vector, loop over the indexes: `for (long i = 0; i < " + range +
                            ".size(); i++) " + range + "[i] = ...;`. Of a hashmap: `for (auto e : " + range + ") " + range + "[e.first] = ...;`";
                notYet(peek().loc, "references in range-based for loops", help);
            }
            if (ty->is(Type::Kind::Void))
                fail(typeLoc, "a loop variable cannot have type void", "write `auto` or the element type",
                     spec::RangeFor);
            const Token& name = expectNewName("for the name of the loop variable");
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
        rejectOtherLanguages();
        checkUnknownType();
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
    Nesting nesting(*this);
    ExprPtr lhs = parseTernary();
    if (check(Tok::Assign) && peek(1).kind == Tok::Greater) notYet(peek().loc, "lambdas", "write a function, and call it by name");
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
    Nesting nesting(*this);
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
    for (int terms = 0;; ++terms) {
        if (terms > kMaxChain) failTooDeep();
        BinOp op;
        int prec = precedence(peek().kind, op);
        if (prec == 0 || prec < minPrec) return lhs;
        Loc opLoc = advance().loc;
        ExprPtr rhs = parseBinary(prec + 1);
        lhs = std::make_unique<BinaryExpr>(opLoc, op, std::move(lhs), std::move(rhs));
    }
}

ExprPtr Parser::parseUnary() {
    Nesting nesting(*this);
    const Token& t = peek();
    switch (t.kind) {
        case Tok::KwTry:
            advance();
            if (check(Tok::LBrace))
                fail(t.loc, "a try block is a statement, so it has no value",
                     "declare a variable before it and assign it inside, or write `try f()` for a single call", spec::Errors);
            return std::make_unique<TryExpr>(t.loc, parseUnary());
        case Tok::Minus: {
            advance();
            // Fold "-<literal>" so that -2147483648 is an int and -9223372036854775808 a long.
            if (check(Tok::IntLit)) {
                const Token& lit = advance();
                Loc loc = t.loc;
                loc.len = lit.loc.col + lit.loc.len - t.loc.col;
                if (lit.intValue > (uint64_t)INT64_MAX + 1)
                    fail(loc, "the number -" + lit.text + " is too small for any integer type",
                         "write a larger number: the smallest long is -9223372036854775808", spec::IntegerLiterals);
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
        case Tok::KwThrow:
            fail(t.loc, "`throw` is a statement, so it cannot be part of an expression",
                 "write `if (bad) throw Error(\"...\");` on a line of its own", spec::Errors);
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
            if (!check(Tok::LParen) && isTypeAt(0))
                fail(peek().loc, "`sizeof` of a type takes parentheses", peek(1).kind == Tok::Less || peek(1).kind == Tok::Star || check(Tok::KwConst) ? std::string("put the whole type in parentheses, as in `sizeof(vector<int>)`") : "write `sizeof(" + peek().text + ")`", spec::Sizeof);
            return std::make_unique<SizeofExpr>(t.loc, nullptr, parseUnary());  // `sizeof x` or `sizeof(x)`
        }
        case Tok::KwAwait: notYet(t.loc, "async and await", "remove `await`: a call waits for its result. To run something without waiting for it: `auto t = spawn f();`, then `t.wait()`");
        case Tok::KwSpawn: {
            advance();
            Loc loc = t.loc;
            ExprPtr call = parseUnary();
            return std::make_unique<SpawnExpr>(loc, std::move(call));
        }
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
             "write `" + name + " n = " + name + "(...);`, with no `*`: the object is reference-counted and freed automatically", spec::Classes);
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
        if (!ty->isStruct() && !ty->isVector() && !ty->isArray())  // reported at the '{', so recovery skips the list
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
ExprPtr Parser::parseMemberAccess(ExprPtr e, const char* after, std::string qualifier) {
    const Token* name = &expect(Tok::Ident, std::string("for the field or method name after '") + after + "'");
    if (qualifier.empty() && match(Tok::ColonColon)) {  // `d.Animal::speak()`
        qualifier = name->text;
        name = &expect(Tok::Ident, "after '" + qualifier + "::'");
    }
    if (!match(Tok::LParen)) {
        auto member = std::make_unique<MemberExpr>(name->loc, std::move(e), name->text);
        member->qualifier = qualifier;
        return member;
    }
    std::vector<ExprPtr> args;
    if (!check(Tok::RParen)) {
        do args.push_back(parseExpr());
        while (match(Tok::Comma));
    }
    expect(Tok::RParen, "to close the argument list of '" + name->text + "'");
    auto call = std::make_unique<MethodCallExpr>(name->loc, std::move(e), name->text, std::move(args));
    call->qualifier = qualifier;
    return call;
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
    for (int links = 0;; ++links) {
        if (links > kMaxChain) failTooDeep();
        const Token& t = peek();
        if (t.kind == Tok::PlusPlus || t.kind == Tok::MinusMinus) {
            advance();
            e = std::make_unique<IncDecExpr>(t.loc, t.kind == Tok::PlusPlus, false, std::move(e));
        } else if (t.kind == Tok::LParen) {
            fail(t.loc, "only functions can be called, by writing their name followed by (...)", "write a function and call it by name, `f(x)`: C+ has no function values yet, so a value cannot be called", spec::Functions);
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
        case Tok::KwWeak: {  // `weak<Node>(object)`, or `weak<Node>()` for an empty one
            --pos_;  // back to `weak`, to read the whole type
            Loc loc;
            const Type* ty = parseBaseType(loc);
            if (check(Tok::LBrace))
                fail(peek().loc, "a weak reference is not built with a { } list",
                     "write `" + std::string(ty->name) + "(object)`, or `" + ty->name + "()` for an empty one", spec::Weak);
            expect(Tok::LParen, "after '" + std::string(ty->name) + "'");
            ExprPtr operand = check(Tok::RParen) ? ExprPtr(std::make_unique<NullLitExpr>(loc)) : parseExpr();
            expect(Tok::RParen, "to close '" + std::string(ty->name) + "(...)'");
            return std::make_unique<CastExpr>(loc, ty, std::move(operand));
        }
        case Tok::IntLit:
            if (t.intValue > (uint64_t)INT64_MAX)
                fail(t.loc, "the number " + t.text + " is too large for any integer type",
                     "write a smaller number: the largest long is 9223372036854775807", spec::IntegerLiterals);
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
            // `auto x = Foo<int>()`, which has been read as a comparison up to here.
            if (pos_ >= 3 && toks_[pos_ - 2].kind == Tok::Less && toks_[pos_ - 3].kind == Tok::Ident && !classTemplate(toks_[pos_ - 3].text)) {
                if (isOtherContainer(toks_[pos_ - 3].text)) notYet(toks_[pos_ - 3].loc, "'" + toks_[pos_ - 3].text + "'", "use a `vector<T>` or a `hashmap<K, V>` for now");
                failUnknownGeneric(pos_ - 3);
            }
            fail(t.loc, "expected an expression, found " + describeToken(t),
                 "a type name can only start a declaration, e.g. `" + std::string(tokSpelling(t.kind)) + " x = ...;`", spec::Grammar);
        case Tok::KwNull: return std::make_unique<NullLitExpr>(t.loc);
        case Tok::KwThis: return std::make_unique<ThisExpr>(t.loc);
        case Tok::LParen: {
            size_t close = 0;
            for (int depth = 1; depth > 0 && close < 256 && peek(close).kind != Tok::Eof; ++close) {
                if (peek(close).kind == Tok::LParen) ++depth;
                else if (peek(close).kind == Tok::RParen) --depth;
            }
            if (peek(close).kind == Tok::Assign && peek(close + 1).kind == Tok::Greater)  // `(a, b) => a + b`
                notYet(t.loc, "lambdas", "write a function at the top level of the file, and call it by name");
            // `(char*)p` or `(const Node*)p`: a type with stars, then ')'.
            size_t i = peek().kind == Tok::KwConst ? 1 : 0;
            bool baseType = isCastType(peek(i).kind) || peek(i).kind == Tok::KwVoid || isTypeName(peek(i));
            size_t afterBase = i + 1;
            if (peek(i).kind == Tok::Ident && peek(i + 1).kind == Tok::Less && isTypeAt(i)) {  // `(array<int, 3>*)p`
                afterBase = afterAngles(i + 1);
                baseType = afterBase != 0;
            }
            if (baseType && peek(afterBase).kind == Tok::Star) {
                for (i = afterBase; peek(i).kind == Tok::Star || peek(i).kind == Tok::KwConst; ++i) {}
                if (peek(i).kind == Tok::RParen) {
                    Loc typeLoc;
                    const Type* ty = parseType(typeLoc);
                    expect(Tok::RParen, "to close the cast");
                    Loc loc = t.loc;
                    if (prev().loc.line == loc.line) loc.len = prev().loc.col + 1 - t.loc.col;
                    return std::make_unique<CastExpr>(loc, ty, parseUnary());
                }
            }
            if ((isCastType(peek().kind) || isTypeName(peek())) && peek(1).kind == Tok::RParen) {  // `(int)x`, as in C
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
            if ((t.text == "static_cast" || t.text == "reinterpret_cast" || t.text == "dynamic_cast") && check(Tok::Less)) {  // `static_cast<int>(x)`
                advance();
                Loc typeLoc;
                const Type* ty = parseType(typeLoc);
                expectCloseAngle("to close " + t.text + "<...>");
                expect(Tok::LParen, "after " + t.text + "<" + std::string(ty->name) + ">");
                ExprPtr operand = parseExpr();
                expect(Tok::RParen, "to close the " + t.text);
                Loc loc = t.loc;
                if (prev().loc.line == loc.line) loc.len = prev().loc.col + 1 - loc.col;
                auto cast = std::make_unique<CastExpr>(loc, ty, std::move(operand));
                cast->reinterpret = t.text == "reinterpret_cast";
                cast->dynamic = t.text == "dynamic_cast";
                return cast;
            }
            if (t.text == "const_cast")
                fail(t.loc, "C+ has no const_cast", "a const variable cannot be changed; copy it into a variable that is not const",
                     spec::Casts);
            if ((t.text == "function" || t.text == "func" || t.text == "fn") && check(Tok::LParen)) {  // `function(a) { ... }`
                size_t close = 1;
                for (int depth = 1; depth > 0 && close < 256 && peek(close).kind != Tok::Eof; ++close) {
                    if (peek(close).kind == Tok::LParen) ++depth;
                    else if (peek(close).kind == Tok::RParen) --depth;
                }
                if (peek(close).kind == Tok::LBrace) notYet(t.loc, "lambdas", "write a function at the top level of the file, and call it by name");
            }
            if (check(Tok::ColonColon)) {
                if (t.text == "std") failStd(t.loc, peek(1));
                if ((isTypeName(t) || classTemplate(t.text)) && peek(1).kind == Tok::Ident) {  // `Animal::speak()`: a member of `this`
                    advance();
                    return parseMemberAccess(std::make_unique<ThisExpr>(t.loc), "::", t.text);
                }
                if (isBuiltinModule(t.text))
                    fail(t.loc, "'" + t.text + "::" + peek(1).text + "' is in the module '" + t.text + "', which this program does not import",
                         "write `import " + t.text + ";` at the top of the file", spec::Modules);
                notYet(t.loc, "module members like '" + t.text + "::" + peek(1).text + "'", "write the name without `" + t.text + "::`. The modules today are `http`, `json` and `reflect`, each with its `import`. Built-ins need no prefix");
            }
            if (t.loc.line == 0 && (t.text == "__zero" || t.text == "__type_of")) {  // see Builtins.h
                expect(Tok::LParen, "after " + t.text);
                Loc typeLoc;
                const Type* ty = parseType(typeLoc);
                expect(Tok::RParen, "to close " + t.text);
                auto made = std::make_unique<SizeofExpr>(t.loc, ty, nullptr);
                made->what = t.text == "__zero" ? SizeofExpr::What::Zero : SizeofExpr::What::Descriptor;
                return made;
            }
            if (auto bound = bindings_.find(t.text); bound != bindings_.end() && !bound->second.type) {  // `N`
                const TemplateBinding& b = bound->second;
                if (b.valueType->is(Type::Kind::Bool)) return std::make_unique<BoolLitExpr>(t.loc, b.value.value != 0);
                if (b.valueType->is(Type::Kind::Char)) return std::make_unique<CharLitExpr>(t.loc, (uint8_t)b.value.value);
                return std::make_unique<IntLitExpr>(t.loc, b.value.value, b.valueType->is(Type::Kind::Long));
            }
            if (const Type* bound = boundType(t); bound && (check(Tok::LParen) || check(Tok::LBrace)))
                return parseBoundValue(t, bound);
            if (check(Tok::Less) && isConstraintName(t.text) && isTypeAt(1)) {  // `Number<T>`
                advance();
                Loc loc;
                const Type* first = parseType(loc);
                const Type* second = nullptr;
                if (t.text == "same_as" || t.text == "derived_from") {
                    expect(Tok::Comma, "and the second type of '" + t.text + "'");
                    second = parseType(loc);
                }
                expectCloseAngle("to close '" + t.text + "<...>'");
                return std::make_unique<ConstraintExpr>(t.loc, t.text, first, second);
            }
            TemplateDecl* tmpl = classTemplate(t.text);
            if (tmpl && !boundType(t) && (check(Tok::LParen) || check(Tok::LBrace)))
                fail(t.loc, "this compiler does not support working out the template arguments of '" + t.text + "' yet",
                     "write e.g. `" + exampleArguments(*tmpl) + "(...)`" + constraintNote(*tmpl), spec::Generics);
            // A variable can have the same name: `Box < 3` is a comparison.
            const bool ownName = injected_ && currentTemplate_ && t.text == currentTemplate_->name;
            if (tmpl && (!boundType(t) || ownName) && check(Tok::Less) && (tmpl->params[0].valueType || isTypeAt(1))) {
                --pos_;  // back to the name, to read the whole type
                Loc loc;
                const Type* ty = parseBaseType(loc);
                if (match(Tok::ColonColon)) return parseMemberAccess(std::make_unique<ThisExpr>(t.loc), "::", ty->name);
                if (match(Tok::LBrace)) return parseInitList(loc, ty);
                expect(Tok::LParen, "or '{' after '" + std::string(ty->name) + "'");
                std::vector<ExprPtr> args;
                if (!check(Tok::RParen)) {
                    do args.push_back(parseExpr());
                    while (match(Tok::Comma));
                }
                expect(Tok::RParen, "to close the argument list of '" + std::string(ty->name) + "'");
                return std::make_unique<CallExpr>(loc, ty->name, std::move(args));
            }
            if (TemplateDecl* tmpl = functionTemplate(t.text);
                tmpl && check(Tok::Less) && afterAngles(0) && peek(afterAngles(0)).kind == Tok::LParen) {  // `max<long>(a, b)`
                std::vector<TemplateArg> templateArgs = parseTemplateArgs(*tmpl, t.loc, true);
                expect(Tok::LParen, "after the template arguments");
                std::vector<ExprPtr> args;
                if (!check(Tok::RParen)) {
                    do args.push_back(parseExpr());
                    while (match(Tok::Comma));
                }
                expect(Tok::RParen, "to close the argument list of '" + t.text + "'");
                auto call = std::make_unique<CallExpr>(t.loc, t.text, std::move(args));
                call->templateArgs = std::move(templateArgs);
                return call;
            }
            if (isTypeName(t) && match(Tok::LBrace)) return parseInitList(t.loc, &prog_->findStruct(t.text)->type);
            if (t.text == "make_unique" && check(Tok::Less)) {
                size_t closing = 0;
                const std::string whole = writtenBetweenAngles(pos_ + 1, closing);
                fail(t.loc, "C+ has no make_unique", "write `unique<" + whole + ">(...)`, which builds a new " + whole, spec::Unique);
            }
            if (t.text == "make_shared" && check(Tok::Less))
            {
                size_t closing = 0;
                const std::string whole = writtenBetweenAngles(pos_ + 1, closing);
                fail(t.loc, "C+ has no make_shared",
                     peek(1).kind == Tok::Ident && peek(2).kind == Tok::Less && classTemplate(peek(1).text) == nullptr
                         ? "write `" + whole + "(...)`. It is a value, which is copied, so to share one hold it in a class"
                     : peek(1).kind == Tok::Ident ? "write `" + whole + "(...)`. A class variable is already a counted reference. A struct is a value, which is copied, so to share one make it a class"
                                                : std::string("write the value itself, as in `int(3)`: a value needs no pointer"),
                     spec::Classes);
            }
            // With a name that is no type, what can only be this goes on, so that the error can say so.
            if ((t.text == "unique" || t.text == "local") && check(Tok::Less) &&
                (isTypeAt(1) || (peek(1).kind == Tok::Ident && peek(2).kind == Tok::Greater && peek(3).kind == Tok::LParen))) {  // `unique<Node>(args)`
                --pos_;  // back to the word, to read the whole type
                Loc loc;
                const Type* ty = parseBaseType(loc);
                if (match(Tok::LBrace)) return parseInitList(loc, ty);
                expect(Tok::LParen, "or '{' after '" + std::string(ty->name) + "'");
                std::vector<ExprPtr> args;
                if (!check(Tok::RParen)) {
                    do args.push_back(parseExpr());
                    while (match(Tok::Comma));
                }
                expect(Tok::RParen, "to close the argument list of '" + std::string(ty->name) + "'");
                auto call = std::make_unique<CallExpr>(loc, ty->elem->name, std::move(args));
                call->uniqueType = ty;
                return call;
            }
            if (t.text == "vector" && check(Tok::Less)) {
                --pos_;  // back to `vector`, to read the whole type
                Loc loc;
                const Type* ty = parseBaseType(loc);  // a '?' after it is an operator
                if (match(Tok::LBrace)) return parseInitList(loc, ty);
                if (!match(Tok::LParen))
                    fail(peek().loc, "expected '{' or '(' after '" + std::string(ty->name) + "'",
                         "write `" + std::string(ty->name) + (ty->elem->isNumber() ? "{1, 2, 3}" : "{...}") + "` to list the elements, or `" + ty->name +
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
            if (t.text == "array" && check(Tok::Less) && isTypeAt(1)) {
                --pos_;  // back to `array`, to read the whole type
                Loc loc;
                const Type* ty = parseBaseType(loc);  // a '?' after it is an operator
                if (match(Tok::LBrace)) return parseInitList(loc, ty);
                std::string name = ty->name;
                if (!match(Tok::LParen))
                    fail(peek().loc, "expected '{' or '(' after '" + name + "'",
                         "write `" + name + "{1, 2, 3}` to list the elements, or `" + name + "()` for default values",
                         spec::Arrays);
                if (!check(Tok::RParen))
                    fail(peek().loc, "'" + name + "(...)' takes no values",
                         "to list the elements, use braces: `" + name + "{1, 2, 3}`. To set them all to one value, "
                         "use `a.fill(value)`",
                         spec::Arrays);
                advance();
                auto list = std::make_unique<InitListExpr>(loc);  // the same as `array<int, 3>{}`
                list->explicitType = ty;
                return list;
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
            return std::make_unique<NameExpr>(t.loc, t.text);
        }
        case Tok::LBracket: {
            // `[](int x) { ... }` is a lambda, and `[1, 2, 3]` a list as other languages write it.
            size_t close = 0;
            for (int depth = 1; depth > 0 && close < 256 && peek(close).kind != Tok::Eof; ++close) {
                if (peek(close).kind == Tok::LBracket) ++depth;
                else if (peek(close).kind == Tok::RBracket) --depth;
            }
            if (peek(close).kind != Tok::LParen && peek(close).kind != Tok::LBrace)
                fail(t.loc, "C+ writes a list in braces, as C++ does", "write the list in braces: `{1, 2, 3}`. `auto` needs the type written: `vector<int>{1, 2, 3}`", spec::Vectors);
            notYet(t.loc, "lambdas", "write a function, and call it by name");
        }
        case Tok::KwElse:
            fail(t.loc, "an `else` comes right after the body of an `if`", "check the braces before it, or remove it", spec::ControlFlow);
        case Tok::KwCase: case Tok::KwDefault:
            notYet(t.loc, "switch statements", "use if / else if for now");
        default:
            fail(t.loc, "expected an expression, found " + describeToken(t), "write a value, a name or a call, such as `1`, `x` or `f(x)`", spec::Grammar);
    }
}

// A type parameter used like a type's name: `T(x)`, `T()`, `T{1, 2}`, whatever it stands for.
ExprPtr Parser::parseBoundValue(const Token& t, const Type* bound) {
    bool buildsValue = bound->hasFields() || bound->isVector() || bound->isArray() || bound->is(Type::Kind::TypeParam) ||
                       bound->is(Type::Kind::Dependent);
    if (match(Tok::LBrace)) {
        if (buildsValue) return parseInitList(t.loc, bound);
        notYet(t.loc, "brace initializers for '" + std::string(bound->name) + "'", "write `" + t.text + "(value)`");
    }
    advance();  // the '('
    std::vector<ExprPtr> args;
    if (!check(Tok::RParen)) {
        do args.push_back(parseExpr());
        while (match(Tok::Comma));
    }
    expect(Tok::RParen, "to close the argument list of '" + t.text + "'");
    if (bound->isVector()) {
        auto call = std::make_unique<CallExpr>(t.loc, bound->name, std::move(args));
        call->vectorType = bound;
        return call;
    }
    if (bound->isArray() && args.empty()) {
        auto list = std::make_unique<InitListExpr>(t.loc);
        list->explicitType = bound;
        return list;
    }
    if (bound->isArray()) {  // only a copy of one
        if (args.size() > 1)
            fail(t.loc, "'" + std::string(bound->name) + "(...)' takes no values", "use braces to list the elements", spec::Arrays);
        return std::make_unique<CastExpr>(t.loc, bound, std::move(args[0]));
    }
    if (buildsValue) return std::make_unique<CallExpr>(t.loc, bound->name, std::move(args));
    if (args.size() > 1)
        fail(t.loc, "'" + std::string(bound->name) + "' takes one value, but " + std::to_string(args.size()) + " were given", "give it one value, such as `T(x)`", spec::Generics);
    return std::make_unique<CastExpr>(t.loc, bound, args.empty() ? nullptr : std::move(args[0]));
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
