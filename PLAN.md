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
- Declarations are written as in C++, `extern "C" int abs(int n);` or `extern "C" { ... }`,
  but with C+ types: `long strlen(string s);`, `string? getenv(string name);`. C spellings
  such as `const char*`, `size_t` and `unsigned` are errors that name the C+ type to write.
  Decided 2026-11-05.
- A `string` argument is passed as a copy that ends in a zero byte, freed when the call
  returns. A zero byte inside the string stops the program. A null `string?` is NULL.
- A returned `const char*` is copied into a string; C+ never frees it. A `string` result
  that is NULL stops the program, and a `string?` result makes it null.
- Libraries are named on the command line, `-lname` and `-Ldir`, as with a C compiler.
  libc is always linked.
- `double` and `float` are passed as they are, since C has the same types. A `char` is
  passed as C's `char`, the same byte whether the platform's `char` is signed or not.
- Pointers pass unchanged (see §2.21). `char*` is a raw byte pointer, for buffers a
  function fills and text the caller must free; `const char*` stays `string`. A C array
  parameter is written as the pointer C passes, `int* values`.
- A C type used only through pointers, such as `FILE` or `sqlite3`, is declared by name:
  `extern "C" struct FILE;`, then written `FILE*`. Nothing but a pointer to it can be
  used. Decided 2026-11-05.
- A struct of C types (numbers, `char`, `bool`, pointers, and such structs) has C's
  layout, as in C++, with no special form. Inside `extern "C"` its fields are checked where it
  is defined, and any struct is checked where a C function takes or returns it, directly or
  through a pointer. Decided 2026-11-05.
- Structs by value follow the C calling convention, written out in Codegen for x86-64 and
  aarch64, the Tier 1 targets. Other targets report an error. Decided 2026-11-05.
- A C array field is written `array<char, 64> name;` (see §2.23). A C array parameter stays
  a pointer. Decided 2026-11-06.
- 16-bit integers in structs, unions, variadic functions and callbacks follow with the
  features they need.

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
- Manual `new`/`delete` emit an on-by-default warning naming the alternative. It is turned
  off with `[[allow(new_delete)]]` before a function or method, `[[allow(new_delete)]];` at
  the top of a file, or `-Wno-new-delete` for the whole build. Decided 2026-11-05: an
  `unsafe` block was rejected, because it would suggest that the code outside it is safe,
  and a `new_delete { }` block because its braces would end the scope of what it allocates.
- Debug builds run under ASan and UBSan, so use-after-free, double free, buffer overrun,
  and null dereference stop the program with a readable message and a stack trace.
  Decided 2026-11-06: a debug build is asked for with `--debug`, and `cplus build` without
  it stays a plain build, since ASan needs libraries that not every machine has and makes a
  program about twice as slow. UBSan is written into clang, not LLVM, so C+ generates what it
  needs itself: checks for misaligned pointers and invalid bools, reported as ordinary
  runtime errors. The rest of what UBSan checks either stops the program in every build or
  cannot happen in the IR C+ generates.

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
- A value type, like `std::string`, holding UTF-8 text by convention and bytes in fact:
  `size()` counts bytes and positions are byte positions, as in C++. Walking code points
  is planned.
- Representation: `{data, length, owner}`. A literal has no owner and costs nothing.
  Otherwise `owner` is a reference-counted buffer that copies share. Bytes another string
  can see never change: `+=` appends in place only when nothing else holds the buffer, and
  grows it by doubling, so building a string in a loop is linear. `substr` shares the
  buffer when the part is at least a quarter of it and copies smaller parts, so a
  substring never keeps alive more than four times its own size.
- `+` joins two strings, or a string and a `char`, as `std::string` does. `s + 5`, which
  C++ compiles as appending character 5, is an error that points to `to_string(n)` and
  `format`. `a + b + c` is joined with one allocation.
