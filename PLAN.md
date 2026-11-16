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
- AddressSanitizer, UBSan in debug builds for free, and ThreadSanitizer in a build of its own.
- Built-in coroutine lowering for the async model.
- DWARF debug info, so gdb and lldb work on day one.
- Cross-compilation to every tier above from one machine.

### 2.3 Compiler implementation language — DECIDED
C++20, built with clang. Self-hosting in C+ is a later milestone, not a requirement.

### 2.4 C interop — DECIDED
- First (project Phase 1): manual `extern` declarations for C functions and structs.
- Later (project Phase 6): import C headers directly via libclang, Zig translate-c style. Done as
  `import <sqlite3.h>;` and `import "mine.h";`, C++20's header units, with `-I` for other folders. See §2.30.
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
- 16-bit integers in structs, unions and variadic functions follow with the features they
  need. Callbacks are §2.36.

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
- What is thrown is an object of the built-in class `Error`, or of a class deriving from it. Decided
  2026-11-06. A thrown value is then one pointer, and `catch` chooses by class with what `dynamic_cast` uses.
  `throw 42` is an error. A function does not say which errors it throws, as in Swift before typed throws.
- A function marked `throws` takes one more argument, the address of where to put an error, and returns;
  the caller looks there after the call. So a call that does not fail costs one load and a branch. Decided
  2026-11-06.
- `try f()` applies to one call and its arguments, not to all that follows, as Swift's does, so that each call that can
  leave is marked. An error goes to the nearest try block, prefix or not. Nothing that can fail goes unmarked, and the
  compiler checks that every error has somewhere to go.
- Constructors cannot fail yet: releasing a half-built object and its base classes needs its own design. Until
  then a function that can fail makes the object.

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
  operators for hashing and crypto code are planned. A compiler flag could disable checks for a
  hot loop, if profiling ever justifies it.
  - Reversed by the owner on 2026-11-11: every integer, signed and unsigned, wraps on overflow, as C# does, so that
    hash and crypto code works as written and one rule covers both. `checked(a * b)` stops the program on overflow
    instead, as does all of a function marked `[[checked]]`, or a file that starts with `[[checked]];`, where
    `unchecked(...)` wraps again. It is lexical: it covers the operators written inside it, not the functions they call.
    - Wrapping: `+ - *`, unary `-`, `++ --` and compound assignment, on every integer type and `char`; `MIN / -1` is
      `MIN` and `abs(MIN)` is `MIN`. `MIN % -1` is 0. Division by zero always stops.
    - A shift count out of range is masked to the width, as in JavaScript and C#, so `1 << 32` on an `int` is 1. Bits
      shifted out fall off. In `checked`, a count out of range stops. A written count out of range is a warning, and in
      `checked` an error.
    - A narrowing cast, `int(someLong)`, keeps the low bits, as in C++20. A float out of an integer's range becomes the
      nearest end, and NaN becomes 0. In `checked`, both stop. `Color(n)` with no enumerator still stops.
    - Constants and `constexpr` work out what the running program would, so they wrap too. In `checked`, an overflow in a
      `constexpr` is a compile error, and in a `const` it stops the program where the constant is used, as `1 / 0` does.
      `--debug` changes nothing.
    - Built on 2026-11-11. Each expression knows whether it is checked, set where the parser reads it, so generics,
      `if constexpr` branches and default arguments keep what is written around them. `checked` and `unchecked` cannot
      name a function. One cost: infinite recursion such as `f(n + 1) + 1` can become an endless loop under `-O`, where
      checked arithmetic kept the calls, as it does in C++.
    - Unsigned integers: signed and unsigned never mix without a cast, and literals adapt. `char`, `uint8_t`, `uint16_t`
      and `uint32_t` widen by themselves to a wider unsigned type, and `uint32_t` and smaller to `long`. `size()` stays
      `long`. A C `unsigned` from a header becomes the unsigned type.
      - Built on 2026-11-12, with these choices made while building it: a `uint8_t` and a `uint16_t` are worked out in
        an `int`, as a `char` and C++ do, and `b += n` wraps back into the type, or in `checked` stops when it does not
        fit. `u` gives a `uint32_t`, or a `uint64_t` when larger, and `ul` a `uint64_t`; a hexadecimal, octal or binary
        literal above the largest `long` is a `uint64_t`, and a decimal one is an error that says to write `u`. `-5u`
        is the minus of `5u`, which wraps. `abs` of a `uint32_t` or `uint64_t` is an error. From C, `size_t` and
        `uintptr_t` stay `long`, `unsigned char` stays `char`, and a `uint8_t` behind a pointer is a `char*`; `uid_t`
        is a `uint32_t`. A `uint8_t` prints as a number. JSON writes and reads them, checking the range; a
        `json::Value` holds a `uint64_t` above the largest `long` only as a fraction. Not yet: `int8_t`, `int16_t`,
        `short`, enums held in an unsigned type, `stoul`.

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
- It is written `weak<Node>` and used as C++'s `std::weak_ptr`: `lock()` gives a `Node?`, with
  `expired()` and `reset()`. Decided 2026-11-06, over Swift's `weak var parent: Node?`, since a
  type also works in a vector, and nothing reads the object behind the code's back. It prints as
  `weak<Node>` or `null`, not as the object, which is often the one printing it.
- Both counts share the object's 64-bit count word, so objects do not grow: 32 bits strong, 30 weak,
  one flag for "going away", and the sign for a part that is not at the start (§2.11). At zero strong
  references the destructors run and the fields go. The object's memory stays until the weak references
  go too, as with `std::make_shared`. A side table, as in Swift, would free it sooner, at the cost
  of a slot in every object or a table lookup in every release.
- Debug builds print a leak report at exit: how many objects leaked, and for the first ten its type and where it
  was allocated.

### 2.8 Ownership forms and threads — DECIDED
| Form | Count | Thread-safe | Use |
|------|-------|-------------|-----|
| class reference (default) | atomic | yes | everything, unless profiling says otherwise |
| `unique<T>` | none | n/a, single owner | zero-overhead ownership |
| `local<T>` | non-atomic | no | hot single-threaded code |

The compiler refuses to pass any value containing a `local<T>` into `spawn`. This is a
shallow check that catches the common mistake. ThreadSanitizer catches the rest, in a build
with `--race`, since it cannot go with AddressSanitizer in `--debug`.

`local<T>` decided 2026-11-07:
- It is written `local<Node>`, built with `local<Node>(args)`, used with `.`, and copied like a class reference. It is never empty, and `local<Node>?` can be.
- It only converts to and from another `local`, as a plain `Node` could reach a thread while this one counts without care. `x.get()` lends the object when a function needs a `Node`.
- The check of `spawn` follows the types of arguments, of what a method is called on, and of results, through fields, class fields included. What a function does with `this` or a lent reference is left to `--race`.
- Its count is the same word as any object's, so a class is compiled once. Only the common case is written without atomic steps. The last reference, and the part of an object that does not start it, go to the runtime.

`unique<T>` decided 2026-11-06:
- It is written `unique<Node>`, built with `unique<Node>(args)`, used with `.`, and handed over with `move(a)`.
  `b = a` is an error. Unlike C++, it is never empty, and using a moved variable is a compile error: `move` feeds
  definite assignment, through branches, loops, `&&` and try blocks. A moved field or element cannot be followed
  while compiling, so it is left empty, and using it stops the program. `unique<Node>?` is the one that can be empty.
- An object has the same layout however it is held, so a class is compiled once. So "no count" in the table above means no
  count work on a move or a read of a field through the variable, not that the object has none. A method call or a
  value read for a statement still counts, which keeps the object alive if the code replaces its owner, and `u.get()`
  lends a counted reference. The object can then outlive its owner, but it can never dangle. Rust checks borrows
  instead, which C+ does not do.
- `move` works on every type, so generic code can write `move(x)` for any `T`.

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

