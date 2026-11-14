// The functions of tests/c/callbacks.c, for the tests of C callbacks: C code that keeps function pointers, calls them, gives
// them back, and calls one from a thread of its own.
#ifndef CPLUS_TESTS_CALLBACKS_H
#define CPLUS_TESTS_CALLBACKS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int (*cb_visit)(void* data, int value);
typedef void (*cb_text)(const char* text, void* data);

// Calls visit with each of 1 to count, and stops at the first that gives nonzero, which it returns.
int cb_each(int count, cb_visit visit, void* data);
// Calls f with "one", "two" and "three".
void cb_words(cb_text f, void* data);
// A table of functions, as C libraries such as sqlite's keep them.
struct cb_ops {
    int (*add)(int a, int b);
    double (*scale)(double x);
    bool (*test)(char c, uint8_t small, uint16_t medium);
};
int cb_use_ops(const struct cb_ops* ops);
// The function it keeps, which it gives back.
cb_visit cb_keep(cb_visit visit);
int cb_call_kept(void* data, int value);
// Calls visit once on a thread it starts, and waits for it.
int cb_on_thread(cb_visit visit, void* data);
// A function of its own, to give to C+.
int cb_double(void* data, int value);
size_t cb_write(char* bytes, size_t size, size_t count, void* data);
size_t cb_feed(size_t (*write)(char*, size_t, size_t, void*), void* data);

#endif
