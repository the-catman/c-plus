// A C header for the tests of `import "...";`, with each kind of thing a header has, and what C+ makes of it.
#ifndef CPLUS_TESTS_HEADER_H
#define CPLUS_TESTS_HEADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Constants, as macros and enums.
#define HEADER_ANSWER 42
#define HEADER_NEGATIVE (-HEADER_ANSWER * 2)
#define HEADER_BIG 5000000000L
#define HEADER_MASK 0xFFFFFFFFu
#define HEADER_ALL_BITS 0xFFFFFFFFFFFFFFFFull
#define HEADER_LETTER 'A'
#define HEADER_HALF 0.5
#define HEADER_THIRD (1.0f / 3)
#define HEADER_NAME "header \"one\"\n\ttab"
#define HEADER_BYTES "\x01\xff"
#define HEADER_TRUE ((bool)1)
#define HEADER_NONE ((void*)0)
#define HEADER_SENTINEL ((void (*)(void*))-1)
#define HEADER_INFINITE (1.0 / 0.0)
#define HEADER_EMPTY
#define HEADER_TWICE(x) ((x) * 2)
#define HEADER_COUNTER header_counter
#define HEADER_CALL header_add(1, 2)
#define HEADER_ALIAS header_add
#define HEADER_BLOCK { 1, 2 }

enum header_color { HEADER_RED, HEADER_GREEN = 5, HEADER_BLUE };
typedef enum { HEADER_SMALL = -1, HEADER_LARGE = 1 } header_size;

// Structs, which C+ holds when it can, and knows by name when it cannot.
typedef struct header_point {
    int x, y;
} header_point_t;
struct header_named {
    char name[16];
    const char* label;
    double weight;
    header_point_t where;
    bool flag;
};
struct header_list {
    long value;
    struct header_list* next;
};
struct header_words {
    int class;
    int new;
    int string;
};
typedef struct header_opaque header_opaque;
union header_either {
    int i;
    float f;
};
struct header_bits {
    unsigned flag : 1;
    unsigned rest : 31;
};
struct header_short {
    short s;
};
struct header_packed {
    char c;
    int i;
} __attribute__((packed));
struct header_has_union {
    int kind;
    union {
        int i;
        float f;
    };
};

struct header_flexible {
    int count;
    int items[];
};
struct header_unnamed_type {
    struct {
        int a;
    } inner;
};
struct header_aligned {
    int x;
} __attribute__((aligned(16)));
struct header_empty {};

typedef size_t header_size_t;
typedef void (*header_callback)(void* data);
typedef unsigned short header_port;

// Functions.
int header_add(int a, int b);
long header_length(const char* text);
const char* header_greeting(void);
char* header_copy(const char* text);
void header_free(char* text);
int header_count();
header_point_t header_move(header_point_t p, int dx, int dy);
void header_fill(struct header_named* n, const char* name);
long header_sum(const struct header_list* list);
header_opaque* header_open(int n);
int header_value(const header_opaque* h);
void header_close(header_opaque* h);
int header_call_back(header_callback f, void* data);
int header_words_sum(struct header_words w);
int header_union_int(union header_either* e);
int header_keyword_params(int string, int new, int __private);
int header_sum_values(const int* values, size_t count);
int header_strings(const char* const* texts, int count);
const unsigned char* header_bytes(void);
int header_first_byte(const unsigned char* bytes);
header_port header_port_of(int n);
unsigned int header_unsigned_add(unsigned int a, unsigned int b);
unsigned long long header_all_bits(void);
size_t header_size_of(const char* text);
uint8_t header_low_byte(uint8_t b);

// What C+ cannot call.
int header_print(const char* format, ...);
static inline int header_inline(int x) { return x + 1; }
int header_take_short(short s);
int header_take_bits(struct header_bits b);
int header_take_union(union header_either e);
long double header_precise(void);
int header_take_packed(struct header_packed p);
int header_take_has_union(struct header_has_union u);
int header_take_flexible(struct header_flexible f);
int header_take_unnamed_type(struct header_unnamed_type u);
int header_take_aligned(struct header_aligned a);
int header_take_empty(struct header_empty e);
__int128 header_wide(void);
int header_count_items(const struct header_flexible* f);

extern int header_counter;

#endif