`spawn` decided 2026-11-07:
- `Task<T>` when the function cannot fail and `Task<T, throws>` when it can, so `wait()` needs `try` only then, as for any call. Having `wait()` always throw would be simpler, but a function not marked `throws` could then not use a task without pretending it can fail.
- Letting go of a task neither stops its thread nor waits for it. The program waits for all of its threads when `main` is done, as Node and Python do. Go and Rust end them instead, which silently loses work that was started. A thread that never ends keeps the program running.
- An error that no `wait()` can see any more fails the program, like one that gets out of `main`, as Node does with a promise nothing handles. It must not go unnoticed.
- `spawn` takes what is written in C+. A built-in, a C function or a constructor needs a function around it, until lambdas.
- Every thread has 8 MiB of stack and its own crash report. Threads are not pooled: each `spawn` is one.
- `Mutex` and `LockGuard` are in, beyond what was first planned, since a class reference could otherwise be shared but never safely changed. Atomic values, condition variables and channels can wait.
- ThreadSanitizer is `--race`, as it cannot go with AddressSanitizer in `--debug`. It has a runtime of its own, as it must see the runtime's atomic steps and locks. The tests build every program with `spawn` this way too.
- ThreadSanitizer's own stack holds 64K calls and is not checked, and going past it writes over what the loader uses, so a crash report can no longer be trusted. So `--race` stops a program at 60000 calls, with a message, by putting a count in the way of what it calls for each function (`runtime/race_stack.c`). A smaller thread stack would have done it too, but only for threads, and at a cost to a program with large frames that never gets near the limit.
- A fatal error is reported by one thread. Any other that stops as well waits for the first to end the program, so there is one message. The report does not wait long for a thread stuck in the output.
- A `const T&` parameter of what `spawn` runs is given the thread's own copy, made before it starts, as `std::thread` does, and
  one of an `async` function a copy made at the call (changed 2026-11-16, after the exit check's first round, where both were
  errors; §2.31). A `T&` stays an error.

`spawn` with a lambda decided 2026-11-14 by the owner:
- `spawn` still takes a call: `spawn [=] { return work(n); }()`, as Go writes `go func() { ... }()`, and `spawn job()` for a
  function value. A lambda with no call after it is an error that says to add `()`. The call is also how a thread is handed a
  `unique`, as a parameter of the lambda. The form without `()` can still come later, and could not be taken away.
- A lambda written at the `spawn`, or as an argument of the spawned call, cannot share a variable with `[&]`, as two threads
  would change it at once, and a string or a vector that two threads change can break. It copies, and gives back what it
  works out through its task. A `const` variable is copied anyway. A class's `this` is shared, as `spawn this.method()` shares
  it, and a struct's follows the rule of kept lambdas. Its captures cannot hold a `local<T>`, as arguments cannot.
- A function value can be spawned, though its type does not show what its lambda shares. Closing that would take a second
  function type, such as Swift's `@Sendable`, which §2.34 decided against; as with `local<T>`, the check catches the common
  mistake, and `--race` the rest.
- A built-in, a C function or a constructor still needs C+ code around it, which is now a lambda:
  `spawn [=] { println("{}", n); }()`. What the lambda's body works out runs on the thread, unlike an argument of the call.
- The task's type comes from the function's: `[=]() throws { ... }` gives a `Task<T, throws>`, as the lambda is marked.
- An object used after it was freed, in a program with threads, is most likely a race, so the report says to run with `--race`.

`async` decided 2026-11-10:
- An async function is written `async int f() throws`, with `async` in front as in JavaScript. Calling it gives a `Task<int, throws>`, the same type `spawn` gives, so `await t` and `t.wait()` work on any task, whatever runs it. A second, promise-like type would mean nearly the same thing.
- A call runs the function until it waits for something that is not done yet, and then gives the task, as in JavaScript. What it throws goes in the task, as with `spawn`, so the call takes no `try`, while waiting for a task that can fail does: `try await t`.
- `await` is only allowed in an async function, and `main` can be one. A plain function waits with `t.wait()`, which runs the thread's event loop until the task is done. `wait()` inside an async function is an error, since it would hold up everything else on the loop. It is written `await t` there.
- Throwing away the task of an async call is an error, since a missing `await` is the most common mistake with async in JavaScript. To let it run on, keep the task in a variable. A `spawn` can still be let go of, as the word itself says that much.
- After an `await`, a function may go on on another thread. The first version keeps it on the thread it started on, with one libuv loop per thread, but programs must not count on that, so that a scheduler that shares the work between threads can come later without breaking any. So a class object that two async functions change needs a `Mutex`, as with threads, and neither a `local` reference nor a `LockGuard` can be kept across an `await`. Keeping a function on one thread can be added later, as something a program asks for. The other way round, a scheduler that shares work would have broken programs that counted on staying on one thread.
- The program does not end while anything is pending: when `main` returns, the main thread runs its loop until it has nothing left, and a thread does the same when its function returns, before it counts as finished. This is what Node does, and what `spawn` already does.
- `delay(ms)` is the timer. It is async, so a plain function waits with `delay(ms).wait()`. It is not named `sleep`, since C's `sleep` counts seconds.
- Not in Phase 5: stopping a task, waiting with a time limit, async lambdas.

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
- `constexpr` is worked out by a small interpreter over the syntax tree, separate from code generation, which
  Sema and the parser share, since an array's size is part of a type, known before Sema runs. Decided 2026-11-06. It keeps
  the running program's rules, and its answers are tested against the program's. A plain `const` with a constant value can
  size an array, as in C++. Only numbers, `char`, `bool` and strings can be values for now.
- In a generic, `if constexpr` reads a branch only once it is known to run, so a branch not taken makes no new types
  and reports nothing. Constraints are also `bool` values when written with their type, `Number<T>`, with C++20's
  `same_as` and `derived_from`.
- A generic is kept as its tokens and read again for each set of arguments, with its parameters
  standing for them, which gives an ordinary function or type. Decided 2026-11-06. So type checking and
  code generation never see a type that is not known, and need no second set of rules. A definition is read
  once to report syntax errors. As in C++, the rest is checked when used, and a generic class's
  methods only when they are used. Checking a body before it is used, as Rust does, would need
  every constraint to say everything the body does.
- A constraint names a built-in (`Comparable`, `Number`, `Integer`, `Floating`, `Hashable`), an interface or class the
  type must derive from, or a C++20 `concept X = requires(T a) { ... };`, which is tried on the type. Decided
  2026-11-06.
- A generic type's arguments are always written for now: `Stack<int>(1)`. Working them out, as C++17 does,
  is planned.

### 2.11 Classes — DECIDED
- Classes with virtual dispatch via vtables, interfaces, destructors.
- Multiple inheritance with C++ default semantics: a diamond holds two copies of the
  shared base, and ambiguous members must be qualified, `d.B::speak()`.
- No virtual inheritance until a real case demands it.
- Printing any object works out of the box. An optional `toString` that overrides it is planned; for now a method of that
  name is an ordinary method. Printing shows every field, private ones included, except that what holds something of the
  runtime's, such as a `Mutex` or a `WebSocket`, prints as its name: its fields would show addresses, or a secret in a URL.
- A class reference is never empty. `User u;` declares a variable that must be assigned
  before it is read, not a default-constructed object. Empty references are `User?`.
- `const` on a class variable is shallow: the variable cannot refer to another object, but
  the object can change, as with `const` in JavaScript.
- A type can have several constructors, which overload as functions do (§2.33).
- Syntax is C++'s: `class Dog : public Animal`, `virtual`, `override`, `= 0`, `Animal::f()`,
  `dynamic_cast`. Decided 2026-11-06, where C++ allows something easy to get wrong, C+ makes it an
  error, so any program it accepts means what C++ would mean: `public` must be written, `override` is required,
  and a member cannot hide an inherited one. A destructor always runs for what the object really
  is, so it needs no `virtual`.
