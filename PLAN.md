# C+ — Project Plan

A compiled, general-purpose language for consumer software and servers. It looks and
feels like C/C++, keeps the abstractions that earn their place, and drops the bloat,
the headers, and the footguns. Built for personal use first.

**What it is not:** a C++ replacement, a strict superset of C, or a language for
embedded systems and kernels.

Status markers used below: **DECIDED** and **OPEN**.

---

## 1. Goals and non-goals

**Goals**
- Readable to anyone who knows C or C++. Deviate from C++ only where the semantics differ.
- Safer by default, not safe by force. The easy path is the safe path; the low-level path is
  always available and never requires ceremony.
- Batteries included for modern consumer and server software: JSON, HTTP, WebSockets,
  printing of any value, reflection.
- Native code via LLVM. Tiny runtime, no VM. Only the library code a program imports is linked.
- First-class documentation written so an LLM with zero training data on C+ can write
  correct code from it.

**Non-goals**
- C source compatibility.
- C++ interop beyond `extern "C"` shims.
- Embedded, kernels, or anything without an OS.

---

## 2. Decisions

### 2.1 Platforms — DECIDED
| Tier | Target | When |
|------|--------|------|
| 1 | Linux x86_64, aarch64 | v1, CI-tested |
| 2 | macOS (POSIX, nearly free) | after v1 core |
| 3 | Windows (needs a platform layer: sockets, threads, fs) | much later |
| 3 | WebAssembly, wasm32-wasi first, browser last | after async exists |

### 2.2 Compiler backend — DECIDED
LLVM IR directly. Pin one LLVM version. Benefits beyond codegen:
- AddressSanitizer, UBSan, ThreadSanitizer in debug builds for free.
- Built-in coroutine lowering for the async model.
- DWARF debug info, so gdb and lldb work on day one.
- Cross-compilation to every tier above from one machine.

### 2.3 Compiler implementation language — DECIDED
C++20, built with clang. Self-hosting in C+ is a later milestone, not a requirement.

### 2.4 C interop — DECIDED
- First (project Phase 1): manual `extern` declarations for C functions and structs.
- Later (project Phase 6): import C headers directly via libclang, Zig translate-c style.
- C+ is not a superset of C. No compatibility mode.

### 2.5 C++ interop — DECIDED
None. C++ libraries are consumed through hand-written `extern "C"` shims. No tooling
is built for this.

### 2.6 Errors — DECIDED
Error values, not exceptions.
- A function declares that it can fail. `try` propagates. The compiler inserts cleanup
  (destructor calls, refcount drops) on the propagation path.
- No stack unwinding, so it lowers cleanly and works on wasm.
- Compiler diagnostics are a first-class feature: every error states what was expected,
  shows a fix, and cites the spec section. See §5.

### 2.7 Memory model — DECIDED ("safer, not safe")
The default path:
- Class instances are heap objects with reference counting. Freed deterministically at
  count zero, destructors run.
- Structs are values.
- Arrays and vectors are bounds-checked.
- Reading a variable before assignment is a compile error (definite assignment analysis).
- Plain values and class references are never null. Optionals, written `T?`, express a
  missing value. Raw pointers `T*` may be null.
- Integer overflow traps always, with an error naming file and line. Explicit wrapping
  operators exist for hashing and crypto code. A compiler flag disables checks for a hot
  loop if profiling ever justifies it.

Always available, no `unsafe` block:
- Raw pointers `T*`, address-of, pointer arithmetic, casts, `new`/`delete`, `malloc`/`free`.
- Manual `new`/`delete` emit an on-by-default warning naming the alternative. Silenceable
  per file.
- Debug builds run under ASan and UBSan, so use-after-free, double free, buffer overrun,
  and null dereference stop the program with a readable message and a stack trace.

Reference cycles:
- Swift model. Strong by default, `weak` for back references. Cycles leak, and that is
  documented.
- Debug builds print a leak report at exit: each leaked object, its type, and where it
  was allocated.

### 2.8 Ownership forms and threads — DECIDED
| Form | Count | Thread-safe | Use |
|------|-------|-------------|-----|
| class reference (default) | atomic | yes | everything, unless profiling says otherwise |
| `unique<T>` | none | n/a, single owner | zero-overhead ownership |
| `local<T>` | non-atomic | no | hot single-threaded code |

The compiler refuses to pass any value containing a `local<T>` into `spawn`. This is a
shallow check that catches the common mistake. ThreadSanitizer in debug builds catches
the rest.

### 2.9 Concurrency and async — DECIDED
Model: JavaScript-style `async`/`await` with a built-in event loop, plus real threads.
- `spawn f()` runs `f` on a thread and returns `Task<T>` with `.wait()`.
- `async` functions return a promise-like value; `await` suspends them. One event loop
  per thread, running on the main thread by default.
