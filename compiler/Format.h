#pragma once

#include <string>

#include "Diagnostics.h"

namespace cplus {

// `cplus fmt`: C+ in its one style. See "Formatting" in docs/SPEC.md.
enum class FormatResult {
    Formatted,    // `out` has the file in its style, which may be what it was already
    HasErrors,    // it does not parse, and the errors are in `diags`: it is left as it is
    WouldChange,  // what it would write has other tokens or comments than the file, which is a bug in the formatter
};

// The modules that the file imports are read for the names in them, from its project root, which is found going up no
// further than `top`: see Modules::placeOf. What is wrong in them is not said, and one that is not there stops nothing.
FormatResult formatSource(const SourceFile& file, Diagnostics& diags, std::string& out, const std::string& top = "");

}  // namespace cplus
