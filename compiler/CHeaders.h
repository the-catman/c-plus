#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "Diagnostics.h"

namespace cplus {

// `import <sqlite3.h>;` or `import "mine.h";`, which declares what the program uses from a C header. See "C headers" in
// docs/SPEC.md.
struct HeaderImport {
    std::string name;     // sqlite3.h
    bool angled = true;   // written in angle brackets, rather than in quotes
    Loc loc;              // of the name
    std::string shown() const { return angled ? "<" + name + ">" : "\"" + name + "\""; }
};

// Where headers are looked for, besides the system's folders.
struct HeaderSearch {
    std::string sourceDir;                 // the program's folder, where a header in quotes is looked for first
    std::vector<std::string> includeDirs;  // from -I, in order
};

// What the program has, which decides what is taken from the headers.
struct NamesInProgram {
    std::set<std::string> used;                // the names it uses, which a header may declare
    std::set<std::string> declared;            // its own functions and constants, which are used instead of a header's
    std::map<std::string, bool> types;         // its own types, and whether each is declared in `extern "C"`
};

// Why a name that a header declares cannot be used from C+, said where the program uses it.
struct NotImported {
    std::string message, help;
};

struct HeaderError {
    Loc loc;
    std::string message, help;
};

struct HeaderTranslation {
    std::string source;                               // C+ declarations of what the program uses, in an `extern "C"` block
    std::map<std::string, NotImported> notImported;  // what it uses that C+ cannot take, and why
    std::vector<HeaderError> errors;                  // a header that cannot be found or read
};

// Reads the headers with libclang, and declares in C+ the functions, structs and constants of theirs that the program uses.
// Only those, and the types they need, so a big header costs little, and its names cannot clash with the program's.
HeaderTranslation translateHeaders(const std::vector<HeaderImport>& headers, const HeaderSearch& search, const NamesInProgram& names);

}  // namespace cplus
