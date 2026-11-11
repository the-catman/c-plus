#include "Explain.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace cplus {

namespace {

struct Section {
    int level;           // how many #s
    std::string title;
    std::string anchor;  // as a Markdown link to it, and the compiler's errors, name it
    size_t start, end;   // of its own text, up to the next heading of any level
};

// What people ask for, by the words they already know, when the section goes by another name.
struct Alias {
    const char* word;
    const char* anchor;
};
constexpr Alias kAliases[] = {
    {"await", "async-functions"},    {"async", "async-functions"},   {"auto", "variables"},
    {"break", "control-flow"},       {"c", "c-functions"},           {"cast", "casts"},
    {"catch", "errors"},             {"chars", "char"},              {"class", "classes"},
    {"concept", "constraints"},      {"const", "variables"},         {"continue", "control-flow"},
    {"delete", "new-and-delete"},    {"double", "floating-point-types"}, {"dynamic_cast", "inheritance"},
    {"error", "errors"},             {"extern", "c-functions"},      {"fetch", "http"},
    {"float", "floating-point-types"}, {"for", "control-flow"},      {"foreach", "range-based-for-loops"},
    {"format", "printing"},          {"function", "functions"},      {"get", "http"},
    {"hash", "hash-maps"},           {"hashmap", "hash-maps"},       {"if", "control-flow"},
    {"import", "modules"},           {"int", "integer-types"},       {"interface", "interfaces"},
    {"local", "local-references"},   {"lock", "mutex"},              {"lockguard", "mutex"},
    {"long", "integer-types"},       {"main", "the-main-function"},  {"map", "hash-maps"},
    {"method", "methods"},           {"move", "unique-references"},  {"new", "new-and-delete"},
    {"null", "optionals"},           {"optional", "optionals"},      {"override", "inheritance"},
    {"parse", "json"},               {"pointer", "pointers"},        {"print", "printing"},
    {"println", "printing"},         {"race", "the-race-check"},     {"reflect", "reflection"},
    {"refcount", "memory-model"},    {"release", "memory-model"},    {"spawn", "concurrency"},
    {"static_cast", "casts"},        {"stringify", "json"},          {"struct", "structs"},
    {"task", "concurrency"},         {"template", "generics"},       {"thread", "concurrency"},
    {"threads", "concurrency"},      {"throw", "errors"},            {"throws", "errors"},
    {"try", "errors"},               {"unique", "unique-references"}, {"vector", "vectors"},
    {"virtual", "inheritance"},      {"wait", "concurrency"},        {"weak", "weak-references"},
    {"websocket", "http"},           {"websockets", "http"},         {"while", "control-flow"},
    {"array", "arrays"},             {"delay", "async-functions"},   {"sleep", "async-functions"},
    {"fmt", "formatting"},           {"formatter", "formatting"},    {"style", "formatting"},
    {"lsp", "the-language-server"},  {"editor", "the-language-server"}, {"vscode", "the-language-server"},
    {"include", "c-headers"},        {"header", "c-headers"},        {"headers", "c-headers"},
    {"libclang", "c-headers"},       {"macro", "c-headers"},         {"define", "c-headers"},
    {"fs", "files"},                 {"file", "files"},              {"read", "files"},
    {"readline", "reading-input"},   {"input", "reading-input"},     {"stdin", "reading-input"},
    {"getline", "reading-input"},    {"cin", "reading-input"},       {"stoi", "parsing-numbers"},
    {"stol", "parsing-numbers"},     {"stod", "parsing-numbers"},    {"stof", "parsing-numbers"},
    {"atoi", "parsing-numbers"},     {"number", "parsing-numbers"},
    {"split", "splitting-and-joining"}, {"trim", "splitting-and-joining"}, {"join", "splitting-and-joining"},
    {"replace_all", "splitting-and-joining"},
};

std::string slug(std::string_view heading) {
    std::string out;
    for (char c : heading) {
        unsigned char u = (unsigned char)c;
        if (std::isalnum(u) || c == '_' || c == '-') out += (char)std::tolower(u);
        else if (c == ' ') out += '-';
    }
    return out;
}

std::vector<Section> readSections(std::string_view text) {
    std::vector<Section> out;
    bool inCode = false;
    size_t at = 0;
    while (at < text.size()) {
        size_t lineEnd = text.find('\n', at);
        if (lineEnd == std::string_view::npos) lineEnd = text.size();
        std::string_view line = text.substr(at, lineEnd - at);
        if (line.substr(0, 3) == "```") inCode = !inCode;
        if (!inCode && !line.empty() && line[0] == '#') {
            size_t level = line.find_first_not_of('#');
            if (level != std::string_view::npos && line[level] == ' ') {
                if (!out.empty()) out.back().end = at;
                std::string title(line.substr(level + 1));
                out.push_back({(int)level, title, slug(title), at, text.size()});
            }
        }
        at = lineEnd + 1;
    }
    return out;
}

std::string normalized(std::string_view topic) {
    std::string out;
    for (char c : topic) {
        unsigned char u = (unsigned char)c;
        if (std::isalnum(u) || c == '_') out += (char)std::tolower(u);
        else if (!out.empty() && out.back() != '-') out += '-';
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out;
}

const char* exampleFor(const std::string& anchor) {
    for (const EmbeddedExample* e = kEmbeddedExamples; e->name; ++e)
        if (anchor == e->name) return e->text;
    return nullptr;
}

// The sections just under this one.
std::vector<size_t> childrenOf(const std::vector<Section>& all, size_t index) {
    std::vector<size_t> out;
    for (size_t i = index + 1; i < all.size() && all[i].level > all[index].level; ++i)
        if (all[i].level == all[index].level + 1) out.push_back(i);
    return out;
}

void show(std::string_view spec, const std::vector<Section>& all, size_t index) {
    const Section& s = all[index];
    std::string_view text = spec.substr(s.start, s.end - s.start);
    while (!text.empty() && (text.back() == '\n' || text.back() == '-')) text.remove_suffix(1);  // the rule that ends a part of the spec
    std::printf("%.*s\n", (int)text.size(), text.data());
    if (const char* example = exampleFor(s.anchor))
        std::printf("\nAn example that runs, examples/features/%s.cplus:\n\n%s", s.anchor.c_str(), example);
    std::vector<size_t> children = childrenOf(all, index);
    if (!children.empty()) {
        std::printf("\nMore on this: ");
        for (size_t i = 0; i < children.size(); ++i) std::printf("%s`cplus explain %s`", i ? ", " : "", all[children[i]].anchor.c_str());
        std::printf("\n");
    }
}

void listTopics(const std::vector<Section>& all) {
    std::printf("usage: cplus explain <topic>\n\nWhat the language says on a topic, with an example that runs. Any of these, or a word such as\n"
                "`hashmap`, `spawn` or `try`. `cplus explain cheatsheet` is how C+ differs from C++, on one page.\n");
    for (const Section& s : all) {
        if (s.level == 1) continue;
        if (s.level == 2) std::printf("\n%s:\n", s.title.c_str());
        std::printf("  %s\n", s.anchor.c_str());
    }
}

// How far apart two words are, for suggesting the one that was meant.
size_t distance(std::string_view a, std::string_view b) {
    std::vector<size_t> row(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) row[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        size_t diagonal = row[0];
        row[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            size_t above = row[j];
            row[j] = std::min({above + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
            diagonal = above;
        }
    }
    return row[b.size()];
}

}  // namespace

int explainTopic(const std::string& topic) {
    std::string_view spec = kEmbeddedSpec;
    std::vector<Section> all = readSections(spec);
    auto findAnchor = [&](std::string_view anchor) -> long {
        for (size_t i = 0; i < all.size(); ++i)
            if (all[i].anchor == anchor) return (long)i;
        return -1;
    };
    if (topic.empty()) {
        listTopics(all);
        return 0;
    }
    if (topic == "--check") {  // for the tests: every word leads somewhere, and every example belongs to a section
        int problems = 0;
        for (const Alias& alias : kAliases)
            if (findAnchor(alias.anchor) < 0) {
                std::fprintf(stderr, "the word '%s' leads to '%s', which is not a section of the spec\n", alias.word, alias.anchor);
                ++problems;
            }
        for (const EmbeddedExample* e = kEmbeddedExamples; e->name; ++e)
            if (findAnchor(e->name) < 0) {
                std::fprintf(stderr, "the example '%s' is not named after a section of the spec\n", e->name);
                ++problems;
            }
        return problems ? 1 : 0;
    }
    std::string wanted = normalized(topic);
    if (wanted == "cheatsheet" || wanted == "cheat-sheet") {
        std::fputs(kEmbeddedCheatSheet, stdout);
        return 0;
    }
    long found = findAnchor(wanted);
    if (found < 0)
        for (const Alias& alias : kAliases)
            if (wanted == alias.word) found = findAnchor(alias.anchor);
    if (found >= 0) {
        show(spec, all, (size_t)found);
        return 0;
    }
    // A part of a section's name, if it leads to just one. Otherwise, what it may have meant.
    std::vector<size_t> partial;
    for (size_t i = 0; i < all.size(); ++i)
        if (all[i].level > 1 && all[i].anchor.find(wanted) != std::string::npos) partial.push_back(i);
    if (partial.size() == 1) {
        show(spec, all, partial[0]);
        return 0;
    }
    if (partial.empty()) {
        std::vector<std::pair<size_t, size_t>> close;
        for (size_t i = 0; i < all.size(); ++i)
            if (all[i].level > 1) close.push_back({distance(wanted, all[i].anchor), i});
        std::sort(close.begin(), close.end());
        for (size_t i = 0; i < close.size() && i < 3; ++i) partial.push_back(close[i].second);
    }
    std::fprintf(stderr, "error: no section of the spec goes by '%s'\n  = help: perhaps ", topic.c_str());
    for (size_t i = 0; i < partial.size(); ++i) std::fprintf(stderr, "%s`cplus explain %s`", i ? ", " : "", all[partial[i]].anchor.c_str());
    std::fprintf(stderr, ". `cplus explain` lists them all\n");
    return 1;
}

}  // namespace cplus
