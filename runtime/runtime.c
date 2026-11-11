// The C+ runtime. Intentionally small: everything here is called by generated code.
#define _GNU_SOURCE  // for the registers in a signal's context
#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

#include "internal.h"

// This file is built twice. Programs built with --debug link the copy built with
// CPLUS_DEBUG_RUNTIME, under AddressSanitizer, which frees memory for real so that
// AddressSanitizer can catch a later use of it.
#ifdef CPLUS_DEBUG_RUNTIME
void __sanitizer_print_stack_trace(void);
#endif

// One thread reports a fatal error. If another stops too, it waits for the first to end the program, so there is one message.
static void claim_the_report(void) {
    static _Thread_local char this_thread;
    static char* reporting = NULL;
    char* first = NULL;
    if (!__atomic_compare_exchange_n(&reporting, &first, &this_thread, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) && first != &this_thread)
        for (;;) pause();
}

// What was printed comes before the message. Another thread may have the output and be stuck, so this does not wait for it
// for long. It keeps the output locked if it got it, as nothing else should be printed after a fatal error.
static void flush_the_output(void) {
    int tries = 0;
    while (ftrylockfile(stdout) != 0 && ++tries < 100) usleep(10000);
    if (tries < 100) fflush(stdout);
}

void cplus_fatal_begin(void) {
    claim_the_report();
    flush_the_output();
}

// A third copy is built with CPLUS_RACE_RUNTIME too, under ThreadSanitizer, for programs built with --race.
#ifdef CPLUS_RACE_RUNTIME
// ThreadSanitizer cannot see what a fence does, so it takes this to be a race on what was just freed. The stronger form costs
// nothing that matters in a build like this.
#define LAST_RELEASE __ATOMIC_ACQ_REL

int __tsan_get_report_data(void* report, const char** description, int* count, int* stack_count, int* mop_count, int* loc_count,
                           int* mutex_count, int* thread_count, int* unique_tid_count, void** sleep_trace, unsigned long trace_size);

// Read by ThreadSanitizer as the program starts. A race stops the program, as any other error found while it runs does.
const char* __tsan_default_options(void) {
    return "halt_on_error=1:exitcode=134:second_deadlock_stack=1:report_signal_unsafe=0"
#ifdef CPLUS_SYMBOLIZER
           ":external_symbolizer_path='" CPLUS_SYMBOLIZER "'"
#endif
        ;
}

// Called by ThreadSanitizer just after its report, so this is what is left on the screen. Says what it means in C+'s terms.
void __tsan_on_report(void* report) {
    const char* description = "";
    int unused = 0;
    void* no_trace = NULL;
    __tsan_get_report_data(report, &description, &unused, &unused, &unused, &unused, &unused, &unused, &unused, &no_trace, 0);
    const char* message = "threads were used in a way that is not safe";
    const char* help = "the report above shows where";
    if (strstr(description, "data-race")) {
        message = "two threads used the same memory at the same time, and at least one of them was changing it";
        help = "the report above shows both places. Protect them with a Mutex, or give each thread a copy of its own";
    } else if (strstr(description, "lock-order")) {
        message = "threads can get stuck waiting for each other's Mutex, as they lock them in different orders";
        help = "the report above shows where. Lock them in the same order everywhere";
    } else if (strstr(description, "use-after-free")) {
        message = "memory was used by one thread after another had freed it";
        help = "the report above shows both places";
    }
    cplus_fatal_begin();
    fprintf(stderr, "fatal error: %s\n  = help: %s\n", message, help);
}
#else
#define LAST_RELEASE __ATOMIC_RELEASE
#endif

#if defined(CPLUS_DEBUG_RUNTIME) && !defined(CPLUS_RACE_RUNTIME)
const char* __asan_get_report_description(void);

// Read by AddressSanitizer as the program starts. Leaks are left to report_leaks below.
// Running out of memory is left to the same message as in other builds.
const char* __asan_default_options(void) {
    return "detect_leaks=0:print_legend=0:allocator_may_return_null=1"
#ifdef CPLUS_SYMBOLIZER
           ":external_symbolizer_path='" CPLUS_SYMBOLIZER "'"
#endif
        ;
}

// Called by AddressSanitizer just before its report, which would otherwise end the
// program with the output so far unwritten. Says what the report means in C+'s terms.
__attribute__((no_sanitize("address"))) void __asan_on_error(void) {
    static const struct {
        const char* report;
        const char* message;
        const char* help;
    } meanings[] = {
        {"heap-use-after-free", "memory was used after it was freed",
         "the traces below show where it was used, where it was freed, and where it was allocated; free it after its "
         "last use"},
        {"heap-buffer-overflow", "a pointer went outside the memory it was allocated with",
         "the first trace below shows where, and the last where the memory was allocated; check the index and the size"},
        {"stack-buffer", "a pointer went outside a local variable or array",
         "the first trace below shows where; check the index, or use `a[i]` on the array, which is checked"},
        {"stack-use-after-return", "a pointer to a local variable was used after its function returned",
         "the first trace below shows where; return the value itself, or keep it in memory from `new`"},
        {"stack-overflow", "the program ran out of stack",
         "if the trace below repeats a function, its recursion may never stop; a very large local array can go in a "
         "vector instead"},
        {"double-free", "memory was freed twice", "the traces below show both places; free it only once"},
        {"bad-free", "`free` was given a pointer that did not come from `malloc`",
         "memory from `new` is freed with `delete`, and a local variable is not freed at all"},
        {"use-after-poison", "a pointer went before the start of memory from `new`",
         "the first trace below shows where; check the index"},
    };
    const char* report = __asan_get_report_description();
    const char* message = "the program used memory it does not own";
    const char* help = "the first trace below shows where";
    for (size_t i = 0; report && i < sizeof meanings / sizeof meanings[0]; ++i)
        if (strstr(report, meanings[i].report)) {
            message = meanings[i].message;
            help = meanings[i].help;
            break;
        }
    cplus_fatal_begin();
    fprintf(stderr, "fatal error: %s\n  = help: %s\n", message, help);
}
#endif

