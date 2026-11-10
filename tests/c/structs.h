// The structs and functions of tests/c/structs.c, for the tests that import them: `import "../c/structs.h";` must give C+
// the same layouts and calls as the C compiler has.
#ifndef CPLUS_TESTS_STRUCTS_H
#define CPLUS_TESTS_STRUCTS_H

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
struct Node { long value; const struct Node* next; };
// With arrays as fields.
struct Named { char name[8]; int id; };
struct Vec3f { float v[3]; };
struct Mat2 { double m[4]; };
struct FloatsDouble { float f[2]; double d; };
struct IntsDouble { int i[2]; double d; };
struct Block { int data[100]; long tail; };
struct Cell { int xy[2]; };
struct Cells { struct Cell cells[2]; };
struct Text16 { char text[16]; };
struct Text17 { char text[17]; };
struct FloatAndTwo { float a; float rest[2]; };
struct FiveFloats { float v[5]; };

OneChar one_char(OneChar s);
OneInt one_int(OneInt s);
TwoInts two_ints(TwoInts s);
ThreeInts three_ints(ThreeInts s);
TwoLongs two_longs(TwoLongs s);
ThreeLongs three_longs(ThreeLongs s);
OneFloat one_float(OneFloat s);
TwoFloats two_floats(TwoFloats s);
ThreeFloats three_floats(ThreeFloats s);
OneDouble one_double(OneDouble s);
TwoDoubles two_doubles(TwoDoubles s);
ThreeDoubles three_doubles(ThreeDoubles s);
FourDoubles four_doubles(FourDoubles s);
FiveDoubles five_doubles(FiveDoubles s);
DoubleLong double_long(DoubleLong s);
LongDouble long_double(LongDouble s);
IntFloat int_float(IntFloat s);
Mixed mixed(Mixed s);
Nested nested(Nested s);
NestedFloats nested_floats(NestedFloats s);
Small small(Small s);
Text text_of(Text s);
long ints_run_out(long a, long b, long c, long d, long e, TwoLongs s, long f);
double floats_run_out(double a, double b, double c, double d, double e, double f, double g, TwoDoubles s, double h);
long mixed_run_out(long a, long b, long c, long d, long e, DoubleLong s, double x);
double structs_after_registers(double a, double b, double c, double d, double e, double f, double g, double h,
                               TwoFloats s, ThreeFloats t, float x, OneFloat u);
ThreeLongs result_takes_register(long a, long b, long c, long d, long e, TwoLongs s);
double all_kinds(ThreeLongs big, int n, TwoDoubles d, float f, Small small, const char* text, FourDoubles four);
long changes_its_copy(ThreeLongs s);
void scale(TwoDoubles* p, double factor);
Text* shared_text(void);
long sum_nodes(const struct Node* head);
long named_sum(struct Named n);
struct Named named_make(const char* text, int id);
float vec3f_sum(struct Vec3f v);
struct Vec3f vec3f_scale(struct Vec3f v, float k);
double mat2_det(struct Mat2 m);
struct Mat2 mat2_identity(void);
double floats_double_sum(struct FloatsDouble x);
struct FloatsDouble floats_double_make(float a, float b, double d);
double ints_double_sum(struct IntsDouble x);
struct IntsDouble ints_double_make(int a, int b, double d);
long block_sum(struct Block b);
struct Block block_make(int start);
void block_set(struct Block* b, int i, int value);
int cells_sum(struct Cells c);
struct Cells cells_make(int a, int b, int c, int d);
long text16_len(struct Text16 t);
struct Text16 text16_make(const char* s);
long text17_len(struct Text17 t);
struct Text17 text17_make(const char* s);
float float_and_two_sum(struct FloatAndTwo t);
struct FloatAndTwo float_and_two_make(float a, float b, float c);
float five_floats_sum(struct FiveFloats f);
struct FiveFloats five_floats_make(float k);
long many_named(struct Named a, struct Named b, struct Named c, struct Named d);
float many_vec3f(struct Vec3f a, struct Vec3f b, struct Vec3f c, struct Vec3f d, struct Vec3f e);

#endif