- Implementation: LLVM coroutine lowering for the state machine, libuv for the event
  loop and non-blocking I/O on every platform.
- Standard library convention: blocking and async versions carry distinct names, e.g.
  `http::get` blocks, `http::fetch` is async.

Order: the synchronous core and `spawn` ship first. Async is the phase immediately
after, designed against a working compiler. `async` and `await` are reserved keywords
from day one.

Why not C++'s model: `std::async` is a thread launcher with a blocking destructor, and
C++20 coroutines are a library-author toolkit with no standard loop or task type.

### 2.10 Generics — DECIDED
"Templates" in the plain sense, not C++ template metaprogramming.
- Generic functions and classes with type parameters and value parameters, e.g.
  `vector<T>`, `array<T, 16>`. Monomorphized: one copy per instantiation.
- Constraints on parameters using C++20 syntax, e.g.
  `template<Comparable T> T max(T a, T b)`. Constraints are what make errors readable:
  "int has no `<` operator" instead of a template expansion dump.
- `constexpr` functions the compiler can evaluate, and `if constexpr` for branching on
  types. These replace specialization and SFINAE for compile-time work.
- Explicitly left out: template specialization syntax, SFINAE, variadic template
  metaprogramming.
- If more power is ever needed, copy Zig: types as compile-time values passed to ordinary
  functions.

### 2.11 Classes — DECIDED
- Classes with virtual dispatch via vtables, interfaces, destructors.
- Multiple inheritance with C++ default semantics: a diamond holds two copies of the
  shared base, and ambiguous members must be qualified, `d.B::speak()`.
- No virtual inheritance until a real case demands it.
- Printing any object works out of the box via reflection; an optional `toString` overrides it.
  Printing shows every field, private ones included.
- A class reference is never empty. `User u;` declares a variable that must be assigned
  before it is read, not a default-constructed object. Empty references are `User?`.
- `const` on a class variable is shallow: the variable cannot refer to another object, but
  the object can change, as with `const` in JavaScript.
- One constructor per type until function overloading arrives.
- `protected:` acts as `private:` until inheritance arrives.

### 2.12 Strings — DECIDED
UTF-8 value type with copy-on-write. Slicing is bounds-checked. `==` compares contents.

### 2.13 Modules — DECIDED
- No header files. One file is one module. Imports are dotted names mapped to paths:
  `import net.server;` loads `net/server.cplus` from the project root.
- `.hplus` is an interface file the compiler generates from a module, never written by
  hand. It summarizes a library's public API and lets a library ship compiled without its
  source. Built together with multi-file modules.
- A single `cplus build` command compiles a directory. No package manager in v1.

### 2.14 Reflection — DECIDED
Compile-time generated type descriptors: field names, offsets, types. Emitted only for
types that are printed, serialized, or explicitly reflected, so dead stripping keeps the
"only what you import" promise. Printing and JSON are built on it.

### 2.15 Standard library, v1 — DECIDED
- `vector`, `array`, `hashmap`, `string`, optionals.
- `print` for any value.
- `json` parse and serialize via reflection.
- `http` client and WebSockets, wrapping libcurl, statically linked. This also proves
  the C FFI early.
- Threads and `spawn`.
- Later, after async exists: `fs`, `net` on libuv.

### 2.16 Syntax — DECIDED
Rule: where C+ and C++ mean the same thing, the syntax is identical.
- `struct` is a value type, copied on assignment. `class` is a reference type: objects are
  refcounted and shared on assignment. This is the largest difference from C++.
- Class objects are created without `new`: `auto u = User("Alice");`. `new` only produces a
  raw `T*` and triggers the manual-memory warning.
- Class members are accessed with `.`. `->` is only for raw pointers.
- Generics use C++20 syntax: `template<typename T>`, `template<Comparable T>`.
- Errors: a fallible function is marked `throws`. `throw` raises, `try { } catch (E e) { }`
  handles. Every fallible call is inside a `try` block or prefixed with `try`, which passes
  the error to the caller. Implemented as error values, not exceptions.
- Optionals: `T?`, empty value `null`, unwrap with `if (auto x = opt)`, chain with `?.`,
  default with `??`.
- Namespaces and module members use `::`, e.g. `http::get`. Built-ins such as `vector`,
  `string`, and `println` need no prefix.
- Numbers convert implicitly to `bool`, as in C++: zero is false, anything else is true.
  This covers conditions and `bool b = 1;`. `bool` does not convert to a number.
- Integer literals: decimal, `0x` hex, `0o` octal, `0b` binary, `'` separators. A leading
  zero (`010`) is an error, because C silently reads it as octal. The `L` suffix makes a
  literal a `long`; `LL` means the same.
- Several variables per declaration are allowed, as in C++: `int a = 1, b = 2;`. Once
  pointers exist, a declaration that mixes pointers and non-pointers (`int* a, b;`) is an
  error.
