# C+ cheat sheet

C+ is a compiled language shaped like C++. **If you know C++, assume C++ syntax and meaning,
except for what this page lists.** Full rules: [SPEC.md](SPEC.md). Files end in `.cplus`.

```cpp
// No #include. Functions can be called before they are defined.
int main() {
    string name = "Ada";
    long total = 0;
    for (int i = 1; i <= 10; i++) total += square(i);
    println("{}: {}", name, total);   // C++23-style formatting
    return 0;
}

long square(long n) { return n * n; }
```

## Works today

| Area | C+ |
|------|----|
| Types | `int` (32-bit), `long` (64-bit), `double` (64-bit), `float` (32-bit), `bool`, `char` (one byte, 0 to 255), `string`, `void`, `auto`, structs, classes, `vector<T>`, `array<T, N>`, `T?`, `T*` |
| Floating point | `1.5`, `.5`, `1e9`, `2.5f`, as in C++. `7 / 2` is 3, `7 / 2.0` is 3.5. IEEE: `1 / 0.0` is `inf`, no traps. `float f = 0.5;` works for a written number |
| Casts | `int(x)`, `static_cast<int>(x)`, `(int)x` between numbers, `char` and `bool`. A fraction is dropped toward zero; a value that does not fit **stops the program**. `(int*)p`, `static_cast<int*>(p)` between pointer types. `reinterpret_cast<void*>(-1)`, `reinterpret_cast<long>(p)` between a pointer and a number |
| Math | `sqrt`, `pow`, `sin`, `floor`, `round`, `fmod`, `isnan`, `abs`, `min`, `max` and the rest of `<cmath>`, with no `std::` |
| Characters | `char c = 'a';`, escapes as in strings. `c + 1` and `c - '0'` are `int`s, as in C++; `char(n)` is checked. `c++`, `c += 2` stay chars. `isdigit`, `isalpha`, `isspace`, `toupper`, `tolower` and the rest of `<cctype>` (ASCII, return `bool` or `char`) |
| Structs | `struct P { int x; int y = 0; };` is a **value** that is copied. Build with `{1, 2}`, `{.y = 2}`, `P{1, 2}`, `P(1, 2)`, `{}`. `P(p)` copies one |
| Fields | `p.x`, `p.x += 1`. `Point p;` can be filled field by field before use |
| Classes | `class U {...}; U u = U("a");` is a **reference-counted object**: `U v = u;` shares it. No `new`; freed when the last reference goes away |
| Methods | written inside the type: `int area() const { return w * h; }`. Use fields by name or as `this.w` |
| Constructors | `U(string n) : name(n) {}`, one per type. `~U() {}` destructors for classes only |
| Access | `public:`, `private:`, `protected:`; class members are private by default, struct members public |
| Inheritance | `class Dog : public Animal, public Pet {...};`. `virtual string sound() const { ... }`, `string sound() const override { ... }`, `virtual double area() const = 0;`. `Dog(string n) : Animal(n) {}`. A `Dog` converts to an `Animal`, as the same object. `Animal::sound()` for the base class's version. A diamond holds the shared base class twice, and `d.Left::id` says which |
| Interfaces | `interface Speaker { string speak() const; };`: methods only, no bodies or fields. `class Dog : public Speaker { string speak() const override { ... } };`, then `Speaker s = Dog();` |
| Finding the real type | `if (auto d = dynamic_cast<Dog?>(a))` (null when it is not a `Dog`), `dynamic_cast<Dog>(a)` (**stops the program** when it is not) |
| Weak references | `weak<Node> parent;` starts empty and does not keep the object alive, which breaks cycles. `parent = node;`, `if (auto p = parent.lock())`, `parent.lock()?.name`, `parent.expired()`, `parent.reset()`. As `std::weak_ptr` |
| Errors | `int f() throws { if (bad) throw Error("why"); return 1; }`. Each call that can fail is `try f()`, which passes the error on, or is inside `try { f(); } catch (NotFound e) { } catch (Error e) { println("{}", e.what()); }`. `class NotFound : public Error {...}`. `catch (...)`, `throw;`, `int main() throws`. Everything is released on the way out |
| Generics | `template<typename T> T largest(T a, T b) { ... }`, called as `largest(1, 2)` or `largest<long>(1, 2)`. `template<typename T> class Stack { ... };`, used as `Stack<int>` with its arguments always written. `template<typename T, int N> struct Buffer { array<T, N> items; };`. `T()` is the default value of T |
| Constraints | `template<Comparable T>`, `template<Number T>`, `template<Integer T>`, `template<Floating T>`, `template<Hashable T>`, `template<Speaker T>` for an interface or class. Your own: `template<typename T> concept Addable = requires(T a, T b) { a + b; };` |
| Unique references | `unique<Node> a = unique<Node>("x");` is the one owner, never empty: `a.name`, `a.go()`. `unique<Node> b = move(a);` leaves `a` with no value. `take(move(b))` hands it over, `lend(a.get())` lends it as a `Node`. `unique<Node>?` can be empty. Copying is an error |
| Local references | `local<Node> a = local<Node>("x");` counts without atomic steps, so only one thread can use it. `local<Node> b = a;` shares it. `a.name`, `a.go()`, `a.get()` lends it as a `Node`. `local<Node>?` can be empty. Cannot go into `spawn` |
| Threads | `Task<long> t = spawn sum(v);` or `auto t = spawn obj.method(x);`, then `t.wait()`, `t.done()`. `Task<void>` for no result. The function `throws`: `Task<long, throws>`, and `try t.wait()`. Arguments are copied, class references are shared. The program waits for its threads when `main` returns. An error that no `wait()` can see fails the program |
| Locks | `Mutex m = Mutex();`, `m.lock()`, `m.unlock()`, `m.try_lock()`. `LockGuard g = LockGuard(m);` unlocks at the end of the block. Locking twice on one thread **stops the program**. `cplus run app.cplus --race` finds what is not protected |
| Async | `async string load(string name) { await delay(10); return name; }`. A call starts it, and gives a `Task<string>`: `auto t = load("a");`, then `await t` in an async function, or `t.wait()` in a plain one. `async int main()`. A function that `throws` gives a `Task<string, throws>`, and is waited for with `try await t`. `await` works on `spawn`'s tasks too. `delay(ms)` waits. A task must be kept or waited for, and the program waits for async work when `main` returns. No `LockGuard` or `local` across an `await` |
| Vectors | `vector<int> v = {1, 2};` is a **value**, copied like a struct (cheaply: copy-on-write). `vector<int> v;` is empty. `v.push_back(x)`, `v.pop_back()`, `v[i]`, `v.size()` (a `long`), `v.empty()`, `v.front()`, `v.back()`, `v.clear()` |
| Building vectors | `{1, 2, 3}`, `vector<int>{1, 2}`, `vector<int>(n)`, `vector<int>(n, value)`, `vector<vector<int>>` |
| Hash maps | `hashmap<string, int> m = {{"a", 1}};` is a **value**, copied like a vector, that keeps the order entries were added in. `m["b"] = 2`, `m["b"]++`, `m[k].push_back(x)`. `m[k]` adds a missing key with a default value, as in C++, or **stops the program** if `V` has none. `m.at(k)` (**stops the program** if missing), `m.get(k)` (a `V?`), `m.contains(k)`, `m.count(k)`, `m.insert({k, v})` (keeps what is there), `m.erase(k)`, `m.size()`, `m.empty()`, `m.clear()`, `m.keys()`, `m.values()`. `for (auto e : m)` with `e.first`, `e.second`. Prints as `{"a": 1}`. A key is a number, `char`, `bool`, string, pointer or class reference, or an optional, vector, array or struct of those |
| JSON | `import json;`. `json::stringify(x)`, `json::pretty(x)`, `User u = try json::parse<User>(text);`, for structs, classes, vectors, arrays, optionals, hashmaps and what they hold. Public fields only. A missing key keeps the field's default, and a key that is not a field is left out. `catch (json::Error e)` has `e.line`, `e.column` and `e.what()` |
| Any JSON | `json::Value doc = try json::parse<json::Value>(text);`, then `doc.get("a")?.at(0)?.text() ?? "none"`. `isNull()`, `number()`, `whole()`, `boolean()`, `keys()`, `items()`, `size()`. `json::object()`, `json::array()`, `v.set(k, json::of(x))`, `v.push(json::of(x))` |
| HTTP | `import http;`. `http::Response r = try http::get(url);`, then `r.status`, `r.ok`, `r.body`, `r.header("content-type")`. `try http::post(url, body, "application/json")`, `put`, `patch`, `delete`. `try http::send({.method = "HEAD", .url = url, .headers = {{"Authorization", token}}, .timeout = 5});`. A 404 is an answer, not an error. Other failures: `catch (http::Error e)`, with `e.kind` (`"timeout"`, `"connect"`, `"tls"`, ...) and `e.what()`. `http::encode(text)` for a part of a URL. Async: `try await http::fetch(url)`, `http::fetchWith(request)` |
| WebSockets | `http::WebSocket ws = try http::connect("wss://...");`, `try ws.send(text);`, `try ws.sendBinary(bytes);`, `while (auto m = try ws.receive())` with `m.data` and `m.isBinary`, null once closed. `ws.close()`, `ws.closeCode()`, `ws.timeout = 5;`. `http::connectWith(request)` for headers |
| Reflection | `import reflect;`. `auto t = reflect::type<User>();` or `reflect::of(x)`, then `t.name()`, `t.kind()`, `t.size()`, `t.elem()`, and `for (auto f : t.fields())` with `f.name`, `f.offset`, `f.type`, `f.isPublic` |
| Arrays | `array<int, 3> a;` (all 0), `array<int, 3> a = {1, 2};`, `array<int, 3>{1, 2}` are **values**, copied whole, with `N` a number or a constant. `a[i]` (checked), `a.size()` (a `long`), `a.empty()`, `a.front()`, `a.back()`, `a.fill(x)`, `a.data()`, `&a[i]`, `==`, `for (auto x : a)`. Prints as `[1, 2, 3]`. `array<array<int, 3>, 2>` for rows. `string(a)` reads the text in an `array<char, N>` |
| Strings | a **value**, like `std::string`. `a + b`, `s += t`, `s.size()` / `s.length()` (bytes, a `long`), `s.empty()`, `s.substr(pos, n)`, `s.find(t)` (`string::npos` when absent), `s.contains(t)`, `s.starts_with(t)`, `s.ends_with(t)`, `s.clear()`, `==` `<` `>` |
| String chars | `s[i]` and `s[i] = 'x'` (checked), `for (char c : s)`, `s + '!'`, `s += c`, `s.push_back(c)`, `s.pop_back()`, `s.front()`, `s.back()`, `string(3, '-')`, `s.find('/')` |
| Raw strings | `R"(no "escapes" here, and any number of lines)"`, or `R"end(has )" inside)end"`, as in C++ |
| Output | `println("x = {}", x)`, `print(...)`, `println()`. `{{` `}}` for braces. Structs print as `{x: 1, y: 2}`, objects as `U {name: "a"}`, vectors as `[1, 2]`. A double prints its shortest exact digits (`0.1`); `{:.2f}`, `{:e}`, `{:g}`, `{:.3}` choose them |
| Building text | `format("{} of {}", a, b)` returns the string `println` would print. `to_string(x)` for any number |
| Optionals | `User? u = find(name);` holds a `User` or `null`; `int? n;` starts out null. `u?.name` is null when `u` is, `u?.name ?? "nobody"` gives a fallback, `if (auto x = u)` runs with the `User` inside. `has_value()`, `value()`, `value_or(x)`, `u == null` |
| Pointers | `int* p = &x;`, `*p = 5`, `p->x`, `p[i]` (unchecked), `p + n`, `p - q`, `p++`, `const int*`, `int* const`, `void*`, `int**`, `null`, `if (p)`. Following a null pointer **stops the program**. `sizeof(T)`, `sizeof x` (a `long`). `string(p)` copies a `char*`'s text. A pointer prints as `0x...`. Returning the address of a local, `return &x;`, is an error. Run with `--debug` to catch overruns and use after free |
| Manual memory | `int* p = new int(5); delete p;`, `new Point{1, 2}`, `int* a = new int[n]; delete[] a;`. Values start as their default, and each `new`/`delete` **warns**. Turn it off with `[[allow(new_delete)]]` before a function, `[[allow(new_delete)]];` at the top of the file, or `-Wno-new-delete`. The wrong form of delete **stops the program** |
| C functions | `extern "C" int abs(int n);` or `extern "C" { ... }`, written with C+ types: `long strlen(string s);`, `string? getenv(string name);`, `void* malloc(long n);`, `double frexp(double x, int* e);`. Strings are converted at each call. Link other libraries with `-lname` |
| C types by name | `extern "C" struct FILE;`, then `FILE* f = fopen(...)`. Only pointers to it can be used |
| C headers | `import <sqlite3.h>;` or `import "mine.h";` (next to the program first), then its functions, structs, macro constants and enum constants by their C names. `-I dir` for another folder. A `const char*` parameter is `string?`, a `const char*` result `string`, `size_t` `long`, a function pointer `void*`. What C+ cannot take, such as `printf` or `short`, is an error that says why. The program's own declaration of a name wins |
| C structs | `extern "C" { struct ldiv_t { long quot; long rem; }; ldiv_t ldiv(long a, long b); tm* gmtime_r(const long* t, tm* out); }`. Fields are numbers, `char`, `bool`, pointers, such structs and arrays of those, in C's order. By value or through a pointer. Before C fills one: `tm t = {};` |
| Control flow | `if`/`else`, `while`, `do`/`while`, `for`, `for (auto x : v)` over a vector or an array, `break`, `continue`, `?:` |
| Operators | all C++ arithmetic, comparison and bit operators, `&&` `\|\|` `!`, compound assignment, `++`/`--`. `%` and bit operators take integers only |
| Equality | `==` `!=` on any two values of one type: strings by contents, structs field by field (automatic), vectors and arrays element by element, objects by identity. `opt == 3`, `opt == null` |
| Variables | `const`, `auto`, `int a = 1, b = 2;`, block scope, shadowing in inner blocks |
| Constants | outside any function, in any order: `const int MAX = 100;`, `const string NAME = "a" + OTHER;`, `void* const NONE = null;`. A number, `char`, `bool`, `string` or pointer, from literals, other constants, operators, casts and calls to `constexpr` functions that can run while compiling |
| constexpr | `constexpr int MAX = square(4);`, `constexpr int square(int n) { return n * n; }`, worked out while compiling, so `1 / 0` there is a compile error. `array<int, MAX * 2> a;`, `Buffer<int, MAX>`, also with a plain `const` |
| if constexpr | `if constexpr (Integer<T>) { ... } else { ... }`: only the branch taken is checked. Also `Number<T>`, `Floating<T>`, `Comparable<T>`, `same_as<T, int>`, `derived_from<T, Base>`, your own `Addable<T>`, and `N == 0` |
| Program | `int main()`, or `int main(int argc, char** argv)`, with `string(argv[1])` for the first argument. `cplus run app.cplus -- arg1 arg2` |
| Literals | `42`, `0xFF`, `0o755` (octal), `0b1010`, `1'000'000`, `1L` (long), `1.5`, `1e-9`, `2.5f` (float), `'a'`, `'\n'` (char) |
| Conditions | `bool`, a number, a pointer or an optional, as in C++: `if (n)`, `while (n--)`, `!n`, `if (p)` (not null), `if (opt)` (holds a value). Not a `char`: write `c != '\0'` |

