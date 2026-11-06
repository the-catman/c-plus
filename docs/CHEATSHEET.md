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
| Casts | `int(x)`, `static_cast<int>(x)`, `(int)x` between numbers, `char` and `bool`. A fraction is dropped toward zero; a value that does not fit **stops the program**. `(int*)p`, `static_cast<int*>(p)` between pointer types |
| Math | `sqrt`, `pow`, `sin`, `floor`, `round`, `fmod`, `isnan`, `abs`, `min`, `max` and the rest of `<cmath>`, with no `std::` |
| Characters | `char c = 'a';`, escapes as in strings. `c + 1` and `c - '0'` are `int`s, as in C++; `char(n)` is checked. `c++`, `c += 2` stay chars. `isdigit`, `isalpha`, `isspace`, `toupper`, `tolower` and the rest of `<cctype>` (ASCII, return `bool` or `char`) |
| Structs | `struct P { int x; int y = 0; };` is a **value** that is copied. Build with `{1, 2}`, `{.y = 2}`, `P{1, 2}`, `P(1, 2)`, `{}` |
| Fields | `p.x`, `p.x += 1`. `Point p;` can be filled field by field before use |
| Classes | `class U {...}; U u = U("a");` is a **reference-counted object**: `U v = u;` shares it. No `new`; freed when the last reference goes away |
| Methods | written inside the type: `int area() const { return w * h; }`. Use fields by name or as `this.w` |
| Constructors | `U(string n) : name(n) {}`, one per type. `~U() {}` destructors for classes only |
| Access | `public:` / `private:`; class members are private by default, struct members public |
| Vectors | `vector<int> v = {1, 2};` is a **value**, copied like a struct (cheaply: copy-on-write). `vector<int> v;` is empty. `v.push_back(x)`, `v.pop_back()`, `v[i]`, `v.size()` (a `long`), `v.empty()`, `v.front()`, `v.back()`, `v.clear()` |
| Building vectors | `{1, 2, 3}`, `vector<int>{1, 2}`, `vector<int>(n)`, `vector<int>(n, value)`, `vector<vector<int>>` |
| Arrays | `array<int, 3> a;` (all 0), `array<int, 3> a = {1, 2};`, `array<int, 3>{1, 2}` are **values**, copied whole, with `N` written as a number. `a[i]` (checked), `a.size()` (a `long`), `a.empty()`, `a.front()`, `a.back()`, `a.fill(x)`, `a.data()`, `&a[i]`, `==`, `for (auto x : a)`. Prints as `[1, 2, 3]`. `array<array<int, 3>, 2>` for rows. `string(a)` reads the text in an `array<char, N>` |
| Strings | a **value**, like `std::string`. `a + b`, `s += t`, `s.size()` / `s.length()` (bytes, a `long`), `s.empty()`, `s.substr(pos, n)`, `s.find(t)` (`string::npos` when absent), `s.contains(t)`, `s.starts_with(t)`, `s.ends_with(t)`, `s.clear()`, `==` `<` `>` |
| String chars | `s[i]` and `s[i] = 'x'` (checked), `for (char c : s)`, `s + '!'`, `s += c`, `s.push_back(c)`, `s.pop_back()`, `s.front()`, `s.back()`, `string(3, '-')`, `s.find('/')` |
| Output | `println("x = {}", x)`, `print(...)`, `println()`. `{{` `}}` for braces. Structs print as `{x: 1, y: 2}`, objects as `U {name: "a"}`, vectors as `[1, 2]`. A double prints its shortest exact digits (`0.1`); `{:.2f}`, `{:e}`, `{:g}`, `{:.3}` choose them |
| Building text | `format("{} of {}", a, b)` returns the string `println` would print. `to_string(x)` for any number |
| Optionals | `User? u = find(name);` holds a `User` or `null`; `int? n;` starts out null. `u?.name` is null when `u` is, `u?.name ?? "nobody"` gives a fallback, `if (auto x = u)` runs with the `User` inside. `has_value()`, `value()`, `value_or(x)`, `u == null` |
| Pointers | `int* p = &x;`, `*p = 5`, `p->x`, `p[i]` (unchecked), `p + n`, `p - q`, `p++`, `const int*`, `int* const`, `void*`, `int**`, `null`, `if (p)`. Following a null pointer **stops the program**. `sizeof(T)`, `sizeof x` (a `long`). `string(p)` copies a `char*`'s text. A pointer prints as `0x...`. Run with `--debug` to catch overruns and use after free |
| Manual memory | `int* p = new int(5); delete p;`, `new Point{1, 2}`, `int* a = new int[n]; delete[] a;`. Values start as their default, and each `new`/`delete` **warns**. Turn it off with `[[allow(new_delete)]]` before a function, `[[allow(new_delete)]];` at the top of the file, or `-Wno-new-delete`. The wrong form of delete **stops the program** |
| C functions | `extern "C" int abs(int n);` or `extern "C" { ... }`, written with C+ types: `long strlen(string s);`, `string? getenv(string name);`, `void* malloc(long n);`, `double frexp(double x, int* e);`. Strings are converted at each call. Link other libraries with `-lname` |
| C types by name | `extern "C" struct FILE;`, then `FILE* f = fopen(...)`. Only pointers to it can be used |
| C structs | `extern "C" { struct ldiv_t { long quot; long rem; }; ldiv_t ldiv(long a, long b); tm* gmtime_r(const long* t, tm* out); }`. Fields are numbers, `char`, `bool`, pointers, such structs and arrays of those, in C's order. By value or through a pointer. Before C fills one: `tm t = {};` |
| Control flow | `if`/`else`, `while`, `do`/`while`, `for`, `for (auto x : v)` over a vector or an array, `break`, `continue`, `?:` |
| Operators | all C++ arithmetic, comparison and bit operators, `&&` `\|\|` `!`, compound assignment, `++`/`--`. `%` and bit operators take integers only |
| Equality | `==` `!=` on any two values of one type: strings by contents, structs field by field (automatic), vectors and arrays element by element, objects by identity. `opt == 3`, `opt == null` |
| Variables | `const`, `auto`, `int a = 1, b = 2;`, block scope, shadowing in inner blocks |
| Literals | `42`, `0xFF`, `0o755` (octal), `0b1010`, `1'000'000`, `1L` (long), `1.5`, `1e-9`, `2.5f` (float), `'a'`, `'\n'` (char) |
| Conditions | `bool`, a number, a pointer or an optional, as in C++: `if (n)`, `while (n--)`, `!n`, `if (p)` (not null), `if (opt)` (holds a value). Not a `char`: write `c != '\0'` |

