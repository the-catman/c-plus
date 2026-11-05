// The C+ runtime. Intentionally small: everything here is called by generated code.
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void cplus_print_str(const char* data, int64_t length) {
    if (length > 0) fwrite(data, 1, (size_t)length, stdout);
}

// Prints a string in double quotes with special characters escaped, as it would be
// written in source. Used for strings inside printed structs.
void cplus_print_quoted(const char* data, int64_t length) {
    fputc('"', stdout);
    for (int64_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)data[i];
        switch (c) {
            case '"': fputs("\\\"", stdout); break;
            case '\\': fputs("\\\\", stdout); break;
            case '\n': fputs("\\n", stdout); break;
            case '\t': fputs("\\t", stdout); break;
            case '\r': fputs("\\r", stdout); break;
            default:
                if (c < 0x20 || c == 0x7f) printf("\\x%02x", c);
                else fputc(c, stdout);
        }
    }
    fputc('"', stdout);
}
void cplus_print_i32(int32_t value) { printf("%" PRId32, value); }
void cplus_print_i64(int64_t value) { printf("%" PRId64, value); }
void cplus_print_bool(int32_t value) { fputs(value ? "true" : "false", stdout); }
void cplus_print_newline(void) { fputc('\n', stdout); }

// Returns 1 when two strings have the same bytes. An empty string may have no data.
int32_t cplus_str_eq(const char* a, int64_t a_length, const char* b, int64_t b_length) {
    if (a_length != b_length) return 0;
    return a_length == 0 || memcmp(a, b, (size_t)a_length) == 0;
}

// Called when a runtime check fails (integer overflow, division by zero, ...).
// Output printed so far is flushed first so the error appears after it.
_Noreturn void cplus_trap(const char* message, const char* file, int32_t line, int32_t column) {
    fflush(stdout);
    fprintf(stderr, "fatal error: %s\n  --> %s:%" PRId32 ":%" PRId32 "\n", message, file, line, column);
    abort();
}

// Called when an index is outside a vector.
_Noreturn void cplus_trap_index(int64_t index, int64_t size, const char* file, int32_t line, int32_t column) {
    char message[128];
    snprintf(message, sizeof message, "index %" PRId64 " is out of range for a vector of size %" PRId64, index, size);
    cplus_trap(message, file, line, column);
}

// ---- Heap objects -----------------------------------------------------------
//
// Class objects and vector buffers start with a 64-bit reference count. Generated code
// retains an object when it stores a new reference to it and releases it when a
// reference goes away; the object is destroyed when the count reaches zero. Counts are
// atomic, so objects can later be shared between threads.

// Set CPLUS_CHECK_LEAKS=1 to report objects that are never freed when the program
// exits. Freed objects are then kept and marked, so a later use of one is caught
// instead of reading reused memory. The test suite runs every program this way.
static int64_t live_objects;
static int check_leaks = -1;
static const int64_t freed_mark = INT64_MIN;

static int leak_checking(void) {
    if (check_leaks < 0) {
        const char* value = getenv("CPLUS_CHECK_LEAKS");
        check_leaks = value && *value && strcmp(value, "0") != 0;
    }
    return check_leaks;
}

_Noreturn static void bad_count(void) {
    fflush(stdout);
    fputs("internal error: an object was used after it was freed; this is a bug in the C+ compiler\n", stderr);
    abort();
}

void* cplus_alloc(int64_t size) {
    int64_t* object = calloc(1, (size_t)size);
    if (!object) {
        fflush(stdout);
        fputs("fatal error: out of memory\n", stderr);
        abort();
    }
    *object = 1;
    __atomic_add_fetch(&live_objects, 1, __ATOMIC_RELAXED);
    return object;
}

void cplus_free(void* object) {
    __atomic_sub_fetch(&live_objects, 1, __ATOMIC_RELAXED);
    if (leak_checking()) *(int64_t*)object = freed_mark;
    else free(object);
}

void cplus_retain(void* object) {
    if (!object) return;
    if (__atomic_fetch_add((int64_t*)object, 1, __ATOMIC_RELAXED) <= 0) bad_count();
}

// Returns 1 when this was the last reference, so the caller destroys the object.
int32_t cplus_release(void* object) {
    if (!object) return 0;
    int64_t before = __atomic_fetch_sub((int64_t*)object, 1, __ATOMIC_RELEASE);
    if (before == 1) {
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        return 1;
    }
    if (before <= 0) bad_count();
    return 0;
}

_Noreturn static void out_of_memory(void) {
    fflush(stdout);
    fputs("fatal error: out of memory\n", stderr);
    abort();
}

