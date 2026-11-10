// C functions for the tests of structs passed by value. A C compiler lays out each call
// here, so calling them from C+ checks that C+ follows the platform's rules.
#include <string.h>

#include "structs.h"

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
long sum_nodes(const struct Node* head) {
    long total = 0;
    for (; head; head = head->next) total += head->value;
    return total;
}

// Arrays inside structs.
long named_sum(struct Named n) {
    long sum = n.id;
    for (int i = 0; i < 8; i++) sum += n.name[i];
    return sum;
}
struct Named named_make(const char* text, int id) {
    struct Named n;
    memset(&n, 0, sizeof n);
    strncpy(n.name, text, sizeof n.name);
    n.id = id;
    return n;
}
float vec3f_sum(struct Vec3f v) { return v.v[0] + v.v[1] + v.v[2]; }
struct Vec3f vec3f_scale(struct Vec3f v, float k) {
    for (int i = 0; i < 3; i++) v.v[i] *= k;
    return v;
}
double mat2_det(struct Mat2 m) { return m.m[0] * m.m[3] - m.m[1] * m.m[2]; }
struct Mat2 mat2_identity(void) {
    struct Mat2 m = {{1, 0, 0, 1}};
    return m;
}
double floats_double_sum(struct FloatsDouble x) { return x.f[0] + x.f[1] + x.d; }
struct FloatsDouble floats_double_make(float a, float b, double d) {
    struct FloatsDouble x = {{a, b}, d};
    return x;
}
double ints_double_sum(struct IntsDouble x) { return x.i[0] + x.i[1] + x.d; }
struct IntsDouble ints_double_make(int a, int b, double d) {
    struct IntsDouble x = {{a, b}, d};
    return x;
}
long block_sum(struct Block b) {
    long sum = b.tail;
    for (int i = 0; i < 100; i++) sum += b.data[i];
    return sum;
}
struct Block block_make(int start) {
    struct Block b;
    for (int i = 0; i < 100; i++) b.data[i] = start + i;
    b.tail = 1000;
    return b;
}
void block_set(struct Block* b, int i, int value) { b->data[i] = value; }
int cells_sum(struct Cells c) { return c.cells[0].xy[0] + c.cells[0].xy[1] + c.cells[1].xy[0] + c.cells[1].xy[1]; }
struct Cells cells_make(int a, int b, int c, int d) {
    struct Cells x = {{{{a, b}}, {{c, d}}}};
    return x;
}
static long text_length(const char* text, long size) {
    long n = 0;
    while (n < size && text[n]) n++;
    return n;
}
long text16_len(struct Text16 t) { return text_length(t.text, sizeof t.text); }
struct Text16 text16_make(const char* s) {
    struct Text16 t;
    memset(&t, 0, sizeof t);
    strncpy(t.text, s, sizeof t.text);
    return t;
}
long text17_len(struct Text17 t) { return text_length(t.text, sizeof t.text); }
struct Text17 text17_make(const char* s) {
    struct Text17 t;
    memset(&t, 0, sizeof t);
    strncpy(t.text, s, sizeof t.text);
    return t;
}
float float_and_two_sum(struct FloatAndTwo t) { return t.a + t.rest[0] + t.rest[1]; }
struct FloatAndTwo float_and_two_make(float a, float b, float c) {
    struct FloatAndTwo t = {a, {b, c}};
    return t;
}
float five_floats_sum(struct FiveFloats f) { return f.v[0] + f.v[1] + f.v[2] + f.v[3] + f.v[4]; }
struct FiveFloats five_floats_make(float k) {
    struct FiveFloats f = {{k, k, k, k, k}};
    return f;
}
// More structs than registers: the last ones are read from the stack.
long many_named(struct Named a, struct Named b, struct Named c, struct Named d) {
    return a.id + 10L * b.id + 100L * c.id + 1000L * d.id;
}
float many_vec3f(struct Vec3f a, struct Vec3f b, struct Vec3f c, struct Vec3f d, struct Vec3f e) {
    return a.v[0] + b.v[1] + c.v[2] + d.v[0] + e.v[1];
}
