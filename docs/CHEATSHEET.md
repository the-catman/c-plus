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
| Types | `int` (32-bit), `long` (64-bit), `bool`, `string`, `void`, `auto`, structs, classes, `vector<T>`, `T?` |
| Structs | `struct P { int x; int y = 0; };` is a **value** that is copied. Build with `{1, 2}`, `{.y = 2}`, `P{1, 2}`, `P(1, 2)`, `{}` |
| Fields | `p.x`, `p.x += 1`. `Point p;` can be filled field by field before use |
| Classes | `class U {...}; U u = U("a");` is a **reference-counted object**: `U v = u;` shares it. No `new`; freed when the last reference goes away |
| Methods | written inside the type: `int area() const { return w * h; }`. Use fields by name or as `this.w` |
| Constructors | `U(string n) : name(n) {}`, one per type. `~U() {}` destructors for classes only |
| Access | `public:` / `private:`; class members are private by default, struct members public |
| Vectors | `vector<int> v = {1, 2};` is a **value**, copied like a struct (cheaply: copy-on-write). `vector<int> v;` is empty. `v.push_back(x)`, `v.pop_back()`, `v[i]`, `v.size()` (a `long`), `v.empty()`, `v.front()`, `v.back()`, `v.clear()` |
| Building vectors | `{1, 2, 3}`, `vector<int>{1, 2}`, `vector<int>(n)`, `vector<int>(n, value)`, `vector<vector<int>>` |
| Strings | a **value**, like `std::string`. `a + b`, `s += t`, `s.size()` / `s.length()` (bytes, a `long`), `s.empty()`, `s.substr(pos, n)`, `s.find(t)` (`string::npos` when absent), `s.contains(t)`, `s.starts_with(t)`, `s.ends_with(t)`, `s.clear()`, `==` `<` `>` |
| Output | `println("x = {}", x)`, `print(...)`, `println()`. `{{` `}}` for braces. Structs print as `{x: 1, y: 2}`, objects as `U {name: "a"}`, vectors as `[1, 2]` |
| Building text | `format("{} of {}", a, b)` returns the string `println` would print. `to_string(n)` for an `int` or `long` |
| Optionals | `User? u = find(name);` holds a `User` or `null`; `int? n;` starts out null. `u?.name` is null when `u` is, `u?.name ?? "nobody"` gives a fallback, `if (auto x = u)` runs with the `User` inside. `has_value()`, `value()`, `value_or(x)`, `u == null` |
| C functions | `extern "C" int abs(int n);` or `extern "C" { ... }`, written with C+ types: `long strlen(string s);`, `string? getenv(string name);`. Strings are converted at each call. Link other libraries with `-lname` |
| Control flow | `if`/`else`, `while`, `do`/`while`, `for`, `for (auto x : v)`, `break`, `continue`, `?:` |
| Operators | all C++ integer operators, `&&` `\|\|` `!`, compound assignment, `++`/`--` |
| Equality | `==` `!=` on any two values of one type: strings by contents, structs field by field (automatic), vectors element by element, objects by identity. `opt == 3`, `opt == null` |
| Variables | `const`, `auto`, `int a = 1, b = 2;`, block scope, shadowing in inner blocks |
| Literals | `42`, `0xFF`, `0o755` (octal), `0b1010`, `1'000'000`, `1L` (long) |
| Conditions | `bool`, a number or an optional, as in C++: `if (n)`, `while (n--)`, `!n`, `if (opt)` (holds a value) |

## Different from C++. Do not write the C++ version

