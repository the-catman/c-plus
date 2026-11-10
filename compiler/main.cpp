// The `cplus` command: check, build, or run a C+ program.
#include <pthread.h>
#include <spawn.h>
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
#include "Explain.h"
#include "Lexer.h"
#include "Parser.h"
#include "Sema.h"

extern char** environ;

namespace fs = std::filesystem;
using namespace cplus;

static const char* kUsage =
    "usage:\n"
    "  cplus check <file.cplus>                      report errors without building\n"
    "  cplus build <file.cplus> [-o <output>] [-O]   compile to a native executable\n"
    "  cplus build <file.cplus> --emit-llvm [-O]     print the LLVM IR instead\n"
    "  cplus run <file.cplus> [-O] [-- <args>...]    build to a temporary file and run it\n"
    "  cplus explain [<topic>]                       what the language says on a topic, with an example\n"
    "  cplus --version\n"
    "\n"
    "build and run also take -l<library> and -L<directory>, as a C compiler does, for\n"
    "the libraries of C functions declared with extern \"C\", e.g. -lsqlite3.\n"
    "-Wno-new-delete turns off the warning for new and delete in every file.\n"
    "--debug makes a debug build: slower, but memory errors stop the program with a stack\n"
    "trace, and leaks are reported at exit. Build and run take it, with or without -O.\n"
    "--race makes a build that stops the program, with a report, when two threads use the same\n"
    "memory at the same time and one of them is changing it. It cannot go with --debug.\n";

struct Options {
    std::string command;
    std::string input;
    std::string output;
    bool optimize = false;
    bool debug = false;
    bool race = false;
    bool emitLLVM = false;
    bool warnNewDelete = true;  // -Wno-new-delete turns the warning off
    std::vector<std::string> linkArgs;  // -l and -L, passed on to the linker
    std::vector<std::string> programArgs;
};

// An error about the command or its surroundings, and not the program, said as the others are.
static void toolError(const std::string& message, const std::string& help) {
    std::fprintf(stderr, "error: %s\n  = help: %s\n  = spec: docs/SPEC.md#the-cplus-command, or run `cplus explain the-cplus-command`\n", message.c_str(), help.c_str());
}

static int usageError(const std::string& message) {
    toolError(message, "write the command as one of those below");
    std::fprintf(stderr, "\n%s", kUsage);
    return 2;
}

