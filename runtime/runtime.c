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

// Called when a runtime check fails (integer overflow, division by zero, ...).
// Output printed so far is flushed first so the error appears after it.
_Noreturn void cplus_trap(const char* message, const char* file, int32_t line, int32_t column) {
    fflush(stdout);
    fprintf(stderr, "fatal error: %s\n  --> %s:%" PRId32 ":%" PRId32 "\n", message, file, line, column);
    abort();
}

// ---- Class objects ---------------------------------------------------------
//
// Every class object starts with a 64-bit reference count, followed by its fields.
// Generated code retains an object when it stores a new reference to it and releases
// it when a reference goes away; the object is destroyed when the count reaches zero.
// Counts are atomic, so objects can later be shared between threads.

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
    fputs("internal error: a class object was used after it was freed; this is a bug in the C+ compiler\n", stderr);
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

__attribute__((destructor)) static void report_leaks(void) {
    if (!leak_checking()) return;
    int64_t count = __atomic_load_n(&live_objects, __ATOMIC_RELAXED);
    if (count != 0) {
        fflush(stdout);
        fprintf(stderr, "cplus: %" PRId64 " class object%s never freed\n", count, count == 1 ? " was" : "s were");
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