| C++ habit | C+ |
|-----------|----|
| `#include`, `#define` | no preprocessor. Built-ins need nothing; modules use `import x;`; C functions are declared with `extern "C"` |
| `extern "C" size_t strlen(const char* s);` | `extern "C" long strlen(string s);`: C+ types in C declarations. `const char*` is `string` (or `string?` for a result that may be NULL), `size_t` is `long`, `unsigned` is `int` |
| `extern "C" int printf(const char*, ...);` | variable arguments are not supported; use `println` |
| `std::cout << x`, `printf` | `println("{}", x)` |
| `std::vector<int>`, `std::string`, `std::to_string` | `vector<int>`, `string`, `to_string`, with no `std::` |
| `std::`, `using namespace std` | no `std::`; built-ins have no prefix |
| prototypes / headers | none. Define each function once, in any order |
| `int x;` then read `x` | compile error until `x` is assigned on every path |
| `int i = someLong;` | error: only `int`→`long` converts implicitly |
| `(long)x`, `static_cast<long>(x)` | casts are planned; `int` to `long` and number to `bool` need none |
| `int n = true;` | error: `bool` does not convert to a number (numbers do convert to `bool`) |
| `010` (octal) | `0o10`. A leading zero is an error |
| `10u`, `1.5f` | unsigned and floating point are planned |
| `long long`, `size_t`, `int32_t` | `long` / `int` |
| `int f() { if (c) return 1; }` | error: every path must return |
| signed overflow is UB | overflow, `/0`, bad shifts **stop the program** with file:line |
| `int x{5}`, `int x(5)` | `int x = 5;` for now |
| `new User("a")`, `delete u` | `User("a")`; the object is freed automatically |
| `this->x`, `u->name` | `this.x`, `u.name`; `->` is only for raw pointers |
| `int Rect::area() { ... }` outside the type | write the method body inside the type |
| `User u;` then use `u` | error until assigned: a class reference is never empty. `User? u;` starts out null |
| `nullptr`, `NULL`, `std::optional<T>`, `std::nullopt` | `null` and `T?`; only an optional can be null |
| `*opt`, `opt->name` | `opt.value()`, `opt?.name`, or unwrap with `if (auto u = opt)` |
| `int x = opt;`, `opt + 1` | error: unwrap first, e.g. `(opt ?? 0) + 1` |
| `if (flag)` on a `bool?` | error: write `flag == true` or `flag != null` |
| `opt?.age = 3` | error: `?.` only reads. Write `if (auto u = opt) u.age = 3;` |
| `struct Node { Node* next; }` | `class Node { Node? next; };`. A struct cannot hold itself, not even as an optional |
| `const User u` freezes the object | `const` only stops `u` from referring to another object |
| `u1 == u2` compares two objects' fields | it is true only for the same object, like `===` in JS. Compare fields instead |
| `bool operator==(...) = default` | not needed: every struct already has `==` |
| several constructors | one per type for now; leave it out and use field defaults instead |
| `auto p = {1, 2};` | `auto p = Point{1, 2};`, since a bare `{ }` needs a known type |
| `int n = v.size();` | error: `size()` is a `long`, since there are no unsigned types. Write `long n` or `auto n`; `i < v.size()` with an `int i` is fine |
| `v[i]` past the end is UB | every index is checked and a bad one **stops the program** with file:line; so do `pop_back()`, `front()`, `back()` on an empty vector |
| `v.push_back(...)` inside `for (x : v)` is UB | safe: the loop goes over `v` as it was when the loop started |
| `for (auto& x : v)`, `const vector<int>& v` | no references yet: `for (auto x : v)` and `vector<int> v` copy cheaply. Change elements with `v[i] = ...` |
| `vector<int> v(5);`, `vector<int> v{1, 2};` | `auto v = vector<int>(5);`, `vector<int> v = {1, 2};` |
| `v.emplace_back(x)`, `v.at(i)` | `v.push_back(x)`, `v[i]` (always checked) |
| `int a[5];` | `vector<int> a = vector<int>(5);`; `array<T, N>` is planned |
| `if (v)`, `if (!v)` | `if (!v.empty())`, `if (v.empty())`; a vector itself is not a condition |
| `s + 5`, `s + 'c'`, `"abc" + 1` | error: `+` only joins two strings. Write `s + to_string(5)`; a number is never a character code |
| `std::stringstream`, `sprintf`, `snprintf` | `format("{}: {}", name, n)` returns the string |
| `size_t pos = s.find(t)` | `long pos = s.find(t)` or `auto pos`. `string::npos` is `-1`, so `pos != string::npos` and `pos >= 0` both work |
| `s.substr(pos)` past the end throws | it **stops the program** with file:line, as do a negative position and `s.find(t, -1)` |
| `s[i]`, `for (char c : s)`, `'a'`, `s.push_back(c)` | planned with `char`. Use `s.substr(i, 1)` for one character and `s += "x"` to append |
| `s.append(t)` | `s += t` |
| overloaded functions | planned; one function per name for now |

## Planned. The compiler rejects these today with "does not support ... yet"

| Feature | C+ form |
|---------|---------|
| Inheritance | `class Admin : User {...}`, multiple inheritance, `virtual`, `override` |
| Operators | operator overloading with C++ syntax, after function overloading |
| Errors | `int f() throws`, `throw Error("..")`, `try f()`, `try { } catch (Error e) { }` |
| Generics | `template<typename T>`, `template<Comparable T>`; no specialization or SFINAE |
| Modules | `import net.server;`, `http::get(url)` |
| Memory | `weak` to break reference cycles, `unique<T>`, `local<T>`; raw `T*`, `new`/`delete` warn |
| Containers | `array<T, N>`, `hashmap<K, V>`; `insert`, `erase` and iterators on vectors |
| References | `T&` parameters, `for (auto& x : v)` |
| Characters | `char`, `'a'`, `s[i]`, `for (char c : s)`, `s.push_back(c)` |
| More string methods | `append`, `insert`, `erase`, `replace`, `rfind`, `compare`; parsing numbers from strings |
| Concurrency | `spawn f()` → `Task<T>`; later `async`/`await` |

## Tools

```
cplus check file.cplus      # errors only
cplus run file.cplus        # build and run
cplus build file.cplus -o app [-O] [--emit-llvm]
cplus build app.cplus -lsqlite3 -L dir   # link C libraries, as with a C compiler
```

Compiler errors say what is wrong, how to fix it, and which spec section explains the rule.
Read the `help:` line and follow it.
