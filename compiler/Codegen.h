// Lowers a checked Program to LLVM IR and native object code.
#pragma once

#include <memory>
#include <string>

#include "AST.h"

namespace cplus {

struct CodegenOptions {
    bool optimize = false;  // run the LLVM -O2 pipeline
    bool debug = false;     // line numbers, AddressSanitizer and the checks only debug builds make
    bool race = false;      // line numbers and ThreadSanitizer
};

class Codegen {
public:
    Codegen(const SourceFile& file, CodegenOptions options);
    ~Codegen();

    // Returns false and fills `error` if the target machine cannot be set up.
    bool generate(const Program& program, std::string& error);
    std::string llvmIR() const;
    bool writeObjectFile(const std::string& path, std::string& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cplus