- `<` and the other orderings compare bytes, as C++ does (code point order for UTF-8).
- C++ names and meanings: `size`, `length`, `empty`, `substr`, `find`, `contains`,
  `starts_with`, `ends_with`, `clear`. Where C++ throws `out_of_range`, the program stops.
  With no unsigned types, `string::npos` is `-1`, a `long`; a `substr` count of
  `string::npos` takes the rest as in C++, and other negative counts stop the program.
- `format(...)`, C++20's `std::format`, returns what `print` would print. `to_string(n)`
  takes an `int` or `long`; `to_string(true)`, which is `"1"` in C++, is an error that
  points to `format`.
- `s[i]` is a checked `char`, for reading and writing, and `s[s.size()]` stops the
  program. Writing copies the buffer first unless nothing else holds it. `for (char c :
  s)` goes over the bytes. `push_back`, `pop_back`, `front`, `back` and `string(n, c)`
  have C++'s meanings, and stop the program where C++ is undefined. `s.at(i)` is left
  out, since `s[i]` is always checked.

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

### 2.17 Vectors — DECIDED
- `vector<T>` is a value type, copied on assignment like `std::vector`. Copies share one
  buffer of elements until one of them changes (copy-on-write), so passing and returning
  vectors is cheap without references.
- C++ names: `push_back`, `pop_back`, `size`, `empty`, `front`, `back`, `clear`, `v[i]`.
  `vector<int> v;` is an empty vector, as in C++.
- `size()` returns `long`, since there are no unsigned types.
- An index out of range, and `pop_back`, `front` or `back` on an empty vector, stop the
  program with the file and line.
- A range-based `for` loops over the vector as it was when the loop started, so changing
  the vector in the body is safe. `for (auto& x : v)` waits for references.

### 2.18 Optionals — DECIDED
- `T?` holds a `T` or `null`, for any type but `void` and `auto`. An optional cannot hold
  another optional.
- A `User?` is a reference that may be null, the size of a pointer. Any other `T?` is the
  value plus a flag, like `std::optional<T>`.
- A `T`, or anything that converts to `T`, converts to `T?`, and so does `null`. A `T?`
  never converts to `T`, and optionals of different types do not convert to each other.
- An optional is a condition, true when it holds a value, as with `std::optional`.
  `bool?` is not a condition, since `if (flag)` would be ambiguous.
- `if (auto u = opt)` and `while (auto u = opt)` unwrap, and the variable exists only in
  the branch or the body. With a value that is not optional they keep their C++ meaning.
- `?.` skips the rest of the chain when the value is null, as in JavaScript and Swift. Its
  result is optional, unless it already is or is `void`. It only reads: assigning through
  it is an error, and so is calling a method that changes a struct, string or vector,
  since `?.` works on a copy of those.
- `??` sits between `||` and `?:`, groups to the right, and evaluates the fallback only
  when it is needed.
- `has_value()`, `value()`, which traps when empty, and `value_or(x)` as in
  `std::optional`. `==` compares an optional with another of the same type, a value, or
  `null`.
- A struct cannot contain itself, even through an optional. A class can, so reference
  cycles can now be built, and they leak until `weak` arrives (§2.7). Freeing a long chain
  of objects uses constant stack.

### 2.19 Floating point and casts — DECIDED
Decided 2026-11-05.
- `double` (64-bit) and `float` (32-bit), IEEE 754, with C++'s literals (`1.5`, `1e9`,
  `2.5f`) and C++'s usual arithmetic conversions. No `long double` and no hexadecimal
  floating-point literals.
- Implicit: an integer to a floating-point type, `float` to `double`, and a `double`
  written as a number to `float` (`float f = 0.5;`). Never a floating-point value to an
  integer, and no other `double` to `float`.
- Floating-point arithmetic is not checked: division by zero gives an infinity, and NaN
  is a value. `%` and the bit operators stay integer-only; `fmod` gives the remainder.