- `int` is 32-bit and `long` is 64-bit on every platform. There is no `long long` type.
- `==` and `!=` work on every struct without writing anything: fields are compared in
  order. On class references they compare identity, true only for the same object, like
  `===` on objects in JavaScript and `==` on `shared_ptr` in C++.
- Operator overloading uses C++ syntax, `Point operator+(Point other) const`, and arrives
  after function overloading. `&&`, `||` and the comma operator cannot be overloaded.
- Printing follows C++23: `println("x = {}", x)`. The format string must be a literal and
  the compiler checks that placeholders match arguments.
- Source files use `.cplus`.
- LLVM is pinned to version 18.

---

## 3. Compiler architecture

```
source (.cplus)
  → lexer
  → parser                → AST
  → name resolution / modules
  → type checker          → typed AST, generic instantiation, constraint checking
  → semantic passes       → definite assignment, ownership forms, spawn/local check
  → lowering              → refcount insertion, destructor insertion, error propagation,
                            reflection descriptors
  → LLVM IR               → coroutine lowering (later), sanitizers in debug
  → object file → link    → runtime + stdlib + libcurl (+ libuv later)
```

Runtime, in C, intentionally small: refcount helpers, allocation, trap handlers, leak
report, string and container primitives, platform layer.

Open implementation questions are listed in §6.

---

## 4. Phases

Status: Phase 0 has a first draft (`docs/SPEC.md`, `docs/CHEATSHEET.md`). Phase 1 is in
progress. The compiler builds programs with integers, bools, string literals, structs, and
reference-counted classes with methods, constructors and destructors, using functions,
control flow, definite assignment (down to struct fields), overflow traps, and `println`,
all through LLVM. Destructors and a leak check (`CPLUS_CHECK_LEAKS=1`) arrived with classes,
ahead of Phase 2. Next in Phase 1: `vector`, string operations, optionals, and `extern` C
calls.

**Phase 0 — Spec v0.1.** Grammar, type system, memory model, ownership forms, error
model, module system. Written as the document models will read. Includes the one-page
cheat sheet from day one.

**Phase 1 — Core compiler.** Lexer, parser, type checker, LLVM backend. Structs,
functions, control flow, refcounted classes without inheritance, arrays, vectors,
strings, optionals, definite assignment, overflow traps, `print`. Manual `extern` for C.
Debug builds with ASan/UBSan. Exit criterion: a non-trivial program that calls a C library.

**Phase 2 — Abstractions.** Classes with inheritance and interfaces, multiple
inheritance, generics with constraints, `constexpr`, error values and
`try`, `unique` and `local`, `weak`, leak report.

**Phase 3 — Batteries.** Reflection descriptors, `json`, `http` and WebSockets via
libcurl, `hashmap`, `spawn` and `Task`.

**Phase 4 — Diagnostics and docs pass.** Rewrite every diagnostic to the standard in §5.
Finish the spec. Examples corpus from the test suite. `cplus explain`.

**Phase 5 — Async.** `async`/`await`, event loop on libuv, LLVM coroutine lowering,
async standard library surface, `http::fetch`.

**Phase 6 — Tooling.** Formatter, LSP, C header import via libclang.

**Phase 7 — Platforms.** macOS CI. wasm32-wasi. Windows platform layer. Browser wasm.

---

## 5. LLM-first documentation

This is a deliverable, not an afterthought. A model has zero training data on C+ and
will confidently write C++ wherever the two differ.

- **Cheat sheet.** One page, fits in a prompt, lists every place C+ differs from C++.
  Versioned with the compiler, lives in the repo, referenced from CLAUDE.md.
- **Spec.** Short, example-heavy, sectioned so an error can cite it.
- **Diagnostics as the teaching channel.** Every compiler error: what was expected, a
  concrete fix, the spec section. Models iterate on compiler output, so this is where
  most of the learning happens.
- **`cplus explain <topic>`.** Dumps the relevant spec section so agents can query the
  language instead of guessing. Later exposed through the LSP.
- **Examples corpus.** The test suite doubles as few-shot material; every feature has a
  small, idiomatic, runnable example.
- **Syntax discipline.** Where C+ and C++ mean the same thing, the syntax is identical.
  Divergence is reserved for places where the semantics differ, so prior knowledge
  transfers and the cheat sheet stays one page.

---

## 6. Open items

- Remaining syntax: interfaces, casts, `unique<T>` and `local<T>` construction, `async`
  functions, lambdas, enums, `switch`. Settled in Phase 0 as each feature is reached.
- Function overloading is planned, as in C++, after the v1 core.
- Event loop and threads: one loop per thread versus a work-stealing scheduler. Decided
  in Phase 5 with real workloads.
- Package manager, after v1.
- Virtual inheritance, if ever.