// Ends the program once a fatal error has been printed, with the code a shell shows for
// abort(). The error has been explained, and abort() would wake a crash reporter, which can
// take a second. A debug build adds a stack trace.
_Noreturn static void stop_program(void) {
#ifdef CPLUS_DEBUG_RUNTIME
    __sanitizer_print_stack_trace();
#endif
    _Exit(134);
}

#ifdef CPLUS_OWN_CRASH_HANDLER
#include <stdbool.h>
// A plain build has no AddressSanitizer to report a crash, so it says what happened here.
// Output printed so far would otherwise be lost. The program is out of stack when the
// address it could not reach is close to its stack pointer.
//
// The crash may have damaged what stdio uses, or have happened inside it, so flushing the output can
// crash too, or abort if the C library notices. The handler then runs again, since it does not block
// its own signal, and the message still gets out: only the flush uses stdio, and the message
// goes through write().
//
static void write_error(const char* message) {
    size_t left = strlen(message);
    while (left) {
        ssize_t wrote = write(STDERR_FILENO, message, left);
        if (wrote <= 0) return;
        message += wrote;
        left -= (size_t)wrote;
    }
}

static const char out_of_stack[] =
    "fatal error: the program ran out of stack\n"
    "  = help: if a function calls itself, check that its recursion stops; a very large local array can go "
    "in a vector instead\n";
static const char bad_memory[] =
    "fatal error: the program used memory it does not own\n"
    "  = help: a pointer may be wrong; run with --debug to see where\n";

static void on_crash(int signal_number, siginfo_t* info, void* context) {
    (void)signal_number;
    claim_the_report();
    // Where the handler got to: nowhere yet, flushing, or past that.
    static volatile sig_atomic_t state = 0;
    static const char* volatile message = NULL;
    if (state == 2) _Exit(134);  // the message or stopping crashed: nothing is left to try
    if (state == 1) {  // flushing crashed: what was printed is lost, and the message is not
        write_error(message);
        _Exit(134);
    }
    uintptr_t address = (uintptr_t)info->si_addr, stack = 0;
#if defined(__linux__) && defined(__x86_64__)
    stack = (uintptr_t)((ucontext_t*)context)->uc_mcontext.gregs[REG_RSP];
#elif defined(__linux__) && defined(__aarch64__)
    stack = (uintptr_t)((ucontext_t*)context)->uc_mcontext.sp;
#else
    (void)context;  // Elsewhere the stack pointer lies in other places, and the general message is still true.
#endif
    const uintptr_t near = 64 * 1024;
    message = stack && address + near >= stack && address <= stack + near ? out_of_stack : bad_memory;
    struct sigaction again;
    memset(&again, 0, sizeof again);
    again.sa_sigaction = on_crash;
    again.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&again.sa_mask);
    sigaction(SIGABRT, &again, NULL);
    state = 1;
    flush_the_output();
    state = 2;
    write_error(message);
    stop_program();
}
#endif

// Hashing. What a hash is depends on the seed each run of the program starts with, so that input cannot be
// made to collide in a hashmap on purpose, and nothing can come to depend on it: a hashmap goes over its
// entries in the order they were added. Strings are hashed with SipHash-1-3.
int64_t cplus_hash_seed = 0x2d358dccaa6c78a5;
static uint64_t hash_seed2 = 0x8bb84b93962eacc9;

static uint64_t rotate_left(uint64_t x, int n) { return (x << n) | (x >> (64 - n)); }

static void sip_round(uint64_t* v) {
    v[0] += v[1]; v[1] = rotate_left(v[1], 13); v[1] ^= v[0]; v[0] = rotate_left(v[0], 32);
    v[2] += v[3]; v[3] = rotate_left(v[3], 16); v[3] ^= v[2];
    v[0] += v[3]; v[3] = rotate_left(v[3], 21); v[3] ^= v[0];
    v[2] += v[1]; v[1] = rotate_left(v[1], 17); v[1] ^= v[2]; v[2] = rotate_left(v[2], 32);
}

int64_t cplus_hash_bytes(const char* data, int64_t size) {
    uint64_t k0 = (uint64_t)cplus_hash_seed, k1 = hash_seed2;
    uint64_t v[4] = {k0 ^ 0x736f6d6570736575ULL, k1 ^ 0x646f72616e646f6dULL, k0 ^ 0x6c7967656e657261ULL, k1 ^ 0x7465646279746573ULL};
    int64_t whole = size - size % 8;
    for (int64_t i = 0; i < whole; i += 8) {
        uint64_t word;
        memcpy(&word, data + i, 8);
        v[3] ^= word;
        sip_round(v);
        v[0] ^= word;
    }
    uint64_t last = (uint64_t)size << 56;
    for (int64_t i = whole; i < size; i++) last |= (uint64_t)(unsigned char)data[i] << (8 * (i - whole));
    v[3] ^= last;
    sip_round(v);
    v[0] ^= last;
    v[2] ^= 0xff;
    sip_round(v);
    sip_round(v);
    sip_round(v);
    return (int64_t)(v[0] ^ v[1] ^ v[2] ^ v[3]);
}

static int leak_checking(void);