- Casts use C++'s spellings, `int(x)`, `static_cast<int>(x)` and `(int)x`, all with the
  same meaning, between the number types and `bool`, and between pointer types (§2.21). A
  value that does not fit the new type, including NaN and the infinities converted to an
  integer, stops the program, as integer overflow does. `reinterpret_cast` and
  `const_cast` do not exist; `dynamic_cast` would come with inheritance. A cast never
  makes or reads a string, except `string(p)` from a `char*`.
- `{}` prints the shortest digits that read back exactly, as C++'s `std::format` does,
  except that NaN always prints as `nan`, so output does not change with optimization.
  `{:.2f}`, `{:e}`, `{:g}` and `{:.3}` follow `std::format`. `to_string(x)` matches `{}`,
  as in C++26.
- The math functions of `<cmath>`, plus `abs`, `min` and `max`, are built in with their
  C++ names and overload rules. A function of the program with the same name takes their
  place, so C's own can still be declared.

### 2.20 Characters — DECIDED
Decided 2026-11-05.
- `char` is one byte, unsigned, from 0 to 255: what a string holds. Unicode code points
  would be a separate type later, if at all. Literals are C++'s, `'a'` and `'\n'`; a
  literal of more than one byte (`'ab'`, `'é'`) and the prefixed forms are errors.
- In arithmetic a char is promoted to `int`, as in C++, so `c - '0'` is an `int`. A char
  converts to every number type by itself. A number never becomes a char by itself:
  `char(n)` is a checked cast. `c++`, `c--`, `c += n` and `c -= n` keep a char, as in
  C#, and stop the program outside 0 to 255.
- A char is not a condition and does not convert to `bool`: with no NUL-terminated
  strings, `if (c)` is rarely the intent. `c != '\0'` says it.
- `{}` prints the character. Inside a struct, vector or optional it prints quoted, with
  escapes, so `'\0'` and `' '` stay visible. `to_string(c)` is an error, since C++ gives
  the code.
- The `<cctype>` functions are built in, ASCII only, and take a `char`. The tests return
  `bool` and `toupper`/`tolower` a `char`, not C's `int`, since there is no `EOF` to
  return. As with `<cmath>`, a function of the program with the same name replaces them.

### 2.21 Raw pointers — DECIDED
Decided 2026-11-05.
- C++'s pointers and syntax: `T*`, `T**`, `void*`, `const T*`, `T* const`, `&x`, `*p`,
  `p->x`, `p[i]`, `p + n`, `p - q` (a `long`), `++`/`--`, and ordering by address. `null`
  is the null pointer; `0` and `nullptr` are not. A pointer cannot be optional, since it
  can already be null.
- Following a null pointer with `*`, `->` or `[ ]` stops the program with file:line.
  `p[i]` and arithmetic are not bounds-checked, and lifetimes are not tracked: a dangling
  pointer is undefined, as in C++, and is left to ASan in debug builds.
- `&x` needs a place that already has a value, since definite assignment cannot follow
  writes through pointers. `&` of a const place gives `const T*`. No address of `this`,
  temporaries, or values reached through `?.`.
- Pointers into vectors and strings are left for later: a `push_back` moves the elements,
  so they need a rule first.
- A pointer is a condition, but converts to `bool` only with `bool(p)`. Implicitly, only
  `null` → `T*`, `T*` → `const T*`, and `T*` → `void*`. Casts between any pointer types
  are allowed except removing `const`; numbers and pointers never convert.
- A pointer prints as hex. Printing a `char*` is an error, since its end is not known;
  `string(p)` copies the text up to the zero byte.
- Pointers are not counted. A pointer to a class variable points to the variable, not
  the object.
- `int* a, b;` is an error (the C++ trap); `sizeof` is C++'s and gives a `long`.

### 2.22 new and delete — DECIDED
Decided 2026-11-05.
- C++'s forms: `new T`, `new T(...)`, `new T{...}`, `new T[n]`, `delete`, `delete[]`, with
  the warning and its off switches from §2.7. No placement new and no `new auto`.