- `dynamic_cast<Dog?>(a)` gives null and `dynamic_cast<Dog>(a)` stops the program. These are C++'s
  `Dog*` and `Dog&` forms, for a language where a class variable is already a reference.
- Every object starts with its count and a vtable pointer, 16 bytes, also when its class has no
  virtual methods, since it can be released through a base class. Swift's objects have the same.
- A reference to a base class other than the first points into the object, as in C++, so fields and
  methods cost nothing extra. That part's count slot holds how far back the count is, as a
  negative number, so counting finds the object even while its constructors run. Only a program
  with multiple inheritance pays the read this takes.
- `interface Speaker { string speak() const; };` is a class with only methods that have no body,
  so it needs no new rules. Decided 2026-11-06. It holds its 16 bytes as a part of each object, as
  any base class does.
- A struct cannot have a base class or virtual methods, since copying one into a variable of its
  base type would lose part of it.

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
  As positions are `long`s, `string::npos` is `-1`; a `substr` count of
  `string::npos` takes the rest as in C++, and other negative counts stop the program.
- `format(...)`, C++20's `std::format`, returns what `print` would print. `to_string(n)`
  takes a number; `to_string(true)`, which is `"1"` in C++, is an error that
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
- Built 2026-11-11, except `.hplus` files. Decided then: a name at the top of a module is private to its file unless
  `export` is written before it. Other files name it with the module's whole path, `net::server::start()`, and its own file
  by its name alone. The root is the folder of the main file, which cannot be imported. Imports are not transitive, and
  cycles are allowed. A program cannot add names to another module, its own or one that comes with the language:
  `struct json::Mine` is an error. `export { }`, `export import` and aliases of modules wait.

### 2.14 Reflection — DECIDED
Compile-time generated type descriptors: field names, offsets, types. Emitted only for
programs that import `json` or `reflect`, and for debug builds, whose leak report says what an object is, so the
"only what you import" promise holds. JSON is built on it, and printing could be (§6).

### 2.15 Standard library, v1 — DECIDED
- `vector`, `array`, `hashmap`, `string`, optionals.
- `print` for any value. Added 2026-11-16, after the exit check's first round, where a program had to declare C's `write` for
  it: `println(stderr, ...)` and `print(stderr, ...)`, C++23's spelling, `stderr` and `stdout` being names only there. One
  call's text is one write, after what standard output has waiting. Printing to an `fs::File` waits.
- `json` parse and serialize via reflection.
- `http` client and WebSockets, wrapping libcurl. This also proves the C FFI early. Decided 2026-11-07: linked as a shared
  library for now, and programs that stand alone come in Phase 9. The `libcurl.a` of a distribution still needs a dozen other
  shared libraries, some with no static form, so linking it would not let a program run on any more machines. That takes a
  libcurl built for it, with few features and its TLS library linked in too. One setting, `CPLUS_CURL_LIBRARY`, chooses which is used.
- Threads and `spawn`.
- Later, after async exists: `net` on libuv, and the async forms of `fs`.
- Decided 2026-11-11: `fs::read` and the rest of `fs` that waits are written on C's stdio, so a small program needs
  neither async nor libuv. The async forms, such as `fs::readFile`, take Node's names and use libuv, and the same tests
  of errors run against both, so that `e.kind` cannot drift. `fs` uses POSIX calls too, so Windows (Phase 9) needs a
  layer for it either way. `readLine()` is built in, like `println`, and `stoi`, `stol`, `stod` and `stof` throw on
  text that is not wholly a number, where C++'s take what they can and leave the rest.

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
- Module members use `::`, e.g. `http::get`. There are no namespaces. Built-ins such as `vector`,
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
- Type aliases, decided 2026-11-11: `using Name = type;` at the top of a file, generic ones with
  `template<...> using`, and `export using`. An alias is the same type, not a new one. `typedef` stays
  out, with the `using` line as its help, and so does `using namespace`. Aliases inside a function or a
  type wait for types inside them.
- Operator overloading uses C++ syntax, `Point operator+(Point other) const` (built 2026-11-14, see §2.38). `&&`, `||`
  and the comma operator cannot be overloaded. A class's own `==` replaces identity for it, and `same_object(a, b)` asks that (§2.38).
- Printing follows C++23: `println("x = {}", x)`. The format string must be a literal and
  the compiler checks that placeholders match arguments, and that their options suit them (§2.37).
- Source files use `.cplus`.
- LLVM is pinned to version 18.

### 2.17 Vectors — DECIDED
- `vector<T>` is a value type, copied on assignment like `std::vector`. Copies share one
  buffer of elements until one of them changes (copy-on-write), so passing and returning
  vectors is cheap without references.
- C++ names: `push_back`, `pop_back`, `size`, `empty`, `front`, `back`, `clear`, `v[i]`.
  `vector<int> v;` is an empty vector, as in C++.
- `size()` returns `long`, as every size and count does, unsigned types being for bits.
- An index out of range, and `pop_back`, `front` or `back` on an empty vector, stop the
  program with the file and line.
- `front()` and `back()` of a vector, an array or a string that is a place are the element itself, as C++'s references are
  (2026-11-14, §2.31): `v.back().x = 1` and `v.back().set()` change it, and a reference bound to one is found again by
  its index, worked out where it is bound. Of a temporary they are a copy. Until then they gave copies, and changing
  what they gave was an error (decided 2026-11-07).
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
  cycles can be built, and they leak unless one reference is `weak` (§2.7). Freeing a long chain
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
  smaller integer type keeps the low bits, and a float out of range gives the nearest integer, NaN 0 (§2.7). In checked
  code, a value that does not fit the new type stops the program, as integer overflow does. `reinterpret_cast` is the one cast
  between a pointer and a number (§2.21). `const_cast` does not exist; `dynamic_cast` is in
  §2.11. A cast never makes or reads a string, except `string(p)` from a `char*` and
  `string(a)` from an `array<char, N>`.
- `{}` prints the shortest digits that read back exactly, as C++'s `std::format` does,
  except that NaN prints as if its sign bit were clear, as `nan`, so output does not change with optimization.
  `{:.2f}`, `{:e}`, `{:g}`, `{:.3}` and the other options follow `std::format` (§2.37). `to_string(x)` matches `{}`,
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
  `char(n)` is a cast, which keeps the low byte. `c++`, `c--`, `c += n` and `c -= n` keep a char, as in
  C#, and wrap at 256, or stop the program outside 0 to 255 in checked code (§2.7).
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
  are allowed except removing `const`. A number and a pointer convert only with
  `reinterpret_cast`, as in C++. Decided 2026-11-06, replacing "numbers and pointers never
  convert": C libraries use addresses as signals, such as `SQLITE_TRANSIENT`, `MAP_FAILED` and
  `SIG_IGN`, and C+ had no honest way to write them. `(int*)0x1000` and `int* p = 0;` stay errors.
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
- `N` is a whole number, zero or more, known while compiling: a literal, a constant, or a call to a
  `constexpr` function.
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
  local array can be too large, but stack probes make that a crash that says the program ran out of stack, and never let the
  program step over the guard page into other memory.
- Codegen handles an array, and whatever holds one by value, through its address, with
  memory copies. LLVM needed 95 seconds at -O0 to compile one load and store of
  `[65536 x i32]` as a single value, while memory copies take no time.

### 2.24 Hash maps — DECIDED
`hashmap<K, V>`, used like `std::unordered_map`. Decided 2026-11-06.
- A value, copied like a vector, and as cheaply.
- It goes over its entries in the order they were added, as a `Map` does in JavaScript and a `dict` in Python. C++
  leaves the order open, which makes output change between runs and programs depend on it by accident.
- `m[key]` adds a missing key with a default value, as in C++, and so cannot be used on a const map. If the type has
  no default value, where C++ gives a compile error, a missing key stops the program, since `users[id].name` is
  how a map of class references is used. `m[key] = value` always works, with any type.