// `int main(vector<string> args)`: the command line as a vector of strings, the first being how the program was started.
void* cplus_args(int argc, char** argv) {
    void* vec = cplus_vec_new((int64_t)sizeof(cplus_string), argc);
    cplus_string* items = (cplus_string*)((cplus_vec*)vec + 1);
    for (int i = 0; i < argc; ++i) {
        items[i] = (cplus_string){NULL, 0, NULL};
        cplus_str_append(&items[i], argv[i], (int64_t)strlen(argv[i]));
    }
    return vec;
}

// While the variables outside functions are worked out, before main, no other thread may start.
int cplus_before_main = 0;
void cplus_globals_start(void) { cplus_before_main = 1; }
void cplus_globals_done(void) { cplus_before_main = 0; }

// Called as the program starts.
void cplus_start(void) {
    leak_checking();  // it keeps what it finds out, which must not happen in two threads at once
    uint64_t seeds[2];
    if (getentropy(seeds, sizeof seeds) == 0) {  // otherwise the fixed ones stay, which still work
        cplus_hash_seed = (int64_t)seeds[0];
        hash_seed2 = seeds[1];
    }
#ifdef CPLUS_DEBUG_RUNTIME
    // Some systems, such as Ubuntu, set this by default. It has llvm-symbolizer ask a server
    // on the internet about the program during a stack trace, which takes seconds and has no
    // business happening when a program crashes.
    unsetenv("DEBUGINFOD_URLS");
#endif
#ifdef CPLUS_OWN_CRASH_HANDLER
    // The handler needs a stack of its own, since the program's may be what ran out. It can
    // run again if it crashes itself, which it is written to handle.
    static char crash_stack[64 * 1024];
    stack_t stack = {.ss_sp = crash_stack, .ss_size = sizeof crash_stack};
    sigaltstack(&stack, NULL);
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_sigaction = on_crash;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGBUS, &action, NULL);
#endif
}

// Called when a runtime check fails (integer overflow, division by zero, ...).
// Output printed so far is flushed first so the error appears after it.
_Noreturn void cplus_trap(const char* message, const char* file, int32_t line, int32_t column) {
    cplus_fatal_begin();
    fprintf(stderr, "fatal error: %s\n  --> %s:%" PRId32 ":%" PRId32 "\n", message, file, line, column);
    stop_program();
}

// Called for a mistake in the program that was found where no line of it is known.
_Noreturn void cplus_stop(const char* message, const char* help) {
    cplus_fatal_begin();
    fprintf(stderr, "fatal error: %s\n", message);
    if (*help) fprintf(stderr, "  = help: %s\n", help);
    stop_program();
}

// The same, for a message that was worked out and may hold anything.
_Noreturn void cplus_trap_text(const char* message, int64_t size, const char* file, int32_t line, int32_t column) {
    cplus_fatal_begin();
    fprintf(stderr, "fatal error: %.*s\n  --> %s:%" PRId32 ":%" PRId32 "\n", (int)size, message, file, line, column);
    stop_program();
}

// Called when an index is outside a vector.
_Noreturn void cplus_trap_index(int64_t index, int64_t size, const char* file, int32_t line, int32_t column) {
    char message[128];
    snprintf(message, sizeof message, "index %" PRId64 " is out of range for a vector of size %" PRId64, index, size);
    cplus_trap(message, file, line, column);
}

// Called when an index is outside an array.
_Noreturn void cplus_trap_array_index(int64_t index, int64_t size, const char* file, int32_t line, int32_t column) {
    char message[128];
    snprintf(message, sizeof message, "index %" PRId64 " is out of range for an array of size %" PRId64, index, size);
    cplus_trap(message, file, line, column);
}

// Called when an index is outside a string.
_Noreturn void cplus_trap_string_index(int64_t index, int64_t size, const char* file, int32_t line, int32_t column) {
    char message[128];
    snprintf(message, sizeof message, "index %" PRId64 " is out of range for a string of size %" PRId64, index, size);
    cplus_trap(message, file, line, column);
}

// Called when a reference to an element of a vector, or to a char of a string, is used after the container has shrunk
// to where the element is gone.
_Noreturn void cplus_trap_gone(int64_t index, int64_t size, const char* name, int32_t chars, const char* file, int32_t line, int32_t column) {
    char message[256];
    snprintf(message, sizeof message, "'%s' refers to the %s at index %" PRId64 " of a %s that now has %" PRId64 " %s, so it is gone", name,
             chars ? "char" : "element", index, chars ? "string" : "vector", size, chars ? (size == 1 ? "char" : "chars") : (size == 1 ? "element" : "elements"));
    cplus_trap(message, file, line, column);
}

// Called when a value meant for a char is not from 0 to 255. `what` says where it came
// from, such as "char(x) was given".
_Noreturn void cplus_trap_char(int64_t value, const char* what, const char* file, int32_t line, int32_t column) {
    char message[160];
    snprintf(message, sizeof message, "%s %" PRId64 ", which does not fit in a char (from 0 to 255)", what, value);
    cplus_trap(message, file, line, column);
}

// ---- Heap objects -----------------------------------------------------------
//
// Class objects and vector buffers start with a 64-bit reference count. Generated code
// retains an object when it stores a new reference to it and releases it when a
// reference goes away; the object is destroyed when the count reaches zero. Counts are
// atomic, so objects can later be shared between threads. A class object's word also counts
// weak references; see "Class objects" below.

// Set CPLUS_CHECK_LEAKS=1 to report objects that are never freed when the program
// exits. Freed objects are then kept and marked, so a later use of one is caught
// instead of reading reused memory. The test suite runs every program this way.
// A debug build reports leaks unless CPLUS_CHECK_LEAKS=0, and frees memory for real.
static int64_t live_objects;
static int check_leaks = -1;
static const int64_t freed_mark = INT64_MIN;