## Different from C++. Do not write the C++ version

| C++ habit | C+ |
|-----------|----|
| `#include`, `#define` | no preprocessor. Built-ins need nothing; modules use `import x;`; C functions are declared with `extern "C"` |
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
| `int x;` then read `x` | compile error until `x` is assigned on every path |
| `int i = someLong;`, `int i = 2.5;` | error: a conversion that can lose information needs a cast: `int(x)`, which stops the program if `x` does not fit |
| `float f = someDouble;` | error: write `float(x)`. Only a double written as a number, `float f = 0.5;`, converts by itself |
| `reinterpret_cast`, `const_cast`, `dynamic_cast` | do not exist. `static_cast`, `int(x)` and `(int)x` convert numbers; `static_cast<int*>(p)` and `(int*)p` convert pointers. `const` cannot be cast away |
| `(long)p`, `(int*)0x1000`, `int* p = 0;` | error: numbers and pointers never convert. A pointer to nothing is `null` |
| `bool ok = p;` | error: write `p != null` or `bool(p)`. `if (p)` is fine |
| `int* a, b;` | error, since `b` would be a plain `int`. Write `int* a, * b;` or two declarations |
| `int*? p` | error: a pointer can already be null; write `int*` |
| `println("{}", charPtr)` | error: write `string(p)` for the text or `(void*)p` for the address |
| `&v[0]`, `s.data()` | pointers into vectors and strings are planned; `&a[0]` and `a.data()` work on an array |
| `std::sqrt(x)`, `#include <cmath>`, `M_PI` | `sqrt(x)`, with nothing to include. `acos(-1)` is π for now |
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
| `opt?.age = 3` | error: `?.` only reads. Write `if (auto u = opt) u.age = 3;` |
| `struct Node { Node next; }`, `Node? next` in a struct | a struct cannot hold itself, not even as an optional. Use `class Node { Node? next; };`, or a pointer, `Node* next = null;` |
| `const User u` freezes the object | `const` only stops `u` from referring to another object |
| `u1 == u2` compares two objects' fields | it is true only for the same object, like `===` in JS. Compare fields instead |
| `bool operator==(...) = default` | not needed: every struct already has `==` |
| several constructors | one per type for now; leave it out and use field defaults instead |
| `auto p = {1, 2};` | `auto p = Point{1, 2};`, since a bare `{ }` needs a known type |
| `int n = v.size();` | error: `size()` is a `long`, since there are no unsigned types. Write `long n` or `auto n`; `i < v.size()` with an `int i` is fine |
| `v[i]` past the end is UB | every index is checked and a bad one **stops the program** with file:line; so do `pop_back()`, `front()`, `back()` on an empty vector |
| `v.push_back(...)` inside `for (x : v)` is UB | safe: the loop goes over `v` as it was when the loop started |
| `for (auto& x : v)`, `const vector<int>& v` | no references yet: `for (auto x : v)` and `vector<int> v` copy cheaply. Change elements with `v[i] = ...` |
| `void f(int& n)` | no references yet: `void f(int* n)`, called as `f(&x)` |
| `vector<int> v(5);`, `vector<int> v{1, 2};` | `auto v = vector<int>(5);`, `vector<int> v = {1, 2};` |
| `v.emplace_back(x)`, `v.at(i)` | `v.push_back(x)`, `v[i]` (always checked) |
| `int a[5];`, `int a[5] = {1, 2};` | `array<int, 5> a;`, `array<int, 5> a = {1, 2};`. Every element starts with a value, 0 if none is given |
| `int a[n];`, `array<int, N>` with a named `N` | write the number: `array<int, 5>`. For a size known only at run time, `auto a = vector<int>(n);` |
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
| overloaded functions | planned; one function per name for now |