- No iterators yet. `m.contains(k)` and `m.get(k)`, which gives a `V?`, take the place of `m.find(k)`.
- Any type with `==` can be a key, if that `==` looks at what can be hashed, with no hash to write. A type with its own
  `operator==`, or holding one, cannot be a key until it can have its own hash, as one made from its fields could tell
  apart two values that its `==` calls equal (§2.38).
- `m.at(k)` of a map that is a place is the value itself (2026-11-14, §2.31), and never adds the key: a missing one
  stops the program. `m.get(k)` gives a copy, so changing what it gives is an error, and `m[k]` or `m.at(k)` is the place.
  So are `?:` when both of what it chooses from are what C++ would refer to (then C++ would change the one it chose), and `=`.
  `??`, `value_or()`, `values()`, `items()`, and `?:` with only one such, give copies in C++ too, so they are errors only when nothing
  receives the result. The same goes for `?.`: on a place it is an error, and on what a function gave only when the result is unused.
  What a function gave changes nothing the program has, so `(f ? make(1) : make(2)).with(5)` and `list().back().with(5)` are
  allowed, and so is a method under `spawn`, which runs on a copy. Through a pointer or a class
  object the change is real, so `ps.back()[0].set()` and `ps.back()->set()` stay allowed. Decided 2026-11-07.
- A help text gives code only for a shape it can be sure of: a place that means the same each time it is written, with a key that is a literal or a name. Anything else, such as `v.back().items.back()` or `m.get(keyOf())`, gets a sentence and no code, since code that compiles and changes only a copy is worse than none. `tests/advice_grid.py` follows the code it gives, and checks that what was meant to change did.
- Strings hash with SipHash-1-3 and a seed chosen at each start, as HTTP brings input from anyone. Since the order does not
  come from the hash, the seed cannot change what a program does.
- It is written in C+, on top of `vector`, in `compiler/Builtins.h`, and only added to programs that use it. Copies,
  releases, leak checks and `unique` then work as for any struct. The compiler adds what C+ cannot say yet: `m[key]` as a
  place, `for`, `{ }` lists, printing, `==` and hashing.

### 2.25 JSON — DECIDED
`import json;`, with `json::stringify(x)`, `json::pretty(x)` and `json::parse<T>(text)`, by the descriptors of 2.14. Decided 2026-11-06.
- The JavaScript names, as what they do is what JavaScript's do: a key that is not a field is left out, the last of two with one
  name counts, and NaN is written as `null`.
- `parse` can fail, as text comes from outside, so it throws a `json::Error`, with the line, column and path. `stringify` does not, since
  every value can be written, and a circle of objects is a mistake in the program, which stops it.
- A missing key keeps the field's default. A field with no default that the type cannot do without is an error, instead of
  the zero values Go would give it, since the program could not have read such a field either.
- Only public fields. A class's private fields are what it does not show, which may be a password. This differs from printing, which
  is for the person writing the program.
- A class reference is written as what the object really is, as when printing, and read as the class that is asked for.
- `json::Value` is for what has no known shape, and each step of looking through it gives an optional, so there is no way to stop
  the program with text from outside. `json::of(x)` makes one directly, by the rules the writer has, so it costs no text in between.

### 2.26 HTTP — DECIDED
`import http;`, with `http::get`, `post`, `put`, `patch`, `delete` and `send`, and WebSockets with `http::connect`, on libcurl, as in 2.15. Decided 2026-11-06.
- A status such as 404 is an answer and not an error, as in `fetch`, so `r.ok` tells. Only what stopped there being an answer throws, an `http::Error` with a `kind`.
- There is one function for each name, until overloading: `http::get(url)`, and `http::send(request)` for everything else. `post` must be told the type of what it sends, instead of guessing.
- `timeout` starts at 30 seconds and `maxBytes` at 1 GiB, each set by request. Other languages have no such limits by default, and programs hang forever or run out of memory because of what one server does.
- Certificates are always checked, and there is no way to turn it off: `caFile` trusts a server of one's own.
- Redirects are followed by the runtime, as `fetch` follows them, rather than by libcurl. libcurl would send a PUT again after a 303.
- WebSockets speak the protocol themselves, on the connection libcurl makes. `curl_ws_*` is left out of many builds of libcurl, such as those of Debian and Ubuntu.
- `http::fetch(url)` and `http::fetchWith(request)` are the async requests, decided 2026-11-10, as `connect` and `connectWith` are for WebSockets. A single `fetch(request)` would make the commonest request the longest to write, and an async twin of each function would double the module. They run on libcurl's multi interface, on the thread's loop, and fail as `send` does.

### 2.27 Documentation for models — DECIDED
What §5 asks for, as built in Phase 4. Decided 2026-11-07.
- `cplus explain` has the spec, the cheat sheet and the examples built into the compiler at build time, so it works wherever the compiler is, with no repository and no network. A topic is the name of a section, as an error's `spec:` line shows it, or a word someone already knows, such as `hashmap` or `spawn`.
- The examples corpus is `examples/features/`, one small program for each section of the spec, named after its anchor. They carry the directives of the tests, so the test suite runs them all, in every build. An example cannot go out of date without a test failing, which is what "the test suite doubles as few-shot material" asks for. The tests themselves stay as they are, since they are written to find mistakes and not to teach.
- Errors are held to the standard by tools as well as by reading: the arguments are required by the compiler's own types, the test runner fails on any error it sees without a `help` and a `spec`, and a measurement with coverage, `tests/errors_reached.py`, shows which errors and which pieces of advice no test reaches. Whether a help is good advice only reading can tell, and following it. On 2026-11-07 readers who had not written them did that for all of them, compiling what each told them to write and running it, and then again for what had been rewritten, until what was left to find was small. What they found was fixed. Reading comes with each new one after that, and a program in `tests/cases/advice_followed*` shows that what the help says compiles and does what it says.
- The documents are held to the compiler by the test runner: everything called planned must be rejected with "does not support ... yet" (`check_planned_features`), every form the cheat sheet tells a reader to write must compile, alone or in a `cheatsheet_forms` test, and every C++ habit it warns of must be rejected (`check_cheat_sheet`).

### 2.28 Formatting — DECIDED
`cplus fmt` writes C+ in one style, with no settings, as `gofmt` does for Go. Decided 2026-11-10.
- The style is the commonest one of C and C++: an indent of four spaces, `{` at the end of the line, `} else {`, `int* p`. Settings
  would let projects differ, and one style is so that they do not.
- Where lines break is the writer's choice, except that each statement in a block goes on a line of its own. Long lines are not
  wrapped: doing that well takes rules for each kind of code, and doing it badly is worse than a long line.
- It works on the tokens and the comments, once the parser has accepted the file. What it writes is read again, and kept only if it
  has the same tokens and comments, so it cannot change what a program does. A file with syntax errors is left as it is.

### 2.29 Language server — DECIDED
`cplus lsp`, with errors as you type, hover, go to definition and formatting first, and an extension for VS Code in
`editors/vscode`. Decided 2026-11-10.
- It is part of the compiler, so it is always the version that builds the code, and there is nothing more to install.
- Completion comes later, when it is wanted: it needs the parser to make sense of a line that is half written.
- Each version of a file is checked in a child process. The compiler keeps types in tables for as long as it runs, keyed by
  where things are in memory, which is right for one compile and wrong for a server that runs for hours, and a crash on half
  written code then costs one check and not the server. The child comes from `fork`, which Windows does not have: Phase 9 starts
  `cplus` itself there instead.
- The protocol is read and written with LLVM's JSON, which the compiler links already.

### 2.30 C header import — DECIDED
`import <sqlite3.h>;` reads a C header with libclang, and declares in C+ what the program uses from it. A name in quotes is
looked for next to the program first, as `#include` does, and `-I` adds folders. Decided 2026-11-10.
- The header is translated to C+ source, an `extern "C"` block and constants, which the parser reads after the program, as it
  reads what comes with the language. Only the names the program uses are translated, with the types they need, so a big
  header costs little and its other names cannot clash with the program's.
