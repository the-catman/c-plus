#pragma once

#include <string>
#include <vector>

#include "AST.h"
#include "Diagnostics.h"

namespace cplus {

// A name in a checked program, for `cplus lsp`: where it is written, what it is, and where that is declared.
struct Reference {
    Loc at;
    std::string what;  // Markdown: its declaration as C+ writes it, then the comment above that
    Loc declared;      // line 0 for what comes with the language
};

// Every name in `file` that the checks found the meaning of, the first meaning only where generic code has several.
std::vector<Reference> indexProgram(const Program& program, const SourceFile& file);
// The same for a program of several files, `files` by their ids, in which the comment above a declaration is read from the
// file it is in.
std::vector<Reference> indexProgram(const Program& program, const SourceFile& file, const std::vector<const SourceFile*>& files);

}  // namespace cplus
