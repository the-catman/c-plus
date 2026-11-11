# C+

C+ is a compiled language shaped like C++, with safer defaults and more built in. The
compiler is written in C++20 and generates code through LLVM 18. Phases 1 to 6 of `PLAN.md` are in,
the last being tooling: `cplus fmt`, `cplus lsp` and `import <header.h>;`. Phase 7, the core language, is under way, then servers, then platforms.

## Where things are

- `PLAN.md` holds the design decisions, the phases, and the open questions. Decisions marked
  DECIDED are settled. Don't reopen them without asking.
- `docs/SPEC.md` is the language specification. Compiler errors link to its headings.
- `docs/CHEATSHEET.md` is the one-page summary of how C+ differs from C++.
- `compiler/` is the `cplus` compiler: Lexer, Parser, Sema (type checking and definite
  assignment), Codegen (LLVM IR), `Builtins.h` (what is written in C+ itself, such as `hashmap`),
  Format (`cplus fmt`), Lsp (`cplus lsp`) and Index (what each name means, for it), CHeaders (`import <name.h>;`, through
  libclang), and `main.cpp` (the driver).
- `runtime/` is the small C runtime linked into every program.
- `tests/cases/` holds the language tests. `tests/run_tests.py` documents their directives.
- `tests/fmt/` holds the formatter's tests: what `cplus fmt` must make of each `.in.cplus` is its `.out.cplus`.
- `editors/vscode/` is the extension for VS Code, which starts `cplus lsp` and colors C+ code.
- `examples/` holds small sample programs. `examples/features/` has one for each section of the spec, named after its
  anchor: the tests run them, and `cplus explain` shows them.

## Writing C+ code

- Read `docs/CHEATSHEET.md` before writing or editing any `.cplus` file. C+ looks like C++,
  but conditions, conversions, printing, and declarations differ in ways that are easy to
  get wrong.
- Use only features the cheat sheet lists under "Works today". Everything else is rejected
  by the compiler.
- Programs in `examples/` are in the style of `cplus fmt`, which the tests check. Run
  `build/cplus fmt examples` after changing them. Tests in `tests/cases/` are left as they are written.

## Building and testing

- After changing the compiler, runtime, docs, or tests, build and run the tests yourself.
  Don't leave it for the user to run.

```
cmake -S . -B build -G Ninja      # first time only
ninja -C build
python3 tests/run_tests.py build/cplus
```

- `python3 tests/run_tests.py build/cplus <name>` runs only the tests whose file name
  contains `<name>`.
- `tests/errors_reached.py` lists the errors and the advice that no test reaches, and says how to measure that.
- For memory bugs in the compiler itself, configure a second build with
  `-DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"` into `build-asan/`, then run the same tests.

## Conventions

- **Same meaning, same syntax.** Where C+ and C++ mean the same thing, C+ uses C++ syntax.
  Syntax diverges only where the meaning differs.
- **Diagnostics are written for a reader who has never seen C+**, human or model. Every
  error states what is wrong, gives a concrete fix in `help`, and cites a spec anchor from
  `compiler/SpecRefs.h`. A planned but unimplemented feature reports "this compiler does not
  support ... yet". When a common C or C++ habit is likely, such as `printf`, `#include`,
  `std::`, or `if (n)`, the help says what to write instead.
- **Spec anchors must exist.** A new entry in `SpecRefs.h` needs a matching heading in
  `docs/SPEC.md`. The test runner checks this. A new heading needs a program in `examples/features/`.
- **One change updates everything it affects.** When language behavior changes, update
  `docs/SPEC.md`, its implementation status table, `docs/CHEATSHEET.md`, the tests, and
  `PLAN.md` if a decision changed.
- **Every feature gets tests.** Add files to `tests/cases/` for programs that work, for
  each error message, and for each runtime trap.

## Privacy

This repo is shared, so nothing in it may identify the user or their computer.

- Never put personal information into anything that ends up in the repo: files, code
  comments, test expectations, commit messages, PR text. That includes email addresses,
  usernames, absolute paths (home directory or anywhere else), host names, and other details
  of the user's machine.
- Write paths relative to the repo root, never absolute ones. Tests pass relative paths to
  the compiler, so file names in diagnostics stay relative.
- Settings for one machine, like a non-default LLVM location or compiler, go on the
  `cmake` command line or in `CMakeUserPresets.json`, which is gitignored. They never go
  in `CMakeLists.txt`.
- Before every commit and push, read through the staged diff for anything personal, and
  take it out first.

## Commits

- Keep commit messages short: one line of a few words, like "Add do-while loops".
- Don't add a "Co-Authored-By" line to commit messages.
