// Diagnostics: every compiler message is meant to be read by a human or an LLM
// that has never seen C+ before. Each error says what is wrong, where, how to fix
// it (help), and which section of docs/SPEC.md explains the rule (spec).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cplus {

struct SourceFile {
    std::string path;
    std::string text;
    std::vector<size_t> lineStarts;  // byte offset where each line begins

    SourceFile(std::string path, std::string text);
    std::string_view line(uint32_t lineNo) const;  // 1-based, without the newline
};

struct Loc {
    uint32_t line = 0;  // 1-based; 0 means "no location"
    uint32_t col = 0;   // 1-based byte column
    uint32_t len = 1;   // how many columns to underline
};

class Diagnostics {
public:
    explicit Diagnostics(const SourceFile& file);

    // help: a concrete fix. spec: a heading anchor in docs/SPEC.md (see SpecRefs.h).
    void error(Loc loc, const std::string& message, const std::string& help = "",
               const std::string& spec = "");
    void warning(Loc loc, const std::string& message, const std::string& help = "",
                 const std::string& spec = "");
    // A note adds context to the diagnostic printed just before it.
    void note(Loc loc, const std::string& message);

    bool hasErrors() const { return errorCount_ > 0; }
    int errorCount() const { return errorCount_; }

private:
    void report(const char* level, const char* color, Loc loc, const std::string& message,
                const std::string& help, const std::string& spec);

    const SourceFile& file_;
    bool color_ = false;
    int errorCount_ = 0;
    bool suppressed_ = false;
};

}  // namespace cplus
