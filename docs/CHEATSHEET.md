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
| Types | `int` (32-bit), `long` (64-bit), `bool`, `string`, `void`, `auto`, structs, classes |
| Structs | `struct P { int x; int y = 0; };` is a **value** that is copied. Build with `{1, 2}`, `{.y = 2}`, `P{1, 2}`, `P(1, 2)`, `{}` |
| Fields | `p.x`, `p.x += 1`. `Point p;` can be filled field by field before use |
| Classes | `class U {...}; U u = U("a");` is a **reference-counted object**: `U v = u;` shares it. No `new`; freed when the last reference goes away |
| Methods | written inside the type: `int area() const { return w * h; }`. Use fields by name or as `this.w` |
| Constructors | `U(string n) : name(n) {}`, one per type. `~U() {}` destructors for classes only |
| Access | `public:` / `private:`; class members are private by default, struct members public |
| Output | `println("x = {}", x)`, `print(...)`, `println()`. `{{` `}}` for braces. Structs print as `{x: 1, y: 2}`, objects as `U {name: "a"}` |
| Control flow | `if`/`else`, `while`, `do`/`while`, `for`, `break`, `continue`, `?:` |
| Operators | all C++ integer operators, `&&` `\|\|` `!`, compound assignment, `++`/`--` |
| Variables | `const`, `auto`, `int a = 1, b = 2;`, block scope, shadowing in inner blocks |
| Literals | `42`, `0xFF`, `0o755` (octal), `0b1010`, `1'000'000`, `1L` (long) |
| Conditions | `bool` or a number, as in C++: `if (n)`, `while (n--)`, `!n` |

## Different from C++. Do not write the C++ version

| C++ habit | C+ |
|-----------|----|
| `#include`, `#define` | no preprocessor. Built-ins need nothing; modules use `import x;` |
| `std::cout << x`, `printf` | `println("{}", x)` |
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
| `User u;` then use `u` | error until assigned: a class reference is never empty |
| `const User u` freezes the object | `const` only stops `u` from referring to another object |
| several constructors | one per type for now; leave it out and use field defaults instead |
| `auto p = {1, 2};` | `auto p = Point{1, 2};`, since a bare `{ }` needs a known type |
| overloaded functions | planned; one function per name for now |

## Planned. The compiler rejects these today with "does not support ... yet"

| Feature | C+ form |
|---------|---------|
| Inheritance | `class Admin : User {...}`, multiple inheritance, `virtual`, `override` |
| Operators | `==` on structs and objects; operator overloading |
| Optionals | `User?`, `null`, `if (auto u = opt)`, `opt?.name`, `opt ?? fallback` |
| Errors | `int f() throws`, `throw Error("..")`, `try f()`, `try { } catch (Error e) { }` |
| Generics | `template<typename T>`, `template<Comparable T>`; no specialization or SFINAE |
| Modules | `import net.server;`, `http::get(url)` |
| Memory | `weak`, `unique<T>`, `local<T>`; raw `T*`, `new`/`delete` warn |
| Containers | `vector<T>`, `array<T, N>`, `hashmap<K, V>`, bounds-checked |
| Concurrency | `spawn f()` → `Task<T>`; later `async`/`await` |

## Tools

```
cplus check file.cplus      # errors only
cplus run file.cplus        # build and run
cplus build file.cplus -o app [-O] [--emit-llvm]
```

Compiler errors say what is wrong, how to fix it, and which spec section explains the rule.
Read the `help:` line and follow it.
