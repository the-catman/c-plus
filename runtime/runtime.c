// The C+ runtime. Intentionally small: everything here is called by generated code.
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

// ---- Strings ---------------------------------------------------------------
//
// A string is { data, length, owner }, laid out as generated code lays it out. `owner`
// is null for a literal or an empty string; otherwise it is a heap buffer holding the
// bytes, and every string that points into the buffer holds a reference to it. A
// string never changes the bytes it can see, so copies and substrings share one
// buffer. Appending writes after the end of a string's bytes, and only when no other
// string holds the buffer.

typedef struct {
    const char* data;
    int64_t length;
    void* owner;
} cplus_string;

typedef struct {
    int64_t count;     // the reference count, as in every heap object
    int64_t capacity;  // the number of bytes that follow
} cplus_str_buf;

static char* buf_bytes(cplus_str_buf* b) { return (char*)(b + 1); }

static int64_t add_lengths(int64_t a, int64_t b) {
    if (a > INT64_MAX - b) out_of_memory();
    return a + b;
}

static cplus_str_buf* str_buf_new(int64_t capacity) {
    if (capacity > INT64_MAX - (int64_t)sizeof(cplus_str_buf)) out_of_memory();
    cplus_str_buf* b = cplus_alloc((int64_t)sizeof(cplus_str_buf) + capacity);
    b->capacity = capacity;
    return b;
}

void cplus_str_release(void* owner) {
    if (cplus_release(owner)) cplus_free(owner);
}

// Returns 1 when two strings have the same bytes. An empty string may have no data.
int32_t cplus_str_eq(const char* a, int64_t a_length, const char* b, int64_t b_length) {
    if (a_length != b_length) return 0;
    return a_length == 0 || memcmp(a, b, (size_t)a_length) == 0;
}

// Orders two strings by their bytes, as C++ does: -1, 0 or 1. For UTF-8 text this is
// the order of the code points.
int32_t cplus_str_compare(const char* a, int64_t a_length, const char* b, int64_t b_length) {
    int64_t n = a_length < b_length ? a_length : b_length;
    int c = n > 0 ? memcmp(a, b, (size_t)n) : 0;
    if (c != 0) return c < 0 ? -1 : 1;
    return a_length < b_length ? -1 : a_length > b_length ? 1 : 0;
}

// Joins `count` strings into `out`, a new string the caller owns.
void cplus_str_concat(cplus_string* out, const cplus_string* parts, int64_t count) {
    int64_t total = 0, nonempty = 0;
    const cplus_string* only = NULL;
    for (int64_t i = 0; i < count; ++i) {
        if (parts[i].length == 0) continue;
        total = add_lengths(total, parts[i].length);
        ++nonempty;
        only = &parts[i];
    }
    if (nonempty <= 1) {  // nothing to join: share the one string there is
        *out = only ? *only : (cplus_string){NULL, 0, NULL};
        cplus_retain(out->owner);
        return;
    }
    cplus_str_buf* b = str_buf_new(total);
    char* p = buf_bytes(b);
    for (int64_t i = 0; i < count; ++i) {
        if (parts[i].length == 0) continue;
        memcpy(p, parts[i].data, (size_t)parts[i].length);
        p += parts[i].length;
    }
    *out = (cplus_string){buf_bytes(b), total, b};
}

// Appends bytes to the string stored at `s`, as `s += x` does. When nothing else holds
// its buffer and the buffer has room, the bytes go right after the string's own;
// otherwise the string moves to a new buffer twice the size it needs, so appending
// in a loop takes linear time overall.
void cplus_str_append(cplus_string* s, const char* data, int64_t length) {
    if (length <= 0) return;
    cplus_str_buf* old = s->owner;
    if (old && __atomic_load_n(&old->count, __ATOMIC_ACQUIRE) == 1) {
        int64_t end = (int64_t)(s->data - buf_bytes(old)) + s->length;
        if (old->capacity - end >= length) {
            memmove((char*)s->data + s->length, data, (size_t)length);
            s->length += length;
            return;
        }
    }
    int64_t needed = add_lengths(s->length, length);
    int64_t capacity = needed > INT64_MAX / 2 ? needed : needed * 2;
    if (capacity < 16) capacity = 16;
    cplus_str_buf* b = str_buf_new(capacity);
    if (s->length > 0) memcpy(buf_bytes(b), s->data, (size_t)s->length);
    memcpy(buf_bytes(b) + s->length, data, (size_t)length);
    *s = (cplus_string){buf_bytes(b), needed, b};
    cplus_str_release(old);  // after copying, since `data` may lie in the old buffer
}

// `s.substr(pos, count)`, usually sharing the bytes of `s`. As in C++, a count that runs past
// the end takes the rest of the string, and so does string::npos (-1). A position
// past the end, or another negative count, stops the program.
void cplus_str_substr(cplus_string* out, const cplus_string* s, int64_t pos, int64_t count, const char* file,
                      int32_t line, int32_t column) {
    char message[160];
    if (pos < 0 || pos > s->length) {
        snprintf(message, sizeof message, "substr() position %" PRId64 " is out of range for a string of size %" PRId64,
                 pos, s->length);
        cplus_trap(message, file, line, column);
    }
    int64_t rest = s->length - pos;
    if (count == -1 || count > rest) count = rest;
    if (count < 0) {
        snprintf(message, sizeof message, "substr() was given the negative length %" PRId64, count);
        cplus_trap(message, file, line, column);
    }
    if (count == 0) {
        *out = (cplus_string){NULL, 0, NULL};
        return;
    }
    // A small part of a large buffer is copied, so a substring never keeps alive much
    // more memory than it uses: at most four times its size.
    cplus_str_buf* owner = s->owner;
    if (owner && count < owner->capacity / 4) {
        cplus_str_buf* b = str_buf_new(count);
        memcpy(buf_bytes(b), s->data + pos, (size_t)count);
        *out = (cplus_string){buf_bytes(b), count, b};
        return;
    }
    *out = (cplus_string){s->data + pos, count, owner};
    cplus_retain(owner);
}