## Different from C++. Do not write the C++ version

| C++ habit | C+ |
|-----------|----|
| `#include`, `#define` | no preprocessor. Built-ins need nothing; modules use `import x;`; C headers use `import <stdio.h>;`, and C functions can be declared with `extern "C"` |
| `let x = 1`, `var x = 1`, `function f() { }` | `auto x = 1;`, `void f() { }`: a type or `auto` always comes first |
| `console.log(x)`, `Math.sqrt(x)` | `println("{}", x)`, `sqrt(x)` |
| `typedef`, `using`, `inline`, `goto`, `register`, `volatile`, `mutable`, `explicit`, `friend`, `union` | left out, and each says what to write instead. `goto` is a loop or a function, and `volatile` is a `Mutex` |
| `a = 1, b = 2;` | no comma operator: two statements |
| `#define MAX 100`, `static const int MAX = 100;` | `const int MAX = 100;`, or `constexpr int MAX = 100;` |
| `int count = 0;` outside a function | not supported yet. A value that never changes is a `const`; otherwise pass it to the functions that use it |
| `extern "C" size_t strlen(const char* s);` | `extern "C" long strlen(string s);`: C+ types in C declarations. `const char*` is `string` (or `string?` for a result that may be NULL), `char` is `char`, `size_t` is `long`, `unsigned` is `int`. `char*` is a raw pointer, for buffers and text to free |
| `struct FILE* f`, `struct tm t;`, `typedef struct x x;` | `extern "C" struct FILE;` once, then `FILE* f`; `tm t;`, no word `struct`; no `typedef` |
| `char name[64];` in a C struct | `array<char, 64> name;`, read with `string(s.name)` |
| `short` in a C struct | not supported yet; use the struct through pointers, declared by name |
| `void f(int v[])` in `extern "C"` | `void f(int* v)` |
| `extern "C" int printf(const char*, ...);` | variable arguments are not supported; use `println` |
| `std::cout << x`, `printf` | `println("{}", x)` |
| `std::vector<int>`, `std::string`, `std::to_string` | `vector<int>`, `string`, `to_string`, with no `std::` |
| `std::`, `using namespace std` | no `std::`; built-ins have no prefix |
| prototypes / headers | none. Define each function once, in any order |
| a function or a lambda inside a function | write it at the top level of the file, and pass it what it needs |
| `int x;` then read `x` | compile error until `x` is assigned on every path |
| `int i = someLong;`, `int i = 2.5;` | error: a conversion that can lose information needs a cast: `int(x)`, which stops the program if `x` does not fit |
| `float f = someDouble;` | error: write `float(x)`. Only a double written as a number, `float f = 0.5;`, converts by itself |
| `const_cast` | does not exist. `const` cannot be cast away |
| `throw 42`, `throw "bad"`, `throw new Error(...)` | only an `Error` or a class deriving from it: `throw Error("bad");` |
| `f();` when `f` can fail | error: `try f();`, or inside `try { }`. A function that can fail is marked `throws` |
| `catch (const Error& e)`, `catch (e)`, `finally`, `noexcept` | `catch (Error e)`. No `finally`: use a destructor. No `noexcept`: a function not marked `throws` cannot fail |
| `x = c ? 1 : throw ...` | `throw` is a statement: use `if` |
| `Stack s(1);`, `auto s = Stack(1);` on a generic class | write the arguments: `Stack<int> s = Stack<int>(1);` |
| `template<>` specialization, SFINAE, `typename... Ts` | left out. One definition for every type; a vector for any number of values |
| `template<typename T> requires Comparable<T>` | `template<Comparable T>` |
| an error only from inside a generic that is used | a generic is only checked when used, once for each set of types. `cplus check` needs code that uses it |
| `std::shared_ptr<Node>`, `std::make_shared<Node>()` | `Node n = Node();`: a class variable is already a counted reference |
| `std::unique_ptr<Node>`, `std::make_unique<Node>(x)`, `std::move(a)` | `unique<Node>`, `unique<Node>(x)`, `move(a)`. Reading `a` after `move(a)` is an error |
| `u->x`, `*u`, `if (u)`, `u.reset()` on a unique reference | `u.x`, `u.get()`. It is never empty; `unique<Node>?` can be, with `opt = null` |
| `std::thread t(f, x); t.join();`, `std::async(f, x)`, `pthread_create` | `auto t = spawn f(x);`, then `t.wait()`. A task can be let go of: it keeps running, and the program waits for it at the end |
| `std::future<int> r = ...; r.get();` | `Task<int> r = spawn f();`, then `r.wait()`, which can be called again |
| `spawn try f()`, `try spawn f()` | `auto t = spawn f();`, then `try t.wait()` |
| `spawn println("x")`, `spawn usleep(1)`, `spawn P(1)` | `spawn` takes a C+ function or method. Write a function that does it |
| `spawn f(x).wait()` | `spawn` takes all of the call after it: `(spawn f(x)).wait()` |
| `std::mutex m;`, `std::lock_guard<std::mutex> g(m);` | `Mutex m = Mutex();`, `LockGuard g = LockGuard(m);` |
| `std::atomic<int>` | planned. Use a class field and a `Mutex` |
| `-fsanitize=thread` | `cplus run app.cplus --race` |
| `async function load() { }`, `await load("a")` in a plain function | `async string load(string name) { ... }`. `await` is only for an async function, where `wait()` is an error. A plain function waits with `t.wait()` |
| `load("a");`, which throws away the task of an async call | error, since nothing waits for it: `await load("a");`, or `auto t = load("a");` to let it run on |
| `await try load("a")` | `try await t`: `try` comes first |
| `sleep(1)`, `std::this_thread::sleep_for(1s)`, `setTimeout(f, 100)` | `delay(1000).wait();`, or `await delay(1000);` in an async function: `delay` counts milliseconds. For later, an async function that waits first |
| `Node n = a;` from a `local<Node>` | `a.get()` lends it. A `local<Node>` only converts to another |
| `std::weak_ptr<Node>` | `weak<Node>`, with `lock()`, which gives a `Node?`, `expired()` and `reset()` |
| `w == null`, `if (w)` on a `weak<Node>` | `w.expired()`, `if (auto n = w.lock())`. A weak reference is not a condition |
| `dynamic_cast<Dog*>(a)`, `static_cast<Dog*>(a)` | a class variable is already a reference: `dynamic_cast<Dog?>(a)` |
| `class Dog : Animal` | error, since that is private in C++: `class Dog : public Animal`. Only classes derive; a struct cannot |
| `string sound() const` replacing a virtual method | error: `override` is required, and the parameters, return type, `const` and `throws` must match |
| a method or field with the name of an inherited one | error, unless it overrides a virtual method. C++ would quietly hide the inherited one |
| `virtual ~Animal() {}` | optional: a destructor always runs for what the object really is |
| `class D : public virtual B` | no virtual inheritance. A diamond holds two `B`s; use an interface, or write `d.Left::x` |
| `(long)p`, `(int*)0x1000`, `int* p = 0;` | error. A pointer to nothing is `null`; only `reinterpret_cast` converts between a pointer and a number |
| `#define SQLITE_TRANSIENT ((void*)-1)` | `void* const SQLITE_TRANSIENT = reinterpret_cast<void*>(-1);`, or `import <sqlite3.h>;`, which has it |
| `bool ok = p;` | error: write `p != null` or `bool(p)`. `if (p)` is fine |
| `int* a, b;` | error, since `b` would be a plain `int`. Write `int* a, * b;` or two declarations |
| `int*? p` | error: a pointer can already be null; write `int*` |
| `println("{}", charPtr)` | error: write `string(p)` for the text or `(void*)p` for the address |
| `&v[0]`, `s.data()` | pointers into vectors and strings are planned; `&a[0]` and `a.data()` work on an array |
| `std::sqrt(x)`, `#include <cmath>`, `M_PI` | `sqrt(x)`, with nothing to include. `M_PI` after `import <math.h>;`, or `acos(-1)` |
| `x % 2.5` on a double | error: write `fmod(x, 2.5)` |
| `printf("%.2f", x)`, `std::setprecision(2)` | `println("{:.2f}", x)` |
| `std::stod(s)`, `std::stoi(s)` | planned; for now declare C's `atof` or `atoi` with `extern "C"` |
| `long double`, `1.5L` | no `long double`; `double` is 64 bits |
| `int n = true;` | error: `bool` does not convert to a number (numbers do convert to `bool`) |
| `010` (octal) | `0o10`. A leading zero is an error |
| `10u`, `unsigned` | unsigned types are planned |
| `long long`, `size_t`, `int32_t` | `long` / `int` |
| `int f() { if (c) return 1; }` | error: every path must return |
| signed overflow is UB | overflow, `/0`, bad shifts **stop the program** with file:line |
| `int x{5}`, `int x(5)` | `int x = 5;` for now |
| `new User("a")`, `delete u` on a class | `User("a")`; the object is freed automatically. `new` is for other values: `new Point{1, 2}` |
| `int* p = new int;` then read `*p` | it is 0: `new` always gives a default value |
| `#pragma`, `[[nodiscard]]`, `[[maybe_unused]]` | the one attribute is `[[allow(new_delete)]]` |
| `-g`, `-fsanitize=address,undefined`, valgrind for a memory bug | `cplus run app.cplus --debug`: memory errors stop the program with a stack trace, and leaks are reported at exit |
| `new (buffer) T`, `new int[3]{1, 2, 3}` | no placement new; set array elements afterward, in a loop |
| `this->x`, `u->name` | `this.x`, `u.name`; `->` is only for pointers, `T*` |
| `int Rect::area() { ... }` outside the type | write the method body inside the type |
| `User u;` then use `u` | error until assigned: a class reference is never empty. `User? u;` starts out null |
| `nullptr`, `NULL`, `std::optional<T>`, `std::nullopt` | `null` and `T?`; only an optional or a pointer can be null |
| `*opt`, `opt->name` | `opt.value()`, `opt?.name`, or unwrap with `if (auto u = opt)` |
| `int x = opt;`, `opt + 1` | error: unwrap first, e.g. `(opt ?? 0) + 1` |
| `if (flag)` on a `bool?` | error: write `flag == true` or `flag != null` |
| `opt?.age = 3` | error: `?.` only reads. Write `if (auto u = opt) u.age = 3;` for an object. For a struct, `u` is a copy, so store it back: `if (auto u = opt) { u.age = 3; opt = u; }` |
| `struct Node { Node next; }`, `Node? next` in a struct | a struct cannot hold itself, not even as an optional. Use `class Node { Node? next; };`, or a pointer, `Node* next = null;` |
| `const User u` freezes the object | `const` only stops `u` from referring to another object |
| `u1 == u2` compares two objects' fields | it is true only for the same object, like `===` in JS. Compare fields instead |
| `bool operator==(...) = default` | not needed: every struct already has `==` |
| several constructors | one per type for now; leave it out and use field defaults instead |
| `auto p = {1, 2};` | `auto p = Point{1, 2};`, since a bare `{ }` needs a known type |
| `int n = v.size();` | error: `size()` is a `long`, since there are no unsigned types. Write `long n` or `auto n`; `i < v.size()` with an `int i` is fine |
| `v[i]` past the end is UB | every index is checked and a bad one **stops the program** with file:line; so do `pop_back()`, `front()`, `back()` on an empty vector |
| `v.back().x = 1`, `v.back().set()`, `v.back().p.set()`, `m.at(k) = 1`, `(first ? a : b).set()` | error: `back()`, `front()`, `at()` and `?:` give copies. Write `v[v.size() - 1].x = 1`, `m[k] = 1`, `if (first) a.set(); else b.set();` |
| `v.push_back(...)` inside `for (x : v)` is UB | safe: the loop goes over `v` as it was when the loop started |
| `for (auto& x : v)`, `const vector<int>& v` | no references yet: `for (auto x : v)` and `vector<int> v` copy cheaply. Change elements with `v[i] = ...` |
| `void f(int& n)` | no references yet: `void f(int* n)`, called as `f(&x)` |
| `vector<int> v(5);`, `vector<int> v{1, 2};` | `auto v = vector<int>(5);`, `vector<int> v = {1, 2};` |
| `std::unordered_map<K, V>`, `std::map<K, V>` | `hashmap<K, V>`, in the order entries were added. Sorted maps are planned |
| `for (auto [k, v] : m)`, `for (const auto& [k, v] : m)` | structured bindings are planned: `for (auto e : m)`, then `e.first` and `e.second` |
| `nlohmann::json`, `JSON.parse(text)`, `JSON.stringify(x)` | `import json;`, then `try json::parse<User>(text)` or `try json::parse<json::Value>(text)`, and `json::stringify(x)` |
| `NLOHMANN_DEFINE_TYPE_...`, `to_json`, `from_json` | not needed: the compiler knows the fields of every type |
| `struct json::Mine { ... };`, `namespace json { ... }` | a module's names are its own, and a program cannot add to them: `struct Mine { ... };` |
| `fetch(url)`, `await fetch(url)`, `curl_easy_perform` | `import http;`, then `try await http::fetch(url)` in an async function, or `try http::get(url)`, which waits for the answer |
| `res.text()`, `res.json()`, `res.headers.get(x)` | `r.body`, `try json::parse<T>(r.body)`, `r.header(x)` |
| `http::get(url, options)`, `fetch(url, {method: "PUT"})` | one function per name for now: `try http::send({.method = "PUT", .url = url})`, or `try await http::fetchWith({.method = "PUT", .url = url})` |
| `new WebSocket(url)`, `ws.onmessage = ...` | `try http::connect(url)`, then a loop over `try ws.receive()`, which waits |
| `curl -k`, `verify=False` | certificates are always checked. Trust a server's own with `.caFile = "its-certificate.pem"` |
| `obj["a"]["b"]` on JSON | `obj.get("a")?.get("b")`, since any step may not be there |
| `m.find(k) != m.end()`, `it->second` | `m.contains(k)`, `m.get(k)`, which gives a `V?`. No iterators yet |
| `m.emplace(k, v)`, `m.insert_or_assign(k, v)` | `m.insert({k, v})`, `m[k] = v` |
| `m[k]` on a `const` map | error, as in C++, since it adds a missing key: `m.at(k)` or `m.get(k)` |
| `std::hash<Point>` for a struct key | not needed: a struct of hashable fields is hashable. Your own hash is planned |
| `v.emplace_back(x)`, `v.at(i)` | `v.push_back(x)`, `v[i]` (always checked) |
| `int a[5];`, `int a[5] = {1, 2};` | `array<int, 5> a;`, `array<int, 5> a = {1, 2};`. Every element starts with a value, 0 if none is given |
| `int a[n];` with a variable `n` | an array's size must be known while compiling: `constexpr int N = 5; array<int, N> a;`. For a size known only at run time, `auto a = vector<int>(n);` |
| `int* p = a;`, `f(a)` for a C array | an array does not turn into a pointer: `a.data()` |
| `std::array<int, 3> a{1, 2};`, `a.at(i)` | `array<int, 3> a = {1, 2};`, `a[i]` (always checked) |
| a huge local array, `array<int, 10000000> a;` | it is on the stack, like in C++, and running out of stack **stops the program**. Use a vector |
| `if (v)`, `if (!v)` | `if (!v.empty())`, `if (v.empty())`; a vector itself is not a condition |
| `s + 5`, `"abc" + 1` | error: `+` joins a string with a string or a `char` (`s + 'c'` works). Write `s + to_string(5)`; a number is never a character code |
| `char c = 65;`, `char c = n;` | error: write `'A'`, or `char(n)`, which stops the program unless `n` is from 0 to 255 |
| `if (c)` on a char | error: a char is not a condition. Write `c != '\0'` |
| `to_string(c)` on a char | error, since C++ gives the code. Write `string(1, c)`, or `to_string(int(c))` for the code |
| `'ab'`, `'é'`, `L'a'` | error: a char literal is one byte. Use a string, `"é"`, for anything longer |
| `s[s.size()]` reads `'\0'` | every `s[i]` is checked; past the end **stops the program**, as do `pop_back()`, `front()`, `back()` on an empty string |
| `std::stringstream`, `sprintf`, `snprintf` | `format("{}: {}", name, n)` returns the string |
| `size_t pos = s.find(t)` | `long pos = s.find(t)` or `auto pos`. `string::npos` is `-1`, so `pos != string::npos` and `pos >= 0` both work |
| `s.substr(pos)` past the end throws | it **stops the program** with file:line, as do a negative position and `s.find(t, -1)` |
| `s.at(i)`, `s.append(t)` | `s[i]` (always checked), `s += t` |
| overloaded functions | planned; one function per name for now: rename one |

