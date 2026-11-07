// `cplus explain <topic>`: the part of the spec on a topic, and an example that runs, so that a person or a model can ask
// the language instead of guessing. All of it is built into the compiler: see cmake/EmbedDocs.cmake.
#pragma once

#include <string>

namespace cplus {

struct EmbeddedExample {
    const char* name;  // the anchor of the spec section it shows
    const char* text;
};

extern const char* const kEmbeddedSpec;
extern const char* const kEmbeddedCheatSheet;
extern const EmbeddedExample kEmbeddedExamples[];

// What `cplus explain` does with its one argument, empty if none was given. Returns the exit code.
int explainTopic(const std::string& topic);

}  // namespace cplus
