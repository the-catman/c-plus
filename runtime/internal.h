#ifndef CPLUS_INTERNAL_H
#define CPLUS_INTERNAL_H

// What the files of the runtime share with each other. Programs do not see any of this.

#include "reflect.h"

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

// Where the descriptor of a class is, in the vtable of each of its parts.
#define CPLUS_VTABLE_TYPE 4

#endif
