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

FormatResult formatSource(const SourceFile& file, Diagnostics& diags, std::string& out);

}  // namespace cplus
