// C functions for the tests of structs passed by value. A C compiler lays out each call
// here, so calling them from C+ checks that C+ follows the platform's rules.
#include <stdbool.h>

typedef struct { char c; } OneChar;
typedef struct { int a; } OneInt;
typedef struct { int a; int b; } TwoInts;
typedef struct { int a; int b; int c; } ThreeInts;
typedef struct { long a; long b; } TwoLongs;
typedef struct { long a; long b; long c; } ThreeLongs;
typedef struct { float x; } OneFloat;
typedef struct { float x; float y; } TwoFloats;
typedef struct { float x; float y; float z; } ThreeFloats;
typedef struct { double x; } OneDouble;
typedef struct { double x; double y; } TwoDoubles;
typedef struct { double x; double y; double z; } ThreeDoubles;
typedef struct { double x; double y; double z; double w; } FourDoubles;
typedef struct { double a; double b; double c; double d; double e; } FiveDoubles;
typedef struct { double x; long n; } DoubleLong;
typedef struct { long n; double x; } LongDouble;
typedef struct { int n; float x; } IntFloat;
typedef struct { float x; int n; double d; } Mixed;
typedef struct { TwoInts inner; long n; } Nested;
typedef struct { TwoFloats a; TwoFloats b; } NestedFloats;
typedef struct { bool flag; char c; int n; } Small;
typedef struct { const char* text; long size; } Text;

OneChar one_char(OneChar s) { s.c += 1; return s; }
OneInt one_int(OneInt s) { s.a *= 2; return s; }
TwoInts two_ints(TwoInts s) { TwoInts r = {s.b, s.a}; return r; }
ThreeInts three_ints(ThreeInts s) { ThreeInts r = {s.c, s.b, s.a}; return r; }
TwoLongs two_longs(TwoLongs s) { TwoLongs r = {s.b, s.a}; return r; }
ThreeLongs three_longs(ThreeLongs s) { ThreeLongs r = {s.c, s.b, s.a}; return r; }
OneFloat one_float(OneFloat s) { s.x *= 2; return s; }
TwoFloats two_floats(TwoFloats s) { TwoFloats r = {s.y, s.x}; return r; }
ThreeFloats three_floats(ThreeFloats s) { ThreeFloats r = {s.z, s.y, s.x}; return r; }
OneDouble one_double(OneDouble s) { s.x *= 2; return s; }
TwoDoubles two_doubles(TwoDoubles s) { TwoDoubles r = {s.y, s.x}; return r; }
ThreeDoubles three_doubles(ThreeDoubles s) { ThreeDoubles r = {s.z, s.y, s.x}; return r; }
FourDoubles four_doubles(FourDoubles s) { FourDoubles r = {s.w, s.z, s.y, s.x}; return r; }
FiveDoubles five_doubles(FiveDoubles s) { FiveDoubles r = {s.e, s.d, s.c, s.b, s.a}; return r; }
DoubleLong double_long(DoubleLong s) { s.x *= 2; s.n *= 2; return s; }
LongDouble long_double(LongDouble s) { s.x *= 2; s.n *= 2; return s; }
IntFloat int_float(IntFloat s) { s.x *= 2; s.n *= 2; return s; }
Mixed mixed(Mixed s) { s.x *= 2; s.n *= 2; s.d *= 2; return s; }
Nested nested(Nested s) { Nested r = {{s.inner.b, s.inner.a}, s.n * 2}; return r; }
NestedFloats nested_floats(NestedFloats s) { NestedFloats r = {s.b, s.a}; return r; }
Small small(Small s) { s.flag = !s.flag; s.c += 1; s.n += 1; return s; }
Text text_of(Text s) { s.text += 1; s.size -= 1; return s; }

// With few registers left, a struct goes on the stack, whole, and later values still use
// the registers that remain.
long ints_run_out(long a, long b, long c, long d, long e, TwoLongs s, long f) {
    return a + 10 * b + 100 * c + 1000 * d + 10000 * e + 100000 * s.a + 1000000 * s.b + 10000000 * f;
}
double floats_run_out(double a, double b, double c, double d, double e, double f, double g, TwoDoubles s, double h) {
    return a + 10 * b + 100 * c + 1000 * d + 10000 * e + 100000 * f + 1000000 * g + 10000000 * s.x + 100000000 * s.y +
           1000000000 * h;
}
long mixed_run_out(long a, long b, long c, long d, long e, DoubleLong s, double x) {
    return a + b + c + d + e + s.n + (long)s.x + (long)x;
}
double structs_after_registers(double a, double b, double c, double d, double e, double f, double g, double h,
                               TwoFloats s, ThreeFloats t, float x, OneFloat u) {
    return a + b + c + d + e + f + g + h + 10 * s.x + 100 * s.y + 1000 * t.x + 10000 * t.y + 100000 * t.z + 1000000 * x +
           10000000 * u.x;
}
// The hidden pointer to a large result takes an integer register.
ThreeLongs result_takes_register(long a, long b, long c, long d, long e, TwoLongs s) {
    ThreeLongs r = {a + b + c + d + e, s.a, s.b};
    return r;
}
// Several kinds of value in one call.
double all_kinds(ThreeLongs big, int n, TwoDoubles d, float f, Small small, const char* text, FourDoubles four) {
    return big.a + big.b + big.c + n + d.x + d.y + f + small.n + (text ? text[0] : 0) + four.x + four.y + four.z + four.w;
}
// The callee owns its copy, so changing it does not change the caller's struct.
long changes_its_copy(ThreeLongs s) {
    s.a = 100;
    return s.a + s.b + s.c;
}

// Through a pointer.
void scale(TwoDoubles* p, double factor) {
    p->x *= factor;
    p->y *= factor;
}
const char* greeting = "hello";
Text* shared_text(void) {
    static Text t;
    t.text = greeting;
    t.size = 5;
    return &t;
}
struct Node { long value; const struct Node* next; };
long sum_nodes(const struct Node* head) {
    long total = 0;
    for (; head; head = head->next) total += head->value;
    return total;
}