## Planned. The compiler rejects these today with "does not support ... yet"

| Feature | C+ form |
|---------|---------|
| Inheritance | `class Admin : User {...}`, multiple inheritance, `virtual`, `override` |
| Operators | operator overloading with C++ syntax, after function overloading |
| Errors | `int f() throws`, `throw Error("..")`, `try f()`, `try { } catch (Error e) { }` |
| Generics | `template<typename T>`, `template<Comparable T>`; no specialization or SFINAE |
| Modules | `import net.server;`, `http::get(url)` |
| Memory | `weak` to break reference cycles, `unique<T>`, `local<T>`; pointers into vectors and strings |
| Containers | `hashmap<K, V>`; `insert`, `erase` and iterators on vectors; array sizes given by a name |
| References | `T&` parameters, `for (auto& x : v)` |
| Format options | width and alignment, `{:x}`, `{:>8}` |
| More string methods | `append`, `insert`, `erase`, `replace`, `rfind`, `compare`; parsing numbers from strings |
| Concurrency | `spawn f()` → `Task<T>`; later `async`/`await` |

## Tools

```
cplus check file.cplus      # errors only
cplus run file.cplus        # build and run
cplus build file.cplus -o app [-O] [--emit-llvm]
cplus build app.cplus -lsqlite3 -L dir   # link C libraries, as with a C compiler
cplus build app.cplus -Wno-new-delete    # no warning for new and delete
cplus run app.cplus --debug              # slower, but catches memory errors, with a stack trace
```

Compiler errors say what is wrong, how to fix it, and which spec section explains the rule.
Read the `help:` line and follow it.
