// readLine(), for a program that uses it: see kReadLineSource in compiler/Builtins.h. It reads C's stdin, so it mixes with
// what C's functions read from there.

#define _POSIX_C_SOURCE 200809L  // for getline
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

#include "internal.h"

// The next line of standard input into `out`, which starts empty, without its "\n" or "\r\n". A last line with no "\n" still
// counts. False when nothing more can be read: at the end, or when reading fails.
bool cplus_read_line(cplus_string* out) {
    fflush(stdout);  // a prompt printed before, which a pipe would otherwise hold back
    char* line = NULL;
    size_t capacity = 0;
    errno = 0;
    ssize_t length = getline(&line, &capacity, stdin);
    if (length < 0) {
        const bool noMemory = errno == ENOMEM;
        free(line);
        if (noMemory) cplus_malloc((size_t)-1);  // reports that there is none, rather than taking it for the end
        return false;
    }
    if (length > 0 && line[length - 1] == '\n') {
        --length;
        if (length > 0 && line[length - 1] == '\r') --length;
    }
    cplus_str_append(out, line, (int64_t)length);
    free(line);
    return true;
}
