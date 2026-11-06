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
- **A leading zero is an error** (`010`, `00`, `0'10`). C and C++ silently read `010` as octal 8, a classic
  bug when numbers are padded to line up. Write `0o10` for octal or `10` for decimal.
  JavaScript, Python, Rust and Swift use the same `0o` prefix.
- A literal is an `int` if it fits in 32 bits, otherwise a `long`. `-2147483648` is an `int`.
- The suffix `L` (or `LL`) makes a literal a `long`: `1L << 40`. Both mean the same thing,
  because `long` is always 64 bits. `u` for unsigned arrives with unsigned types. No other
  suffixes exist.
- A number with a decimal point or an exponent, such as `1.5` or `1e9`, is a floating-point
  literal instead (see [Floating-point types](#floating-point-types)).

### String literals

`"text"` with escapes `\n \t \r \0 \\ \" \'` and `\xHH`. Adjacent literals are joined:
`"a" "b"` is `"ab"`. A string cannot span lines. `\0` cannot be followed by a digit, since C
and C++ read `\012` as one byte, in octal: write `\x0A` for that byte, or `\x0012` for a zero
byte and then `12`.

### Character literals

`'a'` is a [char](#char), one byte, with the same escapes as strings: `'\n'`, `'\''`,
`'\0'`, `'\x41'`. A literal holds exactly one byte, so `'ab'`, which C++ reads as an
`int` whose value depends on the compiler, is an error, and so is `'é'`, which takes two
bytes in UTF-8: write text of any length as a string, `"é"`. `''` is an error too. C++'s
prefixed literals, `L'a'`, `u'a'`, `U'a'` and `u8"text"`, do not exist; strings are
UTF-8 already.

---

## Types

### Integer types

| Type | Size | Notes |
|------|------|-------|
| `int` | 32-bit signed | same on every platform |
| `long` | 64-bit signed | same on every platform; there is no `long long` |

There are no unsigned types yet and no `size_t`, `int32_t` and so on. Use `int` or `long`.
Arithmetic never wraps silently; see [Integer overflow](#integer-overflow).

### Floating-point types

| Type | Size | Notes |
|------|------|-------|
| `double` | 64-bit IEEE 754 | about 16 significant digits, up to about 1.8e308 |
| `float` | 32-bit IEEE 754 | about 7 significant digits, up to about 3.4e38 |

There is no `long double`.

Literals are written as in C++. A number with a decimal point or an exponent is a
`double`: `1.5`, `.5`, `2.`, `1e9`, `6.022e23`, `1'000.5`. The suffix `f` (or `F`) makes it
a `float`: `2.5f`, `1.f`, `1e3f`.

```cpp
double price = 9.99;
float ratio = 0.5f;
float half = 0.5;                  // ok: a double written as a number converts to a float
double average = total / 4.0;      // a double, since one side is
```

- `5f` is an error, as in C++: write `5.0f`, or just `5`, since an `int` converts to a
  `float`. The suffix `L` is an error, since there is no `long double`, and so are C++'s
  hexadecimal floating-point literals, such as `0x1.8p3`. A literal too large for its
  type, such as `1e400`, or `1e39` for a `float`, is an error.
- Arithmetic follows C++'s usual arithmetic conversions. When the two sides differ, an
  `int` or `long` becomes the floating-point type, and a `float` becomes a `double`. So
  `7 / 2` is `3` but `7 / 2.0` is `3.5`, and `f * 0.5` is a `double` even when `f` is a
  `float`, because `0.5` is a double; write `f * 0.5f` to stay a `float`.
- `+ - * /`, the comparisons, `++`, `--` and the compound assignments work as in C++. `%`
  and the bit operators take only integers; the remainder of a division is `fmod(a, b)`
  (see [Math functions](#math-functions)).
- Floating-point arithmetic follows IEEE 754 and never stops the program: `1 / 0.0` is
  infinity and `0 / 0.0` is NaN, "not a number". NaN is not equal to anything, itself
  included, and every ordering comparison with it is false; test for it with `isnan(x)`.
- As a condition, zero is false and anything else is true, NaN included, as in C++.
- A `double` converts to a `float` by itself only when it is written as a number, so
  `float x = 0.1;` works. Any other `double` needs `float(x)`, since it loses precision.
  A floating-point value never converts to an integer by itself; see [Casts](#casts).
- `{}` prints the fewest digits that read back as the same value, as C++'s `std::format`
  does: `0.1` prints as `0.1`, `1.0 / 3` as `0.3333333333333333`, `100.0` as `100` and `1e21`
  as `1e+21`. The infinities print as `inf` and `-inf`, and NaN always as `nan`; C++
  prints `-nan` when its sign bit is set, which can change with optimization. `{:.2f}` and the
  other options of [Printing](#printing) choose the digits. `to_string(x)` gives the same
  text as `{}`, as in C++26; earlier C++ printed six decimals.

### bool

`true` or `false`. As in C++, a number converts to `bool` implicitly: zero is `false`,
anything else is `true`. So `bool b = 1;` works, and so does `if (count)` (see
[Conditions](#conditions)). A `bool` does not convert back to a number: `int n = true;`
is an error.

### char

A `char` is one byte, from 0 to 255, as an `unsigned char` is in C++. It is what a string
holds: `s[i]` is a `char`, and so is each `c` in `for (char c : s)`.

```cpp
char c = 'a';
int code = c;                     // 97: a char converts to its code
char next = char(c + 1);          // 'b': arithmetic gives an int, as in C++
for (char x = 'a'; x <= 'z'; x++) print("{}", x);
if (isdigit(s[0])) value = s[0] - '0';
string word = string(1, c);       // "a"
```

- **A char is its code in arithmetic.** As in C++, `c + 1`, `c - '0'` and `-c` are `int`s,
  and a char converts to `int`, `long`, `float` or `double` by itself. A number does not
  convert to a `char`: write a character such as `'A'`, or `char(n)`, which stops the
  program unless n is from 0 to 255 (see [Casts](#casts)).
- **`++`, `--`, `+=` and `-=` step through the characters** and keep a char: `c++`,
  `c += 2`. A result outside 0 to 255 stops the program instead of wrapping around. The
  other compound assignments, such as `c *= 2`, are errors.
- **Comparisons** order chars by their codes: `'a' < 'b'`, `'Z' < 'a'`, and a byte above
  127 is above every ASCII character. A char compares with a number by its code.
- **A char is not a condition**, unlike in C++: write `c != '\0'` instead of `if (c)`.
  Without NUL-terminated strings, a test for the zero byte is rarely what was meant.
- **Printing** a char prints the character, as `std::format` does: `println("{}", c)`
  prints `a`. Inside a struct or a vector it prints in quotes, `'a'`, with escapes such as
  `'\n'`, and a byte that is not ASCII prints as `'\xc8'`. `int(c)` prints the code.
- `to_string(c)` is an error: C++ gives the code, `"97"`. Write `string(1, c)` for the
  character, or `to_string(int(c))` for the code.
- The character functions of C++'s `<cctype>` are built in and take a `char`:
  `isalpha`, `isdigit`, `isalnum`, `isspace`, `isupper`, `islower`, `ispunct` and
  `isxdigit` return a `bool`, and `toupper` and `tolower` return a `char`; in C++ all of
  them take and return an `int`. They know only ASCII, as C's do in the default locale,
  so `isalpha` is false for each byte of `"é"`. A function of the program with the same
  name takes their place.
- A `char` is not Unicode text: one letter can take several chars in UTF-8. Strings hold
  text; a char holds a byte.

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
char first = name[0];                       // 'A'
name[0] = 'E';                              // "Eda"; other copies keep "Ada"
```

- `a + b` joins two strings, and `s += t` appends. One side may be a `char` instead, as
  with `std::string`: `s + '!'`, `'>' + s`, `s += c`. A number cannot: `s + 5`, which C++
  compiles as adding the character with code 5, is an error. Write `s + to_string(5)` or
  `format("{}{}", s, 5)`. Two literals can be joined: `"a" + "b"`.
- `s[i]` is the `char` at position `i`, and `s[i] = c` changes it. Every index is checked:
  a position outside the string stops the program, including `s[s.size()]`, which C++
  allows for reading. Changing a char changes only this string; copies and substrings
  that shared its bytes keep their text.
- `for (char c : s)` goes over the bytes of the string, in order. As with vectors, the
  loop goes over the string as it was when the loop started.
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
- `contains(text)`, `starts_with(text)` and `ends_with(text)` return a `bool`. These and
  `find` also take a `char`: `s.find('/')`.
- `push_back(c)` adds a char at the end, `pop_back()` removes the last one, and `front()`
  and `back()` return the first and last. On an empty string, `pop_back()`, `front()`
  and `back()` stop the program; in C++ they are undefined.
- `clear()` empties the string.
- `string(count, c)` is `count` copies of the char `c`, as in C++: `string(3, '-')` is
  `"---"`. `string()` is empty, and `string(s)` is a copy of the string `s`. `string(p)`
  copies the text a C `char*` points to (see [Pointers](#pointers)).
- `to_string(x)` turns a number into text: `to_string(42)` is `"42"` and `to_string(2.5)`
  is `"2.5"`. `format("...", values...)`,
  C++20's `std::format`, builds a string with the rules of [Printing](#printing):
  `format("{}", true)` is `"true"`, and `format("{}", user)` describes an object.

Copies of a string, and large substrings, share their bytes instead of copying them, and
`+=` adds to a string in place when no other string shares it, so building a string in a
loop takes linear time. See [Memory model](#memory-model).

`s.at(i)` does not exist, since `s[i]` is always checked. Planned: `append`, `insert`,
`erase`, `replace`, `rfind` and `compare`, and parsing numbers out of strings.

### Implicit conversions

These are the only implicit conversions:
- **`int` to `long`**, which never loses information.
- **an integer to a floating-point type, and `float` to `double`**, as in C++. A `long`
  above 2^53 rounds to the nearest `double`.
- **a `double` written as a number to `float`**: `float x = 0.1;`.
- **a `char` to a number**, its code from 0 to 255, as in C++: `int code = 'a';`.
- **a number to `bool`**: zero is `false`, anything else is `true`, as in C++.
- **a value to an optional**: a `T`, or `null`, converts to `T?`. The value converts as it
  would to `T`, so an `int` also converts to `long?` (see [Optionals](#optionals)).
- **pointers**: `null` to any pointer, `T*` to `const T*`, and any pointer to `void*`
  (see [Pointers](#pointers)).

Everything else is an error, including `long` to `int`, a floating-point value to an
integer, any other `double` to `float`, a number to `char`, `char` to `string` or `bool`,
`bool` to a number, `T?` to `T`, one optional type to another, a pointer to `bool`, a
number to a pointer, and one pointer type to another. A [cast](#casts) converts between
numbers, `char` and `bool`, and between pointer types, explicitly.

```cpp
long big = 5;       // ok: int widens to long
double d = big;     // ok: an integer converts to a double
bool flag = 1;      // ok: true
int small = big;    // error: a long may not fit in an int; write int(big)
int whole = 2.5;    // error: the fraction would be lost; write int(2.5)
int n = true;       // error: bool does not convert to a number; write int(true)
```

In arithmetic with mixed types, the narrower side is converted, as in C++: `int + long` is
a `long`, `long + float` is a `float`, and `float + double` is a `double`.

### Casts

A cast converts between the number types, `char` and `bool`. The three C++ spellings work and
mean the same thing:

```cpp
int n = int(3.9);                   // 3
long big = static_cast<long>(n);
int k = (int)ratio;
double share = double(n) / total;   // without the cast, n / total divides integers
```

- Converting to an integer drops the fraction, rounding toward zero, as in C++:
  `int(-2.7)` is `-2`. For the nearest integer, write `int(round(x))`.
- **A value the new type cannot hold stops the program** with file:line, as integer
  overflow does: `int(3000000000L)`, `int(1e10)`, and NaN or an infinity converted to an
  integer. C++ wraps the first around and leaves the others undefined.
- `double(x)` and `float(x)` take any number; `float(x)` rounds to the nearest `float`.
- `bool(x)` is true when `x` is not zero, and `int(b)` is 1 or 0.
- `char(x)` is the char whose code is `x`, which must be from 0 to 255: `char(65)` is
  `'A'`. `int(c)` is the code of `c`.
- A cast binds like a unary operator, as in C++: `(int)x * 2` is `((int)x) * 2`.
- A cast cannot make a string: write `to_string(x)` or `format("{}", x)`, or
  `string(1, c)` for a char. `string(s)` of a string is allowed, and is a copy. Reading a number
  from a string is planned.
- A cast also converts one pointer type to another, as in C:
  `static_cast<int*>(memory)` or `(int*)memory`, and `bool(p)` is true when `p` is not
  null. It cannot remove `const` from what a pointer points to, and a number and a
  pointer do not convert into each other (see [Pointers](#pointers)).
- `reinterpret_cast` does not exist, since the casts above already convert pointers, and
  neither does `const_cast`, since `const` cannot be cast away. `dynamic_cast` would need
  inheritance, which is planned.

### Optionals

`T?` holds a `T` or `null`. No other type can be null: a `User` always refers to an
object, and an `int` always holds a number. Any type but `void` and `auto` can be made
optional, but an optional cannot hold another optional: `int??` is an error.

```cpp
User? find(vector<User> users, string name) {
    for (User u : users)
        if (u.name == name) return u;           // a User converts to User?
    return null;
}

User? found = find(users, "bob");
if (auto u = found) println("{}", u.name);      // u is a plain User inside the if
string name = found?.name ?? "nobody";
```

- **Values.** A `T` converts to `T?`, and `null` converts to any optional. An optional
  declared without a value is null: `int? n;`. A `{ }` list builds the value inside, as in
  `Point? p = {1, 2};`. A `T?` never converts to `T`, and an optional never converts to
  an optional of another type, not even `int?` to `long?`. Unwrap it first.
  `auto x = null;` is an error, because it does not say what `x` may hold.
- **Representation.** A `User?` is a reference that may be null, the same size as a
  `User`. Any other `T?` holds a `T` and a flag, like `std::optional<T>`. Optional
  structs, strings and vectors are values, copied like the type they hold.
- **Testing for a value.** An optional is a condition, true when it holds a value, as with
  `std::optional`: `if (found)`, `while (cur)`, `!found`. A zero still counts as a value,
  so `int? n = 0;` is true. A `bool?` is not a condition, since `if (flag)` could mean
  either thing; write `flag == true` or `flag != null`. `opt == null` and `opt != null`
  work for every optional.
- **Unwrapping.** `if (auto u = found) ...` declares `u` as the value inside, and runs the
  branch only when there is one. `u` exists only in that branch, not in the `else`.
  `while (auto x = next()) ...` works the same way, with a new `x` each time around. A
  type can replace `auto`, and the value converts to it as in an initialization:
  `if (long n = maybeInt)`. When the value is not an optional, this is the C++ form: the
  variable holds the value, and the condition is the value itself. To keep the optional,
  declare an optional: `if (User? u = found)`.
- **`?.`** reads a field or calls a method of the value inside, and is null when there is
  none: `found?.name` is a `string?`. As in JavaScript, a null skips the rest of the
  chain, so in `found?.manager.name`, neither `.manager` nor `.name` runs. A `?` is needed
  again only where the chain reaches another optional, as in `found?.manager?.name`. The
  result is optional unless it already is, or the method returns `void`. The left side is
  evaluated once.
- `?.` only reads. `found?.age = 3` is an error; unwrap it to make a change:
  `if (auto u = found) u.age = 3;`. On a class, `found?.birthday()` changes the object it
  refers to. On a struct, string or vector, `?.` works on a copy, so a method that changes
  the copy is an error.
- **`??`** gives a fallback: `found?.name ?? "nobody"` is a `string`. The fallback is
  evaluated only when the left side is null. When the fallback may be null too, the
  result is an optional, so `a ?? b ?? c` tries each in turn (`??` groups to the right).
  `??` binds more loosely than `||` and more tightly than `?:`, so `n ?? 0 + 1` is
  `n ?? (0 + 1)`.
- **Methods**, as on `std::optional`: `has_value()`; `value()`, which stops the program
  with file:line when there is no value; and `value_or(fallback)`, which evaluates the
  fallback either way, as in C++.
- **Equality.** `==` and `!=` compare two optionals of the same type, or an optional with
  a value or `null`. Two nulls are equal, null never equals a value, and two values
  compare as their type does. `opt == 3` is true when `opt` holds 3, as with
  `std::optional`.
- Everything else needs the value: `found.name`, `list[i]`, `n + 1` and
  `for (auto x : list)` are errors that say how to unwrap it. Write `(n ?? 0) + 1`.
- An optional prints its value, or `null`.
- A struct cannot hold itself, not even as an optional, since it would be infinitely
  large. A class can: a `Node? next;` field refers to another object, so linked lists and
  trees work as they do in Java or JavaScript.

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
  with a `{ }` list, `User{"Alice", 30}`. `new User(...)` is an error, since the object is
  already on the heap and freed automatically.
- A class without a constructor is built from its fields, like a struct:
  `Counter{.count = 1}`. With a constructor, the values are its arguments, in order.
- Fields and methods are reached with `.`, also through `this`: `this.age`. A class
  variable is not a pointer, so `u->age` is an error; `->` is only for
  [pointers](#pointers).
- **A class reference is never empty.** `User u;` declares a variable that must be
  assigned before it is read (see [Definite assignment](#definite-assignment)). For the
  same reason, a class-typed field must get a value whenever an object is built: from the
  `{ }` list, from the constructor, or from a default written in the class. A class that
  needs an existing object of its own type to be built, such as a list node with a
  `Node next` field, can never be built. Make such a field optional: `Node? next;` starts
  out null (see [Optionals](#optionals)).
- `const User u = ...;` stops `u` from referring to another object. The object itself can
  still change, as with `const` in JavaScript.
- Members are private by default in a class and public in a struct, as in C++. `public:`
  and `private:` work as in C++. `protected:` acts as `private:` until inheritance arrives.
- A class has at most one constructor until overloading arrives. To let a caller leave
  out values, leave out the constructor and give the fields defaults.
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
  through `this` (`this.w`, `this.area()`). `this` refers to the current object, not to
  a pointer to it, so `this->w` and `&this` are errors.
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
  declared, whatever the order of the list, as in C++. So a value in the list can read the
  fields declared before its own, and reading a later field or calling a method there is an
  error, which C++ leaves undefined. The body can read them all.
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
- A method that changes a struct in a vector, `v[0].move(1)`, works on that element in place.
  If its body also writes to `v`, through a pointer or an object holding it, the program
  stops when the method returns, since what the method did to the element would be lost. C++
  leaves this undefined if `v` moves. Call the method on a copy of the element and store it
  back.
- Not yet: `insert`, `erase`, `resize`, `reserve`, iterators (`begin()`, `end()`), and
  `hashmap<K, V>`.

**Copies are cheap.** The copies of a vector share one buffer of elements until one of them
changes; that copy first gets a buffer of its own (copy-on-write). Passing or returning a
vector therefore costs about as much as passing a class reference, so there is no need for
`const vector<int>&` parameters, which C+ does not support. See
[Memory model](#memory-model).

### Arrays

`array<T, N>` holds exactly `N` elements of type `T`, as `std::array` does. The elements
are kept in the array itself, so it needs no allocation, and an array field lies inside
its struct, as a C array does. Like a struct, an array is a **value**: assigning or passing
one copies every element.

```cpp
array<int, 3> a = {1, 2, 3};
a[0] = 30;                          // elements are numbered from 0
println("{} {}", a, a.size());      // [30, 2, 3] 3

array<int, 3> b = a;                // a copy
b[1] = 0;
println("{}", a == b);              // false

array<array<int, 2>, 3> grid;       // three rows of two, all 0
grid[2][1] = 5;
for (auto row : grid) println("{}", row);
```

Building an array:
- `array<int, 3> a;` gives every element its default value: `0`, `false`, `""`, an empty
  vector, or a struct built from its field defaults (or by its constructor, if it takes no
  arguments). This is the one difference from `std::array`, whose numbers would start out
  unknown.
- `{1, 2}` lists the first elements, and the others get their default, as in C++.
  Too many values are an error. Nested lists fill arrays of arrays: `{{1, 2}, {3, 4}}`.
- `array<int, 3>{1, 2}` names the type, and `array<int, 3>()` is the same as `array<int, 3>{}`.
- **A class reference cannot be empty**, and a struct whose constructor needs arguments
  cannot be made on its own. An array of those has no default, so a list must give every
  element, and an array declared without one must be assigned as a whole before it is used.
  An array of `User?` starts out holding nulls.
- `N` is a whole number, zero or more, written out. A name such as `array<int, N>` is not
  supported yet. For a size known only while the program runs, use a [vector](#vectors).
- The two sizes are part of the type: `array<int, 3>` and `array<int, 4>` do not convert into
  each other, and neither do `array<int, 3>` and `array<long, 3>`.
- A value can take at most 2 GiB, so a larger array is an error.

| Method | What it does |
|--------|--------------|
| `a.size()` | the number of elements, as a `long` |
| `a.empty()` | whether the array has no elements, which only `array<T, 0>` does |
| `a.front()`, `a.back()` | a copy of the first or the last element |
| `a.fill(x)` | sets every element to `x` |
| `a.data()` | a pointer to the first element |

- **Every index is checked**, as for a vector. An index that is not from 0 to `size() - 1`
  stops the program with an error naming the file and line, and a written index that is
  out of range, such as `a[3]` for `array<int, 3>`, is a compile error. An index never
  reaches the next row of an array of arrays.
- `a[i]` takes an `int` or a `long`, and an element is used in place as in a vector:
  `points[i].x = 1`, `grid[i][j] += 1`, `points[i].move(1)`.
- `==` and `!=` compare each pair of elements, in order. An array prints as `[1, 2, 3]`,
  with strings and chars in quotes.
- `for (auto x : a)` goes over the array as it was when the loop started, as for a vector,
  by keeping its own copy. To change the elements, loop over the indexes.
- A `const` array cannot change at all. `fill` cannot be called on a temporary, such as
  `makeThree().fill(1)`, because the change would be lost.
- An array is not a [condition](#conditions).

**Pointers.** Unlike the elements of a vector, the elements of an array do not move, so
`&a[i]` and `a.data()` give pointers to them, and these are `const int*` for a `const` array.
They last as long as the array does. As with `&x`, nothing checks that, and following
one after the array has gone away is undefined. An array does not turn into a pointer by
itself, as a C array does: `int* p = a;` is an error, and a C function that takes a pointer
is called with `a.data()`. A pointer to a whole array, `array<int, 3>* p = &a;`, lets a
function change an array without copying it: `(*p)[0] = 1`, `p->size()`.

**Text.** `string(a)` for an `array<char, N>` copies its text, which ends at its first
zero byte or at the end of the array, whichever comes first. This is how C stores text in a
fixed-size buffer. Unlike `string(a.data())`, it cannot read past the array.

```cpp
extern "C" char* strcpy(char* to, string from);

array<char, 16> buffer;
strcpy(buffer.data(), "hello");
println("{}", string(buffer));      // hello
```

**Large arrays.** A local array is on the stack, like any other local variable, and the stack
is small, usually 8 MiB on Linux. Each copy takes that much stack again: a by-value parameter
or the copy a `for` loop keeps, for example. Running out of stack stops the program with an
error; it never overwrites other memory. Use a vector for large amounts of data. An array in
a class object, or from `new`, is not on the stack.

### Pointers

Pointers work as in C++: `&x` is the address of `x`, `*p` is what `p` points to, and
`p->x` is a field of what `p` points to. They are for the code that needs an address: a
function that changes its caller's variable, structures linked by hand, and memory shared
with C functions. Strings, vectors and class references manage their memory themselves,
and most code needs nothing else.

```cpp
void bump(int* n) { *n += 1; }

int n = 41;
int* p = &n;
bump(p);                        // n is 42
*p = 7;                         // n is 7
Point pt;
Point* pp = &pt;
pp->x = 10;                     // the same as (*pp).x = 10
int* none = null;
if (p) println("{}", *p);       // a pointer is true when it is not null
```

- **Types.** `T*` points to a `T`, and `T**` to a `T*`. `const T*` can only read what it
  points to, and `T* const` is a pointer that cannot be pointed anywhere else, as in C++.
  `void*` points to memory of no known type. A pointer can already be null, so `int*?`
  is an error. `auto p = &x;` makes `p` a pointer; `auto*` is an error.
- **`&x`** is the address of a variable, of a field (`&pt.x`, `&pp->y`), or of what a
  pointer points to (`&p[2]`). What it points to must already have a value, since C+
  cannot tell whether the code that gets the pointer writes to it before reading. `&` of
  something `const` is a `const T*`. A temporary, `this`, and a value reached through `?.`
  have no address. Pointers into vectors and strings, such as `&v[0]`, are not supported
  yet, since their elements can move. The elements of an [array](#arrays) do not, so `&a[0]`
  is allowed.
- **`*p`, `p->x` and `p->m()`** reach what `p` points to. Fields and methods are reached
  with `->`, not `.`: `pp.x` is an error. A `const T*` can only call `const` methods.
- **Following a null pointer stops the program** with file:line, whether by `*`, `->` or
  `[ ]`. C++ leaves it undefined.
- **Indexing and arithmetic.** `p[i]` is `*(p + i)`, with an `int` or `long` index. It is
  not checked, since a pointer does not know how much memory follows it, though a
  [debug build](#debug-builds) catches most overruns. `p + n`,
  `p - n`, `p += n`, `p -= n`, `++` and `--` move by whole elements, and `p - q` is the
  number of elements between two pointers to the same type, as a `long`. None of these
  work on a `void*`.
- **Comparisons.** `==` and `!=` compare a pointer with another pointer or with `null`,
  and `<`, `<=`, `>`, `>=` order two pointers by address. `0` is a number, not a null
  pointer: write `null`.
- **Conversions.** `null` converts to any pointer, `T*` to `const T*`, and any pointer to
  `void*`, or a `const T*` to `const void*`. Other pointer types need a cast,
  `static_cast<int*>(v)` or `(int*)v` (see [Casts](#casts)). A pointer is a
  [condition](#conditions), as in C++, but does not convert to a `bool` variable by
  itself: write `p != null` or `bool(p)`. Numbers and pointers do not convert into each
  other, even with a cast.
- **Text.** `string(p)` copies the text that a `char*` or `const char*` points to, up to
  its zero byte, as C stores it. A null pointer stops the program.
- **Printing.** A pointer prints its address in hex, such as `0x7ffd5e8c`, and null
  prints `0x0`. Printing a `char*` is an error: C++ prints the text it points to, but C+
  cannot tell where that text ends. Write `string(p)` for the text, or `(void*)p` for the
  address.
- **A struct can point to its own type**, as a list or tree written by hand does:
  `Node* next = null;`.
- **A pointer to a class variable** points to the variable, which holds a reference.
  `p->name` reaches the object's field, and `*p = User("Grace")` makes the variable refer
  to another object.
- **A pointer is not checked for lifetime.** It does not keep alive what it points to, so
  following a pointer to a local variable whose function has returned, or to memory that
  has been freed, is undefined, as in C++. A [debug build](#debug-builds) catches most of
  these. A pointer to a class variable does not count as a reference to the object.
- **Memory from `malloc` holds no values yet.** Numbers, chars, bools and pointers can
  be stored into it. A string, vector, class reference or optional, or a struct holding
  one, cannot: assigning releases the old value first, and there is none, so this is
  undefined, as it is in C++. Memory from [`new`](#new-and-delete) starts with real values,
  so it can hold anything.
- Memory that a pointer owns comes from [`new`](#new-and-delete) or from C's `malloc`
  (see [C functions](#c-functions)). Planned: pointers into vectors and strings, and
  references (`T&`); meanwhile a pointer parameter lets a function change the caller's
  variable.

### Pointer declarations

In C++, `int* a, b;` makes `a` a pointer but `b` a plain `int`, a well-known trap. In C+,
a declaration that mixes pointers and non-pointers is an error. Declare them separately:

```cpp
int* a, b;          // error
int* a, * b;        // ok: both are pointers
int* a; int b;      // ok
```

### new and delete

`new` and `delete` work as in C++: `new T` makes one value on the heap and returns a `T*`,
`new T[n]` makes `n` of them, and `delete` and `delete[]` free them. C+ frees memory by
itself nearly everywhere else, so every `new` and `delete` gets a warning that names the
safer choice, which can be turned off.

```cpp
[[allow(new_delete)]];              // no warning anywhere in this file

int* n = new int(5);                // *n is 5
int* zero = new int;                // 0, unlike C++, which leaves it unset
Point* p = new Point{1, 2};         // or new Point(1, 2), or new Point for its defaults
int* many = new int[count];         // count ints, all 0
many[0] = *n;
delete n;
delete zero;
delete p;
delete[] many;
```

- **Values.** `new T(x)` starts with `x`, which converts to `T` as in `T v = x;`. A struct or
  vector is built as `T(...)` and `T{...}` build it. `new T` and `new T()` hold the default
  value of `T`: 0, an empty string, or a struct's field defaults or its constructor with no
  arguments. A type with no default, such as a struct holding a class reference, must be
  given a value.
- **Arrays.** `new T[n]` holds `n` default values, and `n` is an `int` or a `long`. A
  negative `n` stops the program, and `n` can be 0. `new int[n]()` means the same as
  `new int[n]`. `new int[3]{1, 2, 3}` and arrays of arrays are not supported yet.
- **Classes.** `new User(...)` is an error: an object is made with `User(...)` and freed
  automatically. `new` can make anything else, including a `User?` or a `User*`.
- **`delete`** releases what the memory holds, such as strings, vectors and class
  references, the last element first, as in C++, and then frees it. Deleting null does
  nothing. A `const T*` can be deleted.
- **What is checked.** `delete` on memory from `new T[n]`, and `delete[]` on memory from
  `new T`, stop the program with file:line; C++ leaves them undefined. Deleting memory
  twice, or memory that did not come from `new`, is undefined, as in C++, but `delete`
  looks for a mark that `new` leaves, so it nearly always stops the program too. Using
  memory after it is deleted is not checked, except in a [debug build](#debug-builds).
- `delete` cannot free a `void*`, since C+ would not know what it holds, or a C type known
  [only by name](#c-types-known-by-name). Memory from C's `malloc` is freed with `free`.
- `CPLUS_CHECK_LEAKS=1` reports how many allocations from `new` were never deleted (see
  [Memory model](#memory-model)).
- Placement new, `new (buffer) T`, and `new auto(x)` do not exist.

**The warning.** It is on by default and does not stop the build. Three things turn it off:

- `[[allow(new_delete)]]` before a function, method, constructor or destructor, for the
  code inside it.
- `[[allow(new_delete)]];` at the top of the file, before the first declaration, for the
  whole file. This also covers a field whose default value uses `new`.
- `-Wno-new-delete` on the command line of `cplus check`, `build` or `run`, for the whole
  program.

This is the one attribute C+ has. C++'s others, such as `[[nodiscard]]`, are errors.

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

### Constants

A constant outside any function can be used anywhere in the file, before or after it is
written.

```cpp
const int MAX_USERS = 100;
const double RATE = 1.5, TWICE = RATE * 2;
const string GREETING = "hello " + NAME;
const string NAME = "Ada";
```

- A constant is a number, a `char`, a `bool` or a `string`, or `auto` for one of those. Other
  types are not supported yet; write a function that returns the value.
- Its value is worked out from literals, other constants, operators, casts and `sizeof`. A
  variable or a function call is an error.
- A value that cannot be worked out, such as `1 / 0`, stops the program when the constant is
  used, naming the constant's line. A compile-time check will come with `constexpr`.
- A constant cannot be changed, and it has no address. To point to it, copy it into a
  variable first.
- A local variable or a parameter can have the same name, and hides the constant.
- A variable outside any function, `int count = 0;`, is not supported yet. Pass the value to
  the functions that use it, or keep it in a class object.

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
  error. The exception is a function written in C, which is declared with
  [`extern "C"`](#c-functions).
- Every path through a non-`void` function must `return` a value. Falling off the end is
  a compile error, not undefined behavior.
- A `void` function cannot return a value.
- A `const` parameter cannot be changed in the function, as in C++.
- Arguments are converted to parameter types under the
  [implicit conversion](#implicit-conversions) rules.
- Overloading (several functions with one name) is planned. Until then, each name is
  defined once.
- User functions never collide with C library symbols.

### The main function

Every program has exactly one `main`, which returns an `int`, the process exit code. Falling
off the end of it returns 0, as in C++. It takes no parameters, or the command-line arguments
as in C++:

```cpp
int main(int argc, char** argv) {
    if (argc < 2) {
        println("usage: greet <name>");
        return 1;
    }
    println("hello {}", string(argv[1]));
    return 0;
}
```

`argc` counts the arguments, and `argv[0]` is how the program was started, so the first real
argument is `argv[1]`. `argv` is a raw [pointer](#pointers), so an index past the end is not
checked. To pass arguments through `cplus run`, write them after `--`:
`cplus run greet.cplus -- Ada`.

### C functions

A function from a C library is declared with `extern "C"`, as in C++, and called like any
other function. The declaration is written with C+ types, and the compiler converts the
values to and from C at each call.

```cpp
extern "C" int abs(int n);

extern "C" {
    long strlen(string s);                                  // size_t strlen(const char* s)
    string? getenv(string name);                            // char* getenv(const char* name)
    int setenv(string name, string value, int overwrite);
}

int main() {
    println("{} {}", abs(-5), strlen("four"));
    println("{}", getenv("HOME") ?? "no home directory");
    return 0;
}
```

| In the C header | In the `extern "C"` declaration |
|-----------------|---------------------------------|
| `int`, `int32_t`, `pid_t`, `unsigned`, `uint32_t` | `int` |
| `long`, `long long`, `int64_t`, `size_t`, `ssize_t`, `time_t`, `uint64_t` | `long` |
| `double`, `float` | `double`, `float` |
| `char`, `signed char`, `unsigned char` | `char`, which passes the same byte |
| `bool` | `bool` |
| `void` result | `void` |
| `const char*` parameter | `string`, or `string?` when NULL may be passed |
| `char*` or `const char*` result | `string?`, which is null when the result is NULL, or `string` when it never is |
| `char*` that the function writes to, or that must be freed | `char*` |
| `int*`, `double*`, `void*` and other pointers | the same pointer type |
| `FILE*`, `sqlite3*` and other types used only through pointers | the same, after [`extern "C" struct FILE;`](#c-types-known-by-name) |
| `struct tm`, `div_t` and other structs whose fields C+ reads, by value or through a pointer | the same fields, [declared in C+](#c-structs) |

- Parameter names are optional, as in C: `int abs(int);`. `int rand();` and
  `int rand(void);` both take no arguments.
- **Strings going to C.** A `string` argument is copied into a buffer that ends in a zero
  byte, as C expects, and the copy is freed when the call returns, so a C function must
  not keep the pointer. A string with a zero byte inside stops the program, since C
  would only see the text before it. A null `string?` is passed as NULL.
- **Strings coming back.** Text a C function returns is copied into a new string. C+
  never frees memory that a C function returns. That is right for functions such as
  `getenv`, which return text they keep. For a function such as `strdup`, whose result
  the caller must free, declare the result `char*`, copy the text out with `string(p)`,
  and pass the pointer to `free`. A result declared `string` that turns out to be NULL
  stops the program with file:line; declare `string?` for a function that can return
  NULL.
- **Pointers** pass to and from C unchanged (see [Pointers](#pointers)). An out
  parameter takes the address of a variable, `frexp(x, &exponent)`, and memory from
  `malloc` is cast to the type it holds: `(int*)malloc(n * sizeof(int))`. A C array
  parameter, `int values[]`, is written as the pointer C passes: `int* values`.
- **Unsigned types** are declared with the C+ type of the same size. The same bits are
  passed, so a value above the largest `int` or `long` comes out negative. C's `long` is
  64 bits on Linux and macOS, like C+'s.
- **Libraries.** C's standard library is always linked. Other libraries are named on the
  command line, as with a C compiler: `cplus build app.cplus -lsqlite3`, adding
  `-L <directory>` for a library outside the usual places. `cplus run` takes the same
  options.
- C's output functions, such as `puts`, share their buffer with `print` and `println`, so
  output comes out in the order it was written.
- A C function cannot be named `main`, or start with `cplus_`, which the C+ runtime uses.
- Not supported yet: `long double`; functions with a variable number of arguments, such as
  `printf`; callbacks; C variables such as `errno`; and reading declarations from C headers.

#### C structs

A struct whose fields are all C types has the same layout as the C struct with those fields
in that order, as in C++. C functions can read and write it through a pointer, and take and
return it by value.

```cpp
extern "C" {
    struct tm {
        int tm_sec; int tm_min; int tm_hour;
        int tm_mday; int tm_mon; int tm_year;
        int tm_wday; int tm_yday; int tm_isdst;
        long tm_gmtoff;
        const char* tm_zone;
    };
    struct ldiv_t { long quot; long rem; };

    tm* gmtime_r(const long* time, tm* result);     // through a pointer
    ldiv_t ldiv(long a, long b);                    // by value
}

int main() {
    long now = 86400;
    tm t = {};                                      // `&t` needs a value, so give it one
    gmtime_r(&now, &t);
    println("{} {}", t.tm_year, t.tm_mday);         // 70 2
    println("{}", ldiv(17, 5).rem);                 // 2
    return 0;
}
```

- **C types** are numbers, `char`, `bool`, pointers to C types, `void*`, other such structs,
  and [arrays](#arrays) of those, with at least one element. C has no equivalent of a
  `string`, `vector`, class reference or optional, so a C function cannot take or return a
  struct holding one, or a pointer to it.
- **A C array field**, `char name[64];`, is written `array<char, 64> name;`, which has the
  same layout. Text in a C struct is an `array<char, N>` field, read with `string(s.name)`, or
  a `char*` or `const char*` field, read with `string(p)`.
- **A C function cannot take or return an array itself**, since C has no such thing. A C
  array parameter is written as the pointer C passes, and called with `a.data()`. A struct
  holding an array does travel by value, as in C.
- **Inside `extern "C"`**, every field is checked where the struct is defined. Otherwise
  it is an ordinary struct, as it is in C++, with field defaults and methods if wanted, and a
  struct written outside `extern "C"` works too when its fields are C types.
- **Write the fields as the C header does**, with the types from the table above. C+ cannot
  compare the struct with C's definition until it can read C headers, so a field that is left
  out or has the wrong type makes C and C+ disagree about where the other fields are.
- **By value**, a struct travels as the platform's C compiler passes it. This is supported
  on Linux for x86-64 and aarch64; elsewhere it is an error, and a pointer still works. C gets
  its own copy, and changing it does not change the caller's struct.
- **As in C++, the type is written without the word `struct`**: `tm t;`, `tm* p`. C's
  `typedef struct { ... } name;` is written `struct name { ... };`.
- Not supported yet: 16-bit fields, such as `short` and `uint16_t`; unions and bit fields. A struct that has them can still be used
  through pointers if C+ does not need its fields, by declaring it
  [by name](#c-types-known-by-name).

#### C types known by name

Many C libraries hand out pointers to a type whose fields are private, such as `FILE` or
`sqlite3`. C+ declares such a type by name only, inside `extern "C"`, and can then hold and
pass pointers to it:

```cpp
extern "C" {
    struct FILE;
    FILE* fopen(string path, string mode);
    int fclose(FILE* f);
}
extern "C" struct sqlite3;      // one type on its own
```

- The type is written without the word `struct` afterward: `FILE*`, not `struct FILE*`,
  as in C++.
- Only a pointer to it can be used: a `FILE` variable, field or parameter, `*f`, `f[i]`,
  `f + 1` and `sizeof(FILE)` are errors, since C+ never sees what is inside.
- C's `typedef struct x x;` is not needed and is an error; neither is `class X;`. A C
  struct whose fields C+ reads is not supported yet.

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
and `||`, may be a `bool`, a number, a pointer or an optional. As in C++, a number is
true when it is not zero (a floating-point NaN is true), a pointer when it is not null,
and an optional when it holds a value. A `bool?` is not a condition (see
[Optionals](#optionals)).

```cpp
if (count) ...          // same as if (count != 0)
while (n) n--;
if (!found) ...         // found is null
if (node) ...           // same as if (node != null), for a pointer
if (auto u = found) ... // runs with the User inside found
```

Strings, vectors, chars and `void` values are not conditions. For a vector, write
`!v.empty()`; for a char, `c != '\0'`.

#### Range-based for loops

`for (T x : v)` runs the body once for each element of the vector or array `v`, in order, as in
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
  undefined behavior. An [array](#arrays) works the same way.
- A string can be looped over too: `for (char c : s)` goes over its bytes.
- Only vectors, arrays and strings can be looped over for now.

---

## Expressions

### Operators

Precedence and associativity match C++. From tightest to loosest:

| Operators | Notes |
|-----------|-------|
| `x++` `x--` `f(...)` `v[i]` `a.b` `a->b` `a?.b` | postfix; `?.` skips the rest of the chain when `a` is null |
| `++x` `--x` `-x` `+x` `!x` `~x` `*p` `&x` `sizeof` | prefix; a cast binds here too |
| `*` `/` `%` | |
| `+` `-` | |
| `<<` `>>` | result has the type of the left side |
| `<` `<=` `>` `>=` | |
| `==` `!=` | |
| `&` | |
| `^` | |
| `\|` | |
| `&&` | short-circuit; operands are conditions (see [Conditions](#conditions)) |
| `\|\|` | short-circuit; operands are conditions |
| `??` | right-associative; the right side is evaluated only when the left is null |
| `?:` | branches must have compatible types; `c ? user : null` is a `User?` |
| `=` `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` | right-associative |

Arithmetic and ordering operators take any numbers, converting mixed operands as C++ does
(see [Implicit conversions](#implicit-conversions)). `%` and the bit operators `~ & | ^ <<
>>` take only integers. A `char` is an integer here, and is promoted to `int` first, as in
C++: `c - '0'`, `c & 0x20` and `-c` are `int`s (see [char](#char)). `+` also joins two
strings, or a string and a char, and the ordering operators also compare two strings (see
[string](#string)). `+` and `-` also move a pointer, `-` measures the distance between two,
and the ordering operators compare them by address (see [Pointers](#pointers)).
`==` and `!=` take any two values of the same type: numbers, `bool`s, `char`s, strings by
contents, structs field by field, vectors element by element, class references by
identity, and pointers by address. They also compare an optional with a value or `null` (see
[Optionals](#optionals)). Operands are evaluated left
to right; in `v[i]`, `v` is evaluated before `i`, as in C++17. In `v[i].f(x)`, `i` is
evaluated before `x`, and the element is found after `x`, so `x` can change `v`.

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

Like every runtime error, it goes to standard error, after whatever the program printed so
far, and ends the program with exit code 134. Destructors do not run. To look around at that
point in a debugger, set a breakpoint: `break cplus_trap` in `gdb`.

C and C++ leave all of these undefined. Explicit wrapping operators for hashing and
cryptography, and a flag to turn checks off in hot code, are planned. Floating-point
arithmetic is not checked; see [Floating-point types](#floating-point-types).

### sizeof

`sizeof(T)` is the size of the type `T` in bytes, and `sizeof x` the size of the type of
`x`, as a `long`. As in C++, the operand is not evaluated. It is mostly for memory from C:

```cpp
int* squares = (int*)malloc(5 * sizeof(int));
```

- An empty struct takes one byte, as in C++, so `sizeof` gives 1, and each element of
  `new E[3]` has its own address. Inside `extern "C"` it takes none, as in C.
- `sizeof(char)` is 1, `sizeof(int)` and `sizeof(float)` are 4, and `sizeof(long)`,
  `sizeof(double)` and a pointer are 8 on 64-bit systems. A struct includes padding, as in
  C: a struct with an `int` field and then a `long` field is 16.
- For a string, a vector or a class, `sizeof` gives the size of the variable, not of the
  text, the elements or the object it refers to. An array holds its elements, so
  `sizeof(array<int, 3>)` is 12.
- `sizeof(void)` and the size of a C type known only by name are errors.

### Math functions

The functions of C++'s `<cmath>` are built in, without `std::` and without an include:

| Functions | Notes |
|-----------|-------|
| `sqrt` `cbrt` `pow(x, y)` `hypot(x, y)` | |
| `exp` `exp2` `log` `log2` `log10` | `log` is the natural logarithm |
| `sin` `cos` `tan` `asin` `acos` `atan` `atan2(y, x)` | angles in radians |
| `sinh` `cosh` `tanh` | |
| `floor` `ceil` `trunc` `round` | `round` takes halves away from zero, as in C |
| `fabs` `fmod(x, y)` | `fmod` is the remainder, with the sign of `x` |
| `isnan` `isinf` `isfinite` | return a `bool` |
| `abs` `min(a, b)` `max(a, b)` | keep the type of their arguments |

```cpp
double r = sqrt(dx * dx + dy * dy);
int cents = int(round(price * 100));
long biggest = max(total, 0L);
```

- They take any numbers. The result is a `double`, or a `float` when every argument is a
  `float`, as with C++'s overloads: `sqrt(2)` is a `double` and `sqrt(2.0f)` a `float`.
- `abs`, `min` and `max` also take integers, and give the type that arithmetic on their
  arguments would: `abs(-5)` is the `int` 5, `max(n, 5L)` is a `long`, `min(1, 2.5)` is a
  `double`. `abs` of the smallest `int` or `long` stops the program, as negating it does.
- A `char` counts as its code, as in other arithmetic: `abs(c)` is an `int`. `min` and
  `max` of chars only give a `char`: `max('a', c)`.
- The others never stop the program: `sqrt(-1)` is NaN and `log(0)` is `-inf`.
- A function of the program, or an `extern "C"` function, with the same name takes the
  place of the built-in one.
- Constants such as C++20's `std::numbers::pi` are planned. Until then, `acos(-1)` is π.

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
- `{{` and `}}` print literal braces.
- A floating-point value prints with the fewest digits that read back as the same value
  (see [Floating-point types](#floating-point-types)). A placeholder can instead give a
  precision and a type, as in C++: `{:.2f}` prints 2 decimals, `{:e}` uses an exponent,
  `{:g}` the shorter of the two, and `{:.3}` 3 significant digits. These work only on
  `float` and `double`. Other format options, such as a width, alignment and `{:x}`, are
  planned.
- Numbers, `bool`, `char`, `string`, structs, class objects and vectors can be printed. A
  struct prints its fields by name, with strings inside it in quotes:
  `{name: "Ada", born: 1815}`. A vector prints its elements: `[1, 2, 3]`, `["a", "b"]`.
  A class object prints its type name first: `User {name: "Ada", born: 1815}`. All fields
  print, private ones included. An object met again while it is being printed prints as
  `User {...}`, so an object that refers back to itself does not print forever.
- There is no `printf`, `cout` or `std::`.

`format` takes the same arguments as `print` and returns the text as a string instead of
printing it, like C++20's `std::format`. `to_string(x)` is the text of a number, as in
C++. `string` covers the other ways to build strings.

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
- When an object is freed, its fields are released, the last declared first. A long
  chain of objects, such as a linked list of a million nodes, is freed one node after
  another, without running out of stack.
- Two objects that refer to each other, such as `a.next = b; b.next = a;`, are never
  freed; this is a reference cycle. `weak` references, which break cycles, are planned.
  Until then, set one of the references to `null` when you are done with them.
  `CPLUS_CHECK_LEAKS=1` reports the objects a cycle kept alive.

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

An array holds its elements itself, so it has no buffer. Each copy holds its own references
to the objects in its elements, which are released when the array goes away, the last
element first.

Set the environment variable `CPLUS_CHECK_LEAKS=1` when running a program to have it
report, at exit, any class objects, vector buffers or string buffers that were never freed,
and any memory from `new` that was never deleted. The test suite runs every program this way.

Raw [pointers](#pointers) are not counted: a pointer does not keep alive what it points
to. Memory from [`new`](#new-and-delete) stays until `delete` frees it.

Planned: `unique<T>` for a single owner with no count, and `local<T>` for non-atomic
sharing that cannot cross threads.

### Debug builds

`--debug` on `cplus build` or `cplus run` makes a debug build. It takes about twice the
time and more memory, and stops the program at memory errors that a plain build lets
pass, so use it while looking for a bug or testing code that uses pointers.

```
cplus run app.cplus --debug
```

```
fatal error: memory was used after it was freed
  = help: the traces below show where it was used, where it was freed, and where it was allocated; free it after its last use
==73189==ERROR: AddressSanitizer: heap-use-after-free on address 0x503000000050 ...
READ of size 4 at 0x503000000050 thread T0
    #0 0x62362444481c in cplus.use app.cplus:4:12
    #1 0x623624444921 in cplus.main app.cplus:11:19
    ...
```

The first lines say what went wrong. Each trace after them lists the functions that led
there, innermost first, with file:line:column. `cplus.` comes before the name of every C+
function. Code that is fine behaves as in a plain build, and every other kind of runtime
error prints its usual message, followed by a trace.

What a debug build adds:

- **[AddressSanitizer](https://clang.llvm.org/docs/AddressSanitizer.html)** stops the
  program when a pointer goes outside memory from `new` or `malloc` or a local variable or
  array, when memory is used after it is freed, when a pointer to a local variable is used
  after its function returned, and when `free` is misused. This covers code in C libraries
  only where it calls `malloc`, `free` and functions like `strlen`, unless the library was
  also built with `-fsanitize=address`.
- **Misaligned pointers.** `*p`, `p->x` and `p[i]` stop the program when the address is not
  a multiple of what the type needs, which a cast from a `char*` can cause.
- **Invalid bools.** Reading a `bool` through a pointer, or as a field or element, stops the
  program when the byte is not 0 or 1. A whole struct copied through a pointer is not looked
  inside until one of its fields is read.
- **Running out of stack, or any other crash,** prints a trace too.
- **Leak reports.** Leaks are [reported at exit](#memory-model) without setting
  `CPLUS_CHECK_LEAKS=1`. `CPLUS_CHECK_LEAKS=0` turns the report off.

Everything else that is undefined in C and C++, such as integer overflow and shifting too
far, already stops the program in every build, so a debug build has nothing to add.

What it cannot catch:

- **`-O` can hide errors.** `--debug` works with `-O`, but the optimizer can remove a bad
  read or inline a function so that its local variable stays alive, before anything checks
  it. Leave `-O` off when looking for a memory bug.
- **A far-off pointer.** A pointer that lands inside other valid memory is not noticed,
  such as `p[1000]` reaching another allocation.
- **A local variable whose block has ended,** while its function is still running. Only the
  end of the function is noticed.
- **Reading memory that was never set,** such as from `malloc`.

A stack trace stays on your machine. Some systems, such as Ubuntu, set `DEBUGINFOD_URLS`,
which would have `llvm-symbolizer` ask a server on the internet about the program, so a debug
build clears it as it starts.

Debug builds need the sanitizer libraries for the C compiler, such as `libclang-rt-18-dev`
on Debian or Ubuntu. Without them, `--debug` says so. The stack traces need `llvm-symbolizer`
to show lines, which comes with LLVM. When the compiler was built with gcc as its C compiler,
traces name the C+ functions but not their lines.

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
| `==` and `!=` on strings, structs, vectors, class references and optionals | implemented |
| Strings: `+`, `+=`, `<`, `size`, `substr`, `find`, `contains`, `starts_with`, `ends_with`, `clear` | implemented |
| `format(...)` and `to_string(x)` | implemented |
| Vectors: `vector<T>`, checked `v[i]`, `push_back` and the other basic methods, copy-on-write | implemented |
| Range-based `for` over vectors | implemented |
| Optionals: `T?`, `null`, `?.`, `??`, `if (auto u = opt)`, `value()`, `value_or()` | implemented |
| `double` and `float`: literals, IEEE arithmetic, shortest printing, `{:.2f}`, `{:e}`, `{:g}` | implemented |
| Casts: `int(x)`, `static_cast<int>(x)`, `(int)x`, stopping when the value does not fit | implemented |
| Math functions from `<cmath>`, `abs`, `min`, `max` | implemented |
| `char`: `'a'`, checked `s[i]`, `for (char c : s)`, `push_back`, `pop_back`, `front`, `back`, `string(n, c)`, `<cctype>` functions | implemented |
| `extern "C"` functions taking and returning numbers, `char`, `bool`, `string`, `string?`; `-l` and `-L` | implemented |
| Pointers: `T*`, `&x`, `*p`, `p->x`, `p[i]`, arithmetic, `const T*`, `void*`, null checks, `sizeof` | implemented |
| Pointers in `extern "C"`, and C types known by name, such as `FILE*` | implemented |
| C structs: fields, array fields, through pointers, and by value on Linux x86-64 and aarch64 | implemented |
| `array<T, N>`: checked `a[i]`, `size`, `empty`, `front`, `back`, `fill`, `data`, range-based `for`, `==`, pointers to elements, `string(a)` | implemented |
| `new`, `new[]`, `delete`, `delete[]`, with a warning; `[[allow(new_delete)]]`, `-Wno-new-delete` | implemented |
| Constants outside functions: numbers, `char`, `bool`, `string` | implemented |
| `main` with `argc` and `argv` | implemented |
| `cplus check`, `build`, `run`, `--emit-llvm`, `-O` | implemented |
| Debug builds with `--debug`: AddressSanitizer, stack traces, misaligned pointer and invalid bool checks, leak report | implemented |
| Imports and multi-file programs | planned |
| Variables outside functions; constants of other types; `constexpr` | planned |
| Enums, `switch`, interfaces, lambdas | planned |
| Default arguments, `auto` parameters and return types | planned |
| `const` fields, nested types, types defined inside a function | planned |
| `int x{5}`, `int x(5)`, brace initializers for fields | planned |
| `short`, and 8-bit and 16-bit integer types | planned |
| Arrays of arrays from `new[]`, initial values for `new T[n]` | planned |
| Numbered format placeholders, like `{0}` | planned |
| C functions with variable arguments; vectors passed to or from C | planned |
| Unsigned types | planned |
| Format options for width, alignment and `{:x}`; math constants | planned |
| Function overloading | planned |
| Strings: `append`, `insert`, `erase`, `replace`, parsing numbers | planned |
| `hashmap<K, V>`; vector `insert`, `erase`, iterators; array sizes given by a name | planned |
| References (`T&`), including `for (auto& x : v)` | planned |
| Inheritance, `virtual`, `protected:` | planned |
| Operator overloading | planned |
| Destructors for structs, static members | planned |
| `weak` references | planned |
| Errors (`throws`, `try`, `catch`) | planned |
| Generics | planned |
| Pointers into vectors and strings | planned |
| C callbacks and variables in `extern "C"`; 16-bit integers and unions in C structs; reading C headers | planned |
| Reflection, JSON, HTTP | planned |
| Threads, async | planned |

Anything marked planned is reported by the compiler as "this compiler does not support ...
yet", with a link here.