- The program's own declaration of a name, in `extern "C"` or not, is used instead of the header's. That is how a program
  says what a header cannot, such as that a `const char*` result can be NULL.
- C types become the C+ types of the C-functions table. A `const char*` parameter is `string?`, and a `const char*` result
  `string`, which stops the program on NULL, as following a null pointer does; `char*` stays a pointer, since text C hands out
  may have to be freed. A function pointer is a C function pointer type, named by an alias (§2.36).
- A struct is declared with its fields only when they are all C+ types and C's layout is the one C+ would give them; otherwise
  by name, for pointers. Object-like macros are evaluated by clang as the values of declarations, and become constants when
  they are numbers, strings or pointers.
- What cannot be taken, such as `printf`, `short` or a macro with arguments, is left out with the reason, which the error at
  its use says.
- libclang is optional when the compiler is built, as libcurl is: without it, a header import is an error.

### 2.31 References — DECIDED
C++'s `T&` and `const T&`, for references that are used but not kept, as chosen on 2026-11-11 (§6). The details were settled the
same day. One rule holds it together: a `T&` does exactly what it does in C++, or the program stops; a `const T&` never stops the
program and never dangles, but when its container is resized, replaced or was sharing its buffer, it may show the element as it was.
- Being a reference belongs to a declaration, not to a type. A parameter of a function, method or constructor, the variable of a
  range-based `for` loop, and a local variable can be references. A field, a constant, an element of a container, a template
  argument and an optional cannot. `T&&` and `auto&&` are errors. `catch (const Error& e)` means `catch (Error e)`.
- `const T&` returns the value, as a copy. `T& f()` returns a place, built 2026-11-14, after the first slices of operator
  overloading (§2.38). What it returns must outlive the call: a field, an element, a `T&` parameter, a variable outside
  functions, or what another `T&` call returns; the compiler says why anything else is gone. The callee puts the buffers on the
  way to the place, and the class object it lies in, on the runtime's holds for the caller, which keeps them until the statement
  ends and then checks that the place is still in its container, or stops the program. A read copies and lets go of them at
  once. A class `C&` return means the object. `spawn` of one is an error, as the holds are the thread's, an override must
  return `T&` if what it replaces does, a function value of one gives a copy, and `try find(k) = v` puts the `try` on the
  target alone. A local reference bound to one, `auto& e = g();`, keeps those holds until its scope ends and checks them at
  each use, so growing the container is fine while it is not used again, as in C++; a write through it also needs no copy to
  share the container, and a read does not. A reference bound to a class's `C& f()` names the object, as `C x = f()` does, and
  assigning to it, or giving it to a `C&` parameter, is an error, since it does not refer to where the object is kept
  (decided 2026-11-14). `T& operator[]` came with §2.38's slice 5. Changed 2026-11-16, after the exit check's first round, in
  which threads that only read a shared class object through `T& at(i)` broke it: a call that is only read, copied at once, given
  to a `const T&`, bound to a `const auto&`, or a field or `const` method of it, runs the function's reading form, told by a
  hidden argument, which finds the place as a read of it would, making no buffer its own, and is not checked when the statement
  ends. `return row(r)[c];` passes its form on, and so does `?:`. A `const auto&` bound to one keeps the holds but is not
  checked at its uses, and may show the element as it was. The const one of a `T&`/`T` pair is still not chosen on a class.
- A function that is `async`, or run with `spawn`, cannot take a `T&`, and no reference is alive at an `await`. Changed 2026-11-16,
  after the exit check's first round: a `const T&` of one is given a copy of its own, made where the task or the thread starts, as
  `std::thread` does (§2.9), where it was an error.