// Runs a program and waits for it. Returns its exit code, or 128 + signal number. If it cannot be started, this says so, with
// `help` as what to do about it, and returns -1.
static int runProcess(const std::vector<std::string>& argv, const std::string& help) {
    std::vector<char*> args;
    for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    std::fflush(nullptr);
    pid_t pid;
    if (int error = posix_spawnp(&pid, args[0], nullptr, nullptr, args.data(), environ)) {
        toolError(std::string("could not run ") + args[0] + ": " + std::strerror(error), help);
        return -1;
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

// Parses and checks a file. Returns false if it has errors.
static bool frontEnd(const SourceFile& file, Diagnostics& diags, Program& program, bool warnNewDelete) {
    Lexer lexer(file, diags);
    Parser parser(lexer.lexAll(), diags);
    parser.parseProgram(program);
    if (!parser.gaveUp()) {  // otherwise what was read is too little to check
        Sema sema(diags, parser, warnNewDelete);
        sema.check(program);
    }
    if (diags.hasErrors()) {
        int n = diags.errorCount();
        std::fprintf(stderr, "error: could not compile %s because of %d error%s\n", file.path.c_str(), n,
                     n == 1 ? "" : "s");
        return false;
    }
    return true;
}

static bool readFile(const std::string& path, std::string& out) {
    std::error_code isFolder;
    if (fs::is_directory(path, isFolder)) return false;  // it opens, and reads as nothing
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
        toolError("cannot read " + opts.input + ": " + std::strerror(errno), "check the name of the file, which is relative to the folder this runs in");  // a folder is caught before
        return 1;
    }
    SourceFile file(opts.input, std::move(text));
    Diagnostics diags(file);
    Program program;
    if (!frontEnd(file, diags, program, opts.warnNewDelete)) return 1;
    if (opts.command == "check") return 0;

    // The tests can only reach these errors by pretending, on a machine that has what they are about.
    const bool lacksSanitizers = std::getenv("CPLUS_NO_SANITIZERS_FOR_TESTS") != nullptr;
    const std::string runtimeLib = (opts.race || opts.debug) && lacksSanitizers ? "" : opts.race ? CPLUS_RUNTIME_LIB_RACE : opts.debug ? CPLUS_RUNTIME_LIB_DEBUG : CPLUS_RUNTIME_LIB;
    if (runtimeLib.empty() && !opts.emitLLVM && opts.race) {
        toolError("--race needs ThreadSanitizer, which the C compiler did not have when this compiler was built",
                  "install the sanitizer libraries for your C compiler, such as `libclang-rt-18-dev` on Debian or Ubuntu, then run cmake and build again");
        return 1;
    }
    if (runtimeLib.empty() && !opts.emitLLVM) {
        toolError("--debug needs AddressSanitizer, which the C compiler did not have when this compiler was built",
                  "install the sanitizer libraries for your C compiler, such as `libclang-rt-18-dev` on Debian or Ubuntu, then run cmake and build again");
        return 1;
    }

    Codegen codegen(file, CodegenOptions{opts.optimize, opts.debug, opts.race});
    std::string error;
    if (!codegen.generate(program, error)) {
        toolError(error, error.find("pass a pointer to the struct") != std::string::npos
                             ? "declare the C function to take a pointer to the struct, `T* p`, and pass `&x`, or build the program on x86-64 or aarch64, where structs pass by value"
                         : error.rfind("internal compiler error", 0) == 0
                             ? "this is a mistake in the compiler, and not in the program. Keep the program that caused it to report it, and change the code near what was last edited to go on"
                             : "this is about the machine the program is being built for, and not the program. Build it on Linux, on x86-64 or aarch64, which are what this compiler has been written for");
        return 1;
    }
    if (opts.emitLLVM) {
        std::fputs(codegen.llvmIR().c_str(), stdout);
        return 0;
    }
    std::string objPath = exePath + ".o";
    if (!codegen.writeObjectFile(objPath, error)) {
        toolError(error, error.rfind("this target", 0) == 0 ? "this is about the machine the program is being built for, and not the program. Build it on Linux, on x86-64 or aarch64, which are what this compiler has been written for"
                         : opts.command == "run" ? "set the environment variable TMPDIR to a folder that can be written to"
                                                 : "check that the folder of the output exists and can be written to, or choose another with -o");
        std::error_code ignored;
        fs::remove(objPath, ignored);  // what there is of it is of no use
        return 1;
    }
    const char* linkerForTests = std::getenv("CPLUS_LINKER_FOR_TESTS");
    std::vector<std::string> link{linkerForTests ? linkerForTests : CPLUS_LINKER, objPath, runtimeLib};
    if (opts.debug) link.push_back("-fsanitize=address");
    if (opts.race) {
        link.push_back("-fsanitize=thread");
#ifndef __APPLE__
        link.insert(link.end(), {"-Wl,--wrap=__tsan_func_entry", "-Wl,--wrap=__tsan_func_exit"});  // runtime/race_stack.c
#endif
    }
    link.insert(link.end(), opts.linkArgs.begin(), opts.linkArgs.end());
#ifdef CPLUS_CURL_LINK
    for (auto& import : program.imports)
        if (import.path == "http") link.insert(link.end(), {CPLUS_CURL_LINK, "-lpthread"});
#endif
#ifdef CPLUS_UV_LINK
    if (program.usesAsync) link.insert(link.end(), {CPLUS_UV_LINK, "-lpthread"});
#endif
    link.push_back("-lm");  // the math functions, part of the C standard library
    if (program.linksThreads) link.push_back("-lpthread");
    link.insert(link.end(), {"-o", exePath});
    int rc = runProcess(link, "this is the C compiler that links programs, named when cplus was built. Install it, or run cmake and build cplus again");
    std::error_code ec;
    fs::remove(objPath, ec);
    if (rc < 0) return 1;
    if (rc != 0) {
        bool callsC = false;
        for (auto& f : program.funcs) callsC = callsC || f->isExtern;
        toolError("linking failed (the linker exited with code " + std::to_string(rc) + ")",
                  callsC ? "if a C function is undefined, name its library with -l, e.g. `cplus " + opts.command + " " + opts.input +
                               " -lsqlite3`, and its directory with -L if it is not installed in the usual place"
                         : "what the linker printed above says why. Check the -l and -L options, and that the folder of the output can be written to");
        return 1;
    }
    return 0;
}

static int compilerMain(int argc, char** argv) {
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
    if (opts.command == "explain") {
        if (argc > 3) return usageError("explain takes one topic");
        return explainTopic(argc == 3 ? argv[2] : "");
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
        } else if (a == "--debug") {
            opts.debug = true;
        } else if (a == "--race") {
            opts.race = true;
        } else if (a == "-g" || a.rfind("-fsanitize", 0) == 0) {
            return usageError("'" + a + "' is not an option of cplus; write --debug, which turns on line numbers and "
                              "the memory checks together");
        } else if (a == "-Wno-new-delete") {
            opts.warnNewDelete = false;
        } else if (a.rfind("-l", 0) == 0 || a.rfind("-L", 0) == 0) {
            // -lm or -l m; -Ldir or -L dir. `check` accepts them too, so one command line works for all.
            std::string value = a.substr(2);
            if (value.empty()) {
                if (++i == argc) return usageError(a + " needs a " + (a == "-l" ? "library name" : "directory"));
                value = argv[i];
            }
            opts.linkArgs.push_back(a.substr(0, 2) + value);
        } else if (a == "--emit-llvm") {
            if (opts.command != "build") return usageError("--emit-llvm is only used with `cplus build`");
            opts.emitLLVM = true;
        } else if (!a.empty() && a[0] == '-') {
            if (a.rfind("-O", 0) == 0) return usageError("'" + a + "' is not an option of cplus; write -O, which is the one level of optimization");
            return usageError("unknown option '" + a + "'");
        } else if (opts.input.empty()) {
            opts.input = a;
        } else {
            if (opts.command == "run") return usageError("to pass arguments to the program, put them after `--`: `cplus run " + opts.input + " -- " + a + "`");
            return usageError("only one source file can be given; multi-file programs are not supported yet");
        }
    }
    if (opts.debug && opts.race)
        return usageError("--debug and --race cannot be used together, as each watches the program's memory in its own way; build once with each");
    if (opts.input.empty()) return usageError("no source file given");
    if (std::error_code isFolder; fs::is_directory(opts.input, isFolder)) {
        toolError("cannot read " + opts.input + ": it is a folder", "give the path of a .cplus file, such as `" + opts.input + "/main.cplus`");
        return 2;
    }
    if (fs::path(opts.input).extension() != ".cplus") {
        toolError(opts.input + " is not a C+ source file", "give it a name that ends in .cplus, as C+ source files do");
        return 2;
    }

    if (opts.command != "run") {
        std::string out = opts.output.empty() ? fs::path(opts.input).stem().string() : opts.output;
        return build(opts, out);
    }

    std::error_code tempError;
    fs::path tempDir = fs::temp_directory_path(tempError);
    if (tempError) {
        toolError(std::string("cannot find a folder for temporary files: ") + tempError.message().c_str(), "set the environment variable TMPDIR to a folder that exists");
        return 1;
    }
    std::string tmpl = (tempDir / "cplus-run-XXXXXX").string();
    if (!mkdtemp(tmpl.data())) {
        toolError("cannot make a folder for temporary files in " + tempDir.string() + ": " + std::strerror(errno), "set the environment variable TMPDIR to a folder that can be written to");
        return 1;
    }
    fs::path exe = fs::path(tmpl) / fs::path(opts.input).stem();
    int rc = build(opts, exe.string());
    if (rc == 0) {
        std::vector<std::string> argvRun{exe.string()};
        argvRun.insert(argvRun.end(), opts.programArgs.begin(), opts.programArgs.end());
        rc = runProcess(argvRun, "it is built in the folder for temporary files, which may not allow programs to run. Set the environment variable TMPDIR to a folder that does");
        if (rc < 0) rc = 1;
    }
    std::error_code ec;
    fs::remove_all(tmpl, ec);
    return rc;
}

// The compiler works through nested code by recursion, and the parser allows code nested
// far deeper than the usual 8 MiB of stack could work through, so it runs on a thread with
// more. The memory is only used as far as it is reached.
struct MainArgs {
    int argc;
    char** argv;
    int result;
};

static void* runCompiler(void* data) {
    auto* args = static_cast<MainArgs*>(data);
    try {
        args->result = compilerMain(args->argc, args->argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "internal compiler error: %s\n", e.what());
        args->result = 70;
    }
    return nullptr;
}

int main(int argc, char** argv) {
    MainArgs args{argc, argv, 0};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 512 * 1024 * 1024);
    pthread_t thread;
    if (pthread_create(&thread, &attr, runCompiler, &args) != 0) {
        runCompiler(&args);  // no room for that much stack: try on this one
        return args.result;
    }
    pthread_join(thread, nullptr);
    return args.result;
}
