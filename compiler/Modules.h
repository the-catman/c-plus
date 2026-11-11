// A program's own modules: `import net.server;` reads net/server.cplus from the project root, the folder of the file given
// to the compiler. The files that the given one reaches become one token stream, which the parser reads as one program, as
// it does what comes with the language. See "Modules" in docs/SPEC.md.
//
// Each name declared at the top of a module's file is renamed with the module's path in front, `net::server::start`, which
// is what other files write. Inside its own file it goes by its name alone: a type's name is renamed there too, as the
// parser needs to know types by their names, and for a function or a constant `resolve` says what a name means, since a
// local variable can have the same name.
#pragma once

#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "Lexer.h"

namespace cplus {

// A name declared at the top of a file, outside `extern "C"`.
struct ModuleMember {
    std::string name;  // as it is written, `start`
    Loc loc;
    bool exported = false;  // `export` is written before it
    bool isType = false;    // a struct, class, interface, enum, generic type or concept: what the parser knows by its name
    bool isFunction = false;
    bool isTemplate = false;  // a generic, which `<` can follow
};

// Whether other files can use the member. Every check of `export` goes through this.
inline bool isExported(const ModuleMember& member) { return member.exported; }

struct ModuleImport {
    std::string name;  // dotted: `net.server`
    Loc loc;           // of all of `import net.server;`
    int file = -1;     // the file it is, once read
};

struct ModuleFile {
    std::string name;    // dotted: `net.server`. Empty for the main file
    std::string prefix;  // `net::server`, which its names have in front of them
    const SourceFile* source = nullptr;
    std::vector<Token> tokens;
    std::vector<ModuleImport> imports;  // of the program's own modules, not those that come with the language
    std::map<std::string, ModuleMember> members;
    std::set<std::string> cFunctions, cTypes;  // what it declares in `extern "C"`
    bool importsHeader = false;    // it has `import <name.h>;` or `import "name.h";`
};

class Modules {
public:
    struct Options {
        std::string root;  // the project root, as given: "" for the folder this runs in
        // Gives the text of a file by its path, or false. Without it, files are read from disk.
        std::function<bool(const std::string& path, std::string& text)> read;
        bool quiet = false;  // for `cplus fmt`: what cannot be read is left out, with nothing said
    };

    // Reads `given`, then the modules it imports and those they import, each once. `moduleName` is empty when `given` is
    // the main file, and otherwise the dotted name it has as a module, for `cplus lsp` and `cplus fmt` on a module's file.
    // Errors go to `diags`, to which each file is added.
    void load(const SourceFile& given, const std::string& moduleName, Diagnostics& diags, const Options& options);
    // Where a file is in its program, for `cplus fmt` and `cplus lsp`, which are given one file: the project root, and the
    // dotted name the file has there as a module, or none for the main file. That is a file with a `main`, or main.cplus, and
    // its folder is the root. For a module, the root is the nearest folder from which all its imports are found, going up as
    // far as `top`, or else its own folder.
    struct Place {
        std::string root, moduleName;
    };
    static Place placeOf(const SourceFile& file, const std::string& top, const Options& options);
    // The whole program's tokens: the given file's, then each module's, with one end of file.
    std::vector<Token> tokens() const;

    // The one lookup of names at the top of a file: what `name`, written in `file`, means when it is no local variable or
    // member. A name of the file's own module gets the module's path; any other is left as it is, for what comes with the
    // language, C's names, and the main file's own.
    std::string resolve(const std::string& name, uint32_t file) const;

    bool isMain(uint32_t file) const { return file >= files.size() || files[file].name.empty(); }
    bool hasModules() const { return files.size() > 1 || (!files.empty() && !files[0].name.empty()); }
    const ModuleFile* byName(const std::string& dotted) const;
    // The module whose path starts `qualified`, the longest one, with what follows it in `rest`: `net::server::Config::x`
    // gives net.server and `Config::x`.
    const ModuleFile* ownerOf(const std::string& qualified, std::string* rest = nullptr) const;
    bool imports(uint32_t file, const ModuleFile& module) const;
    // A program module that a file imports whose last part is `last`, for the help on `server::start()`.
    const ModuleFile* importedEndingIn(uint32_t file, const std::string& last) const;
    // What a module's file is named, from the root: `net/server.cplus`.
    std::string pathOf(const std::string& dotted) const;

    std::vector<ModuleFile> files;  // by SourceFile::id; the given file first
    bool incomplete = false;        // a module could not be read

private:
    std::string root_;
    Options options_;
    std::deque<SourceFile> sources_;  // the modules' files, which Diagnostics and the tokens refer to
    void scanImports(ModuleFile& file, Diagnostics& diags, bool quiet);
    void scanMembers(ModuleFile& file, std::vector<size_t>& declaring);
    void qualify(ModuleFile& file, const std::vector<size_t>& declaring);
    void checkNames(ModuleFile& file, Diagnostics& diags) const;
    void closeBlocks(ModuleFile& file, Diagnostics& diags) const;
};

}  // namespace cplus
