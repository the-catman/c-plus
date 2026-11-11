#include "Modules.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "AST.h"
#include "SpecRefs.h"

namespace cplus {

namespace fs = std::filesystem;

namespace {

bool readFromDisk(const std::string& path, std::string& text) {
    std::error_code ec;
    if (fs::is_directory(path, ec)) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    text = ss.str();
    return true;
}

std::string prefixOf(const std::string& dotted) {
    std::string out;
    for (char c : dotted) out += c == '.' ? std::string("::") : std::string(1, c);
    return out;
}

bool isTypeWord(Tok k) {
    return k == Tok::KwInt || k == Tok::KwLong || k == Tok::KwBool || k == Tok::KwChar || k == Tok::KwDouble || k == Tok::KwFloat ||
           k == Tok::KwString || k == Tok::KwVoid || k == Tok::KwAuto;
}

// Where a name is declared rather than used: the `x` of `Shape x;`, `int x = 1`, `f(Shape x)`. A variable or field can have
// the name of a type.
bool declaratorAt(const std::vector<Token>& t, size_t i) {
    if (i == 0 || i + 1 >= t.size()) return false;
    const Tok before = t[i - 1].kind, after = t[i + 1].kind;
    const bool typeBefore = before == Tok::Ident || before == Tok::Greater || before == Tok::Shr || before == Tok::Star ||
                            before == Tok::Question || isTypeWord(before);
    return typeBefore && (after == Tok::Semi || after == Tok::Assign || after == Tok::Comma || after == Tok::RParen ||
                          after == Tok::LBracket || after == Tok::Colon);
}

// A member of a value, `p.x`, rather than a name of the file. `d.Left::id` still names the type `Left`.
bool afterMemberAccess(const std::vector<Token>& t, size_t i) {
    if (i == 0) return false;
    const Tok before = t[i - 1].kind;
    if (before == Tok::ColonColon) return true;
    return (before == Tok::Dot || before == Tok::Arrow || before == Tok::QuestionDot) && !(i + 1 < t.size() && t[i + 1].kind == Tok::ColonColon);
}

// From a `<` that starts template arguments or parameters, the token after the `>` that closes it.
size_t afterAngles(const std::vector<Token>& t, size_t i, size_t n) {
    int depth = 0;
    for (; i < n; ++i) {
        const Tok k = t[i].kind;
        if (k == Tok::Less) ++depth;
        else if (k == Tok::Greater) --depth;
        else if (k == Tok::Shr) depth -= 2;
        else if (k == Tok::Semi || k == Tok::LBrace || k == Tok::RBrace) return i;
        if (depth <= 0) return i + 1;
    }
    return n;
}

// The names that what comes with the language uses by name, which only the main file can have its own of.
const char* const kReplaceable[] = {"Error", "hashmap", "Task", "Mutex", "LockGuard", "delay"};

bool hasMain(const std::vector<Token>& t) {
    int depth = 0;
    for (size_t i = 0; i + 1 < t.size(); ++i) {
        if (t[i].kind == Tok::LBrace) ++depth;
        else if (t[i].kind == Tok::RBrace) --depth;
        else if (depth == 0 && t[i].kind == Tok::Ident && t[i].text == "main" && t[i + 1].kind == Tok::LParen) return true;
    }
    return false;
}

bool isIdentifier(const std::string& s) {
    if (s.empty() || !(std::isalpha((unsigned char)s[0]) || s[0] == '_')) return false;
    for (char c : s)
        if (!(std::isalnum((unsigned char)c) || c == '_')) return false;
    return true;
}

// Whether a file is a program of its own, with a `main`, rather than a module.
bool isProgram(const std::string& path, const Modules::Options& options) {
    std::string text;
    if (!(options.read ? options.read(path, text) : readFromDisk(path, text))) return false;
    SourceFile file(path, text);
    Diagnostics quiet(file);
    quiet.mute();
    return hasMain(Lexer(file, quiet).lexAll());
}

bool isReplaceable(const std::string& name) {
    for (const char* r : kReplaceable)
        if (name == r) return true;
    return false;
}

}  // namespace

std::string Modules::pathOf(const std::string& dotted) const {
    std::string rel;
    for (char c : dotted) rel += c == '.' ? '/' : c;
    rel += ".cplus";
    return root_.empty() ? rel : (fs::path(root_) / rel).string();
}

const ModuleFile* Modules::byName(const std::string& dotted) const {
    for (const ModuleFile& f : files)
        if (!f.name.empty() && f.name == dotted) return &f;
    return nullptr;
}

const ModuleFile* Modules::ownerOf(const std::string& qualified, std::string* rest) const {
    const ModuleFile* best = nullptr;
    for (const ModuleFile& f : files)
        if (!f.name.empty() && qualified.size() > f.prefix.size() + 2 && qualified.compare(0, f.prefix.size(), f.prefix) == 0 &&
            qualified.compare(f.prefix.size(), 2, "::") == 0 && (!best || f.prefix.size() > best->prefix.size()))
            best = &f;
    if (best && rest) *rest = qualified.substr(best->prefix.size() + 2);
    return best;
}

bool Modules::imports(uint32_t file, const ModuleFile& module) const {
    if (file >= files.size()) return false;
    if (&files[file] == &module) return true;
    for (const ModuleImport& i : files[file].imports)
        if (i.name == module.name) return true;
    return false;
}

const ModuleFile* Modules::importedEndingIn(uint32_t file, const std::string& last) const {
    if (file >= files.size()) return nullptr;
    for (const ModuleImport& i : files[file].imports) {
        const size_t dot = i.name.rfind('.');
        if ((dot == std::string::npos ? i.name : i.name.substr(dot + 1)) == last && i.name != last) return byName(i.name);
    }
    return nullptr;
}

std::string Modules::resolve(const std::string& name, uint32_t file) const {
    if (file >= files.size()) return name;
    const ModuleFile& m = files[file];
    if (m.name.empty() || name.find("::") != std::string::npos) return name;
    return m.members.count(name) ? m.prefix + "::" + name : name;
}

Modules::Place Modules::placeOf(const SourceFile& file, const std::string& top, const Options& options) {
    const fs::path path(file.path), folder = path.parent_path();
    const Place own{folder.string(), ""};
    Diagnostics quiet(file);
    quiet.mute();
    Modules probe;
    probe.options_ = options;
    ModuleFile f;
    f.source = &file;
    f.tokens = Lexer(file, quiet).lexAll();
    if (path.filename() == "main.cplus" || hasMain(f.tokens) || !isIdentifier(path.stem().string())) return own;
    probe.scanImports(f, quiet, true);
    auto read = options.read ? options.read : readFromDisk;
    std::error_code ec;
    const fs::path here = top.empty() && path.is_absolute() ? fs::current_path(ec) : fs::path(top);  // "" is the folder this runs in
    const fs::path end = here.lexically_normal();
    fs::path dir = folder;
    std::string name = path.stem().string();
    for (;;) {
        probe.root_ = dir.string();
        bool found = true;
        std::string text;
        for (const ModuleImport& i : f.imports) found = found && read(probe.pathOf(i.name), text);
        if (found) return Place{dir.string(), name};
        const std::string part = dir.filename().string();
        if (dir.empty() || dir == dir.parent_path() || dir.lexically_normal() == end || !isIdentifier(part)) break;
        name = part + "." + name;
        dir = dir.parent_path();
    }
    return Place{folder.string(), path.stem().string()};
}

void Modules::scanImports(ModuleFile& file, Diagnostics& diags, bool quiet) {
    const std::vector<Token>& t = file.tokens;
    int depth = 0;
    for (size_t i = 0; i + 1 < t.size(); ++i) {
        if (t[i].kind == Tok::LBrace) ++depth;
        else if (t[i].kind == Tok::RBrace && depth > 0) --depth;
        if (depth != 0 || t[i].kind != Tok::KwImport) continue;
        if (t[i + 1].kind == Tok::HeaderName || t[i + 1].kind == Tok::StringLit) {
            file.importsHeader = true;
            continue;
        }
        if (t[i + 1].kind != Tok::Ident) continue;
        std::string name = t[i + 1].text;
        size_t j = i + 2;
        while (j + 1 < t.size() && t[j].kind == Tok::Dot && t[j + 1].kind == Tok::Ident) {
            name += "." + t[j + 1].text;
            j += 2;
        }
        if (t[j].kind != Tok::Semi) continue;  // the parser says what is wrong with it
        Loc loc = t[i].loc;
        if (t[j].loc.line == loc.line) loc.len = t[j].loc.col + 1 - loc.col;
        if (isBuiltinModule(name)) {  // which a file of that name, which is no program of its own, would look like it is
            std::error_code ec;
            const std::string path = pathOf(name);
            if (!quiet && fs::is_regular_file(path, ec) && !fs::equivalent(path, files[0].source->path, ec) && !isProgram(path, options_))
                diags.error(loc, "the file " + pathOf(name) + " has the name of the module '" + name + "', which comes with C+",
                            "rename the file, such as to `my" + name + ".cplus`, and import it by that name: `import my" + name + ";`. `import " + name +
                                ";` is always the one that comes with the language",
                            spec::Modules);
            continue;
        }
        file.imports.push_back(ModuleImport{name, loc, -1});
    }
}

void Modules::scanMembers(ModuleFile& file, std::vector<size_t>& declaring) {
    const std::vector<Token>& t = file.tokens;
    const size_t n = t.empty() ? 0 : t.size() - 1;  // not the end of file
    bool isTemplate = false;  // of the declaration being read
    auto add = [&](size_t at, bool exported, bool isType, bool isFunction) {
        if (at >= n || t[at].kind != Tok::Ident) return;
        ModuleMember& m = file.members[t[at].text];
        if (m.name.empty()) m = ModuleMember{t[at].text, t[at].loc, exported, isType, isFunction, isTemplate};  // the first, of two with one name
        declaring.push_back(at);
    };
    // From inside a declaration: past its end, a `;` or the `}` of its body.
    auto endOf = [&](size_t i, bool hasBody) {
        int parens = 0, braces = 0;
        for (; i < n; ++i) {
            const Tok k = t[i].kind;
            if (k == Tok::LParen || k == Tok::LBracket) ++parens;
            else if ((k == Tok::RParen || k == Tok::RBracket) && parens > 0) --parens;
            else if (k == Tok::LBrace) ++braces;
            else if (k == Tok::RBrace) {
                if (braces > 0 && --braces == 0 && parens == 0 && hasBody) return i + 1 < n && t[i + 1].kind == Tok::Semi ? i + 2 : i + 1;
                if (braces < 0) return i + 1;
            } else if (k == Tok::Semi && parens == 0 && braces == 0) return i + 1;
        }
        return n;
    };
    size_t i = 0;
    while (i < n) {
        bool exported = false;
        bool skipped = false;
        while (i < n) {  // `export` and attributes, in either order
            if (t[i].kind == Tok::KwExport) {
                exported = true;
                ++i;
            } else if (t[i].kind == Tok::LBracket && i + 1 < n && t[i + 1].kind == Tok::LBracket) {
                size_t j = i + 2;
                while (j + 1 < n && !(t[j].kind == Tok::RBracket && t[j + 1].kind == Tok::RBracket)) ++j;
                i = j + 2;
                if (i < n && t[i].kind == Tok::Semi) {  // `[[allow(new_delete)]];`, for the whole file
                    ++i;
                    skipped = true;
                    break;
                }
            } else break;
        }
        if (skipped || i >= n) continue;
        const size_t start = i;
        Tok k = t[i].kind;
        if (k == Tok::KwImport) {
            i = endOf(i, false);
            continue;
        }
        if (k == Tok::KwExtern) {  // not a member: a C name is one thing in the whole program
            const size_t end = endOf(i, true);
            for (size_t j = i; j + 1 < end; ++j) {
                if ((t[j].kind == Tok::KwStruct || t[j].kind == Tok::KwClass) && t[j + 1].kind == Tok::Ident) file.cTypes.insert(t[j + 1].text);
                else if (t[j].kind == Tok::Ident && t[j + 1].kind == Tok::LParen && j > i && t[j - 1].kind != Tok::LParen && t[j - 1].kind != Tok::Comma)
                    file.cFunctions.insert(t[j].text);
            }
            i = end;
            continue;
        }
        isTemplate = false;
        if (k == Tok::KwTemplate) {
            isTemplate = true;
            i = afterAngles(t, i + 1, n);
            if (i >= n) break;
            k = t[i].kind;
        }
        if (k == Tok::KwStruct || k == Tok::KwClass || k == Tok::KwInterface) {
            add(i + 1, exported, true, false);
            i = endOf(i, true);
        } else if (k == Tok::KwEnum) {
            size_t at = i + 1;
            if (at < n && (t[at].kind == Tok::KwClass || t[at].kind == Tok::KwStruct)) ++at;
            add(at, exported, true, false);
            i = endOf(i, true);
        } else if (k == Tok::Ident && t[i].text == "concept") {
            add(i + 1, exported, true, false);
            i = endOf(i, false);
        } else {
            // A function, when a `(` comes before any `=`; otherwise constants, each named just before its `=`.
            int angles = 0;
            size_t at = i;
            for (; at < n; ++at) {
                const Tok a = t[at].kind;
                if (a == Tok::Less) ++angles;
                else if (a == Tok::Greater && angles > 0) --angles;
                else if (a == Tok::Shr) angles = angles > 1 ? angles - 2 : 0;
                else if (angles == 0 && (a == Tok::LParen || a == Tok::Assign || a == Tok::Semi || a == Tok::LBrace || a == Tok::RBrace)) break;
            }
            if (at < n && t[at].kind == Tok::LParen) {
                add(at - 1, exported, false, true);
                i = endOf(at, true);
            } else if (at < n && t[at].kind == Tok::Assign) {
                int depth = 0;
                size_t j = at;
                for (; j < n; ++j) {  // each `name =` at the top of the declaration
                    const Tok a = t[j].kind;
                    if (a == Tok::LParen || a == Tok::LBracket || a == Tok::LBrace) ++depth;
                    else if ((a == Tok::RParen || a == Tok::RBracket || a == Tok::RBrace) && depth > 0) --depth;
                    else if (a == Tok::Semi && depth == 0) break;
                    else if (a == Tok::Assign && depth == 0 && j > start) add(j - 1, exported, false, false);
                }
                i = j < n ? j + 1 : n;
            } else {
                i = endOf(i, false);
            }
        }
        if (i <= start) i = start + 1;  // always onward
    }
}

void Modules::qualify(ModuleFile& file, const std::vector<size_t>& declaring) {
    std::vector<Token>& t = file.tokens;
    std::set<size_t> declares(declaring.begin(), declaring.end());
    // `net :: server :: x`, a path of the program's modules, whose first part may be a name of this one too
    auto startsPath = [&](size_t i) {
        std::string dotted = t[i].text;
        for (size_t j = i; j + 2 < t.size() && t[j + 1].kind == Tok::ColonColon && t[j + 2].kind == Tok::Ident; j += 2) {
            if (byName(dotted)) return true;
            dotted += "." + t[j + 2].text;
        }
        return false;
    };
    for (size_t i = 0; i + 1 < t.size(); ++i) {
        Token& tok = t[i];
        if (tok.kind != Tok::Ident) continue;
        if (declares.count(i)) {
            tok.text = file.prefix + "::" + tok.text;
            tok.moduleName = true;
            continue;
        }
        auto it = file.members.find(tok.text);
        if (it == file.members.end() || !it->second.isType || afterMemberAccess(t, i) || declaratorAt(t, i)) continue;
        if (t[i + 1].kind == Tok::ColonColon && startsPath(i)) continue;
        tok.text = file.prefix + "::" + tok.text;
        tok.moduleName = true;
    }
}

void Modules::checkNames(ModuleFile& file, Diagnostics& diags) const {
    const bool isModule = !file.name.empty();
    if (isModule)
        for (auto& [name, m] : file.members) {
            if (name == "main" && m.isFunction)
                diags.error(m.loc, "'main' can only be in the main file, the one given to `cplus run`, and not in the module '" + file.name + "'",
                            "move it to the main file, or rename it. A module is what other files import", spec::Modules);
            else if (isReplaceable(name)) {
                std::string last = file.name.substr(file.name.rfind('.') + 1);  // `ServerError`, in net.server
                last[0] = (char)std::toupper((unsigned char)last[0]);
                diags.error(m.loc, "'" + name + "' is the name of what comes with C+, and only the main file can have its own",
                            name == "Error" ? "rename it, such as to `" + last + "Error`. A class of errors can derive from the built-in one: `class " + last +
                                                  "Error : public Error { ... };`"
                                            : "rename it, such as to `My" + name + "`: this module uses the built-in one by its name",
                            spec::Modules);
            }
        }
    const ModuleFile* mainFile = !files.empty() && files[0].name.empty() ? &files[0] : nullptr;
    std::vector<Token>& t = file.tokens;
    std::set<std::string> reported;
    for (size_t i = 0; i + 1 < t.size(); ++i) {
        Token& tok = t[i];
        if (tok.kind != Tok::Ident || tok.moduleName || afterMemberAccess(t, i) || declaratorAt(t, i) || file.members.count(tok.text)) continue;
        if (t[i + 1].kind == Tok::ColonColon || isReplaceable(tok.text)) continue;  // a path, or what may be the main file's own for the whole program
        if (reported.count(tok.text)) continue;
        if (isModule && mainFile) {
            auto it = mainFile->members.find(tok.text);
            if (it != mainFile->members.end() && it->second.isType) {
                reported.insert(tok.text);
                diags.error(tok.loc, "'" + tok.text + "' is declared in the main file, which other files cannot use",
                            "move it into a module of its own, export it there, and import that module in each file that uses it", spec::Modules);
                continue;
            }
        }
        if (!file.cTypes.count(tok.text) && !file.importsHeader)
            for (const ModuleFile& other : files)
                if (&other != &file && other.cTypes.count(tok.text)) {
                    reported.insert(tok.text);
                    diags.error(tok.loc, "'" + tok.text + "' is a C type that this file does not declare",
                                "declare it in this file too, as the other file does: `extern \"C\" struct " + tok.text +
                                    ";`. It is the same type in every file, but each file says which C names it uses",
                                spec::Modules);
                    break;
                }
        for (const ModuleFile& other : files) {
            if (&other == &file || other.name.empty()) continue;
            auto it = other.members.find(tok.text);
            if (it == other.members.end() || !it->second.isType || !isExported(it->second)) continue;
            const bool imported = imports(file.source ? file.source->id : 0, other);
            if (reported.insert(tok.text).second)
                diags.error(tok.loc, "'" + tok.text + "' is in the module '" + other.name + "', and outside its file it is written `" + other.prefix + "::" + tok.text + "`",
                            imported ? "write `" + other.prefix + "::" + tok.text + "`"
                                     : "write `import " + other.name + ";` at the top of the file, then `" + other.prefix + "::" + tok.text + "`",
                            spec::Modules);
            tok.text = other.prefix + "::" + tok.text;  // as if it had been written so, which is said once, and not again where it is read
            tok.moduleName = true;
            break;
        }
    }
}

// What comes after a file is read with it, so a `{` the file leaves open would take it in. Each one is closed where the file
// ends, with the error that would be said there if nothing came after it.
void Modules::closeBlocks(ModuleFile& file, Diagnostics& diags) const {
    int depth = 0;
    for (const Token& t : file.tokens) {
        if (t.kind == Tok::LBrace) ++depth;
        else if (t.kind == Tok::RBrace && depth > 0) --depth;  // one that closes nothing is reported where it is
    }
    if (depth == 0 || file.tokens.empty() || files.size() == 1) return;  // the parser closes the blocks of a program of one file
    Token close = file.tokens.back();
    diags.error(close.loc, "expected '}' to close the block, found end of file", "write '}' to close the block", spec::Grammar);
    close.kind = Tok::RBrace;
    close.text.clear();
    file.tokens.insert(file.tokens.end() - 1, (size_t)depth, close);
}

void Modules::load(const SourceFile& given, const std::string& moduleName, Diagnostics& diags, const Options& options) {
    root_ = options.root;
    options_ = options;
    files.clear();
    sources_.clear();
    auto read = options.read ? options.read : readFromDisk;
    {
        ModuleFile first;
        first.name = moduleName;
        first.prefix = prefixOf(moduleName);
        first.source = &given;
        Lexer lexer(given, diags);
        first.tokens = lexer.lexAll();
        files.push_back(std::move(first));
    }
    for (size_t f = 0; f < files.size(); ++f) {
        scanImports(files[f], diags, options.quiet);
        for (size_t k = 0; k < files[f].imports.size(); ++k) {
            const std::string name = files[f].imports[k].name;
            const Loc loc = files[f].imports[k].loc;
            int found = -1;
            for (size_t g = 0; g < files.size(); ++g)
                if (!files[g].name.empty() && files[g].name == name) found = (int)g;
            if (found < 0) {
                const std::string path = pathOf(name);
                std::error_code ec;
                if (files[0].name.empty() && (fs::path(path).lexically_normal() == fs::path(given.path).lexically_normal() || fs::equivalent(path, given.path, ec))) {
                    if (!options.quiet)
                        diags.error(loc, "the main file cannot be imported", "move what other files need from " + given.path + " into a module of its own, such as `" +
                                         (fs::path(path).parent_path() / "shared.cplus").string() + "`, and import that: `import shared;`", spec::Modules);
                    continue;
                }
                std::string text;
                if (!read(path, text)) {
                    incomplete = true;
                    if (!options.quiet)
                        diags.error(loc, "there is no module '" + name + "': the file " + path + " is not there",
                                    "write the file " + path + ", or check the name. Imports are found from the folder of the main file, whichever file "
                                    "they are in: `import net.server;` is net/server.cplus there",
                                    spec::Modules);
                    continue;
                }
                sources_.emplace_back(path, std::move(text));
                diags.addFile(sources_.back());
                ModuleFile m;
                m.name = name;
                m.prefix = prefixOf(name);
                m.source = &sources_.back();
                Lexer lexer(sources_.back(), diags);
                m.tokens = lexer.lexAll();
                found = (int)files.size();
                files.push_back(std::move(m));
            }
            files[f].imports[k].file = found;
        }
    }
    std::vector<std::vector<size_t>> declaring(files.size());
    for (size_t f = 0; f < files.size(); ++f) scanMembers(files[f], declaring[f]);
    for (size_t f = 0; f < files.size(); ++f)
        if (!files[f].name.empty()) qualify(files[f], declaring[f]);
    if (!options.quiet)
        for (ModuleFile& f : files) checkNames(f, diags);
    for (ModuleFile& f : files) closeBlocks(f, diags);
}

std::vector<Token> Modules::tokens() const {
    std::vector<Token> out;
    for (const ModuleFile& f : files)
        if (!f.tokens.empty()) out.insert(out.end(), f.tokens.begin(), f.tokens.end() - 1);
    out.push_back(files.empty() || files.back().tokens.empty() ? Token{} : files.back().tokens.back());
    return out;
}

}  // namespace cplus