#ifdef CPLUS_DEBUG_RUNTIME
enum { leak_check_default = 1, keep_freed_memory = 0 };
#else
enum { leak_check_default = 0, keep_freed_memory = 1 };
#endif

static int leak_checking(void) {
    if (check_leaks < 0) {
        const char* value = getenv("CPLUS_CHECK_LEAKS");
        check_leaks = value && *value ? strcmp(value, "0") != 0 : leak_check_default;
    }
    return check_leaks;
}

_Noreturn static void bad_count(void) {
    cplus_fatal_begin();
    fputs("internal error: an object was used after it was freed; this is a bug in the C+ compiler\n", stderr);
    stop_program();
}

_Noreturn static void out_of_memory(void);

// A debug build keeps a list of the objects that are alive, so that its report can say what each one that was never freed
// is, and where it was made, which AddressSanitizer remembers.
#if defined(CPLUS_DEBUG_RUNTIME) && !defined(CPLUS_RACE_RUNTIME)
size_t __asan_get_alloc_stack(void* address, void** trace, size_t size, int* thread_id);
void __sanitizer_symbolize_pc(void* pc, const char* format, char* out, size_t size);

#define NO_LONGER ((void*)1)  // a place in the list that something was in, which a search goes past
static void** alive;
static size_t alive_size, alive_used;  // how many places, and how many are not empty
static char alive_lock;

static size_t alive_place(void* object) { return ((uintptr_t)object >> 4) * 0x9E3779B97F4A7C15u % alive_size; }

static void remember_object(void* object) {
    while (__atomic_test_and_set(&alive_lock, __ATOMIC_ACQUIRE)) {}
    if ((alive_used + 1) * 2 > alive_size) {
        void** old = alive;
        size_t old_size = alive_size;
        alive_size = old_size ? old_size * 2 : 1024;
        alive = calloc(alive_size, sizeof *alive);
        alive_used = 0;
        if (!alive) out_of_memory();
        for (size_t i = 0; i < old_size; i++) {
            if (!old[i] || old[i] == NO_LONGER) continue;
            size_t place = alive_place(old[i]);
            while (alive[place]) place = (place + 1) % alive_size;
            alive[place] = old[i];
            alive_used++;
        }
        free(old);
    }
    size_t place = alive_place(object);
    while (alive[place]) place = (place + 1) % alive_size;
    alive[place] = object;
    alive_used++;
    __atomic_clear(&alive_lock, __ATOMIC_RELEASE);
}

static void forget_object(void* object) {
    while (__atomic_test_and_set(&alive_lock, __ATOMIC_ACQUIRE)) {}
    size_t place = alive_place(object);
    while (alive[place] != object) place = (place + 1) % alive_size;
    alive[place] = NO_LONGER;
    __atomic_clear(&alive_lock, __ATOMIC_RELEASE);
}

// The first function of the program on the way to where it was made, as the runtime's come before it.
static void say_where_made(void* object) {
    void* trace[32];
    int thread;
    size_t frames = __asan_get_alloc_stack(object, trace, 32, &thread);
    for (size_t i = 0; i < frames; i++) {
        char function[256], place[512];
        __sanitizer_symbolize_pc(trace[i], "%f", function, sizeof function);
        if (strncmp(function, "cplus.", 6) != 0) continue;
        __sanitizer_symbolize_pc(trace[i], "%L", place, sizeof place);
        fprintf(stderr, ", made at %s, in %s", place, function + 6);
        return;
    }
}

static void report_each_leak(void) {
    enum { MOST_SHOWN = 10 };
    int shown = 0;
    for (size_t i = 0; i < alive_size && shown < MOST_SHOWN; i++) {
        void* object = alive[i];
        if (!object || object == NO_LONGER) continue;
        const cplus_type* const* vtable = *(const cplus_type* const* const*)((char*)object + 8);
        const cplus_type* type = vtable ? vtable[CPLUS_VTABLE_TYPE] : NULL;  // what `spawn` keeps for a thread has none
        fprintf(stderr, "  %s", type ? type->name : "what a thread was given");
        say_where_made(object);
        fputc('\n', stderr);
        shown++;
    }
    int64_t count = __atomic_load_n(&live_objects, __ATOMIC_RELAXED);
    if (count > shown) fprintf(stderr, "  and %" PRId64 " more\n", count - shown);
    fputs("  = help: objects that refer to each other in a circle keep each other alive. Make one of the references a weak<T>\n", stderr);
}
#else
#define remember_object(object) ((void)0)
#define forget_object(object) ((void)0)
#define report_each_leak() ((void)0)
#endif

void* cplus_alloc(int64_t size) {
    int64_t* object = calloc(1, (size_t)size);
    if (!object) {
        cplus_fatal_begin();
        fputs("fatal error: out of memory\n", stderr);
        stop_program();
    }
    *object = 1;
    __atomic_add_fetch(&live_objects, 1, __ATOMIC_RELAXED);
    remember_object(object);
    return object;
}

static void free_memory(void* object);

void cplus_free(void* object) {
    __atomic_sub_fetch(&live_objects, 1, __ATOMIC_RELAXED);
    forget_object(object);
    free_memory(object);
}

void cplus_retain(void* object) {
    if (!object) return;
    if (__atomic_fetch_add((int64_t*)object, 1, __ATOMIC_RELAXED) <= 0) bad_count();
}

// What generated code keeps for each class: every part of an object of it, with the first
// vtable of that part's class, which identifies the class, and its distance from the start.
typedef struct {
    void* vtable;
    int64_t offset;
} cplus_object_part;
typedef struct {
    int64_t count;
    cplus_object_part parts[];
} cplus_object_parts;