// ---- Vectors ---------------------------------------------------------------
//
// A vector is a pointer to a buffer, or null when it is empty and has never held
// anything. The buffer is shared by every copy of the vector until one of them
// changes: that copy then gets a buffer of its own (copy-on-write). Elements follow
// the header. Generated code passes the size of one element and, for elements that
// hold references, functions that retain or release the element at an address.

typedef struct {
    int64_t count;  // the reference count, as in every heap object
    int64_t size;
    int64_t capacity;
} cplus_vec;

typedef void (*cplus_elem_fn)(void* element);

static char* vec_elements(cplus_vec* v) { return (char*)(v + 1); }

static size_t vec_bytes(int64_t elem_size, int64_t capacity) {
    if (capacity < 0 || (elem_size > 0 && capacity > (INT64_MAX - (int64_t)sizeof(cplus_vec)) / elem_size))
        out_of_memory();
    return sizeof(cplus_vec) + (size_t)(elem_size * capacity);
}

// A new buffer with `size` elements, all zero bytes, or null for none.
void* cplus_vec_new(int64_t elem_size, int64_t size) {
    if (size == 0) return NULL;
    cplus_vec* v = cplus_alloc((int64_t)vec_bytes(elem_size, size));
    v->size = size;
    v->capacity = size;
    return v;
}

void cplus_vec_release(void* vec, int64_t elem_size, cplus_elem_fn release_elem) {
    cplus_vec* v = vec;
    if (!cplus_release(v)) return;
    if (release_elem)
        for (int64_t i = 0; i < v->size; ++i) release_elem(vec_elements(v) + i * elem_size);
    cplus_free(v);
}

// Returns a buffer that only the caller holds, with room for at least `min_capacity`
// elements and the same elements as `vec`. The caller's reference to `vec` moves to
// the result: a vector that is not shared is grown in place, and one that is shared
// is copied.
void* cplus_vec_reserve(void* vec, int64_t elem_size, int64_t min_capacity, cplus_elem_fn retain_elem,
                        cplus_elem_fn release_elem) {
    cplus_vec* v = vec;
    int64_t size = v ? v->size : 0;
    int64_t capacity = v ? v->capacity : 0;
    int unique = v && __atomic_load_n(&v->count, __ATOMIC_ACQUIRE) == 1;
    if (unique && capacity >= min_capacity) return v;
    if (!v && min_capacity == 0) return NULL;

    int64_t new_capacity = capacity;
    if (new_capacity < min_capacity) {
        new_capacity = capacity < 2 ? 4 : capacity > INT64_MAX / 2 ? INT64_MAX : capacity * 2;
        if (new_capacity < min_capacity) new_capacity = min_capacity;
    }
    if (unique && !leak_checking()) {  // the elements move, so their counts stay as they are
        cplus_vec* grown = realloc(v, vec_bytes(elem_size, new_capacity));
        if (!grown) out_of_memory();
        grown->capacity = new_capacity;
        return grown;
    }
    cplus_vec* copy = cplus_alloc((int64_t)vec_bytes(elem_size, new_capacity));
    copy->size = size;
    copy->capacity = new_capacity;
    if (size > 0) memcpy(vec_elements(copy), vec_elements(v), (size_t)(size * elem_size));
    if (unique) {
        // Leak checking keeps freed memory marked, so the old buffer is freed rather
        // than reallocated; the elements moved to the copy.
        cplus_free(v);
        return copy;
    }
    if (retain_elem)
        for (int64_t i = 0; i < size; ++i) retain_elem(vec_elements(copy) + i * elem_size);
    if (v) cplus_vec_release(v, elem_size, release_elem);
    return copy;
}

__attribute__((destructor)) static void report_leaks(void) {
    if (!leak_checking()) return;
    int64_t count = __atomic_load_n(&live_objects, __ATOMIC_RELAXED);
    if (count != 0) {
        fflush(stdout);
        fprintf(stderr, "cplus: %" PRId64 " object%s never freed\n", count, count == 1 ? " was" : "s were");
    }
}

// Printing an object that refers back to itself would never end. Each thread keeps
// the objects it is printing; one met again prints as `Name {...}`.
enum { max_print_depth = 64 };
static _Thread_local const void* printing[max_print_depth];
static _Thread_local int print_depth;

int32_t cplus_print_enter(const void* object) {
    for (int i = 0; i < print_depth; ++i)
        if (printing[i] == object) return 0;
    if (print_depth == max_print_depth) return 0;
    printing[print_depth++] = object;
    return 1;
}

void cplus_print_leave(void) { --print_depth; }
