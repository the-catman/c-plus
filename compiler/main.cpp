// The `cplus` command: check, build, or run a C+ program.
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "Codegen.h"
#include "Lexer.h"
#include "Parser.h"
#include "Sema.h"

namespace fs = std::filesystem;
using namespace cplus;

static const char* kUsage =
    "usage:\n"
    "  cplus check <file.cplus>                      report errors without building\n"
    "  cplus build <file.cplus> [-o <output>] [-O]   compile to a native executable\n"
    "  cplus build <file.cplus> --emit-llvm [-O]     print the LLVM IR instead\n"
    "  cplus run <file.cplus> [-O] [-- <args>...]    build to a temporary file and run it\n"
    "  cplus --version\n";

struct Options {
    std::string command;
    std::string input;
    std::string output;
    bool optimize = false;
    bool emitLLVM = false;
    std::vector<std::string> programArgs;
};

static int usageError(const std::string& message) {
    std::fprintf(stderr, "error: %s\n\n%s", message.c_str(), kUsage);
    return 2;
}

// Runs a program and waits for it. Returns its exit code, or 128 + signal number.
static int runProcess(const std::vector<std::string>& argv) {
    std::vector<char*> args;
    for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    std::fflush(nullptr);
    pid_t pid = fork();
    if (pid < 0) {
        std::perror("fork");
        return 127;
    }
    if (pid == 0) {
        execvp(args[0], args.data());
        std::fprintf(stderr, "error: could not run %s: %s\n", args[0], std::strerror(errno));
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

// Parses and checks a file. Returns false if it has errors.
static bool frontEnd(const SourceFile& file, Diagnostics& diags, Program& program) {
    Lexer lexer(file, diags);
    Parser parser(lexer.lexAll(), diags);
    program = parser.parseProgram();
    Sema sema(diags);
    sema.check(program);
    if (diags.hasErrors()) {
        int n = diags.errorCount();
        std::fprintf(stderr, "error: could not compile %s because of %d error%s\n", file.path.c_str(), n,
                     n == 1 ? "" : "s");
        return false;
    }
    return true;
}

static bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// Compiles `opts.input` into an executable at `exePath`.
static int build(const Options& opts, const std::string& exePath) {
    std::string text;
    if (!readFile(opts.input, text)) {
        std::fprintf(stderr, "error: cannot read %s: %s\n", opts.input.c_str(), std::strerror(errno));
        return 1;
    }
    SourceFile file(opts.input, std::move(text));
    Diagnostics diags(file);
    Program program;
    if (!frontEnd(file, diags, program)) return 1;
    if (opts.command == "check") return 0;

    Codegen codegen(file, CodegenOptions{opts.optimize});
    std::string error;
    if (!codegen.generate(program, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    if (opts.emitLLVM) {
        std::fputs(codegen.llvmIR().c_str(), stdout);
        return 0;
    }
    std::string objPath = exePath + ".o";
    if (!codegen.writeObjectFile(objPath, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    int rc = runProcess({CPLUS_LINKER, objPath, CPLUS_RUNTIME_LIB, "-o", exePath});
    std::error_code ec;
    fs::remove(objPath, ec);
    if (rc != 0) {
        std::fprintf(stderr, "error: linking failed (the linker exited with code %d)\n", rc);
        return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) return usageError("no command given");
    Options opts;
    opts.command = argv[1];
    if (opts.command == "--version") {
        std::printf("cplus %s (LLVM %s)\n", CPLUS_VERSION, CPLUS_LLVM_VERSION);
        return 0;
    }
    if (opts.command == "--help" || opts.command == "-h" || opts.command == "help") {
        std::fputs(kUsage, stdout);
        return 0;
    }
    if (opts.command != "build" && opts.command != "run" && opts.command != "check")
        return usageError("unknown command '" + opts.command + "'");

    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--") {
            if (opts.command != "run") return usageError("'--' is only used with `cplus run`");
            for (++i; i < argc; ++i) opts.programArgs.push_back(argv[i]);
            break;
        } else if (a == "-o") {
            if (opts.command != "build") return usageError("-o is only used with `cplus build`");
            if (++i == argc) return usageError("-o needs an output path");
            opts.output = argv[i];
        } else if (a == "-O") {
            opts.optimize = true;
        } else if (a == "--emit-llvm") {
            if (opts.command != "build") return usageError("--emit-llvm is only used with `cplus build`");
            opts.emitLLVM = true;
        } else if (!a.empty() && a[0] == '-') {
            return usageError("unknown option '" + a + "'");
        } else if (opts.input.empty()) {
            opts.input = a;
        } else {
            return usageError("only one source file can be given; multi-file programs are not supported yet");
        }
    }
    if (opts.input.empty()) return usageError("no source file given");
    if (fs::path(opts.input).extension() != ".cplus") {
        std::fprintf(stderr, "error: %s is not a C+ source file\n  = help: C+ source files end in .cplus\n",
                     opts.input.c_str());
        return 2;
    }

    if (opts.command != "run") {
        std::string out = opts.output.empty() ? fs::path(opts.input).stem().string() : opts.output;
        return build(opts, out);
    }

    std::string tmpl = (fs::temp_directory_path() / "cplus-run-XXXXXX").string();
    if (!mkdtemp(tmpl.data())) {
        std::perror("mkdtemp");
        return 1;
    }
    fs::path exe = fs::path(tmpl) / fs::path(opts.input).stem();
    int rc = build(opts, exe.string());
    if (rc == 0) {
        std::vector<std::string> argvRun{exe.string()};
        argvRun.insert(argvRun.end(), opts.programArgs.begin(), opts.programArgs.end());
        rc = runProcess(argvRun);
    }
    std::error_code ec;
    fs::remove_all(tmpl, ec);
    return rc;
}