- Elements of containers, as built 2026-11-11 in place of the pins first planned, which needed every copy in Codegen to be able
  to give a new buffer, as a copy of a pinned buffer would have to:
  - A local reference to an element, `auto& e = v[i];`, and the variable of `for (auto& x : v)`, find the element again at each
    use, from the indexes, keys and class objects worked out where they were bound. So they never dangle, a copy of the
    container never sees what is written through them, and growing, replacing or shrinking the container is allowed. Only
    using one whose element is gone stops the program. `for (auto& x : v)` goes as far as `v` went when it started.
  - A reference to an element given to a call, and the place a struct method is called on, are addresses. All the arguments'
    code runs first, then the places are found, then their buffers are held, so `swap(v[0], v[1])` and `swap(m[a], m[b])` work.
    After the call, the program stops if the container was grown, shrunk, copied, replaced or freed, as the trap of §6 does
    for methods, which this extends: the container must still keep the buffer, and share it with nothing but this thread's
    holds. Changed 2026-11-14: the holds are kept by the runtime for each thread (§2.31's `T&` returns use them too), and a
    write in place counts them as its own, so writing another element through a `vector<int>&` parameter works, as in C++,
    where the trap stopped it before; and a copy that took the container's place, which the count alone missed, stops it.
  - `v.back()`, `v.front()` and `m.at(k)` of a place are places (built 2026-11-14), changing §2.17 and §2.24. Still to
    come: `?:` of two places as a place, built 2026-11-15: of one type, it is the place it chooses, found once with its holds,
    as a `T&` call's place is, so `(f ? a : b).set()` and `(f ? a : b) = x` work as in C++.
  - `for (auto& e : m)` over a hashmap (built 2026-11-15) goes over the keys the map has when it starts, passing by one
    erased before its round, and `e` is the entry of each, found again by its key at each use, as `m.at(k)` of a place is,
    so the map may grow meanwhile. `e.first` cannot be changed, and using `e` once its entry is erased stops the program.
    Over a map that cannot change, `auto&` is `const auto&`, as for a vector.
- Aliasing follows C++: `swap(v[0], v[1])` and `f(x, x)` are allowed.
- Order: `const T&` first, with no change to the runtime, then `T&` to variables and fields, then to elements (all done
  2026-11-11), then `T&` returns with item 6 (2026-11-14).

### 2.32 Enums and switch — DECIDED
Decided and built 2026-11-11, with C++'s syntax and meaning where they agree:
- Only `enum class`, held in an `int` or, with `: long`, a `long`. A plain `enum` and `enum struct` are errors whose help gives
  `enum class`. An enum converts to nothing and nothing converts to it without a cast: `int(c)`, `Color(n)`, `static_cast`.
  `Color(n)` stops the program unless n is the value of an enumerator, so an enum always holds one of its enumerators.
- Enums compare with the six comparisons, are hashable, print and go to JSON as the name, and have as default the enumerator
  whose value is 0; a type with none has no default.
- `switch` takes an integer, a `char`, an enum or a `string`. A section that can reach the next is an error unless it ends in
  `[[fallthrough]];`. A switch on an enum with no `default` must name every enumerator.
- Not yet: other underlying types, enumerator values that use other enumerators, and `switch (init; x)`.

### 2.33 Overloading — DECIDED
Decided and built for functions 2026-11-11, with C++'s meaning where it is clear and an error where C++'s rules are hard to
predict:
- Each argument is ranked: its own type (a `T&` given a place it can change beats a `T` or `const T&`), then widening (`char`
  to `int`, `int` to `long`, `float` to `double`), then any other implicit conversion, where a nearer base class beats a
  farther one. The function chosen is at least as good on every argument and better on one; otherwise the call is an error
  whose help gives the conversion that chooses. Unlike C++, `int` to `long` beats `int` to `double`.
- The return type and `throws` do not tell functions apart. `f(T)` beside `f(const T&)` is an error where they are declared.
  A C function and `main` cannot be overloaded. The LLVM name of an overloaded function carries what it takes:
  `cplus.f(int,long)`.
- Methods overload the same way (built the same day). A class that overrides one method of an inherited name keeps the
  others, which C++ hides; it cannot add to them. A `const` and a non-const method with the same parameters, which `T&`
  returns are for, were built 2026-11-15: a call chooses as C++ does, the `const` one for a struct that cannot change and
  `this` in a const method. A class object can change through any reference, so on one the non-const one is chosen.
- Constructors overload the same way (built the same day). One can hand its work to another, `User(string n) : User(n, 0)
  {}`, with nothing else in its list; a cycle of them is an error. A constructor that takes its own type is an error, as C+
  never copies a class object and copies a struct field by field: its help points to a `clone()` method.
- Default arguments (built the same day): the last parameters only, worked out at each call as if written outside any
  function, so they see constants and functions but not the other parameters or fields, and cannot fail. A `T&` cannot have
  one. An override takes the base method's, and may repeat but not change them. A constructor whose parameters all have
  defaults is the one that builds a value from nothing. Each call gets its own copy of the default, read again from its
  tokens; generics wait, since that needs their bindings.
- Not yet: a template beside a function of its name, until templates are chosen between too; `stoi` with a position and a
  base, and `fs::open` with a default mode, which can now be written.

### 2.34 Lambdas — DECIDED
Decided 2026-11-11 by the owner (§6), built 2026-11-12: a lambda captures as a JavaScript closure does.
- `[&]` shares the variable with the function, and the variable lives as long as anything uses it: one that a kept lambda
  captures moves into a counted box. `[=]` copies, as in C++. One type, `function<R(Args...)>`, with `throws` and `?`, and
  no `mutable`. A function named by itself is a value of that type. Functions cannot be compared.
- A lambda is borrowed when it is only called where it is written or passed to a parameter that only calls it, as `sort`'s
  will, and then it boxes nothing. Otherwise it is kept: returned, stored, given to `spawn`.
- A loop's own variable, `for (int i = 0; ...)`, is a new one each round when a kept lambda shares it, as JavaScript's `let`
  is, so the lambdas of three rounds see 0, 1 and 2.
- A kept lambda cannot share what the function only borrows: a `T&` parameter is an error, and a `const T&` one is copied. In
  a struct's method, `this` is copied in a `const` method and an error otherwise.
- A value of a function type is its code and a counted environment of copies and boxes. A lambda that an object holds and
  that captures the object keeps it alive; `weak<T>` breaks the circle, and a debug build reports the leak.
- Not yet: `await` in a lambda, default values for its parameters, sharing a reference variable, moving
  from a shared variable, a kept lambda sharing a variable of a condition, a `catch` or an async function's parameter, and C
  functions as values. Then `sort` and the other functions that take one, `spawn` with a lambda, and C callbacks.

### 2.35 Algorithms — DECIDED
Decided 2026-11-12 by the owner: the functions of C++'s `<algorithm>` whose C++20 or C++23 range form gives no iterator, as
free functions with those forms and no `std::ranges::`, so that no name changes meaning when iterators come.
- `sort(v)` and `sort(v, less)`, unstable; `stable_sort(v)` and `stable_sort(v, less)`, stable. With any comparator, one that
  is not a strict weak order too, both return after O(n log n) calls with exactly the elements they had, and never stop the
  program; only the order is unspecified. NaNs sort last.
- Also `reverse`, `any_of`, `all_of`, `none_of`, `contains`, `binary_search`, `count`, `count_if`, `erase(v, x)` and
  `erase_if(v, pred)` (which return how many they removed), `iota(v, start)`, `min(v)`, `max(v)` and `accumulate(v, init)`,
  with a function as a last argument where C++ takes one. `find`, `find_if`, `min_element`, `max_element`, `lower_bound`,
  `unique`, `transform` and `for_each` wait for iterators, and say what to write today.
- "Where is it" is `index_of(v, x)` and `index_if(v, pred)`, which give a `long?`: names that cannot clash with C++'s.
- Inside `function<...>`, `const T&` means `T`, so both ways of writing a comparator work. `T&` stays a type of its own.
- A function given to one of these can be `throws`. Then the call needs `try`, as Swift's `rethrows` does, and when it fails
  the vector is as it was. There is no syntax for this in a program's own functions.
- The function given sees the vector as it was before the call. If it changes the vector that `sort`, `stable_sort` or
  `erase_if` was given, the program stops when the call returns, as the change would be lost. The others change nothing,
  so a change it makes stays.
- A lambda's `auto` parameters take their types from the function type it is given to. `void f(auto x)` as a generic, and
  a lambda with `auto` parameters whose types are not known, are planned.


### 2.36 C callbacks — DECIDED
Decided 2026-11-14 by the owner: a C function pointer is what it is in C and C++, and only code with nothing captured goes to C.
- It is written as in C: `int (*compare)(const void*, const void*)` for a parameter, field or variable, `void (*)(int)` as a type,
  and `using Handler = void (*)(int);` to name one. A declarator nested in another, as `signal` is declared, is an error that
  shows the `using`. Header import gives aliases, with the header's typedef name where it has one. `function<...>` cannot be
  one, as it holds what a lambda captured, is never empty, and cannot be compared.
- What goes to C for one is a function by name, a C function, or a lambda written there that captures nothing, as C++ converts
  them, with one adapter made for each while compiling. Not a `function<...>` value, and nothing that `throws`, as C cannot pass
  an error on. State goes through the C function's `void*`, as in C: `&rows`, or `new` memory for a callback that runs later.
  Code made at run time, as libffi does, is refused on hardened systems, and C never says when it could be freed.
- What a callback takes and gives are numbers, `char`, `bool` and pointers. `const char*` is the pointer, read with `string(p)`,
  and `string(p, n)` reads n bytes. Strings converted at the boundary can come later, and structs by value are not supported yet.
- A pointer that C gives back can be called, `f(x)`, with a `string` copied for a `const char*`, as for a C function; calling a
  null one stops the program. It compares with `==`, for `SIG_ERR`. It does not convert to a `function<...>`, which is never
  empty: `[p](int x) { return p(x); }` says that.
- C may call one on a thread it started. Its counts work there; running out of stack is only reported as a crash, and `main`
  does not wait for such a thread.
- The variables outside functions are freed after the `atexit` callbacks run, by one the runtime registers first.
- A signal handler can interrupt an allocation or a `println`, so it should only set a number outside functions. Handing signals
  to the event loop, as Go's `signal.Notify` does, waits for the servers phase.

### 2.37 Format options — DECIDED
Decided 2026-11-14: the options of C++20's `std::format`, `[[fill]align][sign][#][0][width][.precision][type]`, with C++'s
meanings, checked against each value's type while compiling, since the format string is always a literal.
- Integers take `d`, `x`, `X`, `o`, `b` and `B`; floats `e`, `E`, `f`, `F`, `g` and `G`; a char `c`, `?` and the integer types,
  which print its code; a bool `s` and the integer types, which print 1 or 0; a string `s` and `?`; a pointer `p`, and `0`,
  which pads after the `0x`, as C++26 does. Sign, `#` and `0` are for numbers only, and a precision for floats and strings.
  `{:c}` takes only a char: a number never becomes a char by itself, and `char(n)` converts one.
- A negative number in hexadecimal, octal or binary prints its sign and its magnitude, as in C++: `-ff`. Its bits come from
  converting it first, `uint32_t(n)`. `{:#o}` is an error, as C++ prints `017`, which C+ does not read as octal: write `0o{:o}`.
- Every value that prints takes fill, alignment and width, which pad its whole text: structs, vectors, enums and optionals
  too. Numbers and pointers go right by default, and the rest left, as in C++.
- A width counts code points: each UTF-8 character as one, and each byte that is not part of one as one. C++ counts wide
  characters as two and combining ones as none, which needs Unicode tables in every program and changes with Unicode
  versions; this is a known difference. A string's precision counts the same way. The fill is one code point.
- `{:{}}` and `{:.{}f}` take the width and the precision from the values after the one printed, which must be integers. A
  negative one stops the program, where C++ throws. One written in the format string is at most 65535.
- Numbered placeholders, `{0}`, as in C++: not mixed with `{}`. Unlike C++, a value that no placeholder uses is an error, as
  it is with `{}`. Each value is worked out once, left to right, before anything prints.
- `{:?}` for strings and chars, as C++23 has it, with C+'s escapes, `\xHH` where C++ writes `\x{HH}`. A byte that is not part
  of a UTF-8 character prints so wherever a string is quoted, so the text is always UTF-8.
- NaN prints as if its sign bit were clear, `+nan` with `{:+}` and never `-nan`; `E`, `F` and `G` print `NAN` and `INF`. Zeros
  do not pad NaN and the infinities, as in C++, and spaces do.
- `L` is an error, as C+ has no locales. Numbers are written and read with a `.` even when a C library has called
  `setlocale`, as C++'s `std::format` does without `L`. `0` next to an alignment is an error, where C++ ignores the `0`.
- `{:a}`, `#` with a float, a precision with `{:?}`, and the options of C++23 for the elements of a range, `{::x}` and `{:n}`,
  are not supported yet. `to_string` takes no options, as in C++26.

### 2.38 Operator overloading — DECIDED
Decided 2026-11-14: C++'s operators and meanings, in slices that each only turn errors into working programs, so that no slice
changes what an accepted program means. One equality per type: whatever `a == b` means for a type is what every container and
algorithm uses.
- Slice 1, built 2026-11-14: `+ - * / %`, `& | ^ << >>`, unary `-` `+` `~`, `==`, and `<` `>` `<=` `>=`, as members, where the
  left side is `this`, or free, taking both sides. A free operator takes a struct or a class of the program, and is written in
  the file that declares one of its types, which stands in for C++'s argument-dependent lookup and makes `a + b` mean the same
  in every file. It needs no `export`, as the module of a file checks names and an operator is never named: it goes with its
  types. A member and a free operator that take the same sides are an error where declared.
- An operator only reads its sides: a member one is `const`, and a parameter is a value or a `const T&`, with no default. It is
  chosen by §2.33's ranking among members and free operators together. `this` takes its own type or a derived class, and never
  another conversion. Lookup runs only when a side is a struct or a class: `P?`, `vector<P>` and `unique<P>` stay built in.
- `==` and the orderings give `bool` and cannot be `throws`. The others may be, and a use of one is then `try (a * b)`, as a call
  is; `try a * b` is `(try a) * b`, and its error says so.
- A struct's own `operator==` that takes another of it replaces comparing field by field everywhere: vectors, arrays, optionals,
  structs that hold it, `contains`, `count`, `index_of` and `erase(v, x)`. A mixed one, `==(int)`, only adds. `!=` is always
  `!(a == b)`, so `operator!=` cannot be written, and `5 == p` runs `p == 5` only when nothing takes `5` on the left, as C++20
  does. `bool operator==(const P&) const = default;` is accepted and changes nothing. A type with its own `==` is not
  `Hashable` until it can have its own hash (§2.24). `unique`, `local` and `weak` `==` stay identity.
- Each of `<`, `>`, `<=`, `>=` is only itself, as in C++: `a > b` with only `<` is an error whose help writes `b < a`.
  `Comparable` is `a < b` giving a `bool`, built in or the program's, and `sort`, `min`, `max` and `binary_search` use only `<`.
- `a += b` runs a written `operator+=` (slice 2, built 2026-11-15) once the left side's type has one, as in C++, and nothing is made
  from `+` for it then: a non-const member returning `T&`, with `return this;`, or `void`, called on the place after `b`. Without
  one, it is `a = a + b`, with the place found
  once, after `b`, as for numbers. `++` and `--` are never made from `+`.
- A member operator is a method: `virtual`, `override`, interfaces and inheritance apply unchanged. Its LLVM name always carries
  what it takes: `cplus.Point.operator+(Point)`. The sides are worked out left to right, as for the language's operators.
- Never: `=`, as a struct copies its fields and a class is shared; `!=`; `&&`, `||` and `,`; `!` and every implicit conversion
  operator, as a struct or a class is never a condition; unary `&`; `->*`; `new` and `delete`; `co_await`; `operator""`; `<<`
  and `>>` that take a stream, where printing has its help.
- A class's own `==` (slice 2, built 2026-11-14) replaces identity in the same way, for the class and those that derive
  from it, which inherit it. It can be `virtual`. `= default` on a class compares the fields, the base classes' first, as
  C++20 does. `same_object(a, b)` is identity, for references of related classes and optionals of them, and is what an
  `operator==` writes in place of `this == other`, which would run it again and is an error there.
- Later slices, each a "not yet" error until it comes: 2, written compound assignments (built 2026-11-15); 3, `<=>` with `strong_ordering`, `weak_ordering` and
  `partial_ordering`, `= default`, and the four comparisons made from it; 4, `[]` and `()` that give a value (built 2026-11-14:
  members only, as in C++; `[]` is `const`, can take more than one index as C++23's does, and gives a copy, so `g[i] = x`
  is an error; `()` may change the object and take default values, as in C++, and is called through a variable or a
  field, as `make()(1)` waits); 5, with `T&`
  returns, `T& operator[]` and a `const` and a non-const pair (built 2026-11-15; a non-const `operator[]` must return `T&`,
  and one used on a struct that cannot change is an error without a `const` one beside it); 6, with iterators, `++`, `--`, unary `*` and `->`. Explicit
  conversion operators, operators on enums, generic free operators, `a.operator+(b)` and `constexpr` operators wait too.

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

Status: Phases 0 to 6 are done, the last with the formatter, the language server and C header import. Phase 7, the core
language, is under way, then servers, then platforms. Of Phase 7, items 1 to 3 are done, and so are references, overloading of functions, methods and constructors, and the library parts of item 7. `examples/books.cplus`, a reading list stored in SQLite through its C API,
met the exit criterion of Phase 1. What each feature does is in `docs/SPEC.md`, whose last section lists what is implemented and what is planned.

**Phase 0 — Spec v0.1.** Grammar, type system, memory model, ownership forms, error
model, module system. Written as the document models will read. Includes the one-page
cheat sheet from day one.

**Phase 1 — Core compiler.** Lexer, parser, type checker, LLVM backend. Structs,
functions, control flow, refcounted classes without inheritance, arrays, vectors,
strings, optionals, definite assignment, overflow traps, `print`. Manual `extern` for C.
Debug builds with ASan/UBSan. Exit criterion: a non-trivial program that calls a C library.

**Phase 2 — Abstractions.** Classes with inheritance and interfaces, multiple
inheritance, generics with constraints, `constexpr`, error values and
`try`, `unique`, `weak`, leak report.

**Phase 3 — Batteries.** Reflection descriptors, `json`, `http` and WebSockets via
libcurl, `hashmap`, `spawn` and `Task`, and `local`, which moved here on 2026-11-06: it only means
something next to threads, and its rule that it cannot cross into `spawn` can only be enforced then.

**Phase 4 — Diagnostics and docs pass.** Rewrite every diagnostic to the standard in §5.
Finish the spec. Examples corpus from the test suite. `cplus explain`.

**Phase 5 — Async.** `async`/`await`, event loop on libuv, LLVM coroutine lowering,
async standard library surface, `http::fetch`.

**Phase 6 — Tooling.** Formatter, LSP, C header import via libclang.

**Phase 7 — Core language.** Added 2026-11-11, before platforms. Phases 1 to 6 built what sets C+ apart, but a
program still lacks much of what C and C++ code uses every day, and the safety of §2.7 is partly owed to features not built
yet: nothing can dangle without references, and nothing can capture a variable that is gone without lambdas. In order of
what holds real programs back most:
1. A first slice of the library, small and with little to design: reading a whole file, reading a line of input, and
   parsing a number from a string. Today all three go through C.
2. A program's own modules, `import net.server;`, as §2.13 decides, so that a program can be more than one file. `.hplus`
   files wait for libraries that ship compiled.
3. Enums and `switch`.
4. References, in the form chosen on 2026-11-11 (§6): C++'s, for references that are used but not kept. This comes
   before lambdas and overloading, which depend on it: what a lambda captures, what `operator[]` gives, and what a
   parameter can be. It may replace the rules of §2.17 and §2.24 on when a result is a copy, and is where the safety of
   §2.7 is tested hardest. `return &x;` of a local, which compiles today, is caught first.
5. Lambdas, and a type for a function (built 2026-11-12, see §2.34), then `sort` and the other functions that take one
   (built 2026-11-14, §2.35), `spawn` with a lambda (built 2026-11-14, §2.9), and C callbacks (built 2026-11-14, §2.36).
6. Function overloading, more than one constructor and default arguments, then operator overloading (its first slice built
   2026-11-14, §2.38). Overloading also
   ends the pairs that stand in for it, such as `http::fetch` and `http::fetchWith`, before more of them are made.
7. The rest of what everyday programs need: writing files and the rest of `fs` (§2.15), `split`, `trim`, `join` and
   `replace` on strings, `insert` and `erase` on vectors, width, alignment and hex in `format` (built 2026-11-14, §2.37),
   unsigned integers (built 2026-11-12, see §2.7), variables outside functions, and `int main(vector<string> args)`.

   Variables outside functions (2026-11-11): each is worked out before `main`, in the order written and each module
   after those it imports. One with a value known only while the program runs has a state, so that one used sooner
   is worked out then, and one that leads back to itself stops the program; C++'s order of initialization across
   files is a bug this cannot have. Their values cannot fail or `await`, and `spawn` before `main` stops the
   program, so working them out needs no locks. They are released in reverse order after the threads and the event
   loop end, and after the `atexit` callbacks run (§2.36). A `const` of a constant's type whose value cannot be worked out while compiling is one of these too,
   as it would be in C++. They are never moved from. `thread_local` and `static` in a function wait.

Each is designed when it is reached, as before. Exit criterion: two or three programs of a few hundred lines each, one of
them in several files, written by a model given only the cheat sheet, with nothing in them that works around a missing
feature.

What remains, and how the exit check runs (decided 2026-11-14):
- Built in this order: a class's own `operator==` (§2.38, slice 2); `operator[]` and `operator()` that give a value (slice
  4); `T&` returns, with the places §2.31 still lists (`v.back()`, `v.front()`, `m.at(k)` and `?:` of two places, and `for
  (auto& e : m)`); `T& operator[]` (slice 5); and written compound assignments (the rest of slice 2), last, so that a `T&
  operator+=` can return `*this` as in C++. `<=>` (slice 3), slice 6 and iterators wait for a later phase: `operator<` is
  C++'s own spelling and all that `sort` uses, and iterators are a design of their own, which range `for` and the index
  algorithms stand in for.