- `new T` and `new T[n]` always hold default values, since definite assignment cannot
  follow memory on the heap. This differs from C++ only where C++ leaves the value unset.
- `new` on a class is an error: an object is already on the heap and freed by counting.
- `delete` releases what the memory holds, the last element first, as in C++.
- Memory from `new` starts with a header holding the element count and a mark. The wrong
  form of delete stops the program, and deleting twice or deleting other memory almost
  always does. Use after delete is left to ASan in debug builds.
- `CPLUS_CHECK_LEAKS=1` also reports memory from `new` that was never deleted.

---

### 2.23 Arrays — DECIDED
`array<T, N>`, spelled and behaving as `std::array`. Decided 2026-11-06.
- A value that holds its elements in itself, with C's layout, so it is also how a C struct
  writes an array field. There is no C-style form, `int a[5]`, because that form turns into a
  pointer. The error for it gives the `array` to write.
- `N` is a written whole number, zero or more. Named sizes wait for constants or generics.
- `array<int, 3> a;` gives every element its default, since elements cannot be tracked one
  by one by definite assignment. C++ would leave numbers unknown. A shorter `{ }` list fills
  the rest with defaults, as in C++. With no default, as for class references, every element
  must be given.
- Every index is checked; a written index out of range is a compile error.
- No implicit conversion to a pointer. `a.data()` and `&a[i]` are allowed, since elements
  do not move.
- A range-based `for` keeps its own copy, so it means the same as on a vector, and changes
  to what holds the array cannot free it under the loop.
- `string(a)` on `array<char, N>` stops at a zero byte or the end, so it cannot over-read.
- A value can take at most 2 GiB. The stack is not limited at compile time: as in C++, a
  local array can be too large, but stack probes make that a crash and never let the
  program step over the guard page into other memory. A clear message for it is open (§6).
- Codegen handles an array, and whatever holds one by value, through its address, with
  memory copies. LLVM needed 95 seconds at -O0 to compile one load and store of
  `[65536 x i32]` as a single value, while memory copies take no time.

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
progress. The compiler builds programs with integers, bools, strings (joining, ordering,
`substr`, `find`, `format`, `to_string`, checked `s[i]`), `char` with the `<cctype>`
functions, `double` and `float` with casts and the
`<cmath>` functions, structs, reference-counted classes with methods, constructors and
destructors, vectors, and optionals (`T?`, `null`, `?.`, `??`), using
functions, control flow including range-based `for`, definite assignment (down to struct
fields), overflow and bounds traps, and `println`, all through LLVM. Destructors and a
leak check (`CPLUS_CHECK_LEAKS=1`) arrived with classes, ahead of Phase 2. C functions
can be called through `extern "C"` declarations with number, `char`, `bool`, `string`
and pointer values. Raw pointers work, with pointer arithmetic, `sizeof`, null traps and
C types known by name such as `FILE*`, `new` and `delete` with their warning, and C structs,
through pointers and by value, including array fields, and `array<T, N>`. `--debug` makes
a debug build with ASan, stack traces and a leak report. Every item of Phase 1 is in, and
`examples/books.cplus`, a reading list stored in SQLite through its C API, meets the exit
criterion.

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

- Remaining syntax: interfaces, `unique<T>` and `local<T>` construction, `async`
  functions, lambdas, enums, `switch`. Settled in Phase 0 as each feature is reached.
- Function overloading is planned, as in C++, after the v1 core.
- References (`T&`): whether C+ gets them, and in what form. They decide how
  `for (auto& x : v)` and `const T&` parameters work; today both are errors that point to a
  copy, which is cheap for vectors.
- Event loop and threads: one loop per thread versus a work-stealing scheduler. Decided
  in Phase 5 with real workloads.
- Package manager, after v1.
- Virtual inheritance, if ever.