// A class object is a count, then a vtable. The vtable starts with the list of parts, then how far
// the part with this vtable is from the start of the whole, as a pointer-sized number.
void* cplus_dynamic_cast(void* object, void* vtable) {
    if (!object) return NULL;
    void** table = ((void***)object)[1];
    const cplus_object_parts* list = table[0];
    char* start = (char*)object - (intptr_t)table[1];
    void* found = NULL;
    for (int64_t i = 0; i < list->count; ++i) {
        if (list->parts[i].vtable != vtable) continue;
        if (found) return NULL;  // a diamond holds it twice, and nothing says which
        found = start + list->parts[i].offset;
    }
    return found;
}

// Returns 1 when this was the last reference, so the caller destroys the object.
int32_t cplus_release(void* object) {
    if (!object) return 0;
    int64_t before = __atomic_fetch_sub((int64_t*)object, 1, LAST_RELEASE);
    if (before == 1) {
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        return 1;
    }
    if (before <= 0) bad_count();
    return 0;
}

// ---- Class objects ----------------------------------------------------------
//
// The word at the start of a class object holds, in one atomic value:
//   bits 0 to 31    strong references, which keep the object alive
//   bits 32 to 61   weak references, which keep only its memory
//   bit 62          set from when the last strong reference goes away
// When the strong references reach zero the destructors run and the fields are released. The
// memory stays until the weak references are gone too, so that they can look at this word.
enum { max_strong = INT32_MAX };
static const int64_t strong_mask = 0xffffffff;
static const int64_t weak_one = (int64_t)1 << 32;
static const int64_t weak_mask = (int64_t)0x3fffffff << 32;
static const int64_t destroying = (int64_t)1 << 62;

_Noreturn static void too_many_references(void) {
    cplus_fatal_begin();
    fputs("fatal error: there are too many references to one object\n", stderr);
    stop_program();
}

static void free_memory(void* object) {
    if (keep_freed_memory && leak_checking()) *(int64_t*)object = freed_mark;
    else free(object);
}

// A reference can point at a base class inside an object, other than the first. Where such a
// part would have a count, it has how far back to look for it, as a negative number.
void* cplus_object_start(void* object) {
    if (!object) return NULL;
    char* part = object;
    for (;;) {
        int64_t count = __atomic_load_n((int64_t*)part, __ATOMIC_RELAXED);
        if (count >= 0 || count == freed_mark) return part;
        part += count;
    }
}

void cplus_retain_class(void* object) {
    if (!object) return;
    int64_t before = __atomic_fetch_add((int64_t*)object, 1, __ATOMIC_RELAXED);
    if (before < 0 || (before & strong_mask) == 0) bad_count();
    if ((before & strong_mask) >= max_strong) too_many_references();
}

void cplus_retain_object(void* object) { cplus_retain_class(cplus_object_start(object)); }

// Returns 1 when this was the last strong reference, so the caller destroys the object.
int32_t cplus_release_class(void* object) {
    if (!object) return 0;
    int64_t before = __atomic_fetch_sub((int64_t*)object, 1, __ATOMIC_RELEASE);
    if (before < 0 || (before & strong_mask) == 0) bad_count();
    if ((before & strong_mask) != 1) return 0;
    __atomic_fetch_or((int64_t*)object, destroying, __ATOMIC_ACQUIRE);
    return 1;
}

// While destructors run, `this` can be passed around, which counts references, so the object
// has one of its own. It is still going away: cplus_weak_lock looks at the flag.
void cplus_destructor_hold(void* object) { __atomic_fetch_add((int64_t*)object, 1, __ATOMIC_RELAXED); }

int64_t cplus_references(void* object) { return __atomic_load_n((int64_t*)object, __ATOMIC_RELAXED) & strong_mask; }

// The object is finished. Its memory goes now, or with the last weak reference.
void cplus_free_class(void* object) {
    __atomic_sub_fetch(&live_objects, 1, __ATOMIC_RELAXED);
    forget_object(object);
    int64_t before = __atomic_fetch_and((int64_t*)object, weak_mask, __ATOMIC_ACQ_REL);
    if (!(before & weak_mask)) free_memory(object);
}

void cplus_weak_retain(void* part) {
    if (!part) return;
    int64_t before = __atomic_fetch_add((int64_t*)cplus_object_start(part), weak_one, __ATOMIC_RELAXED);
    if (before < 0) bad_count();
    if ((before & weak_mask) == weak_mask) too_many_references();
}

void cplus_weak_release(void* part) {
    if (!part) return;
    int64_t* word = cplus_object_start(part);
    if (__atomic_sub_fetch(word, weak_one, __ATOMIC_ACQ_REL) == 0) free_memory(word);
}

