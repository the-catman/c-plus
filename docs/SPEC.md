# C+ Language Specification

Version 0.1. This document is written to be read by people and by language models that
have never seen C+. Compiler errors link to its sections, so heading names are stable:
rename one only together with `compiler/SpecRefs.h`. `cplus explain <topic>` prints any section
of it, with an example that runs.

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

`R"(text)"` is a raw string, as in C++: it has no escapes and can span lines, which suits JSON and
other text full of quotes. For a text with `)"` in it, choose what ends it, up to 16 characters: `R"end(text)end"`.

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
  `isalpha`, `isdigit`, `isalnum`, `isspace`, `isupper`, `islower`, `ispunct`, `isxdigit`, `isprint`, `isgraph`,
  `iscntrl` and `isblank` return a `bool`, and `toupper` and `tolower` return a `char`; in C++ all of
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
- `reinterpret_cast<void*>(-1)` makes a pointer from an `int` or a `long`, and
  `reinterpret_cast<long>(p)` a `long` from a pointer, as in C++. No other cast does this. It
  is for addresses that C libraries use as signals, such as `SQLITE_TRANSIENT`, and for
  looking at an address as a number. Between pointers it works as the other casts do.
- `const_cast` does not exist, since `const` cannot be cast away. `dynamic_cast` would need
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
  `if (auto u = found) u.age = 3;`, for a class object. For a struct, `u` is a copy, so store it back:
  `if (auto u = found) { u.age = 3; found = u; }`. On a class, `found?.birthday()` changes the object it
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
- Members are private by default in a class and public in a struct, as in C++. `public:`,
  `private:` and `protected:` work as in C++; see [Inheritance](#inheritance).
- A class has at most one constructor until overloading arrives. To let a caller leave
  out values, leave out the constructor and give the fields defaults.
- `a == b` is true when `a` and `b` refer to the same object, like `===` on objects in
  JavaScript or `==` on `shared_ptr` in C++. Two objects with equal fields are still two
  objects, so `User("Ann") == User("Ann")` is false. To compare contents, compare the
  fields. Classes will be able to define their own `==` with operator overloading.

### Inheritance

A class can derive from other classes, written as in C++. It gets the fields and methods of its
base classes. A method marked `virtual` runs the version of what the object really is, however the
object is reached.

```cpp
class Animal {
public:
    string name;
    Animal(string n) : name(n) {}
    virtual string sound() const { return "..."; }
    string describe() const { return name + " says " + sound(); }
};

class Dog : public Animal {
public:
    string breed = "mutt";
    Dog(string n) : Animal(n) {}
    string sound() const override { return "Woof"; }
};

Animal a = Dog("Rex");            // the same object, seen as an Animal
println("{}", a.describe());      // Rex says Woof
println("{}", a);                 // Dog {name: "Rex", breed: "mutt"}
```

- A class must say `public`. Without it, C++ derives privately, which is rarely what was meant,
  so `class Dog : Animal` is an error. Only a class can derive, and only from a class: a struct is
  a value, and copying one into a variable of its base type would lose part of it. The base class
  can be defined before or after.
- A `Dog` converts to an `Animal` by itself, wherever a value is stored, passed, returned or
  compared, and so does a `Dog?` to an `Animal?`. It is the same object, not a copy. A
  `vector<Dog>` does not convert to a `vector<Animal>`, but a `vector<Animal>` can hold dogs.
  `c ? dog : animal` is an `Animal`. For two classes that only share a base class, say which:
  `c ? static_cast<Animal>(dog) : cat`.
- **`override` is required** on a method that replaces a virtual one, and its parameters, return
  type and `const` must be the same. C++ only recommends `override`, and a typo there quietly
  makes a new method. A method cannot share its name with a method of a base class that is not
  virtual, nor can a field share its name with a member of a base class: in C++ these quietly hide
  the inherited one, so that which one runs depends on the variable's type.
- `Animal::sound()` runs that class's own version, without looking at what the object really is;
  this is how an overriding method calls the one it replaces. From outside, `d.Animal::sound()`.
  `Animal::name` and `d.Animal::name` reach fields the same way.
- `virtual double area() const = 0;` declares a method without a body. A class that has one, or
  inherits one without overriding it, is abstract: no object of it can be created, but a variable
  of its type can refer to an object of a class deriving from it.
- `protected:` members can be used in the methods of a class and of classes deriving from it, on
  objects of the deriving class, as in C++.
- Fields of the base class print first, and an object always prints as what it really is.

**Constructors.** The base class is built first, then the class's own fields, then the
constructor's body runs, whatever order the initializer list is written in. Its values are passed
with the name of the base class, as in C++: `Dog(string n) : Animal(n), breed("lab") {}`.

- If the base class's constructor needs arguments, the class must have a constructor that passes
  them. A base class without a constructor takes its fields' values in order, `: Plain(1, "a")`, or
  its defaults if the list does not name it.
- A class with a base class and no constructor is built from defaults alone: `Dog()`. To give it
  values, write a constructor.
- A base class's fields belong to it: `: name(n)` is an error in the deriving class, which can
  pass the value on to the base class or set the field in its body.
- The values for the base class cannot use the object or its fields, since nothing exists yet.

**While constructors and destructors run,** an object is only as much as has been built, as in
C++. In `Animal`'s constructor a `Dog` is only an `Animal`: a virtual method runs `Animal`'s
version, and `Dog`'s fields have no values yet. A method with no body called then stops the program.

**Destructors.** `~Dog() {}` runs first, then `Dog`'s fields are released, last declared first,
then the same for `Animal`. A destructor always runs for what the object really is, so unlike in
C++ it does not need `virtual`. `virtual`, `override` and `= default` are allowed on a destructor
and change nothing.

**`dynamic_cast`** finds out what an object really is. The type written is the type of the result:

```cpp
if (auto d = dynamic_cast<Dog?>(a)) println("{}", d.breed);   // null when it is not a Dog
Dog d = dynamic_cast<Dog>(a);                                 // stops the program when it is not
```

These are C++'s `dynamic_cast<Dog*>` and `dynamic_cast<Dog&>`, for a language where a class variable
is already a reference. It also takes an `Animal?`, where null is not a `Dog`. `static_cast<Dog>(a)`
and `(Dog)a`, which C++ does not check, are errors. `static_cast<Animal>(dog)` and `(Animal)dog` are allowed.

#### Multiple inheritance

A class can have several base classes, each built in the order they are declared, and destroyed in
the opposite order. It follows C++'s default rules, and C+ has no virtual inheritance.

```cpp
class Box : public Named, public Sized {
public:
    Box(string n, int s) : Named(n), Sized(s) {}
    int area() const override { return size * size; }    // replaces Sized's
};

Sized s = Box("crate", 3);    // the same object, seen as a Sized
```

- A reference to any base class is a reference to the whole object: it keeps the object alive, and
  it prints and runs virtual methods as what the object really is. `==` on two references compares
  them as parts of an object, as C++ does on pointers.
- A method overrides the virtual method of that name in every base class that has one.
- A name that could mean a member of more than one base class is an error where it is used, not
  where the class is defined. `x.Left::value`, or `Left::value` inside a method, says which.
- **A diamond holds its shared base class twice.** With `class Left : public Base` and
  `class Right : public Base`, a `class Both : public Left, public Right` has two `Base` parts with
  separate fields. Printing shows both. A `Both` does not convert to a `Base` by itself, since it
  is not known which is meant, so say `static_cast<Left>(both)` first. `b.Base::id` is an error too;
  `b.Left::id` is what to write. `dynamic_cast<Base?>` gives null for the same reason.
- `dynamic_cast` can find any part from any other, also on another branch: `dynamic_cast<Right?>(left)`.
  It is an error only if no class derives from both.
- A program that uses multiple inheritance anywhere pays for it with one more memory read for each
  reference count change and for printing an object. Any other program does not.

### Interfaces

An interface lists methods, with no bodies and no fields, for classes to write. It is used as a
type like any class, and it cannot be made into an object. A class derives from one as from a class,
and from as many as it likes.

```cpp
interface Speaker {
    string speak() const;
};

class Dog : public Animal, public Speaker {
public:
    string speak() const override { return "Woof"; }
};

Speaker s = Dog("Rex");
println("{}", s.speak());
```

- Everything in an interface is public. It has no constructor or destructor.
- `virtual` and `= 0` are allowed and mean nothing more, so an abstract class from C++ can become an
  interface by changing one word.
- An interface can derive from other interfaces, but not from a class.
- A class that leaves out a method of its interfaces is abstract: it can be derived from, but no
  object of it can be created.
- C+ interfaces are classes in every other way, so the rules above apply to them, including two
  copies in a diamond: `Pet` and `Robot` deriving from `Named`, and `RobotPet` from both, holds
  `Named` twice, and one `override` replaces both.

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
- `Point(p)`, with `p` a `Point`, copies it, as in C++. On a class, `Animal(a)` is the same object, since a
  class is shared when copied. If the type's constructor or first field can take the value, that comes first.
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
  `v[v.size() - 1] = x`. Changing what they give, `v.back().x = 1` or a method that changes a struct, `v.back().set()` or `v.back().p.set()`, is an
  error, as in C++ it would change the element, and here it would do nothing. So is changing what `?:` gives when both
  of what it chooses from are what C++ would refer to, such as variables, fields, elements or what `back()` gives, as
  `(f ? a : b).set()` would change `a` in C++, and what `=` gives. What `??`, `value_or()`, `values()` and `items()` give,
  and `?:` with only one such, is a copy in C++ too, so changing it is an error only when nothing receives the result. What a function gave changes nothing the program has, so `(f ? make(1) : make(2)).with(5)`
  and `list().back().with(5)` are allowed, and so is any method under `spawn`, which runs on a copy.
- `==` and `!=` compare the sizes and then each pair of elements, in order. Ordering with
  `<` is planned.
- A vector prints as `[1, 2, 3]`, with strings in quotes: `["Ada", "Linus"]`.
- A `const` vector cannot change at all, like a `const` struct: neither its elements nor
  its size.
- `push_back`, `pop_back` and `clear` cannot be called on a temporary, such as
  `makeList().push_back(1)`, because the change would be lost.
- A vector is not a [condition](#conditions); write `!v.empty()`.
- A method that changes a struct in a vector, `v[0].move(1)`, works on that element in place,
  and an argument that holds a copy of `v` keeps seeing `v` as it was. If the method's body
  writes to `v`, keeps a copy of it, or frees what holds it, through a pointer or an object
  holding it, the program stops when the method returns, since what the method did to the
  element would be lost. C++ leaves this undefined if `v` moves. Call the method on a copy of
  the element and store it back.
- Not yet: `insert`, `erase`, `resize`, `reserve`, and iterators (`begin()`, `end()`).

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

### Hash maps

`hashmap<K, V>` finds values by key, and is used like C++'s `std::unordered_map`. It needs nothing to be
imported.

```cpp
hashmap<string, int> ages = {{"ada", 36}, {"bob", 25}};
ages["cid"] = 40;
ages["ada"]++;
println("{}", ages);                                       // {"ada": 37, "bob": 25, "cid": 40}
println("{}", ages["zed"]);                                // 0, and "zed" is in the map now
if (auto age = ages.get("bob")) println("{}", age);        // 25
for (auto e : ages) println("{}: {}", e.first, e.second);
```

- **A value.** It is copied like a vector, and as cheaply: copies share what they hold until one of them
  changes. A copy never sees the changes of another. A function that changes a map it is given takes a pointer.
- **In order.** It goes over its entries in the order they were added, which `std::unordered_map` leaves open, so a
  program prints the same thing every time it runs. Assigning to a key that is there keeps its place. A key
  that is erased and added again goes last.
- **`m[key]`** is the value, in place, as with `v[i]`: `m[key] = 1`, `m[key]++`, `m[key].x = 1`,
  `m[key].push_back(1)`. As in C++, a key that is missing is added first, with a default value. So it cannot be used
  on a const map, or in a const method. If the type has no default value, such as a class reference or a struct
  whose constructor needs arguments, there is nothing to add, so a missing key **stops the program**, except that
  `m[key] = value` always works. A temporary map, `make()["a"]`, has nowhere to add a key either.
- **Keys.** A key can be a number, a `char`, a `bool`, a string, a pointer or a class reference, or an optional, a
  vector, an array or a struct of those. Two keys are the same when they are `==`, so a class reference is
  the same key only for the same object. `0.0` and `-0.0` are one key, and NaN is never found, since it is not
  `==` to itself, as in C++. The hash comes by itself, and a hash function of your own is planned. A generic can ask
  for such a type with the [constraint](#constraints) `Hashable`.
- **Values** can be of any type except `void`. With a `unique<T>`, the map cannot be copied either: put values in
  with `m[key] = move(u)`, reach them as `m[key].x`, and take them out with `move(m[key])`. `get`, `values`, `items` and
  a loop over the map copy values, so they are errors then.
- **Erasing** releases the key and the value right away, and a value's destructor can use the map.

| Method | Does |
|--------|------|
| `m.size()`, `m.empty()` | how many entries it has, as a `long`, and whether it has none |
| `m.contains(key)`, `m.count(key)` | whether the key is there, as a `bool`, or as a `long`, 0 or 1 |
| `m.get(key)` | a `V?`: the value, or null. If `V` can be null already, use `contains` to tell the two apart |
| `m.at(key)` | a copy of the value, or **stops the program** with the key that is missing |
| `m.insert({key, value})` | adds it unless the key is there, which keeps the value that is there, and says whether it added it |
| `m.erase(key)` | removes it, and says how many it removed, as a `long`, 0 or 1 |
| `m.clear()` | removes everything |
| `m.keys()`, `m.values()` | a vector of them, in order |
| `m.items()` | a vector of the entries, in order, with fields `first` and `second` |

- `for (auto e : m)` goes over the entries as they were when the loop started, so the body can change the map.
- `==` is true for the same entries in any order. A map prints as `{"a": 1, "b": 2}`.
- A method of a value in a map, `m[key].go()`, works on it in place, and stops the program if its body adds to or
  otherwise changes the map through a pointer, as with [a vector](#vectors).
- The hash depends on a number chosen each time the program starts, so that input cannot be made to collide on
  purpose. Nothing else depends on it.
- Not yet: `find` and other iterators, `reserve`, sorted maps and sets. `m.find(k)` has an error that says what to write.

### JSON

`import json;` turns values into JSON and back, using what the compiler knows of their types. Nothing has to be
written for a type.

```cpp
import json;

struct Address { string city; int zip = 0; };
struct User { string name; int age = 0; vector<string> tags; Address home; string? nick; };

User u = {"Ada", 36, {"math"}, {"London"}};
string text = json::stringify(u);          // {"name":"Ada","age":36,"tags":["math"],"home":{"city":"London","zip":0},"nick":null}
println("{}", json::pretty(u));            // the same, over many lines, indented by two
User back = try json::parse<User>(text);   // fails with a json::Error if the text is wrong
```

| C+ | JSON |
|----|------|
| `int`, `long`, `float`, `double` | a number. NaN and the infinities are `null`, as in JavaScript |
| `bool` | `true` or `false` |
| `string` | a string. A byte that is not UTF-8 is written as U+FFFD |
| `char` | a string of one char |
| `vector<T>`, `array<T, N>` | an array |
| `T?` | `null`, or the value |
| a struct or class | an object, with its public fields, those of base classes first |
| `unique<T>` | what it owns |
| `weak<T>` | left out, which is what lets a child know its parent without going in circles |
| `hashmap<K, V>` | an object, in order. `K` is a `string`, or an `int` or a `long`, which is written as a string |
| `json::Value` | anything |

A pointer means nothing outside the program, so it is a compile error, naming the field, as long as it is public.

**Writing.** `json::stringify(x)` writes on one line, and `json::pretty(x)` over many. A class reference is written as
what the object really is, with all of that class's public fields, as when printing. Writing does not fail, but objects
that refer to each other in a circle, or a chain more than 512 levels deep, stop the program. A value that holds a
`unique<T>` is given with `move(x)`, as for any function.

**Reading.** `json::parse<T>(text)` can fail, so it is called with `try`, and it throws a `json::Error`, which has
`line` and `column`, and a `what()` that says where the value went wrong:

```
line 4, column 11: in points[1].x: expected a number, but found a string
```

- A key that is not a public field is left out, since what sends the JSON may know of more than this program.
- A field that is missing keeps its default value. Without one, an optional is null, and a vector, hashmap or
  `json::Value` is empty. Anything else that is missing is an error, as the program could not read that field either.
- A number must fit: `3000000000` is no `int`, and neither is `1.5`, while `1e3` and `1.0` are whole. If a key comes twice, the last one counts,
  as in JavaScript. Text must be UTF-8, and nesting past 512 levels is an error.
- An object is made as the program would make it, with the constructor without parameters, or with its fields' defaults, and then
  it is filled. A class whose constructor needs arguments, or with methods that have no body, cannot be read: the
  compiler says so. It can still be written.

**`json::Value`** holds JSON of any shape. `json::parse<json::Value>(text)` reads any JSON, and it can be a field of a struct. Each
step gives null if the value is not there or not of that kind, so a chain says what to do at the end:

```cpp
json::Value doc = try json::parse<json::Value>(text);
string city = doc.get("address")?.get("city")?.text() ?? "nowhere";
long first = doc.get("ids")?.at(0)?.whole() ?? -1;
```

| Method | Does |
|--------|------|
| `isNull()`, `isBool()`, `isNumber()`, `isString()`, `isArray()`, `isObject()` | which kind it is |
| `boolean()`, `number()`, `whole()`, `text()` | the value as a `bool?`, `double?`, `long?` or `string?`. `whole()` is exact, for a whole number that fits |
| `get(key)`, `at(index)` | a `json::Value?`, from an object or an array |
| `size()`, `keys()`, `items()` | how many it holds, the keys of an object, and the values of an array |
| `set(key, value)`, `push(value)` | adds to an object or an array. On another kind, what it held goes, and it becomes one |

`json::object()` and `json::array()` make empty ones, and `json::of(x)` makes one from any value that can be written. It prints as JSON, and it is a value, like a vector.

Not yet: other names for fields in JSON, leaving out fields with a marker, and reading into a class that derives from the one that is asked for.

### HTTP

`import http;` is a client for HTTP and WebSockets, written on [libcurl](https://curl.se/libcurl/). Each call waits for its answer, except
`http::fetch`, which an [async function](#async-functions) waits for with `await`, while the thread goes on with other async work.

```cpp
import http;
import json;

http::Response r = try http::get("https://api.example.com/users/1");
if (r.ok) {
    User u = try json::parse<User>(r.body);
    println("{}", u.name);
} else {
    println("the server said {}", r.status);
}

try http::post(url, json::stringify(user), "application/json");

try http::send({.method = "PUT", .url = url, .headers = {{"Authorization", "Bearer " + token}}, .body = text, .timeout = 5});
```

| Function | Does |
|----------|------|
| `http::get(url)`, `http::delete(url)` | a request with no body |
| `http::post(url, body, contentType)`, `http::put(...)`, `http::patch(...)` | a request with one. Saying what it is cannot be left out, since any guess would often be wrong |
| `http::send(request)` | any request, with everything below |
| `http::fetch(url)`, `http::fetchWith(request)` | as `get` and `send`, but async: `try await http::fetch(url)` |
| `http::encode(text)` | text made safe to be one part of a URL, as `encodeURIComponent` in JavaScript: `url + "?q=" + http::encode(q)` |

| Field of `http::Request` | Starts as | Is |
|--------------------------|-----------|----|
| `method` | `"GET"` | any method. It is sent in capitals |
| `url` | | starts with `http://` or `https://`. No other kind of URL works, so one from outside cannot read a file |
| `headers` | none | a `hashmap<string, string>`. `Content-Length` and `Transfer-Encoding` come from the body, and cannot be set |
| `body` | none | any bytes |
| `timeout` | `30` | seconds for all of the request, redirects included. `0` is no limit |
| `maxBytes` | `1073741824` | how large the body can be once decoded, and also the headers, so a server cannot use up all memory |
| `followRedirects` | `true` | whether to follow redirects, up to 20 |
| `caFile` | none | a file of certificates to trust instead of the system's, for a server with its own |

| Part of `http::Response` | Is |
|--------------------------|----|
| `status`, `ok` | the status as an `int`, and whether it is from 200 to 299 |
| `body` | a `string` of bytes, decoded if it was compressed |
| `url` | where the answer really came from, after redirects |
| `header(name)` | a `string?`, for a name in any case. A header that came more than once has its values joined with `, ` |
| `headers` | a `vector<http::Header>`, each with `name` and `value`, all of them in order. This is how to read each `Set-Cookie` |

**A status is an answer.** A `404` or a `500` is not an error, as in `fetch`, since the server did answer: `r.ok` and `r.status` say what it answered.

**Everything else that goes wrong is an `http::Error`**, with `what()`, `kind`, which is one of these, and `url`, which leaves out any name and password and the query, since those can be secret and errors end up in logs. So does what `what()` says.

| `kind` | What happened |
|--------|---------------|
| `"bad-request"` | the program asked for what cannot be done, such as a URL that is not http or https, or a line break in a header's value, which could otherwise add a header of its own |
| `"dns"` | no address for that name |
| `"connect"` | no server answered at that address |
| `"tls"` | the connection could not be made secure, usually because of the server's certificate |
| `"timeout"` | it took longer than `timeout` |
| `"too-large"` | more came than `maxBytes` |
| `"redirect"` | too many redirects, or one to something other than http or https |
| `"network"` | the connection failed, or the server did not send what it said it would |
| `"handshake"`, `"protocol"`, `"closed"` | for WebSockets, below |

**Redirects are followed as `fetch` follows them.** A 303 turns any method but `HEAD` into a `GET`, and a 301 or 302 turns a `POST` into one, and the body goes with them. A 307 or 308 repeats the request. On reaching another origin, meaning another scheme, host or port, `Authorization`, `Cookie`, `Proxy-Authorization` and `Host` stop being sent, even if a later redirect comes back.

**Certificates are always checked**, along with the name they are for, and there is no way to turn that off. A server with a certificate of its own is trusted with `caFile`.

**Requests on one thread share connections** to the same server, which is much quicker. The proxies of `http_proxy`, `https_proxy` and `all_proxy` are used if the environment names any, and `no_proxy` is respected.

**WebSockets** speak [RFC 6455](https://www.rfc-editor.org/rfc/rfc6455) over `ws://` or `wss://`:

```cpp
http::WebSocket ws = try http::connect("wss://example.com/chat");
try ws.send("hello");
while (auto m = try ws.receive()) println("{}", m.data);
println("closed with {}", ws.closeCode() ?? 0);
```

| Part of `http::WebSocket` | Does |
|---------------------------|------|
| `send(text)`, `sendBinary(bytes)` | send one message. `send` turns down what is not UTF-8, as the protocol requires |
| `receive()` | waits for the next message, an `http::Message?`, with `data` and `isBinary`. It is null when the connection has closed as it should |
| `close()` | sends a close, and waits up to 2 seconds for the other side's. Doing it twice is fine. The destructor does it too |
| `timeout` | seconds that `send` and `receive` wait for, `0` to begin with, which is no limit. When `receive` runs out of time nothing is lost, and the connection still works. When `send` does, the connection is over, since part of a message may have gone |
| `closeCode()`, `closeReason()` | an `int?`, null while open, and a `string`. `1006` means there was no close at all |

`http::connectWith(request)` opens one with what is in an `http::Request`: `url`, `headers`, `timeout`, for opening it, `maxBytes`, for each message, and `caFile`. An offered `Sec-WebSocket-Protocol` header can be set, but the other headers of opening one cannot. Pings are answered without the program doing anything.

**One thread can wait in `receive()` while another sends**, which is what a WebSocket is for, as a class reference can be shared with [`spawn`](#concurrency). A third can `close()` it: what waits to receive then gets null, and what waits to send gets an error of the kind `"closed"`. Messages and frames stay whole, whichever threads send them. Only one thread receives at a time: a second waits for the first.

Connecting fails with the same kinds as a request, and with `"handshake"` when the server does not open a WebSocket. A server that breaks the protocol ends the connection with `"protocol"`, and one that goes away without a close with `"network"`. After that, or after closing, `receive()` gives null and `send` throws an error of the kind `"closed"`.

`import http;` needs a compiler built with libcurl, which comes from the development files of libcurl, such as `libcurl4-openssl-dev` on Debian or Ubuntu. Only a program that imports it links it, and that program then needs the libcurl of the machine it runs on. `http::fetch` also needs libuv, as all async code does.

**`http::fetch` runs requests at once.** It gives a `Task<http::Response, throws>`, so requests started one after another are all on the way at once,
on the thread that started them, which shares connections between them, as it does for `http::send`. It fails as `http::send` does, with the
same kinds. A plain function waits for one with `try http::fetch(url).wait()`. Async WebSockets come later: `receive()` waits as it does now.

Not yet: a body that is sent or received in pieces instead of all at once, keeping cookies, learning which subprotocol the server chose, and
async WebSockets.

### Reflection

`import reflect;` lets a running program ask about types. The compiler writes down what it knows of each type a program
asks about, and of what that type leads to: this is the same data that JSON is read and written with. A program pays for nothing it does not import.

```cpp
import reflect;

struct Point { int x = 0; double y = 0; };

auto t = reflect::type<Point>();
println("{} is a {} of {} bytes", t.name(), t.kind(), t.size());      // Point is a struct of 16 bytes
for (auto f : t.fields()) println("{} at {}: {}", f.name, f.offset, f.type.name());   // x at 0: int, then y at 8: double
```

- `reflect::type<T>()` is the type `T`, and `reflect::of(x)` the type of a value: for a class reference, what the object really is.
- A `reflect::Type` has `name()`, `kind()`, which is `"int"`, `"struct"`, `"class"`, `"unique"`, `"local"`, `"vector"`, `"optional"` and so on, `size()`, which for a class is
  that of a reference, `length()` for an array, `elem()`, a `reflect::Type?` for what a vector, array, optional or unique reference
  holds, and `fields()`. Two are `==` when they are the same type.
- A `reflect::Field` has `name`, `offset`, from the start of the struct or object, `type` and `isPublic`. Private fields are there too.

Not yet: reading or changing a field through its `reflect::Field`, methods, and looking up a type by its name.

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
  itself: write `p != null` or `bool(p)`. A number and a pointer convert only with
  [`reinterpret_cast`](#casts): `(int*)0x1000` and `int* p = 0;` are errors.
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

- A constant is a number, a `char`, a `bool`, a `string` or a pointer, or `auto` for one of
  those. Other types are not supported yet; write a function that returns the value. A
  pointer has its `const` after the `*`, as in C++: `void* const NOTHING = null;`.
- Its value is worked out from literals, other constants, operators, casts, `sizeof` and calls to
  [`constexpr` functions](#constexpr). A variable or any other call is an error.
- The compiler works the value out once, while compiling, whenever it can. A call in the value must be
  able to run then, and it is an error if it cannot, since a constant has no moment to run anything
  while the program runs.
- C's macros are written this way: `const int SQLITE_OK = 0;`, and for a macro that is really
  an address, `void* const SQLITE_TRANSIENT = reinterpret_cast<void*>(-1);`.
- A value that cannot be worked out, such as `1 / 0`, stops the program when the constant is
  used, naming the constant's line. With `constexpr` it is a compile error instead.
- A constant cannot be changed, and it has no address. To point to it, copy it into a
  variable first.
- A local variable or a parameter can have the same name, and hides the constant. So does a field,
  inside its type, as in C++.
- A variable outside any function, `int count = 0;`, is not supported yet. Pass the value to
  the functions that use it, or keep it in a class object.

### constexpr

`constexpr` says a value must be worked out while compiling, as in C++. A mistake in it is then a compile error,
and it can be used where a value must be known that early: an array's size or a template argument.

```cpp
constexpr int square(int n) { return n * n; }
constexpr int SIDE = 4;
constexpr int CELLS = square(SIDE) + 1;

array<int, CELLS> grid;                     // 17 elements
Buffer<string, SIDE * 2> buffer;            // a template argument
constexpr int BAD = 1 / 0;                  // error: division by zero
```

- **A `constexpr` value** is a number, a `char`, a `bool` or a string, at the top level or in a function. It must
  have a value that can be worked out, and it cannot be changed. For a pointer, write `const`.
- **A `constexpr` function** can run while compiling, and is also an ordinary function that runs in the program with
  values only known there. It can be recursive, and have local variables, `if`, loops and `return`, and can use
  numbers, `char`, `bool` and strings, with a string's `+`, `==`, `<`, `size()`, `empty()` and `s[i]`, plus `abs`, `min`, `max`, `sizeof`
  and casts. It can call other `constexpr` functions. What a function does is only looked at when it
  runs while compiling, so it can also do things that only work in the program, such as printing, on paths that
  are not taken then. A `throw` that is reached is a compile error. Methods cannot be `constexpr` yet.
- The rules are the running program's: overflow, a bad shift and a cast that does not fit are errors, not surprises.
  A function that takes too long (about 20 million steps) or calls itself more than 512 deep is an error too.
- **An array's size and a template argument** can be any such expression, including a plain `const` with a value that can be
  worked out, as in C++: `const int N = 3; array<int, N> a;`. A `>` at the top of it would end the type,
  so write `array<int, (a > b ? 1 : 2)>` in parentheses. A function's parameter or a variable is not known then. In these two
  places, `sizeof` works on numbers, `char`, `bool`, strings, pointers and class references so far, since a struct may not have been
  read yet. It works on every type in a `constexpr` value that is not used for one of them.
- A generic `constexpr` function can run while compiling for a `constexpr` value, but not yet in an array's size or a
  template argument.

**`if constexpr`** chooses a branch while compiling. Only that branch is checked and generated. In a generic, the other
branch is not even read for these types, so it can use what they do not have, as in C++. Its condition must be
known at compile time, and cannot declare a variable.

```cpp
template<typename T>
string describe(T x) {
    if constexpr (Integer<T>) {
        return format("integer {}", x + 1);
    } else if constexpr (same_as<T, string>) {
        return "text of " + to_string(x.size());
    } else {
        return "something else";
    }
}
```

The questions about types are the [constraints](#constraints), written with their type: `Comparable<T>`, `Number<T>`, `Integer<T>`,
`Floating<T>`, or your own `Addable<T>`. There are also `same_as<T, U>` and `derived_from<T, Base>`, as C++20 names them. Each of
them is a `bool` that can be used anywhere. Template value parameters can be asked about too: `if constexpr (N == 0)`. Outside generics
both branches are still read, so both must be valid syntax.

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

Partly planned: the modules that come with the language work, which are [`http`](#http), [`json`](#json) and [`reflect`](#reflection). There are no
header files. One file is one module.

```cpp
import json;          // a standard module
import net.server;    // planned: the file net/server.cplus, relative to the project root
```

Members of a module are used with `::`, as in `json::parse<User>(text)`. What a module has is its own: a program
cannot declare a name such as `json::Mine`. There is no `std::` namespace and no `using`; built-ins need no prefix.
The compiler only accepts single-file programs so far, and reports an error for any other `import`. Only what a
program imports is linked into it.

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
`for (long x : ints)`. Over a [hashmap](#hash-maps), each element is an entry, with fields `first` and `second`.

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

### Nesting limits

Code nested too deeply is a compile error, where a C++ compiler might crash. The limits are about 333
nested parentheses, 1000 nested blocks or `else if`s in a row, and 10000 terms in one chain such as
`a + b + c` or `a.b.c`. Split such code into smaller pieces, with local variables or functions.

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
- A method named `toString` is an ordinary method for now, which printing does not call.
- What holds something of the runtime's prints as its name: `Task<int> {...}`, `Mutex {...}`, `LockGuard {...}`,
  `http::WebSocket {...}`, `reflect::Type {...}`. Its fields would only show addresses that change, or a secret in a URL.
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

A function that can fail is marked `throws`. `throw` raises an error, and `try { } catch (E e) { }` handles
it, written as in C++. Every call that can fail says so at the call, so you can see where a function can
stop. These are error values, not C++ exceptions: a failed function returns, and its caller looks at
the result right after the call. There is no unwinding of the stack, and a call that does not fail costs one comparison.

```cpp
class NotFound : public Error {
public:
    string name;
    NotFound(string n) : Error("not found: " + n), name(n) {}
};

int portOf(string name) throws {
    if (name.empty()) throw Error("no name given");
    if (name != "web") throw NotFound(name);
    return 80;
}

int doubled(string name) throws {
    int port = try portOf(name);      // an error goes on to the caller
    return port * 2;
}

int main() {
    try {
        println("{}", doubled("mail"));
    } catch (NotFound e) {
        println("{}", e.name);
    } catch (Error e) {
        println("{}", e.what());
    }
    return 0;
}
```

**What can be thrown.** An object of the built-in class `Error`, or of a class deriving from it. A number or a string
cannot be: `throw "text"` is an error that points to `throw Error("text")`. `Error` looks like this:

```cpp
class Error {
public:
    string message;
    Error(string m) : message(m) {}
    virtual string what() const { return message; }
};
```

A program can define its own class named `Error`, which then takes its place.

**A call that can fail** must be marked in one of two ways, or it is a compile error:

- `try f()` passes an error on. It applies to the one call or chain it is written on, `try a().b()`, including its
  arguments, so `try f() + g()` needs a second `try` if `g` can fail too. Outside a try block, the
  function it is in must be marked `throws`.
- Inside a `try { }` block, the call needs nothing more, and its error goes to the block's catches. `try f()` is
  allowed there too, and means the same.

A function is marked after its parameters, and after `const` on a method: `int f() const throws`. A virtual method
and its overrides must agree on `throws`. `noexcept` does not exist, since a function not marked `throws` cannot fail. A
destructor cannot be marked, and a constructor cannot be yet. Both can handle errors inside them.

**`catch`** is tried in order, and the first that takes the error's class, or a class it derives from, runs.

- `catch (NotFound e)`, or `catch (NotFound)` when the error itself is not needed. `const Error& e` is not used, since a class
  variable already refers to the object. An interface can be caught too, if a class of error derives from it.
- `catch (Error e)` and `catch (...)` take every error. A catch that can never run, because one before it
  takes everything it would, is an error.
- An error no catch takes goes on, as if the block were not there. So either something must take every error, or the
  function must be marked `throws`, which the compiler checks. It goes without checking if nothing in the block can fail.
- `throw;` inside a catch passes the same error on. A new `throw` there goes further out, not to the catches beside it.
- `throw` is a statement, so it cannot be part of an expression, and a try block has no value. There is no
  `finally`: what must always happen belongs in a destructor.

**On the way out,** everything is released as if the function had returned at that point: local variables, last
declared first, with their destructors, and whatever the expression had built so far. In
`take(Noisy("a"), try fail())`, the first argument is released if `fail` fails. Nothing is ever left half done
in memory, and the rest of the expression does not run.

Variables can be relied on as usual. A catch only knows what was assigned before the try block, since any
call inside may have failed. After the try statement, a variable has a value if the block and every
catch that does not leave gave it one.

**`int main() throws`** is allowed. An error that gets out of it prints `uncaught error:` and the error to standard error, and the
program exits with code 1.

Generic functions and methods of generic classes can be marked too. Errors are not part of a function's type
otherwise: a `throws` function does not say which errors it throws.

## Memory model

Class objects are reference-counted, and the counts are atomic, so objects can be
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
  another, without running out of stack. The exception is a link in a class whose base class
  has a destructor: that destructor runs after the link is released, so each node waits for the
  rest of the chain, and a very long chain stops the program when it runs out of stack.
- Two objects that refer to each other, such as `a.next = b; b.next = a;`, are never
  freed; this is a reference cycle. Make one of the references a
  [weak reference](#weak-references), or set one to `null` when you are done with them.
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

There are four ways to refer to an object, all to the same kind of object:

| Written | Count | Across threads | Use |
|---------|-------|----------------|-----|
| `Node` | atomic | yes | everything, unless there is a reason |
| [`unique<Node>`](#unique-references) | none to move it | handed over with `move` | an object with one owner |
| [`local<Node>`](#local-references) | not atomic | no | code on one thread whose counting is too slow |
| [`weak<Node>`](#weak-references) | does not keep it alive | yes | a reference back, which breaks a cycle |

### Local references

`local<Node>` is a class reference whose count only one thread works on. The atomic steps that keep a count safe across threads are what make counting cost anything. In a loop that does nothing but copy references, a local reference is about twenty times faster. Most programs would never notice, so reach for it only when counting shows up in what you measure.

```cpp
local<Node> a = local<Node>("x");     // builds a new Node
local<Node> b = a;                    // the same object
a.name = "y";                         // b.name is "y" too
```

- It is used as a class reference: `a.name`, `a.go()`, shared by copies and freed with the last, `==` for being the same object, in vectors, structs and so on. It is never empty, and `local<Node>?` can be.
- **A local reference only comes from another**, and only gives one, since the object's count must not also be used by other threads: `Node n = a;` and `local<Node> a = n;` are errors. `local<Dog>` converts to `local<Animal>`. `a.get()` lends the object as a `Node`, as for a [unique reference](#unique-references).
- **It cannot go into `spawn`.** The compiler refuses any argument, object or result whose type holds one, however deep, in a field of a class of a struct in a vector.
- That looks at types, so what `a.get()` lent, or what a method did with `this`, can still reach a thread, where the count would go wrong. [`--race`](#the-race-check) reports that.

### Weak references

`weak<Node>` refers to a class object without keeping it alive, so two objects can refer to each
other and still be freed. It works like C++'s `std::weak_ptr`. `lock()` gives a `Node?`, which is
null when the object is gone, and keeps the object alive for as long as that reference lasts.

```cpp
class Tree {
public:
    string name;
    weak<Tree> parent;        // starts out empty
    vector<Tree> children;

    void add(Tree child) {
        child.parent = this;
        children.push_back(child);
    }
    string path() const {
        if (auto p = parent.lock()) return p.path() + "/" + name;
        return name;
    }
};
```

- `lock()` gives a `Node?`. `w.lock()?.name ?? "gone"` and `if (auto n = w.lock())` are how to reach the
  object, since it cannot be reached through `w` itself. `expired()` is true when the object is gone.
  `reset()`, or `w = null`, empties `w`.
- `Node`, `Node?`, `null` and a `weak` to a class deriving from `Node` convert to `weak<Node>` by
  themselves, as a `Dog` converts to an `Animal`. `weak<Node>(n)` makes one where `auto` needs a type, and
  `weak<Node>()` is empty. A `weak<Node>` converts to nothing by itself, since the object may be gone.
- `weak<Node> w = Node("a");` makes the object and frees it at once, as nothing keeps it.
- Only a class can be the target, or an interface. A struct is a value, so nothing refers to it.
  `weak<Node>?` is an error, since it can already be empty.
- It is a value like any other: it can be a field or a parameter, and can go in a vector or an array.
  `weak<Node> w;` is empty, as is a field with no value given.
- `==` is true for two references to one object, and for two with nothing there, whether they were
  always empty or the object has gone. It compares only with another of the same type. `w == null` is an
  error that points to `expired()`.
- `{}` prints `weak<Node>` while the object is there and `null` once it is gone, and does not print the
  object, which is often the very one that is printing it.
- While an object's destructor runs it is already going away, as in C++: `lock()` gives null and
  `expired()` is true, also for a weak reference made in the destructor.

Each object counts its weak references. When the last strong reference goes away, the object's
destructor runs and its fields are released, freeing what they hold. The object's own memory stays
until the last weak reference goes too, as with `std::make_shared`. An object has room for about 2 billion
strong and 1 billion weak references. Going over that stops the program.

### Unique references

`unique<Node>` is the one owner of a class object, like C++'s `std::unique_ptr`, except that it is never empty.
It cannot be copied, only handed over with `move`, and the object goes away with its owner.

```cpp
unique<Node> a = unique<Node>("first");     // builds a new Node, with these constructor arguments
a.name = "changed";                         // used directly, with '.'
println("{} {}", a.name, a);
unique<Node> b = move(a);                   // b owns it now, and a has no value
a.rename("x");                              // error: 'a' has no value here, since it may have been moved away
a = unique<Node>("second");                 // a can be used again
take(move(b));                              // handed to the function, which frees it when it returns
lend(a.get());                              // `void lend(Node n)`: a keeps it
```

- **`move(x)`** gives x's value away, and leaves a variable with no value until it gets a new one. The compiler follows this
  through branches, loops, `&&` and try blocks. A value moved in a loop needs a new value before the loop goes round. `return a;` on a
  local variable moves it by itself, as in C++. `move` works on a value of any type, so generic code can use it for every `T`.
- **Fields and elements** are not followed by the compiler. `move(o.child)` or `move(v[0])` leaves that place empty. Using it or moving
  from it again then stops the program, and printing it shows `moved`. For a type that can be copied, such as a string or a class, `move` on a field or an element copies it and leaves it as it was.
- **It cannot be copied**, so neither can what holds one, such as a struct with a `unique<Node>` field or a `vector<unique<Node>>`: `b = a` is an
  error that shows what to write. Move them instead, and reach elements by position, as `for (auto x : v)` would copy each.
- **Never empty:** `unique<Node> u;` must be assigned before it is read, as a class variable must. For one that can be empty, write
  `unique<Node>?`, and use `?.`, `if (auto n = move(opt))` and `opt = null`. A field needs a value when the object is built.
- **`u.get()`** lends the object as an ordinary `Node`, a counted reference. If it is kept, the object stays until that is gone too, so a borrowed
  reference can never dangle, though the object then outlives its owner. It is the one method of the reference itself, so a method named `get`
  of the object is written `u.get().get()`.
- **Conversions:** `unique<Dog>` converts to `unique<Animal>` and to `Animal` when moved. Going to `Animal` gives up being unique for good. Nothing
  converts to a `unique`, since an object that may be shared has no one owner.
- Only a class can be the target, or an interface. `make_unique` and `unique_ptr` are written `unique<Node>(...)` and `unique<Node>`. There is no `reset` or
  `release`: assign a new object, or use `unique<Node>?` and `null`.
- `==` is true for the same object, and `{}` prints the object, as with a class reference.

An object held by `unique` has the same layout as any other, so a class is compiled once however it is held. The count is only touched when a reference is
lent, or while a method runs or a value is read for a statement, which keeps the object safe if that code replaces its owner. A move touches no count.

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
  `CPLUS_CHECK_LEAKS=1`, and the report says what each object is and where it was made, for
  the first ten. `CPLUS_CHECK_LEAKS=0` turns the report off.

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

A generic function or type is written once with C++20's syntax, and the compiler makes a separate
copy for each set of types it is used with. There is no cost when the program runs, as with C++
templates.

```cpp
template<typename T>
T largest(T a, T b) {
    return a < b ? b : a;
}

template<typename T>
class Stack {
    vector<T> items;
public:
    void push(T x) { items.push_back(x); }
    T pop() {
        T top = items.back();
        items.pop_back();
        return top;
    }
    long size() const { return items.size(); }
};

println("{}", largest(1, 2));           // T is int
println("{}", largest<double>(1, 2.5)); // T is written
Stack<string> s = Stack<string>();
s.push("a");
println("{}", s);                       // Stack<string> {items: ["a"]}
```

- `typename` and `class` mean the same in the parameter list. A parameter can also be a value of type
  `int`, `long`, `bool` or `char`: `template<typename T, int N> struct Buffer { array<T, N> items; };`, used as
  `Buffer<string, 16>`. Inside, `N` is that value, and it can be an array size. The value can be
  [worked out while compiling](#constexpr): `Buffer<string, MAX * 2>`.
- **Calling a generic function** works out its parameters from the arguments, also through what a
  parameter is written as, such as `vector<T>`, `T?`, `T*`, `array<T, N>`, `weak<T>` or `Box<T>`, including a
  base class of the argument. Arguments must agree: `largest(1, 2.5)` is an error, since T cannot be both, so write
  `largest<double>(1, 2.5)`. Written ones come first, and what is not written is worked out. Values
  convert to a written type as they do for any function. A `{ }` list or `null` says nothing about T.
- **A generic type** is always written with its arguments, `Stack<int>`, also when building one. C++ can work
  them out in `Stack(1)`, which is planned here. Inside its own definition, `Stack` alone means the type being made.
- Inside, T is used as any type's name: `T x = ...;`, `vector<T>`, `T*`, `T?`, `sizeof(T)`, `static_cast<T>(x)`. `T(x)` converts
  to it or calls its constructor. `T()` is the default value: zero, `false`, an empty string or vector, null. On
  a struct or class it calls the constructor, as `Point()` does. `T{1, 2}` builds a struct, class, vector or array.
- A generic type can derive from anything, and be derived from: `class Repeat : public Source<T>`,
  `class Counter : public Source<int>`, `template<typename T> interface Source { ... };`, `class Logged : public T`. A
  base class is built with `: Base<T>(x)` or `: Base(x)`, and reached with `Base<T>::f()` or `Base::f()`.
- Function and type names show the arguments in errors, printing and stack traces: `Stack<int>`, `largest<double>`.

**What is checked when.** Syntax is checked where a generic is written. Everything else is checked when it is
used, separately for each set of types, and an error says which types and where they came from.

```
error: '<' cannot be used with 'Point'
 --> shapes.cplus:7:12
  |
7 |     return a < b ? b : a;
  |            ^

note: in 'largest<Point>', which is used here
  --> shapes.cplus:51:19
   |
51 |     println("{}", largest(Point{1}, Point{2}));
   |                   ^~~~~~~
```

As in C++, a method of a generic class is only checked once it is used, so `Box<Point>` is fine if nothing
calls the `less` that needs `<`. Virtual methods, constructors and destructors are always checked, since they may
run without being named. So is every field. A generic that is never used is checked only for syntax, so an
undeclared name in one goes unnoticed until it is used. A generic that keeps needing larger versions of itself,
such as `f<T>` calling `f<vector<T>>`, is an error rather than a compiler that never ends.

Left out on purpose: template specialization, SFINAE and templates with any number of parameters. For different
code for different types, use [`if constexpr`](#constexpr). Not yet: default template arguments, `requires` clauses, generic methods
and working out a type's arguments.

### Constraints

Writing a name in place of `typename` says what the type must be able to do, as with C++20's concepts. It is
checked where the generic is used, so the error is about your type, instead of about code you did not write.

```cpp
template<Comparable T>
T largest(T a, T b) {
    return a < b ? b : a;
}

largest(Point{1}, Point{2});
```

```
error: 'Point' cannot be used for 'T' of 'largest', which must be 'Comparable'
  = help: 'Point' has no `<`. Only numbers, chars, strings and pointers do, until operators can be overloaded
```

The name is one of:

- **Built in.** `Comparable` has `<`: numbers, `char`, strings and pointers. `Number` is `int`, `long`, `float` or
  `double`. `Integer` is `int` or `long`, and `Floating` is `float` or `double`. `Hashable` can be a key of a
  [hashmap](#hash-maps).
- **An interface or a class.** The type must derive from it, holding it once:
  `template<Speaker T> string talk(T s) { return s.speak(); }`.
- **Your own**, written as in C++20. Each line is an expression that must compile for the type:

```cpp
template<typename T>
concept Addable = requires(T a, T b) {
    a + b;
};

template<Addable T>
T sum(vector<T> items) { ... }
```

A constraint is only a check on what is passed in. The generic's body is still checked for each type, and it may
do things the constraint does not mention. A name written in the program takes the place of a built-in one
with the same name.

## Concurrency

`spawn` runs a call on a new thread, and gives a `Task` to get its result from. An [async function](#async-functions) gives one
too, for work that mostly waits, which many of them can share a thread for.

```cpp
long sum(vector<long> numbers) {
    long total = 0;
    for (long n : numbers) total += n;
    return total;
}

Task<long> a = spawn sum(first);
auto b = spawn sum(second);           // the same, with the type worked out
println("{}", a.wait() + b.wait());
```

**What `spawn` takes** is a call of a function or a method written in C+: `spawn f(x)`, `spawn object.method(x)`. The arguments, and what a
method is called on, are worked out right there, in order, before the thread starts, so an error or a change after that has nothing to do with
them. For a built-in such as `println`, a C function or a constructor, write a function that does it, and spawn that. Generic functions work,
and `try` does not belong on the call, since its error goes in the task: an argument that can fail is written `spawn f(try g())`.

`spawn` takes all of the call after it, so `spawn f(x).wait()` would wait inside the new thread, for what `f` gave. To wait for the task at once, write `(spawn f(x)).wait()`.

**A thread gets values of its own.** Strings, vectors, hashmaps, structs and arrays are values, so the thread has a copy, which is cheap, and
what it changes does not show anywhere else. The method of a struct runs on a copy of the struct. A `unique` reference is handed over with
`move`. A **class reference is shared**, so two threads reach the same object: that is what [`Mutex`](#mutex) is for.

**The type of a task** says what `wait()` gives, and whether it can fail:

| The function | `spawn` gives | `wait()` |
|--------------|---------------|----------|
| `int f()` | `Task<int>` | gives an `int` |
| `void f()` | `Task<void>` | gives nothing to use |
| `int f() throws` | `Task<int, throws>` | gives an `int`, and is written `try t.wait()` |
| `void f() throws` | `Task<void, throws>` | gives nothing to use, and is written `try t.wait()` |

- `t.wait()` waits for the thread, and gives what the function returned, or throws what it threw, as what it really is. It can be called again, and
  gives the same. A value that holds a `unique` reference is taken out by the first one, and using it after a second **stops the program**.
- `t.done()` says whether the thread has finished, without waiting.
- A `Task` is a class reference, so copies are the same task. It [prints](#printing) as `Task<int> {...}`.
- If several threads stop the program at once, as with a failed check, one reports it. A report does not wait long for a thread that is stuck writing to the output.

**Letting go of a task does not stop its thread or wait for it**, so `spawn save(data);` is fine by itself, unlike C++'s `std::async`. What it
would have returned is dropped. An error that nothing can ever see any more is not: as with one that gets out of `main`, it is printed, and the
program fails with exit code 1. To handle it, keep the task and wait for it.

**The program does not end while a thread is running.** When `main` returns, it waits for all of them, as Node does for what it has pending, so work
is not cut off without a word, and then it exits with what `main` returned. A thread that never ends keeps the program running. C's `exit()` does not wait.

**A runtime error in any thread stops the whole program**, with the message and the place, as it would on the main thread. Each thread has 8 MiB of
stack. Running out of it is reported as on the main thread.

**What one call of `print` or `println` prints stays together**, in a program with `spawn`. The lines of different threads can come in any order.

**A destructor runs on whichever thread lets go of the object last.** An object's count is atomic, so it is safe to share the references themselves.

### Async functions

An async function can wait for something without holding up its thread, as in JavaScript. Calling it starts it, and gives a
[`Task`](#concurrency), the same type that `spawn` gives. `await` waits for a task, and meanwhile the thread goes on with other async work.

```cpp
async string greet(string name, long ms) {
    await delay(ms);                    // waits without holding up the thread
    return format("hello, {}", name);
}

async int main() {
    auto slow = greet("slow", 40);      // both are waiting at once
    auto quick = greet("quick", 10);
    println("{}", await quick);         // hello, quick
    println("{}", await slow);          // hello, slow
    return 0;
}
```

**`async` is written in front**, before the return type: `async int f(int x)`, `async void save(string text) throws`. In a class it can be a method, and it
can be virtual: `virtual async string read() const { ... }`, which an override must match. A method of a struct must be `const`, since it runs on a copy of
the struct. A generic one is `template<typename T> async T f(T x)`. A constructor or a destructor cannot be async.

**Calling it gives a task**, and `try` does not belong on the call, since what it throws goes in the task:

| The function | A call gives | `await t` |
|--------------|--------------|-----------|
| `async int f()` | `Task<int>` | gives an `int` |
| `async void f()` | `Task<void>` | gives nothing to use |
| `async int f() throws` | `Task<int, throws>` | gives an `int`, and is written `try await t` |

A call runs the function at once, until it waits for something that is not done yet, and then gives the task, as in JavaScript. So after
`auto a = f(1);` and `auto b = f(2);`, both are running, and `await a` then waits for the first. Throwing the task away, as in `f(1);`, is an
error, since nothing would wait for it: write `await f(1);`, or keep the task to let it run on.

**`await t` can only be written in an async function**, and `main` can be one: `async int main()`. It works on any task, `spawn`'s included,
so `await spawn sum(v)` waits for a thread without holding up the others. A plain function waits with `t.wait()`, which runs the thread's
async work until the task is done. In an async function, `wait()` is an error, as it would hold up everything else on the thread there.
`delay(ms)` is an async function that waits that many milliseconds, so a plain function waits with `delay(ms).wait()`.

**The program does not end while an async function is running.** When `main` returns, the main thread goes on with its async work until it
has none left, as Node does, and a thread does the same when its function returns. An error that nothing can see any more fails the program
with exit code 1, as with `spawn`.

**After an `await`, a function may go on on another thread.** This compiler keeps it on the thread it started on, but a program must not count
on that, so that a later one can share the work between threads. So a class object that two async functions change needs a [`Mutex`](#mutex),
as with threads, and a [`local`](#local-references) reference can be neither kept across an `await`, nor taken or returned by an async function.
A `LockGuard` cannot be kept across an `await` either, since other async work of the thread could try to lock the mutex meanwhile: lock it in a
block that ends before the `await`.

**What can never be done stops the program**, with a message: waiting for a task whose function waits, directly or through others, for
its own task, and async functions still waiting at the end of the program for tasks that nothing can finish any more. So does `wait()` in a
plain function that async code calls, once that code has waited for something, since the thread's other async work then waits for that
function to return.

Not yet: stopping a task, and waiting with a time limit. [`http::fetch`](#http) is the async request.

### Mutex

Two threads must not use the same memory at the same time if either of them is changing it, which in C+ can only be a class's field or what a
pointer leads to. A wrong result from that can take a very long time to show up. A `Mutex` keeps them apart:

```cpp
class Account {
public:
    long balance = 0;
    Mutex lock = Mutex();

    void add(long n) {
        LockGuard guard = LockGuard(lock);    // locks it, and unlocks it at the end of the block, however it ends
        balance += n;
    }
};
```

- `Mutex m = Mutex();` is a class object, so copies are the same mutex. `m.lock()`, `m.unlock()` and `m.try_lock()` work as in C++.
- `LockGuard g = LockGuard(m);` unlocks it again when the block ends, an error included. Prefer it, since a `lock()` with no `unlock()` on the way out of an error
  leaves every other thread waiting forever.
- A thread that locks a mutex it already holds, or unlocks one it does not hold, **stops the program**, instead of the undefined behavior of C++.

### The race check

`cplus run app.cplus --race` makes a build with [ThreadSanitizer](https://clang.llvm.org/docs/ThreadSanitizer.html), which stops the program at the
first use of memory by two threads at the same time, when at least one is changing it. It reports both places, whether or not this run gave a wrong answer. It also reports locks that
are taken in different orders, which can leave two threads stuck, even on a run that did not get stuck.

```
WARNING: ThreadSanitizer: data race
  Read of size 4 at 0x720800000030 by thread T2:
    #0 cplus.addMany app.cplus:2:68
  Previous write of size 4 at 0x720800000030 by thread T1:
    #0 cplus.addMany app.cplus:2:68
  ...
fatal error: two threads used the same memory at the same time, and at least one of them was changing it
  = help: the report above shows both places. Protect them with a Mutex, or give each thread a copy of its own
```

- It finds what happens on the run it watches, so run what the threads really do.
- Code that mostly uses memory can be about 15 times slower with it, and take about 8 times the memory, while code that mostly computes is hardly slowed. It cannot go with `--debug`, as each watches memory in its own way. It needs the same libraries as `--debug`, such as `libclang-rt-18-dev` on Debian or Ubuntu.
- It can only follow 60000 calls at once, since ThreadSanitizer has room for no more. A program that goes deeper stops with an error that says so, even if it would run without `--race`.

Not yet: waiting for a thread with a time limit, atomic values, condition variables and channels, and starting a C function directly.

## The cplus command

```
cplus check <file.cplus>                  report errors without building
cplus build <file.cplus> [-o <output>]    compile to a native executable, named after the file if no -o
cplus run <file.cplus> [-- <args>...]     build to a temporary file and run it
cplus fmt <file or folder>... [--check]   rewrite files in the style of C+, as in the next section
cplus lsp                                 a language server, which an editor starts, as in the section after that
cplus explain [<topic>]                   the part of this document on a topic, with an example that runs
cplus --version
```

| Option | Takes effect on | Does |
|--------|-----------------|------|
| `-O` | `build`, `run` | optimizes |
| `--debug` | `build`, `run` | a [debug build](#debug-builds), which finds memory errors and leaks |
| `--race` | `build`, `run` | a build with [the race check](#the-race-check). It cannot go with `--debug` |
| `-l<name>`, `-L<dir>` | `build`, `run`, `check` | the libraries of [C functions](#c-functions), as for a C compiler |
| `-Wno-new-delete` | `build`, `run`, `check` | no warning for [`new` and `delete`](#new-and-delete) |
| `--emit-llvm` | `build` | prints the LLVM IR instead of building |
| `--check` | `fmt` | changes nothing, and says which files are not [formatted](#formatting) |

`cplus explain` with no topic lists them all. A topic is the name of a section, as in the `spec:` line of an error, or a word you know from C++ or JavaScript, such as
`hashmap`, `spawn` or `try`. `cplus explain cheatsheet` is how C+ differs from C++, on one page. All of it is built into the compiler, so it works away from this repository.

**Exit codes.** `check` and `build` exit with 0 if all is well, with 1 for errors in the program or a failure to build it, and with 2 for a wrong command line. `run` exits with what the program does.
A program exits with what `main` returns, with 1 for an error that nothing catches, and with 134 when it is stopped by a runtime check or a crash.

### Formatting

```
cplus fmt app.cplus lib.cplus    rewrites the files in the style of C+
cplus fmt --check app.cplus      changes nothing, and says which files are not in it
cplus fmt src                    rewrites every .cplus file in src and the folders in it
cplus fmt -                      reads a program from stdin, and writes it to stdout
```

A folder's files are taken in the order of their names, and folders whose names start with a dot are left out.

C+ has one style, with no settings, so code reads the same in every project. `cplus fmt` changes only the space between
tokens: how far each line is indented, the spaces within a line, and the blank lines. The tokens, the comments, and
where lines break stay as they are written, except where a rule below breaks a line.

- An indent is four spaces. A block's `{` goes at the end of the line its statement starts on, and its `}` on a line of
  its own, under the start of the statement. `} else {`, `} catch (Error e) {` and the `} while (...);` of a `do` go on one line.
- Each statement in a block goes on a line of its own, so `if (ok) { n = 1; y = 2; }` becomes four lines. A
  function, a type or an `extern "C"` block written on one line stays on one, such as `int area() const { return w * h; }`.
  `{ }` becomes `{}`, and an empty block written over two lines stays on two.
- The statement of an `if`, `else`, `for`, `while` or `do` with no braces is indented four more when it is on a line of
  its own.
- `public:`, `private:` and `protected:` go at the indent of their type.
- There is at most one blank line in a row, and none just after a `{`, just before a `}`, or at the start or end of the
  file.
- A line that goes on from the one before is indented four more than its statement. Inside brackets, it lines up just
  after the `(`, `[` or `{` it is in, if something follows that on the bracket's own line. Otherwise it is four more than
  the bracket's line, or eight in the parentheses of an `if`, `for` or `while` if four would put it where the body goes.
- Spaces go around binary operators and `=`, and after `,` and `;`: `a + b * c`, `min(a, b)`. None go just inside brackets,
  after a unary operator or a cast, or around `.`, `->`, `?.` and `::`: `-x`, `!found`, `(int)x`, `p->x`, `vector<int>`.
  A type's `*` or `?` goes with the type: `int* p`, `User? u`.
- `//` comments at the ends of lines next to each other line up, two spaces after the longest code. What a comment
  says is never changed.
- Long lines are not wrapped. Lines end with a line feed, and the file ends with one.

A file with syntax errors is left as it is, and the errors are reported. After formatting a file, `cplus fmt` reads
what it wrote, and keeps it only if it has the same tokens and comments as the file. If it does not, that is a bug in
`cplus fmt`, which it reports, and the file is left as it is.

`cplus fmt` exits with 0 when all went well, with 1 when a file has errors, cannot be read or written, or, with `--check`, is
not formatted, and with 2 for a wrong command line.

### The language server

`cplus lsp` speaks the Language Server Protocol, which most editors know, on stdin and stdout. An editor starts it, and
does not show it. For VS Code, `editors/vscode` in this repository is an extension that starts it and colors C+ code, and
its README says how to install it. Other editors are told to run `cplus lsp` for files whose names end in `.cplus`.

It gives an editor:

- the errors and warnings of each open file as it changes, with their help and spec, as `cplus check` would report them
- for the name under the mouse, its declaration as C+ writes it, with the `//` comment just above the declaration, and for
  `auto`, the type it turned out to be
- where a name is declared, for "go to definition"
- the file in the style of `cplus fmt`, for "format document". A file with syntax errors is left as it is.

A name in generic code only has a meaning once the generic is used, so a hover there shows what it means in the first use,
and in a generic that nothing uses it shows nothing. What comes with the language, such as `println`, has no declaration to
go to.

Each version of a file is checked in a process of its own, so that a crash of the compiler on code that is half written is
reported as an error in the file, and the server goes on.

## Grammar

The rules of what can be written, in EBNF. `{ x }` is any number of `x`, `[ x ]` is an optional one, and text in quotes is written as it is. What the rules allow can still be an error for other
reasons, such as the type of a value. What is reported as not supported yet is left out.

```
program      = { import | item } ;
import       = "import" name ";" ;
item         = function | record | generic | constant | extern | attribute ;
attribute    = "[[" "allow" "(" "new_delete" ")" "]]" [ ";" ] ;
name         = letter { letter | digit | "_" } ;              (* with `module::name` for what a module has *)

function     = type name "(" [ params ] ")" [ "throws" ] block ;
params       = param { "," param } ;
param        = [ "const" ] type name ;
constant     = ( "const" | "constexpr" ) type name "=" expr { "," name "=" expr } ";"
             | "constexpr" function ;

record       = ( "struct" | "class" | "interface" ) name [ ":" base { "," base } ] "{" { member } "}" ";" ;
base         = "public" name ;                                (* a class or an interface; a struct has none *)
member       = ( "public" | "private" | "protected" ) ":"
             | type name [ "=" expr ] ";"                     (* a field *)
             | [ "virtual" ] type name "(" [ params ] ")" [ "const" ] [ "throws" ] [ "override" ] ( block | "=" "0" ";" | ";" )
             | name "(" [ params ] ")" [ ":" init { "," init } ] block            (* a constructor *)
             | "~" name "(" ")" block ;                       (* a destructor *)
init         = name "(" [ args ] ")" | name "{" [ args ] "}" ;

generic      = "template" "<" tparam { "," tparam } ">" ( function | record )
             | "template" "<" "typename" name ">" "concept" name "=" "requires" "(" params ")" "{" { expr ";" } "}" ";" ;
tparam       = ( "typename" | "class" | name ) name           (* the third is a constraint, or the type of a value *)
             | ( "int" | "long" | "bool" | "char" ) name ;

extern       = "extern" '"C"' ( cItem | "{" { cItem } "}" ) ;
cItem        = type name "(" [ params ] ")" ";" | "struct" name ";" | record ;

type         = [ "const" ] baseType { "*" [ "const" ] } [ "?" ] ;
baseType     = "int" | "long" | "double" | "float" | "bool" | "char" | "string" | "void" | "auto"
             | "vector" "<" type ">"
             | "array" "<" type "," expr ">"
             | "hashmap" "<" type "," type ">"
             | "unique" "<" name ">" | "local" "<" name ">" | "weak" "<" name ">"
             | "Task" "<" type [ "," "throws" ] ">"
             | name [ "<" targ { "," targ } ">" ] ;           (* a struct, class or C type, or one made from a generic *)
targ         = type | expr ;

block        = "{" { statement } "}" ;
statement    = block | ";" | declaration | expr ";"
             | "if" [ "constexpr" ] "(" condition ")" statement [ "else" statement ]
             | "while" "(" condition ")" statement
             | "do" statement "while" "(" expr ")" ";"
             | "for" "(" ( declaration | [ expr ] ";" ) [ expr ] ";" [ expr ] ")" statement
             | "for" "(" type name ":" expr ")" statement
             | "return" [ value ] ";" | "break" ";" | "continue" ";"
             | "throw" [ expr ] ";"
             | "try" block catch { catch } ;
catch        = "catch" "(" ( type [ name ] | "..." ) ")" block ;
condition    = expr | type name "=" expr ;                    (* the second unwraps an optional *)
declaration  = [ "const" | "constexpr" ] type declarator { "," declarator } ";" ;
declarator   = { "*" [ "const" ] } name [ "=" value ] ;
value        = expr | list ;
list         = "{" [ item2 { "," item2 } [ "," ] ] "}" ;
item2        = [ "." name "=" ] value ;

expr         = ternary [ assignOp expr ] ;
assignOp     = "=" | "+=" | "-=" | "*=" | "/=" | "%=" | "&=" | "|=" | "^=" | "<<=" | ">>=" ;
ternary      = coalesce [ "?" expr ":" expr ] ;
coalesce     = binary [ "??" coalesce ] ;
binary       = unary { binaryOp unary } ;                     (* by the table in [Operators] *)
binaryOp     = "||" | "&&" | "|" | "^" | "&" | "==" | "!=" | "<" | ">" | "<=" | ">=" | "<<" | ">>" | "+" | "-" | "*" | "/" | "%" ;
unary        = ( "!" | "-" | "+" | "~" | "*" | "&" | "++" | "--" ) unary
             | "try" unary | "spawn" unary
             | "new" type [ "(" [ args ] ")" | list | "[" expr "]" ]
             | "delete" [ "[" "]" ] unary
             | "sizeof" ( "(" type ")" | unary )
             | "(" type ")" unary
             | postfix ;
postfix      = primary { "." name [ "::" name ] [ "(" [ args ] ")" ]
                       | "?." name [ "(" [ args ] ")" ]
                       | "->" name [ "(" [ args ] ")" ]
                       | "[" expr "]" | "++" | "--" } ;
primary      = literal | name | "this" | "null" | "(" expr ")"
             | name [ "<" targ { "," targ } ">" ] "(" [ args ] ")"
             | type list | type "(" [ args ] ")"              (* builds a value, or converts one *)
             | ( "static_cast" | "reinterpret_cast" | "dynamic_cast" ) "<" type ">" "(" expr ")"
             | "move" "(" expr ")" ;
args         = value { "," value } ;
literal      = integer | float | string | char | "true" | "false" ;
```

Integers, floats, strings and chars are in [Lexical structure](#lexical-structure). There is no comma operator: a comma only separates declarators, arguments and the items of a list.

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
| Casts: `int(x)`, `static_cast<int>(x)`, `(int)x`, stopping when the value does not fit; `reinterpret_cast` | implemented |
| Math functions from `<cmath>`, `abs`, `min`, `max` | implemented |
| `char`: `'a'`, checked `s[i]`, `for (char c : s)`, `push_back`, `pop_back`, `front`, `back`, `string(n, c)`, `<cctype>` functions | implemented |
| `extern "C"` functions taking and returning numbers, `char`, `bool`, `string`, `string?`; `-l` and `-L` | implemented |
| Pointers: `T*`, `&x`, `*p`, `p->x`, `p[i]`, arithmetic, `const T*`, `void*`, null checks, `sizeof` | implemented |
| Pointers in `extern "C"`, and C types known by name, such as `FILE*` | implemented |
| C structs: fields, array fields, through pointers, and by value on Linux x86-64 and aarch64 | implemented |
| `array<T, N>`: checked `a[i]`, `size`, `empty`, `front`, `back`, `fill`, `data`, range-based `for`, `==`, pointers to elements, `string(a)` | implemented |
| `new`, `new[]`, `delete`, `delete[]`, with a warning; `[[allow(new_delete)]]`, `-Wno-new-delete` | implemented |
| Constants outside functions: numbers, `char`, `bool`, `string`, pointers | implemented |
| `main` with `argc` and `argv` | implemented |
| `cplus check`, `build`, `run`, `explain`, `--emit-llvm`, `-O` | implemented |
| `cplus fmt` | implemented |
| `cplus lsp`: errors as you type, hover, go to definition, formatting; an extension for VS Code | implemented |
| Debug builds with `--debug`: AddressSanitizer, stack traces, misaligned pointer and invalid bool checks, leak report | implemented |
| Programs of more than one file | planned |
| `constexpr` values and functions, `if constexpr`, array sizes and template arguments worked out while compiling | implemented |
| `constexpr` methods and structs; a generic `constexpr` function in an array's size | planned |
| Variables outside functions; constants of other types | planned |
| Enums, `switch`, lambdas | planned |
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
| `hashmap<K, V>` | implemented |
| Vector `insert`, `erase`, iterators; `find` on a hashmap, sorted maps, sets | planned |
| References (`T&`), including `for (auto& x : v)` | planned |
| Structured bindings, `for (auto [k, v] : m)` | planned |
| Inheritance, `virtual`, `override`, `= 0`, `protected:`, `dynamic_cast`, `Base::method()` | implemented |
| Multiple inheritance, interfaces | implemented |
| `final`, covariant return types, private and protected base classes | planned |
| Operator overloading | planned |
| Destructors for structs, static members | planned |
| `weak<T>` references: `lock`, `expired`, `reset` | implemented |
| Errors: `throws`, `throw`, `try f()`, `try { } catch (E e) { }`, `catch (...)`, `throw;`, `Error`, `int main() throws` | implemented |
| Constructors that can fail | planned |
| Generics: functions, structs, classes and interfaces, type and value parameters, working out arguments in calls | implemented |
| Constraints: `Comparable`, `Number`, `Integer`, `Floating`, interfaces and classes, `concept ... = requires` | implemented |
| Default template arguments, `requires` clauses, generic methods, working out a generic type's arguments | planned |
| Pointers into vectors and strings | planned |
| `unique<T>` references and `move(x)` | implemented |
| `local<T>` references | implemented |
| C callbacks and variables in `extern "C"`; 16-bit integers and unions in C structs; reading C headers | planned |
| Modules `http`, `json` and `reflect`, and raw strings | implemented |
| `http::fetch` | implemented |
| Modules of your own | planned |
| Threads: `spawn`, `Task`, `Mutex`, `LockGuard`, `--race` | implemented |
| `async` functions, `await`, `delay` | implemented |
| Atomic values, condition variables, channels | planned |

Anything marked planned is reported by the compiler as "this compiler does not support ...
yet", with a link here.
