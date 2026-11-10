#pragma once

namespace cplus {

// `cplus lsp`: a language server, which an editor starts and talks to on stdin and stdout. See "The language server" in
// docs/SPEC.md. Returns the exit code.
int runLanguageServer(const char* version);

}  // namespace cplus