- The programs are written by Sonnet, one fresh agent for each, given only `docs/CHEATSHEET.md` and `build/cplus` (`check`,
  `run` and `explain`, at most 30 times), and briefed by what the program does, never by the features to use: `ledger`, in
  several files, reads transactions from a file named in its arguments, reports bad lines by number, prints aligned monthly
  totals by category and writes a report file; `calc` interprets a small language with variables, functions, `if` and `while`
  from standard input, with errors that give a position; `routes` finds shortest paths on a grid map, with a generic grid and
  priority queue, a 2D point type with arithmetic, and several searches run on threads. Each is 300 to 500 lines, comes with
  sample input and output, and with notes of each first choice that was refused or is planned: what was wanted, what was
  written instead, and the diagnostic.
- Each finding is one of three. A decision: C+ differs on purpose (the cheat sheet's "Different" table, §2.38's "Never"), and
  only the cheat sheet may need fixing. A respelling: one line of the same shape that the help names, recorded as planned,
  which does not fail the check. A workaround: what was written changes the program's shape or gives up safety, as a raw
  pointer, `hashmap<T, bool>` as a set, or a get and a set method do; it fails the check, and is built in Phase 7 when the
  decisions cover it or one more decides it, or else is an exception for the owner to accept.
- At most two rounds. After the first round is built, each program gets its first choices back and must print the same. A
  second round, with fresh writers and the same briefs, runs only if the first found workarounds. The check is a sample, not
  a proof. The final report gives each program's lines, files and compiler runs, every finding with its class and outcome,
  the cheat sheet's fixes, the tests on both builds, and the programs, which go to `examples/` with tests.