// A new strong reference, unless the object is gone or going away. Returns `part`, or null.
void* cplus_weak_lock(void* part) {
    if (!part) return NULL;
    int64_t* word = cplus_object_start(part);
    int64_t current = __atomic_load_n(word, __ATOMIC_RELAXED);
    for (;;) {
        if ((current & strong_mask) == 0 || (current & destroying)) return NULL;
        if ((current & strong_mask) >= max_strong) too_many_references();
        if (__atomic_compare_exchange_n(word, &current, current + 1, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return part;
    }
}

// `part` while its object is there, and null once it is gone or going away.
void* cplus_weak_alive(void* part) {
    if (!part) return NULL;
    int64_t current = __atomic_load_n((int64_t*)cplus_object_start(part), __ATOMIC_RELAXED);
    return (current & strong_mask) == 0 || (current & destroying) ? NULL : part;
}

_Noreturn static void out_of_memory(void) {
    cplus_fatal_begin();
    fputs("fatal error: out of memory\n", stderr);
    stop_program();
}

void* cplus_malloc(size_t size) {
    void* memory = malloc(size ? size : 1);
    if (!memory) out_of_memory();
    return memory;
}

void* cplus_realloc(void* memory, size_t size) {
    void* moved = realloc(memory, size ? size : 1);
    if (!moved) out_of_memory();
    return moved;
}

// ---- new and delete --------------------------------------------------------
//
// Memory from `new` starts with a header recording how many elements follow and
// whether it came from `new T` or `new T[n]`, so that delete can release every element
// and stop the program when the other form of delete is used. With leak checking,
// deleted memory is kept and marked, so deleting it again is caught.

typedef struct {
    int64_t count;
    int64_t kind;  // one of the marks below
} cplus_new_header;

static const int64_t new_one = 0x2165756c61762b43;      // "C+value!"
static const int64_t new_array = 0x2179617272612b43;    // "C+array!"
static const int64_t new_deleted = 0x2165746c65642b43;  // "C+delte!"
static int64_t live_new_blocks;

// In a debug build the header is off limits to the program, so `a[-1]` is caught too.
#if defined(CPLUS_DEBUG_RUNTIME) && !defined(CPLUS_RACE_RUNTIME)
void __asan_poison_memory_region(const volatile void* address, size_t size);
void __asan_unpoison_memory_region(const volatile void* address, size_t size);
#define HIDE_NEW_HEADER(h) __asan_poison_memory_region(h, sizeof *(h))
#define SHOW_NEW_HEADER(h) __asan_unpoison_memory_region(h, sizeof *(h))
#else
#define HIDE_NEW_HEADER(h) ((void)0)
#define SHOW_NEW_HEADER(h) ((void)0)
#endif

void* cplus_new(int64_t elem_size, int64_t count, int32_t is_array, const char* file, int32_t line, int32_t column) {
    if (count < 0) {
        char message[128];
        snprintf(message, sizeof message, "new[] was given %" PRId64 " as the number of elements, which is negative",
                 count);
        cplus_trap(message, file, line, column);
    }
    if (elem_size > 0 && count > (INT64_MAX - (int64_t)sizeof(cplus_new_header)) / elem_size) out_of_memory();
    cplus_new_header* h = calloc(1, sizeof *h + (size_t)(count * elem_size));
    if (!h) out_of_memory();
    h->count = count;
    h->kind = is_array ? new_array : new_one;
    HIDE_NEW_HEADER(h);
    __atomic_add_fetch(&live_new_blocks, 1, __ATOMIC_RELAXED);
    return h + 1;
}

// Checks that `p`, which is not null, came from the same form of new, and returns the
// number of elements it holds. The header it reads may be anything, or already freed, so
// AddressSanitizer does not stop this function: it says what went wrong itself.
__attribute__((no_sanitize("address")))
int64_t cplus_delete_check(void* p, int32_t is_array, const char* file, int32_t line, int32_t column) {
    cplus_new_header* h = (cplus_new_header*)p - 1;
    if (h->kind == (is_array ? new_array : new_one)) return h->count;
    const char* op = is_array ? "delete[]" : "delete";
    char message[160];
    if (h->kind == new_deleted)
        snprintf(message, sizeof message, "'%s' was used on memory that was already deleted", op);
    else if (h->kind == new_one)
        snprintf(message, sizeof message, "'delete[]' was used on memory from `new T`, which is freed with `delete`");
    else if (h->kind == new_array)
        snprintf(message, sizeof message, "'delete' was used on memory from `new T[n]`, which is freed with `delete[]`");
    else
        snprintf(message, sizeof message, "'%s' was used on memory that did not come from new, or was already deleted",
                 op);
    cplus_trap(message, file, line, column);
}

void cplus_delete_free(void* p) {
    cplus_new_header* h = (cplus_new_header*)p - 1;
    SHOW_NEW_HEADER(h);
    h->kind = new_deleted;
    __atomic_sub_fetch(&live_new_blocks, 1, __ATOMIC_RELAXED);
    if (!(keep_freed_memory && leak_checking())) free(h);
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

// An error that got out of main: what it is, already printed into `text`, which is used up here.
void cplus_uncaught(cplus_string* text) {
    // The program goes on to wait for its threads, which may print, and whose errors are still to be reported.
    int tries = 0;
    while (ftrylockfile(stdout) != 0 && ++tries < 100) usleep(10000);
    if (tries < 100) {
        fflush(stdout);
        funlockfile(stdout);
    }
    fprintf(stderr, "uncaught error: %.*s\n  = help: catch it with `try { ... } catch (Error e) { ... }`\n", (int)text->length, text->data);
    cplus_str_release(text->owner);
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

// `s[i] = c` changes a byte of the string at `s`. Strings share their bytes, so the
// string first gets a buffer that it alone holds: its own when nothing else holds it,
// and otherwise a copy. Returns where its bytes are now.
char* cplus_str_mutable(cplus_string* s) {
    cplus_str_buf* old = s->owner;
    if (old && __atomic_load_n(&old->count, __ATOMIC_ACQUIRE) == 1) return (char*)s->data;
    cplus_str_buf* b = str_buf_new(s->length);
    if (s->length > 0) memcpy(buf_bytes(b), s->data, (size_t)s->length);
    *s = (cplus_string){buf_bytes(b), s->length, b};
    cplus_str_release(old);
    return buf_bytes(b);
}

// `string(count, c)`: `count` copies of the byte `c`, as in C++. A negative count stops
// the program.
void cplus_str_repeat(cplus_string* out, int64_t count, int32_t c, const char* file, int32_t line, int32_t column) {
    if (count < 0) {
        char message[128];
        snprintf(message, sizeof message, "string(count, c) was given the negative count %" PRId64, count);
        cplus_trap(message, file, line, column);
    }
    if (count == 0) {
        *out = (cplus_string){NULL, 0, NULL};
        return;
    }
    cplus_str_buf* b = str_buf_new(count);
    memset(buf_bytes(b), c, (size_t)count);
    *out = (cplus_string){buf_bytes(b), count, b};
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
// `string(a)` for an `array<char, N>`: its text, which ends at the first zero byte or at
// the end of the array, whichever comes first.
void cplus_str_from_chars(cplus_string* out, const char* chars, int64_t size) {
    const char* end = memchr(chars, 0, (size_t)size);
    int64_t length = end ? end - chars : size;
    if (length == 0) {
        *out = (cplus_string){NULL, 0, NULL};
        return;
    }
    cplus_str_buf* b = str_buf_new(length);
    memcpy(buf_bytes(b), chars, (size_t)length);
    *out = (cplus_string){buf_bytes(b), length, b};
}

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

// As cplus_print_quoted, for the text an error message quotes, which can come from outside and be of any length: no more
// than its first `most` bytes, and then `...` inside the quotes. A character of UTF-8 is not cut in two.
void cplus_print_quoted_start(cplus_string* out, const char* data, int64_t length, int64_t most) {
    if (length <= most) {
        cplus_print_quoted(out, data, length);
        return;
    }
    int64_t cut = most;
    while (cut > 0 && ((unsigned char)data[cut] & 0xC0) == 0x80) --cut;
    cplus_print_quoted(out, data, cut);
    out->length -= 1;  // its closing quote, which comes after the dots
    out_write(out, "...\"", 4);
}

// Prints a char in single quotes, escaped as it would be written in source: 'a', '\n',
// '\''. Used for chars inside printed structs and vectors. A byte that is not ASCII
// is written as \xHH, since alone it is not UTF-8 text.
void cplus_print_char_quoted(cplus_string* out, int32_t value) {
    unsigned char c = (unsigned char)value;
    char text[16];
    int n;
    switch (c) {
        case '\'': n = snprintf(text, sizeof text, "'\\''"); break;
        case '\\': n = snprintf(text, sizeof text, "'\\\\'"); break;
        case '\n': n = snprintf(text, sizeof text, "'\\n'"); break;
        case '\t': n = snprintf(text, sizeof text, "'\\t'"); break;
        case '\r': n = snprintf(text, sizeof text, "'\\r'"); break;
        case '\0': n = snprintf(text, sizeof text, "'\\0'"); break;
        default:
            if (c < 0x20 || c >= 0x7f) n = snprintf(text, sizeof text, "'\\x%02x'", c);
            else n = snprintf(text, sizeof text, "'%c'", c);
    }
    out_write(out, text, n);
}

void cplus_print_i32(cplus_string* out, int32_t value) {
    char text[16];
    out_write(out, text, snprintf(text, sizeof text, "%" PRId32, value));
}

void cplus_print_i64(cplus_string* out, int64_t value) {
    char text[24];
    out_write(out, text, snprintf(text, sizeof text, "%" PRId64, value));
}

// A pointer prints as its address in hexadecimal, as std::format prints a void*: 0x0 for
// a null pointer.
void cplus_print_ptr(cplus_string* out, const void* p) {
    char text[24];
    out_write(out, text, snprintf(text, sizeof text, "0x%" PRIxPTR, (uintptr_t)p));
}

// ---- Floating-point numbers ------------------------------------------------
//
// `{}` prints a float or a double as C++'s std::format does: the fewest digits that read
// back as the same value, in fixed or scientific notation, whichever is shorter (fixed
// when they tie). So 0.1 prints as 0.1, 100.0 as 100, 1e20 as 1e+20 and 1e-5 as 1e-05.

// Whether `text` reads back as `value`, as a float or a double.
static int reads_back(const char* text, double value, int is_float) {
    return is_float ? strtof(text, NULL) == (float)value : strtod(text, NULL) == value;
}

// Writes the digits of `value` rounded to `count` significant digits into `digits`, if
// they read back as `value`, with the power of ten of the first digit. The nearest
// `count` digits may miss the value by a little where the spacing of floats changes, so
// the digits just above and below are tried too.
static int digits_that_read_back(double value, int is_float, int count, char* digits, int* exponent) {
    char text[40];
    snprintf(text, sizeof text, "%.*e", count - 1, value);
    char* e = strchr(text, 'e');
    int exp10 = atoi(e + 1);
    char mantissa[24];
    int n = 0;
    for (char* c = text; c < e; ++c)
        if (*c != '.') mantissa[n++] = *c;
    mantissa[n] = 0;
    uint64_t m = strtoull(mantissa, NULL, 10);
    uint64_t candidates[3] = {m, m + 1, m - 1};
    for (int k = 0; k < 3; ++k) {
        if (candidates[k] == 0) continue;
        char attempt[48];
        snprintf(attempt, sizeof attempt, "%" PRIu64 "e%d", candidates[k], exp10 - (count - 1));
        if (!reads_back(attempt, value, is_float)) continue;
        snprintf(digits, 24, "%" PRIu64, candidates[k]);
        int len = (int)strlen(digits);
        *exponent = exp10 + (len - count);  // 999 + 1 has one more digit
        while (len > 1 && digits[len - 1] == '0') digits[--len] = 0;
        return 1;
    }
    return 0;
}

// The shortest digits of a positive, finite value, with no trailing zeros, and the power
// of ten of the first digit: 1234.5 gives "12345" and 3. Most computed values need all
// 16 or 17 digits, so those are tried first; shorter ones are searched from 1 up.
static void shortest_digits(double value, int is_float, char* digits, int* exponent) {
    int max = is_float ? 9 : 17;
    int guess = max - 2;
    int start = digits_that_read_back(value, is_float, guess, digits, exponent) ? 1 : guess + 1;
    for (int count = start; count <= max; ++count)
        if (digits_that_read_back(value, is_float, count, digits, exponent)) return;
}

static void print_shortest(cplus_string* out, double value, int is_float) {
    // Every NaN prints as "nan". C++ prints "-nan" when the sign bit is set, but whether
    // it is set can change with optimization, and the output should not.
    if (isnan(value)) {
        out_write(out, "nan", 3);
        return;
    }
    if (signbit(value)) out_write(out, "-", 1);
    value = fabs(value);
    if (isinf(value)) {
        out_write(out, "inf", 3);
        return;
    }
    if (value == 0) {
        out_write(out, "0", 1);
        return;
    }
    char digits[24];
    int exp10;
    shortest_digits(value, is_float, digits, &exp10);
    int n = (int)strlen(digits);
    // Scientific: d.ddde+XX, with at least two digits in the exponent.
    char sci[48];
    int s = snprintf(sci, sizeof sci, "%c%s%s", digits[0], n > 1 ? "." : "", digits + 1);
    s += snprintf(sci + s, sizeof sci - s, "e%c%02d", exp10 < 0 ? '-' : '+', exp10 < 0 ? -exp10 : exp10);
    // Fixed: as many zeros as the exponent needs.
    int fixed_length = exp10 >= 0 ? (n > exp10 + 1 ? n + 1 : exp10 + 1) : n + 1 - exp10;
    if (fixed_length > s) {
        out_write(out, sci, s);
        return;
    }
    char fixed[48];
    int f = 0;
    if (exp10 >= n - 1) {
        // A whole number. Its exact digits are as short as the shortest digits padded
        // with zeros, and closer to the value, so C++ prints them: 2^60 is
        // 1152921504606846976, not 1152921504606847000.
        out_write(out, fixed, snprintf(fixed, sizeof fixed, "%.0f", value));
        return;
    }
    if (exp10 >= 0) {
        for (int i = 0; i <= exp10; ++i) fixed[f++] = i < n ? digits[i] : '0';
        if (n > exp10 + 1) {
            fixed[f++] = '.';
            for (int i = exp10 + 1; i < n; ++i) fixed[f++] = digits[i];
        }
    } else {
        fixed[f++] = '0';
        fixed[f++] = '.';
        for (int i = 0; i < -exp10 - 1; ++i) fixed[f++] = '0';
        for (int i = 0; i < n; ++i) fixed[f++] = digits[i];
    }
    out_write(out, fixed, f);
}

void cplus_print_f64(cplus_string* out, double value) { print_shortest(out, value, 0); }
void cplus_print_f32(cplus_string* out, float value) { print_shortest(out, value, 1); }

// `{:.2f}`, `{:e}`, `{:.3g}` and `{:.3}`, which print as printf's %f, %e and %g do. The
// precision is 6 when it is left out, and `{:.3}` means `{:.3g}`.
void cplus_print_float_spec(cplus_string* out, double value, int32_t type, int32_t precision) {
    if (isnan(value)) {
        out_write(out, "nan", 3);
        return;
    }
    char format[8] = {'%', '.', '*', type ? (char)type : 'g', 0};
    if (precision < 0) precision = 6;
    int length = snprintf(NULL, 0, format, precision, value);
    char small[64];
    char* text = length < (int)sizeof small ? small : malloc((size_t)length + 1);
    if (!text) out_of_memory();
    snprintf(text, (size_t)length + 1, format, precision, value);
    out_write(out, text, length);
    if (text != small) free(text);
}

// `int(x)` given a long that an int cannot hold.
_Noreturn void cplus_trap_narrow(int64_t value, const char* file, int32_t line, int32_t column) {
    char message[160];
    snprintf(message, sizeof message,
             "int(x) was given %" PRId64 ", which does not fit in an int (from -2147483648 to 2147483647)", value);
    cplus_trap(message, file, line, column);
}

// `Color(n)` given a number that is not the value of any enumerator of the enum.
_Noreturn void cplus_trap_enum(int64_t value, const char* type, const char* file, int32_t line, int32_t column) {
    char message[320];
    snprintf(message, sizeof message, "%s(n) was given %" PRId64 ", which is not the value of any enumerator of '%s'", type, value, type);
    cplus_trap(message, file, line, column);
}

// `int(x)` or `long(x)` given NaN, an infinity, or a value too large for the type.
_Noreturn void cplus_trap_float_cast(double value, const char* type, const char* file, int32_t line, int32_t column) {
    cplus_string text = {NULL, 0, NULL};
    print_shortest(&text, value, 0);
    char message[200];
    if (isnan(value))
        snprintf(message, sizeof message, "%s(x) was given NaN, which is not a number", type);
    else
        snprintf(message, sizeof message, "%s(x) was given %.*s, which does not fit in %s %s", type, (int)text.length,
                 text.data, strcmp(type, "int") == 0 ? "an" : "a", type);
    cplus_trap(message, file, line, column);
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
        report_each_leak();
    }
    int64_t blocks = __atomic_load_n(&live_new_blocks, __ATOMIC_RELAXED);
    if (blocks != 0) {
        fflush(stdout);
        fprintf(stderr, "cplus: %" PRId64 " allocation%s from new %s never freed by delete\n", blocks,
                blocks == 1 ? "" : "s", blocks == 1 ? "was" : "were");
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
