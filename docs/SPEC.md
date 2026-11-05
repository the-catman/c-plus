# C+ Language Specification

Draft v0.1. This document is written to be read by people and by language models that
have never seen C+. Compiler errors link to its sections, so heading names are stable:
rename one only together with `compiler/SpecRefs.h`.

C+ is C++-shaped. **Where C+ and C++ mean the same thing, the syntax is identical.** This
spec spends its words on the places where they differ. Each section says whether the
compiler implements it yet; [Implementation status](#implementation-status) has the summary.

---

## Lexical structure

### Source files

A C+ source file is UTF-8 text with the extension `.cplus`. Non-ASCII characters may appear
only inside strings and comments.

### Comments

`// to end of line` and `/* block */`, as in C++. Block comments do not nest.

### No preprocessor

There is no preprocessor. `#include`, `#define`, `#ifdef` and every other `#` line are errors.
- Built-ins (`println`, `string`, `vector`, ...) need no include.
- Other code is brought in with `import` (see [Modules](#modules)).
- Use a `const` variable or a function where C++ would use `#define`.

### Keywords

Reserved, including words for features not implemented yet:

```
async auto await bool break case catch char class const constexpr continue default delete
do double else enum extern false float for if import int interface long namespace new null
operator override private protected public return sizeof spawn static string struct switch
template this throw throws true try typename virtual void weak while
```

### Integer literals

- Decimal `42`, hexadecimal `0xFF`, octal `0o755`, binary `0b1010`.
- `'` separates digits for readability: `1'000'000`.
- **A leading zero is an error** (`010`). C and C++ silently read `010` as octal 8, a classic
  bug when numbers are padded to line up. Write `0o10` for octal or `10` for decimal.
  JavaScript, Python, Rust and Swift use the same `0o` prefix.
- A literal is an `int` if it fits in 32 bits, otherwise a `long`. `-2147483648` is an `int`.
- The suffix `L` (or `LL`) makes a literal a `long`: `1L << 40`. Both mean the same thing,
  because `long` is always 64 bits. `u` for unsigned arrives with unsigned types. No other
  suffixes exist.

### String literals

`"text"` with escapes `\n \t \r \0 \\ \" \'` and `\xHH`. Adjacent literals are joined:
`"a" "b"` is `"ab"`. A string cannot span lines. Character literals (`'a'`) are planned.

---

## Types

### Integer types

| Type | Size | Notes |
|------|------|-------|
| `int` | 32-bit signed | same on every platform |
| `long` | 64-bit signed | same on every platform; there is no `long long` |

There are no unsigned types yet and no `size_t`, `int32_t` and so on. Use `int` or `long`.
Arithmetic never wraps silently; see [Integer overflow](#integer-overflow).

### bool

`true` or `false`. As in C++, a number converts to `bool` implicitly: zero is `false`,
anything else is `true`. So `bool b = 1;` works, and so does `if (count)` (see
[Conditions](#conditions)). A `bool` does not convert back to a number: `int n = true;`
is an error.

### string

A string is a value, as in C++: assigning or passing one gives an independent copy, and
`string s;` is empty. It holds bytes, which are UTF-8 text by convention. As in C++,
`size()` counts bytes and positions are byte positions, so `"é".size()` is 2.

```cpp
string name = "Ada";
string greeting = "Hello, " + name + "!";   // + joins two strings
greeting += " Welcome.";                    // += appends
long n = greeting.size();                   // a long, like a vector's size
string word = greeting.substr(7, 3);        // "Ada": 3 bytes from position 7
if (greeting.find("Ada") != string::npos) println("found");
string line = format("{} is {}", name, 36); // built the way println prints
string digits = to_string(42);
```

- `a + b` joins two strings, and `s += t` appends. Both sides must be strings: `s + 5`,
  which C++ compiles as adding the character with code 5, is an error. Write
  `s + to_string(5)` or `format("{}{}", s, 5)`. Two literals can be joined: `"a" + "b"`.
- `==` and `!=` compare contents, and `<`, `<=`, `>`, `>=` compare bytes in order, as
  C++ does. For UTF-8 text this is the order of the code points, so `"Z" < "a"`.
- `size()` and `length()` give the number of bytes, and `empty()` is `size() == 0`.
- `substr(pos, count)` is the part of the string from `pos`, at most `count` bytes long.
  `substr(pos)` and a `count` that runs past the end both take the rest. A `pos` past the
  end, which throws in C++, stops the program; so does a negative `pos`, or a negative
  `count` other than `string::npos`.
- `find(text)` and `find(text, pos)` return the position of the first match at or after
  `pos`, or `string::npos` when there is none. With no unsigned types, `string::npos` is
  `-1`, a `long`, so both `pos != string::npos` and `pos >= 0` work. A `pos` past the end
  finds nothing, as in C++; a negative `pos` stops the program.
- `contains(text)`, `starts_with(text)` and `ends_with(text)` return a `bool`.
- `clear()` empties the string.
- `to_string(n)` turns an `int` or a `long` into its digits. `format("...", values...)`,
  C++20's `std::format`, builds a string with the rules of [Printing](#printing):
  `format("{}", true)` is `"true"`, and `format("{}", user)` describes an object.

Copies of a string, and large substrings, share their bytes instead of copying them, and
`+=` adds to a string in place when no other string shares it, so building a string in a
loop takes linear time. See [Memory model](#memory-model).

Planned: `s[i]`, `for (char c : s)` and the methods that take or return one character
(`push_back`, `front`, `back`, `at`), which need the `char` type; `append`, `insert`,
`erase`, `replace`, `rfind` and `compare`; and parsing numbers out of strings.

### Implicit conversions

There are exactly two implicit conversions:
- **`int` to `long`**, which never loses information.
- **a number to `bool`**: zero is `false`, anything else is `true`, as in C++.

Everything else is an error, including `long` to `int` and `bool` to a number. Explicit
casts are planned.

```cpp
long big = 5;       // ok: int widens to long
bool flag = 1;      // ok: true
int small = big;    // error: a long may not fit in an int
int n = true;       // error: bool does not convert to a number
```

In arithmetic with mixed types, the `int` side is widened: `int + long` is a `long`.

### Optionals

Planned. `T?` holds a `T` or `null`. Plain types are never null.

```cpp
User? found = findUser("bob");
if (auto u = found) println("{}", u.name);  // u is a plain User inside the if
string name = found?.name ?? "nobody";
```

### Structs

A `struct` is a **value type**: assigning or passing one copies it, like a C struct. This
differs from C++ only in what is missing so far (see the end of this section).

```cpp
struct Point {
    int x;
    int y = 0;                  // a default, used when no value is given
};                              // a definition ends with `};`, as in C++

Point a = {1, 2};               // fields in order
Point b = {.y = 5, .x = 3};     // fields by name, in any order
auto c = Point{1, 2};           // with the type named
auto d = Point(1, 2);           // the same as Point{1, 2}
Point e = {};                   // every field gets its default
a.x += 10;                      // fields are read and written with '.'
Point f = a;                    // a copy: changing f leaves a alone
```

- A missing field gets the default written in the struct, or else `0`, `false`, `""` or an
  empty vector.
  A nested struct field gets its own defaults. A default is evaluated each time a value is
  built, and it cannot read other fields; use a constructor for that.
- A `{ }` list without a type name takes its type from where it is used: the type of the
  variable it initializes, the target of an assignment, the return type, or the parameter
  it is passed to. `auto p = {1, 2};` is an error; write `auto p = Point{1, 2};`.
- One list cannot mix values by position with values by name.
- A struct can be used before its definition. There are no forward declarations.
- A struct can contain other structs, but not itself, except through a vector:
  `struct Tree { int value; vector<Tree> kids; };` works.
- A struct can have methods and a constructor; see [Methods](#methods). Its members are
  public unless marked `private:`, as in C++.
- A struct that holds a class reference copies the reference, not the object, so both
  copies share that object.
- `==` and `!=` work on every struct with no code to write: two structs are equal when
  each pair of fields is equal, compared in order. C++20 needs
  `bool operator==(const Point&) const = default;` for this; C+ does it for you. Strings
  inside compare by contents and class references by identity (see [Classes](#classes)).
- Not yet: destructors for structs.

### Classes

A `class` is a **reference type**, the largest difference from C++. A class object lives
on its own. Variables hold references to it, and copying a variable copies the reference,
so both names see the same object. The object is freed the moment its last reference goes
away, and its destructor runs then (see [Memory model](#memory-model)).

```cpp
class User {
public:
    string name;
    int age = 0;

    User(string n, int a) : name(n), age(a) {}    // constructor
    ~User() { println("bye {}", name); }           // destructor
    void birthday() { age += 1; }
};

User a = User("Alice", 30);    // created without `new`
User b = a;                    // b and a are the same object
b.birthday();
println("{}", a.age);          // 31
println("{}", a);              // User {name: "Alice", age: 31}
```

- An object is created by calling the class like a function, `User("Alice", 30)`, or
  with a `{ }` list, `User{"Alice", 30}`. There is no `new`.
- A class without a constructor is built from its fields, like a struct:
  `Counter{.count = 1}`. With a constructor, the values are its arguments, in order.
- Fields and methods are reached with `.`, also through `this`: `this.age`. The `->`
  operator is only for raw pointers.
- **A class reference is never empty.** `User u;` declares a variable that must be
  assigned before it is read (see [Definite assignment](#definite-assignment)). For the
  same reason, a class-typed field must get a value whenever an object is built: from the
  `{ }` list, from the constructor, or from a default written in the class. A class that
  needs an existing object of its own type to be built, such as a list node with a
  `Node next` field, cannot be built until references that can be empty (`Node?`) arrive
  with [Optionals](#optionals).
- `const User u = ...;` stops `u` from referring to another object. The object itself can
  still change, as with `const` in JavaScript.
- Members are private by default in a class and public in a struct, as in C++. `public:`
  and `private:` work as in C++. `protected:` acts as `private:` until inheritance arrives.
- A class has at most one constructor until overloading arrives. To make values optional,
  leave out the constructor and give the fields defaults.
- `a == b` is true when `a` and `b` refer to the same object, like `===` on objects in
  JavaScript or `==` on `shared_ptr` in C++. Two objects with equal fields are still two
  objects, so `User("Ann") == User("Ann")` is false. To compare contents, compare the
  fields. Classes will be able to define their own `==` with operator overloading.
- Not yet: inheritance. Multiple inheritance will follow
  C++ default semantics: a diamond holds two copies of the shared base, and ambiguous
  members are qualified as `d.B::speak()`. There will be no virtual inheritance.

### Methods

Structs and classes can have methods, a constructor and, for classes, a destructor. They
are written inside the type, as in C++. There are no out-of-line definitions such as
`int Rect::area() { ... }`, because C+ has no headers to declare them in.

```cpp
struct Rect {
    int w;
    int h;
    int area() const { return w * h; }     // const: only reads fields
    void scale(int k) {                     // may change fields
        w *= k;
        this.h *= k;
    }
};

Rect r = {2, 3};
r.scale(2);
println("{}", r.area());    // 24
```

- Inside a method, fields and other methods can be used by name (`w`, `area()`) or
  through `this` (`this.w`, `this.area()`). `this` refers to the current object, and
  `this->w` is an error.
- A `const` method cannot assign fields or call methods that are not `const`. A method
  that is not `const` cannot be called on a `const` struct variable.
- A struct method works on the struct in place: `r.scale(2)` changes `r`. Called on a
  temporary, as in `makeRect().scale(2)`, it changes a copy that is then dropped.
- A method cannot share its name with a field. Two methods cannot share a name until
  overloading arrives.

A **constructor** is written as in C++: the type's name, the parameters, an optional
initializer list, and a body.

```cpp
class Account {
public:
    string owner;
    int balance = 0;
    Account(string o, int start) : owner(o), balance(start) {
        println("opened for {}", owner);
    }
};
```

- Before the body runs, every field gets its value from the initializer list, else its
  default, else zero (`0`, `false`, `""`, an empty vector). Fields are initialized in the order they are
  declared, whatever the order of the list, as in C++.
- A class-typed field without a default must be set by the end of the constructor, in the
  initializer list or in the body. `this` cannot be used until every such field is set.
- When a type has a constructor, every call or `{ }` list that builds it runs the
  constructor, with the values as its arguments in order. Field names (`.x = 1`) cannot be
  used then.
- A field whose type is a struct with a constructor, when not given a value, gets that
  struct's field defaults. The struct's constructor runs only when it is called.

A **destructor** is written `~Name() { ... }` and exists only for classes. It runs when
the object's last reference goes away. Then the object's fields are released, last
declared first. A destructor must not store `this` anywhere that outlives it; doing so
stops the program with an error.

### Vectors

`vector<T>` is a growable array of `T`s with the C++ method names. Like a struct, a vector
is a **value**: assigning or passing one copies it, and the copy is independent, as with
`std::vector`.

```cpp
vector<int> v = {3, 1, 4};          // `vector<int> v;` is an empty one
v.push_back(1);
v[0] = 30;                          // elements are numbered from 0
println("{} {}", v, v.size());      // [30, 1, 4, 1] 4
for (int x : v) total += x;         // see Range-based for loops

auto grid = vector<vector<int>>(3, vector<int>(3, 0));
grid[1][2] = 5;                     // `>>` closes two vectors, as in C++11
```

Building a vector:
- `vector<int> v;` is empty, as in C++. A vector field with no default starts empty too.
- `{1, 2, 3}` lists the elements and takes its type from where it is used, like a struct's
  `{ }` list. `vector<int>{1, 2, 3}` names the type; `vector<int>{}` and `vector<int>()`
  are empty.
- `vector<int>(n)` holds `n` default values: `0`, `false`, `""`, empty vectors, or structs
  built from their field defaults (or by their constructor, if it takes no arguments). A
  class reference cannot be empty, so `vector<User>(n)` is an error.
- `vector<int>(n, value)` holds `n` copies of `value`. For a class, all `n` refer to the one
  object.

| Method | What it does |
|--------|--------------|
| `v.size()` | the number of elements, as a `long` |
| `v.empty()` | whether there are no elements |
| `v.push_back(x)` | adds `x` at the end |
| `v.pop_back()` | removes the last element |
| `v.front()`, `v.back()` | a copy of the first or the last element |
| `v.clear()` | removes every element |

- `size()` returns a `long`, because C+ has no unsigned types. `i < v.size()` works with an
  `int i`, since `int` widens to `long`, but `int n = v.size();` is an error; write
  `long n` or `auto n`.
- `v[i]` takes an `int` or a `long`. An element can be read, assigned (`v[i] = x`,
  `v[i] += 1`, `v[i]++`) and used in place: `points[i].x = 1`, `grid[i].push_back(x)`,
  `points[i].move(1)`.
- **Every index is checked.** An index below 0 or at least `size()`, and `pop_back()`,
  `front()` or `back()` on an empty vector, stop the program with an error naming the file
  and line. C++ leaves all of these undefined. A negative literal index such as `v[-1]` is
  a compile error.
- `front()` and `back()` return a copy. To change the last element, write
  `v[v.size() - 1] = x`.
- `==` and `!=` compare the sizes and then each pair of elements, in order. Ordering with
  `<` is planned.
- A vector prints as `[1, 2, 3]`, with strings in quotes: `["Ada", "Linus"]`.
- A `const` vector cannot change at all, like a `const` struct: neither its elements nor
  its size.
- `push_back`, `pop_back` and `clear` cannot be called on a temporary, such as
  `makeList().push_back(1)`, because the change would be lost.
- A vector is not a [condition](#conditions); write `!v.empty()`.
- Not yet: `insert`, `erase`, `resize`, `reserve`, iterators (`begin()`, `end()`), the
  fixed-size `array<T, N>`, and `hashmap<K, V>`.

**Copies are cheap.** The copies of a vector share one buffer of elements until one of them
changes; that copy first gets a buffer of its own (copy-on-write). Passing or returning a
vector therefore costs about as much as passing a class reference, so there is no need for
`const vector<int>&` parameters, which C+ does not support. See
[Memory model](#memory-model).

---

## Declarations

### Variables

```cpp
int count = 0;
auto total = count + 1;   // type inferred from the initializer
const long limit = 100;   // cannot be assigned again
int later;                // allowed, but must be assigned before it is read
int a = 1, b = a + 1;     // several variables in one declaration
```

- In a declaration of several variables, each initializer may use the ones before it.
  With `auto`, each variable takes the type of its own initializer.
- `auto` and `const` variables need an initializer.
- A name may be declared once per scope. An inner block may shadow an outer name, as in C++.
- Brace and constructor initializers (`int x{5}`, `int x(5)`) are planned.

### Definite assignment

Reading a variable before it has been given a value is a **compile error**, checked on
every path through the code.

```cpp
int x;
if (ready) x = 1;
println("{}", x);         // error: x has no value when ready is false

int y;
if (ready) y = 1; else y = 2;
println("{}", y);         // ok: assigned on both paths
```

The rules:
- An `if` assigns a variable only if both branches do.
- A `while` or `for` body may run zero times, so its assignments do not count after the
  loop, unless the loop is `while (true)` / `for (;;)` and is left only by `break`.
- A `do`-`while` body always runs once, so its assignments count.
- A struct can be filled in field by field: after `Point p; p.x = 1; p.y = 2;` all of `p`
  has a value. A field can be read once it has a value, and the whole struct once every
  field has one.
- A declaration without a value gives strings `""`, vectors no elements, and struct fields
  their defaults, as in C++. Numbers, bools and class references get no value, so reading
  one before assigning it is an error.
- An element of a vector always has a value.
- In a constructor, the class-typed fields of `this` are tracked the same way: each must
  be set before the constructor ends.
- In `a && b` and `a || b`, assignments inside `b` do not count, since `b` may not run.
- Code after `return`, `break` or `continue` is unreachable and not checked.

### Functions

```cpp
long square(long n) {
    return n * n;
}
```

- **No headers and no forward declarations.** A function can be called anywhere in its
  file, before or after its definition. Writing a prototype like `int f(int x);` is an
  error.
- Every path through a non-`void` function must `return` a value. Falling off the end is
  a compile error, not undefined behavior.
- A `void` function cannot return a value.
- Arguments are converted to parameter types under the
  [implicit conversion](#implicit-conversions) rules.
- Overloading (several functions with one name) is planned. Until then, each name is
  defined once.
- User functions never collide with C library symbols.

### The main function

Every program has exactly one `int main()`. Its return value is the process exit code.
Falling off the end of `main` returns 0, as in C++. Command-line arguments are planned.

### Modules

Partly planned. There are no header files. One file is one module.

```cpp
import http;          // a standard module
import net.server;    // the file net/server.cplus, relative to the project root
```

Members of a module are used with `::`, as in `http::get(url)`. There is no `std::`
namespace and no `using`; built-ins need no prefix. The compiler currently accepts single-file
programs only and reports an error for `import`.

Nobody writes header files. For a library, the compiler can generate a `.hplus` interface
file from a module: a readable summary of its public API, with declarations but no
function bodies. It also lets a library ship compiled, without its source. `.hplus` files
are always generated, never hand-written.

---

## Statements

### Control flow

`if`/`else`, `while`, `do`/`while`, `for`, `break`, `continue` and `return` work exactly as
in C++. The step of a `for` loop may list several expressions: `for (...; ...; i++, j--)`.
`switch` is planned.

#### Conditions

The condition of `if`, `while`, `do`-`while`, `for` and `?:`, and the operands of `!`, `&&`
and `||`, may be a `bool` or a number. As in C++, a number is true when it is not zero.

```cpp
if (count) ...          // same as if (count != 0)
while (n) n--;
if (!found) ...
```

Strings, vectors and `void` values are not conditions. For a vector, write `!v.empty()`.

#### Range-based for loops

`for (T x : v)` runs the body once for each element of the vector `v`, in order, as in
C++. The variable can be `auto`, `const`, or of a type the elements convert to, as in
`for (long x : ints)`.

```cpp
for (auto name : names) println("hi {}", name);
for (const Point p : points) total += p.x;
```

- `x` is a copy of the element, so assigning to `x` does not change the vector. To change
  the elements, loop over the indexes: `for (long i = 0; i < v.size(); i++) v[i] *= 2;`.
- `for (auto& x : v)` and `for (const auto& x : v)` are not supported yet, because C+ has
  no references. Write `for (const auto x : v)`; copying an element is cheap.
- The loop goes over the vector as it was when the loop started. Changing the vector in
  the body, even with `push_back` or `clear`, does not affect the loop; in C++ it is
  undefined behavior.
- Only vectors can be looped over for now.

---

## Expressions

### Operators

Precedence and associativity match C++. From tightest to loosest:

| Operators | Notes |
|-----------|-------|
| `x++` `x--` `f(...)` `v[i]` `a.b` | postfix |
| `++x` `--x` `-x` `+x` `!x` `~x` | prefix |
| `*` `/` `%` | |
| `+` `-` | |
| `<<` `>>` | result has the type of the left side |
| `<` `<=` `>` `>=` | |
| `==` `!=` | |
| `&` | |
| `^` | |
| `\|` | |
| `&&` | short-circuit; operands are bools or numbers |
| `\|\|` | short-circuit; operands are bools or numbers |
| `?:` | condition is a bool or a number; branches must have compatible types |
| `=` `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` | right-associative |

Arithmetic, bitwise and ordering operators take `int` and `long`. `+` also joins two
strings, and the ordering operators also compare two strings (see [string](#string)).
`==` and `!=` take any two values of the same type: numbers, `bool`s, strings by
contents, structs field by field, vectors element by element, and class references by
identity. Operands are evaluated left
to right; in `v[i]`, `v` is evaluated before `i`, as in C++17.

Operator overloading is planned with C++ syntax, `Point operator+(Point other) const`,
after function overloading. `&&`, `||` and the comma operator will not be overloadable.

### Integer overflow

Integer arithmetic is checked. When a result does not fit its type, the program stops with
an error naming the operator, the type and the source location:

```
fatal error: integer overflow: the result of '+' does not fit in 'int'
  --> main.cplus:4:15
```

The same happens for:
- division or remainder by zero,
- `MIN / -1`, whose result does not fit (`MIN % -1` is simply 0),
- shifting by a negative amount or by at least the bit width of the type.

When the shift amount is a literal, an out-of-range shift is a compile error instead:
`1 << 40` is rejected because `1` is a 32-bit `int`. Write `1L << 40`.

C and C++ leave all of these undefined. Explicit wrapping operators for hashing and
cryptography, and a flag to turn checks off in hot code, are planned.

---

## Printing

`print` and `println` follow C++23 `std::println`:

```cpp
println("Hello, {}! You are {} years old.", name, age);
print("no newline");
println();                 // just a newline
println("{{literal braces}}");
```

- The first argument must be a string literal. Each `{}` is replaced by the next argument.
- The compiler checks that the number of `{}` matches the number of arguments.
- `{{` and `}}` print literal braces. Format options such as `{:x}` are planned.
- `int`, `long`, `bool`, `string`, structs, class objects and vectors can be printed. A
  struct prints its fields by name, with strings inside it in quotes:
  `{name: "Ada", born: 1815}`. A vector prints its elements: `[1, 2, 3]`, `["a", "b"]`.
  A class object prints its type name first: `User {name: "Ada", born: 1815}`. All fields
  print, private ones included. An object met again while it is being printed prints as
  `User {...}`, so an object that refers back to itself does not print forever.
- There is no `printf`, `cout` or `std::`.

`format` takes the same arguments as `print` and returns the text as a string instead of
printing it, like C++20's `std::format`. `to_string(n)` is the text of an `int` or
`long`, as in C++. `string` covers the other ways to build strings.

```cpp
string label = format("{}/{}", done, total);   // "3/10"
string key = "user" + to_string(id);
```

---

## Errors

Planned. A function that can fail is marked `throws`. `throw` raises an error;
`try { } catch (E e) { }` handles it. Every call to a throwing function is either inside a
`try` block or prefixed with `try`, which passes the error up to the caller. These are
error values, not C++ exceptions: there is no stack unwinding, and cleanup runs on the
way out.

```cpp
int parsePort(string s) throws {
    if (s.empty()) throw Error("port is empty");
    return try s.toInt();
}
```

## Memory model

Class objects are reference-counted, and the counts are atomic, so objects can later be
shared between threads. Counting is automatic:

- An object is freed as soon as nothing refers to it, and its destructor runs at that
  moment. A destructor is a reliable place to close a file or print a message.
- A local variable holds its reference until the end of its block, or until it is
  assigned another object. At the end of a block, the variables declared last are
  released first.
- A temporary, such as the result of `makeUser()` when it is not stored, is released at
  the end of the statement that made it.
- A parameter holds its argument until the function returns.
- Two objects that refer to each other would never be freed; this is a reference cycle.
  Today a cycle cannot be built, because a class reference cannot be empty. `weak`
  references, which break cycles, are planned together with [Optionals](#optionals).

A string's bytes live in a reference-counted buffer, which copies of the string share. A
string never changes bytes another string can see: `s += t` writes after the end of `s`
only when no other string holds its buffer, and otherwise moves `s` to a new buffer. A
literal needs no buffer at all. `substr` shares the buffer when the part is at least a
quarter of it and copies a smaller part, so a substring never keeps alive more than four
times the memory it uses.

A vector is a value, but its elements live in a reference-counted buffer that copies of
the vector share. Changing a vector whose buffer is shared first copies the buffer, so the
change is seen only through that vector (copy-on-write). The buffer is freed, and the
objects in it released, when the last vector using it goes away. An element is released
when it is removed: by `pop_back`, by `clear`, or by being overwritten.

Set the environment variable `CPLUS_CHECK_LEAKS=1` when running a program to have it
report, at exit, any class objects, vector buffers or string buffers that were never freed. The test suite
runs every program this way.

Planned: `unique<T>` for a single owner with no count, and `local<T>` for non-atomic
sharing that cannot cross threads. Raw pointers, `new`, `delete`, `malloc` and pointer
arithmetic will be available everywhere, and manual `new` and `delete` will produce a
warning that names the safer alternative. Debug builds will run with AddressSanitizer and
UBSan.

### Pointer declarations

In C++, `int* a, b;` makes `a` a pointer but `b` a plain `int`, a well-known trap. In C+,
a declaration that mixes pointers and non-pointers is an error. Declare them separately:

```cpp
int* a, b;          // error
int* a, * b;        // ok: both are pointers
int* a; int b;      // ok
```

## Generics

Planned. C++20 syntax: `template<typename T>` and constrained `template<Comparable T>`,
monomorphized. `constexpr` and `if constexpr` cover compile-time work. There is no
template specialization and no SFINAE.

## Concurrency

Planned. `spawn f()` runs `f` on a thread and returns a `Task<T>`. Later,
JavaScript-style `async`/`await` with a built-in event loop.

---

## Implementation status

| Feature | Status |
|---------|--------|
| `int`, `long`, `bool`, string literals and variables | implemented |
| Variables, `auto`, `const`, several per declaration, block scoping, shadowing | implemented |
| Literals: decimal, hex, octal `0o`, binary, `'` separators, `L` suffix | implemented |
| Definite assignment checking | implemented |
| Functions, recursion, calls before definition | implemented |
| `if`, `while`, `do`-`while`, `for`, `break`, `continue`, `?:` | implemented |
| All integer operators with overflow, division and shift traps | implemented |
| `print` / `println` with checked `{}` placeholders | implemented |
| Structs: fields, defaults, `{ }` lists, named fields, copying, printing | implemented |
| Definite assignment of individual struct fields | implemented |
| Classes: reference counting, constructors, destructors, printing | implemented |
| Methods, `const` methods, `this`, `public:` / `private:` | implemented |
| `==` and `!=` on strings, structs, vectors and class references | implemented |
| Strings: `+`, `+=`, `<`, `size`, `substr`, `find`, `contains`, `starts_with`, `ends_with`, `clear` | implemented |
| `format(...)` and `to_string(n)` | implemented |
| Vectors: `vector<T>`, checked `v[i]`, `push_back` and the other basic methods, copy-on-write | implemented |
| Range-based `for` over vectors | implemented |
| `cplus check`, `build`, `run`, `--emit-llvm`, `-O` | implemented |
| Imports and multi-file programs | planned |
| Casts, unsigned types, floating point, `char` | planned |
| Function overloading | planned |
| Strings: `s[i]` and looping over characters (with `char`), `append`, `insert`, `erase`, `replace`, parsing numbers | planned |
| `array<T, N>`, `hashmap<K, V>`; vector `insert`, `erase`, iterators | planned |
| References (`T&`), including `for (auto& x : v)` | planned |
| Inheritance, `virtual`, `protected:` | planned |
| Operator overloading | planned |
| Destructors for structs, static members | planned |
| Optionals | planned |
| Errors (`throws`, `try`, `catch`) | planned |
| Generics | planned |
| `extern` C functions | planned |
| Reflection, JSON, HTTP | planned |
| Threads, async | planned |

Anything marked planned is reported by the compiler as "this compiler does not support ...
yet", with a link here.