// The position of the first `needle` in `s` at or after `from`, or -1 (string::npos).
int64_t cplus_str_find(const char* s, int64_t length, const char* needle, int64_t needle_length, int64_t from) {
    if (from > length) return -1;
    if (needle_length == 0) return from;
    if (needle_length > length - from) return -1;
    const char* last = s + (length - needle_length);
    for (const char* p = s + from; p <= last; ++p) {
        p = memchr(p, (unsigned char)needle[0], (size_t)(last - p) + 1);
        if (!p) return -1;
        if (memcmp(p, needle, (size_t)needle_length) == 0) return p - s;
    }
    return -1;
}

int32_t cplus_str_starts_with(const char* s, int64_t length, const char* prefix, int64_t prefix_length) {
    return prefix_length <= length && (prefix_length == 0 || memcmp(s, prefix, (size_t)prefix_length) == 0);
}

int32_t cplus_str_ends_with(const char* s, int64_t length, const char* suffix, int64_t suffix_length) {
    return suffix_length <= length &&
           (suffix_length == 0 || memcmp(s + (length - suffix_length), suffix, (size_t)suffix_length) == 0);
}

// ---- C functions -------------------------------------------------------------

// The text of a string as C expects it: a copy that ends in a zero byte, which the
// caller frees with cplus_c_free once the C function returns. NULL when `present` is
// 0, for a null `string?`. A zero byte inside the text would cut it short in C, so it
// stops the program instead.
char* cplus_c_string(const char* data, int64_t length, int32_t present, const char* function, const char* file,
                     int32_t line, int32_t column) {
    if (!present) return NULL;
    const char* zero = length > 0 ? memchr(data, 0, (size_t)length) : NULL;
    if (zero) {
        char message[256];
        snprintf(message, sizeof message,
                 "the string passed to the C function '%s' has a zero byte at position %" PRId64
                 ", where C would think it ends",
                 function, (int64_t)(zero - data));
        cplus_trap(message, file, line, column);
    }
    char* copy = malloc((size_t)length + 1);
    if (!copy) out_of_memory();
    if (length > 0) memcpy(copy, data, (size_t)length);
    copy[length] = 0;
    return copy;
}

void cplus_c_free(char* text) { free(text); }

// A string holding a copy of the text a C function returned. NULL gives the empty
// string; for a `string?` result, the generated code makes that null.
void cplus_str_from_c(cplus_string* out, const char* text) {
    size_t length = text ? strlen(text) : 0;
    if (length == 0) {
        *out = (cplus_string){NULL, 0, NULL};
        return;
    }
    cplus_str_buf* b = str_buf_new((int64_t)length);
    memcpy(buf_bytes(b), text, length);
    *out = (cplus_string){buf_bytes(b), (int64_t)length, b};
}

// ---- Output ----------------------------------------------------------------
//
// Printing goes to standard output when `out` is null, and otherwise to the end of
// the string at `out`, which is how format() and to_string() build their result.

static void out_write(cplus_string* out, const char* data, int64_t length) {
    if (out) cplus_str_append(out, data, length);
    else if (length > 0) fwrite(data, 1, (size_t)length, stdout);
}

void cplus_print_str(cplus_string* out, const char* data, int64_t length) { out_write(out, data, length); }

// Prints a string in double quotes with special characters escaped, as it would be
// written in source. Used for strings inside printed structs and vectors.
void cplus_print_quoted(cplus_string* out, const char* data, int64_t length) {
    out_write(out, "\"", 1);
    int64_t plain = 0;  // the start of the characters not written yet
    for (int64_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)data[i];
        char escape[8];
        const char* e = NULL;
        switch (c) {
            case '"': e = "\\\""; break;
            case '\\': e = "\\\\"; break;
            case '\n': e = "\\n"; break;
            case '\t': e = "\\t"; break;
            case '\r': e = "\\r"; break;
            default:
                if (c < 0x20 || c == 0x7f) {
                    snprintf(escape, sizeof escape, "\\x%02x", c);
                    e = escape;
                }
        }
        if (!e) continue;
        out_write(out, data + plain, i - plain);
        out_write(out, e, (int64_t)strlen(e));
        plain = i + 1;
    }
    if (plain < length) out_write(out, data + plain, length - plain);  // an empty string may have no data
    out_write(out, "\"", 1);
}

void cplus_print_i32(cplus_string* out, int32_t value) {
    char text[16];
    out_write(out, text, snprintf(text, sizeof text, "%" PRId32, value));
}

void cplus_print_i64(cplus_string* out, int64_t value) {
    char text[24];
    out_write(out, text, snprintf(text, sizeof text, "%" PRId64, value));
}

void cplus_print_bool(cplus_string* out, int32_t value) {
    if (value) out_write(out, "true", 4);
    else out_write(out, "false", 5);
}

void cplus_print_newline(void) { fputc('\n', stdout); }

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
