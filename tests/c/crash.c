// Gives the program an output that crashes when it is flushed, and a crash to report.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int stops_by_abort;

static ssize_t write_that_crashes(void* cookie, const char* data, size_t size) {
    (void)cookie;
    (void)data;
    (void)size;
    if (stops_by_abort) abort();
    *(volatile int*)8 = 1;
    return 0;
}

void break_output(int by_abort) {
    stops_by_abort = by_abort;
    cookie_io_functions_t functions = {.write = write_that_crashes};
    stdout = fopencookie(NULL, "w", functions);
}

void crash(void) { *(volatile int*)8 = 1; }

static ssize_t write_that_never_ends(void* cookie, const char* data, size_t size) {
    (void)cookie;
    (void)data;
    (void)size;
    for (;;) pause();
    return 0;
}

// An output that a thread gets stuck in, as with a reader that stopped reading, holding what stdio locks it with.
void stick_output(void) {
    cookie_io_functions_t functions = {.write = write_that_never_ends};
    stdout = fopencookie(NULL, "w", functions);
}