**Phase 8 — Servers.** Added 2026-11-11. HTTP and WebSocket servers, on libuv as `net` in §2.15, with handlers
written as the lambdas of Phase 7. Before them, what reads data another machine sends is fuzz tested: WebSocket frames,
HTTP headers and redirects, and `json::parse`, then the servers' own parsing as it is written. A server takes that data
from anyone, and the WebSocket code is C+'s own (§2.26). How a runtime error ends one request and not the whole
program (§6) is settled first too.

**Phase 9 — Platforms.** CI on Linux, which §2.1 already asks for. macOS CI. wasm32-wasi. Windows platform layer. Browser wasm. Programs that use `http` and stand alone.

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

- Lambdas were decided 2026-11-11 and built 2026-11-12, in §2.34. Enums and `switch` are in §2.32.
- Default arguments in generics, and a template beside a function of its name, in Phase 7 (§2.33).
- A struct method changing a vector element stops the program if its body also writes to that
  vector, keeps a copy of it or frees it, since the update to the element would be lost (decided
  2026-11-06). Since 2026-11-11 the same goes for a function given a reference to an element (§2.31). The alternative is to pin a buffer separately from its reference count, so that writes
  land in place, at a cost to every vector write. One case goes unnoticed: a copy taken through a
  pointer and dropped again inside the method's body sees what the method did to the element so far.
- A plain `const` is worked out while compiling when it can be, but `const int X = 1 / 0;` still stops the
  program when `X` is used, and `constexpr` reports it while compiling. They could become one rule.
- Printing still goes by a function the compiler writes for each type, while 2.14 says it is built on descriptors. It works, and
  it costs a program nothing it does not print. Moving it would give one way of walking a value, at the cost of a slower `println`.
- What a module has is one token with `::` in it, not a scope, which is what `joinModuleNames` makes, and a program's own
  modules are named the same way: their names get the module's path in front before parsing (`Modules::qualify`). A program
  cannot tell, since it cannot declare such a name (`err_module_add`). Real scopes would come with nested namespaces.
- `main` takes its arguments as a `vector<string>` (built 2026-11-11), or as in C++, through raw pointers. `args[0]` is
  how the program was started; under `cplus run` it is the program's name, not the temporary path it is built at.
- References (`T&`), in Phase 7. The direction was decided on 2026-11-11: C++'s syntax and meaning, for references that
  are used but not kept, as in Swift and Hylo.
  - In: parameters (`Point& p`, `const T&`), `for (auto& x : v)`, a result used where it is made (`v.back().x = 1`,
    `m[k].count++`), and a reference variable in a block (`auto& e = m[k];`).
  - Out: a reference in a field, and a returned reference to a local. A lambda's `[&]` shares the variable instead,
    as §2.34 decides.
  - While a reference into a vector, string or hashmap lives, that one cannot grow or share its buffer. The compiler checks
    this where it can tell, and the running program where it cannot, so a reference never dangles or changes a copy.
  - Plain C++ references were turned down. Copies share a buffer until one is written (§2.12, §2.17), so writing through a
    reference could change another variable's value, silently, where C++ would at worst crash under `--debug`.
  - The details are in §2.31. `const T&`, and `T&` to variables and fields, work.
- A scheduler that shares async work between threads, which the rules in 2.9 allow
  (decided 2026-11-10), when real workloads need more than one loop per thread.
- A runtime error stops the whole program, as in Swift. A server may want to end only the request that failed, which
  needs somewhere a thread or a request can be stopped. Settled in Phase 8.
- Speed is not measured against C++ yet. In one benchmark on 2026-11-11, walking a vector of two million objects ran about
  twice as slow as C++ with `shared_ptr`, while a sieve ran as fast. Where the time goes is not known.
- Package manager, after v1.
- Virtual inheritance, if ever.
