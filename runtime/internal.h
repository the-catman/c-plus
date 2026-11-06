#ifndef CPLUS_INTERNAL_H
#define CPLUS_INTERNAL_H

// What the files of the runtime share with each other. Programs do not see any of this.

#include "reflect.h"

// A build under AddressSanitizer has that to report crashes. Every other build reports them itself: see on_crash in runtime.c.
#if !defined(CPLUS_DEBUG_RUNTIME) || defined(CPLUS_RACE_RUNTIME)
#define CPLUS_OWN_CRASH_HANDLER
#endif

typedef struct {
    const char* data;
    int64_t length;
    void* owner;
} cplus_string;

typedef struct {
    int64_t count;  // the reference count, as in every heap object
    int64_t size;
    int64_t capacity;
} cplus_vec;

typedef void (*cplus_elem_fn)(void* element);

void cplus_str_append(cplus_string* s, const char* data, int64_t length);
void cplus_str_release(void* owner);
void* cplus_vec_reserve(void* vec, int64_t elem_size, int64_t min_capacity, cplus_elem_fn retain_elem, cplus_elem_fn release_elem);
void cplus_vec_release(void* vec, int64_t elem_size, cplus_elem_fn release_elem);
void* cplus_object_start(void* object);
void cplus_print_f64(cplus_string* out, double value);
void cplus_print_f32(cplus_string* out, float value);
_Noreturn void cplus_stop(const char* message, const char* help);

// How many bytes the valid UTF-8 character at `s` takes, or 0 if it is not one.
static inline int cplus_utf8_length(const unsigned char* s, int64_t left) {
    unsigned char c = s[0];
    if (c < 0x80) return 1;
    if (c < 0xC2) return 0;
    if (c < 0xE0) return left >= 2 && (s[1] & 0xC0) == 0x80 ? 2 : 0;
    if (c < 0xF0) {
        if (left < 3 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return 0;
        if ((c == 0xE0 && s[1] < 0xA0) || (c == 0xED && s[1] >= 0xA0)) return 0;  // too long a form, or half a surrogate pair
        return 3;
    }
    if (c < 0xF5) {
        if (left < 4 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) return 0;
        if ((c == 0xF0 && s[1] < 0x90) || (c == 0xF4 && s[1] >= 0x90)) return 0;  // too long a form, or past the last character
        return 4;
    }
    return 0;
}

// Where the descriptor of a class is, in the vtable of each of its parts.
#define CPLUS_VTABLE_TYPE 4

#endif
