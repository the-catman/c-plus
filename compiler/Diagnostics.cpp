#include "Diagnostics.h"

#include <unistd.h>

#include <algorithm>
#include <cstdio>

namespace cplus {

SourceFile::SourceFile(std::string p, std::string t) : path(std::move(p)), text(std::move(t)) {
    lineStarts.push_back(0);
    for (size_t i = 0; i < text.size(); ++i)
        if (text[i] == '\n') lineStarts.push_back(i + 1);
}

std::string_view SourceFile::line(uint32_t lineNo) const {
    if (lineNo == 0 || lineNo > lineStarts.size()) return {};
    size_t start = lineStarts[lineNo - 1];
    size_t end = lineNo < lineStarts.size() ? lineStarts[lineNo] - 1 : text.size();
    if (end > start && text[end - 1] == '\r') --end;
    return std::string_view(text).substr(start, end - start);
}

Diagnostics::Diagnostics(const SourceFile& file) : file_(file), files_{&file} { color_ = isatty(STDERR_FILENO); }

void Diagnostics::addFile(SourceFile& file) {
    file.id = (uint32_t)files_.size();
    files_.push_back(&file);
}

static constexpr int kMaxErrors = 50;

void Diagnostics::error(Loc loc, const std::string& message, const std::string& help,
                        const std::string& spec) {
    inOtherFile_ = onlyGiven_ && loc.file != 0;
    if (muted_ || inOtherFile_) {
        ++mutedErrors_;
        return;
    }
    ++errorCount_;
    if (errorCount_ > kMaxErrors) {
        if (!suppressed_ && !collected_) std::fprintf(stderr, "error: too many errors, stopping here\n");
        suppressed_ = true;
        return;
    }
    report("error", "\x1b[1;31m", loc, message, help.empty() ? "the part of the spec named below explains the rule" : help, spec);  // the tests fail on this one
    if (!contexts_.empty()) report("note", "\x1b[1;36m", contexts_.back().loc, contexts_.back().message, "", "");
}

void Diagnostics::warning(Loc loc, const std::string& message, const std::string& help,
                          const std::string& spec) {
    inOtherFile_ = onlyGiven_ && loc.file != 0;
    if (suppressed_ || muted_ || inOtherFile_) return;
    report("warning", "\x1b[1;33m", loc, message, help, spec);
}

void Diagnostics::note(Loc loc, const std::string& message) {
    if (suppressed_ || muted_ || inOtherFile_) return;
    report("note", "\x1b[1;36m", loc, message, "", "");
}

void Diagnostics::report(const char* level, const char* color, Loc loc, const std::string& message,
                         const std::string& help, const std::string& spec) {
    if (collected_) {
        collected_->push_back({level, loc, message, help, spec});
        return;
    }
    const char* bold = color_ ? "\x1b[1m" : "";
    const char* reset = color_ ? "\x1b[0m" : "";
    const char* blue = color_ ? "\x1b[1;34m" : "";
    std::fprintf(stderr, "%s%s:%s %s%s%s\n", color_ ? color : "", level, reset, bold, message.c_str(), reset);

    std::string gutter = loc.line ? std::string(std::to_string(loc.line).size(), ' ') : "";
    if (loc.line) {
        const SourceFile& file = fileOf(loc);
        std::fprintf(stderr, "%s%s -->%s %s:%u:%u\n", gutter.c_str(), blue, reset, file.path.c_str(),
                     loc.line, loc.col);
        std::string_view src = file.line(loc.line);
        std::fprintf(stderr, "%s%s |%s\n", gutter.c_str(), blue, reset);
        std::fprintf(stderr, "%s%u |%s %.*s\n", blue, loc.line, reset, (int)src.size(), src.data());
        // Copy tabs from the source so the caret lines up however tabs render.
        std::string marker;
        for (uint32_t i = 0; i + 1 < loc.col && i < src.size(); ++i) marker += src[i] == '\t' ? '\t' : ' ';
        uint32_t avail = loc.col <= src.size() ? (uint32_t)src.size() - loc.col + 1 : 1;
        uint32_t len = std::max<uint32_t>(1, std::min(loc.len, avail));
        marker += '^';
        marker += std::string(len - 1, '~');
        std::fprintf(stderr, "%s%s |%s %s%s%s\n", gutter.c_str(), blue, reset, color_ ? color : "",
                     marker.c_str(), reset);
    }
    if (!help.empty()) std::fprintf(stderr, "%s%s =%s %shelp:%s %s\n", gutter.c_str(), blue, reset, bold, reset, help.c_str());
    if (!spec.empty())
        std::fprintf(stderr, "%s%s =%s %sspec:%s docs/SPEC.md#%s, or run `cplus explain %s`\n", gutter.c_str(), blue, reset, bold, reset,
                     spec.c_str(), spec.c_str());
    std::fprintf(stderr, "\n");
}

}  // namespace cplus
