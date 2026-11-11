// fs::read, for a program that imports fs: see kFsSource in compiler/Builtins.h. It is written on C's stdio, and waits until
// it has read the whole file, without the event loop of async.

#define _POSIX_C_SOURCE 200809L  // for fileno and the strerror_r that fills a buffer
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "internal.h"

// How much of a path an error message quotes. fs::Error's `path` has all of it.
enum { MOST_QUOTED = 200 };

// How much is read at a time.
enum { CHUNK = 64 * 1024 };

// `what` is fs::Error's `kind`, and the message is `could not read "path": why`.
static bool fail_as(const char* what, const cplus_string* path, const char* why, cplus_string* kind, cplus_string* problem) {
    static const char start[] = "could not read ";
    cplus_str_append(kind, what, (int64_t)strlen(what));
    cplus_str_append(problem, start, (int64_t)sizeof start - 1);
    cplus_print_quoted_start(problem, path->data, path->length, MOST_QUOTED);
    cplus_str_append(problem, ": ", 2);
    cplus_str_append(problem, why, (int64_t)strlen(why));
    return false;
}

static bool fail_with(int error, const cplus_string* path, cplus_string* kind, cplus_string* problem) {
    switch (error) {
    case ENOENT:
        return fail_as("not-found", path, "there is no such file", kind, problem);
    case ENOTDIR:
        return fail_as("not-found", path, "a part of the path before its end is a file, not a folder", kind, problem);
    case EACCES:
    case EPERM:
        return fail_as("permission", path, "this program is not allowed to read it", kind, problem);
    case EISDIR:
        return fail_as("is-directory", path, "it is a folder, not a file", kind, problem);
    case ENAMETOOLONG:
        return fail_as("bad-path", path, "the path is too long", kind, problem);
    case ELOOP:
        return fail_as("bad-path", path, "the path goes through too many symbolic links, which may lead in a circle", kind, problem);
    default: {
        char why[256];
        if (strerror_r(error, why, sizeof why) != 0) snprintf(why, sizeof why, "error %d", error);
        return fail_as("io", path, why, kind, problem);
    }
    }
}

// The bytes of the file as they are, into `out`, which starts empty. Reading goes on to the end of the file, and not as far
// as the size the file says it has, which a pipe, or a file the system writes as it is read, does not know.
bool cplus_fs_read(const cplus_string* path, cplus_string* out, cplus_string* kind, cplus_string* problem) {
    if (path->length == 0) return fail_as("bad-path", path, "the path is empty", kind, problem);
    const char* zero = memchr(path->data, 0, (size_t)path->length);
    if (zero) {
        char why[160];
        snprintf(why, sizeof why, "the path has a zero byte at position %lld, which no name of a file can have", (long long)(zero - path->data));
        return fail_as("bad-path", path, why, kind, problem);
    }
    char* name = cplus_malloc((size_t)path->length + 1);
    memcpy(name, path->data, (size_t)path->length);
    name[path->length] = 0;
    FILE* file = fopen(name, "rb");
    const int opening = errno;
    free(name);
    if (!file) return fail_with(opening, path, kind, problem);
    struct stat info;
    if (fstat(fileno(file), &info) == 0 && S_ISDIR(info.st_mode)) {  // which C opens, and only fails to read
        fclose(file);
        return fail_with(EISDIR, path, kind, problem);
    }
    char chunk[CHUNK];
    int reading = 0;
    for (;;) {
        size_t got = fread(chunk, 1, sizeof chunk, file);
        if (got < sizeof chunk && ferror(file)) reading = errno ? errno : EIO;
        cplus_str_append(out, chunk, (int64_t)got);
        if (got < sizeof chunk) break;
    }
    fclose(file);
    if (reading) return fail_with(reading, path, kind, problem);
    return true;
}