## Planned. Where there is something to write for them, the compiler rejects it with "does not support ... yet"

| Feature | C+ form |
|---------|---------|
| Classes | `final`, covariant return types, private and protected base classes |
| Operators | operator overloading with C++ syntax, after function overloading |
| Generics | default template arguments, `requires` clauses, generic methods, `Stack(1)` working out its arguments |
| Modules | your own, `import net.server;` |
| HTTP | a body in pieces instead of all at once; keeping cookies; async WebSockets |
| JSON | other names for fields, marking a field to leave out |
| Reflection | reading a field through its `reflect::Field`, methods |
| Memory | pointers into vectors and strings |
| Types | enums, unions, unsigned integers, `const` fields, types nested in a type or a function, `short` and other small integers |
| Functions | overloading, default arguments, `auto` parameters and return types, lambdas |
| Statements | `switch` |
| Top level | variables outside functions, constants of other types, `constexpr` methods and structs |
| Containers | `insert`, `erase` and iterators on vectors; `find` on a hashmap; sorted maps and sets; `deque`, `list`, `stack`, `queue`, `pair`, `tuple` |
| References | `T&` parameters, `for (auto& x : v)` |
| Structured bindings | `for (auto [k, v] : m)`, `auto [a, b] = f();` |
| Format options | width and alignment, `{:x}`, `{:>8}`, numbered placeholders like `{0}` |
| More string methods | `append`, `insert`, `erase`, `replace`, `rfind`, `compare`; parsing numbers from strings |
| More C | variable arguments, vectors, C variables like `errno`, callbacks |
| Concurrency | waiting with a time limit, stopping a task, atomic values, condition variables, channels |

## Tools

```
cplus explain hashmap       # what the spec says on a topic, with an example that runs; no topic lists them
cplus check file.cplus      # errors only
cplus run file.cplus        # build and run
cplus build file.cplus -o app [-O] [--emit-llvm]
cplus fmt file.cplus        # rewrite it in the one style of C+; --check only says whether it is
cplus lsp                   # the language server an editor starts; editors/vscode is the extension for VS Code
cplus build app.cplus -lsqlite3 -L dir   # link C libraries, as with a C compiler; `import http;` needs no -l
cplus build app.cplus -I include         # another folder for `import <name.h>;`
cplus build app.cplus -Wno-new-delete    # no warning for new and delete
cplus run app.cplus --debug              # slower, but catches memory errors, with a stack trace
cplus run app.cplus --race               # slower, but stops at memory that threads use without a lock; not with --debug
```

Compiler errors say what is wrong, how to fix it, and which spec section explains the rule.
Read the `help:` line and follow it.
