// What `import fs;` gives a program: see kFsSource in compiler/Builtins.h. It is written on C's stdio and POSIX, and each
// function waits until it is done, without the event loop of async.

#define _POSIX_C_SOURCE 200809L  // for fileno, getline, and the strerror_r that fills a buffer
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "internal.h"

// How much of a path an error message quotes. fs::Error's `path` has all of it.
enum { MOST_QUOTED = 200 };

// How much is read at a time.
enum { CHUNK = 64 * 1024 };

// What failed, for the message `could not <verb> "path": why` and for fs::Error's `kind`.
typedef struct {
    const char* verb;       // "read", "write", ...
    const char* missing;    // why, when there is nothing at the path
    const char* forbidden;  // why, when the program may not do it
    const cplus_string* path;
    const cplus_string* other;  // for rename: `could not rename "path" to "other"`
    cplus_string* kind;
    cplus_string* problem;
} failing;

static bool fail_as(const failing* f, const char* what, const char* why) {
    cplus_str_append(f->kind, what, (int64_t)strlen(what));
    cplus_str_append(f->problem, "could not ", 10);
    cplus_str_append(f->problem, f->verb, (int64_t)strlen(f->verb));
    cplus_str_append(f->problem, " ", 1);
    cplus_print_quoted_start(f->problem, f->path->data, f->path->length, MOST_QUOTED);
    if (f->other) {
        cplus_str_append(f->problem, " to ", 4);
        cplus_print_quoted_start(f->problem, f->other->data, f->other->length, MOST_QUOTED);
    }
    cplus_str_append(f->problem, ": ", 2);
    cplus_str_append(f->problem, why, (int64_t)strlen(why));
    return false;
}

static bool fail_with(const failing* f, int error) {
    switch (error) {
    case ENOENT:
        return fail_as(f, "not-found", f->missing);
    case ENOTDIR:
        return fail_as(f, "not-found", "a part of the path before its end is a file, not a folder");
    case EACCES:
    case EPERM:
        return fail_as(f, "permission", f->forbidden);
    case EROFS:
        return fail_as(f, "permission", "it is on a disk that can only be read");
    case EISDIR:
        return fail_as(f, "is-directory", "it is a folder, not a file");
    case ENAMETOOLONG:
        return fail_as(f, "bad-path", "the path is too long");
    case ELOOP:
        return fail_as(f, "bad-path", "the path goes through too many symbolic links, which may lead in a circle");
    case ENOSPC:
#ifdef EDQUOT
    case EDQUOT:
#endif
        return fail_as(f, "no-space", "there is no space left on the disk");
    default: {
        char why[256];
        if (strerror_r(error, why, sizeof why) != 0) snprintf(why, sizeof why, "error %d", error);
        return fail_as(f, "io", why);
    }
    }
}

// The path as C wants it, with a zero byte at its end, which the caller frees. Null, with the error, for a path that no file
// can have: an empty one, or one with a zero byte in it.
static char* c_path(const failing* f, const cplus_string* path) {
    if (path->length == 0) {
        fail_as(f, "bad-path", "the path is empty");
        return NULL;
    }
    const char* zero = memchr(path->data, 0, (size_t)path->length);
    if (zero) {
        char why[160];
        snprintf(why, sizeof why, "the path has a zero byte at position %lld, which no name of a file can have", (long long)(zero - path->data));
        fail_as(f, "bad-path", why);
        return NULL;
    }
    char* name = cplus_malloc((size_t)path->length + 1);
    memcpy(name, path->data, (size_t)path->length);
    name[path->length] = 0;
    return name;
}

// The same for a function that cannot fail, as fs::exists: null for a path no file can have.
static char* c_path_or_null(const cplus_string* path) {
    if (path->length == 0 || memchr(path->data, 0, (size_t)path->length)) return NULL;
    char* name = cplus_malloc((size_t)path->length + 1);
    memcpy(name, path->data, (size_t)path->length);
    name[path->length] = 0;
    return name;
}

// Opens the file for C's `mode`, as fs::read and fs::open do. Null, with the error, if it cannot be.
static FILE* open_file(const failing* f, const char* mode) {
    char* name = c_path(f, f->path);
    if (!name) return NULL;
    FILE* file = fopen(name, mode);
    const int opening = errno;
    free(name);
    if (!file) {
        fail_with(f, opening);
        return NULL;
    }
    struct stat info;
    if (mode[0] == 'r' && fstat(fileno(file), &info) == 0 && S_ISDIR(info.st_mode)) {  // which C opens, and only fails to read
        fclose(file);
        fail_with(f, EISDIR);
        return NULL;
    }
    return file;
}

// The rest of `file` into `out`, to its end. Reading goes on to the end of the file, and not as far as the size the file
// says it has, which a pipe, or a file the system writes as it is read, does not know.
static bool read_to_end(const failing* f, FILE* file, cplus_string* out) {
    char* chunk = cplus_malloc(CHUNK);
    int reading = 0;
    for (;;) {
        size_t got = fread(chunk, 1, CHUNK, file);
        if (got < CHUNK && ferror(file)) reading = errno ? errno : EIO;
        cplus_str_append(out, chunk, (int64_t)got);
        if (got < CHUNK) break;
    }
    free(chunk);
    return reading ? fail_with(f, reading) : true;
}

// `fs::read(path)`: the bytes of the file as they are, into `out`, which starts empty.
bool cplus_fs_read(const cplus_string* path, cplus_string* out, cplus_string* kind, cplus_string* problem) {
    const failing f = {"read", "there is no such file", "this program is not allowed to read it", path, NULL, kind, problem};
    FILE* file = open_file(&f, "rb");
    if (!file) return false;
    const bool ok = read_to_end(&f, file, out);
    fclose(file);
    return ok;
}

// `fs::write(path, text)`, which makes the file or replaces what it had, and `fs::append(path, text)`, which adds to its end,
// and makes it if it is not there.
bool cplus_fs_write(const cplus_string* path, const cplus_string* text, bool append, cplus_string* kind, cplus_string* problem) {
    const failing f = {append ? "append to" : "write", "a folder on the path is not there", "this program is not allowed to write it", path, NULL, kind,
                       problem};
    FILE* file = open_file(&f, append ? "ab" : "wb");
    if (!file) return false;
    int writing = 0;
    if (text->length > 0 && fwrite(text->data, 1, (size_t)text->length, file) != (size_t)text->length) writing = errno ? errno : EIO;
    if (fclose(file) != 0 && !writing) writing = errno ? errno : EIO;  // what was held back is written now, and may fail
    return writing ? fail_with(&f, writing) : true;
}

// `fs::exists(path)`: whether there is a file or a folder at the path, following symbolic links. False when it cannot tell.
bool cplus_fs_exists(const cplus_string* path) {
    char* name = c_path_or_null(path);
    if (!name) return false;
    struct stat info;
    const bool there = stat(name, &info) == 0;
    free(name);
    return there;
}

// `fs::is_directory(path)`: whether there is a folder at the path, following symbolic links.
bool cplus_fs_is_directory(const cplus_string* path) {
    char* name = c_path_or_null(path);
    if (!name) return false;
    struct stat info;
    const bool folder = stat(name, &info) == 0 && S_ISDIR(info.st_mode);
    free(name);
    return folder;
}

// `fs::remove(path)`: removes a file, or a folder with nothing in it. 1 when it did, 0 when nothing was there, as in C++, and
// -1 with the error.
int32_t cplus_fs_remove(const cplus_string* path, cplus_string* kind, cplus_string* problem) {
    const failing f = {"remove", "there is no such file", "this program is not allowed to remove it", path, NULL, kind, problem};
    char* name = c_path(&f, path);
    if (!name) return -1;
    const int done = remove(name);
    const int error = errno;
    free(name);
    if (done == 0) return 1;
    if (error == ENOENT || error == ENOTDIR) return 0;
    if (error == ENOTEMPTY || error == EEXIST) return fail_as(&f, "not-empty", "it is a folder with something in it"), -1;
    return fail_with(&f, error), -1;
}

static int by_bytes(const void* a, const void* b) {
    const cplus_string* x = a;
    const cplus_string* y = b;
    const int64_t n = x->length < y->length ? x->length : y->length;
    const int c = n > 0 ? memcmp(x->data, y->data, (size_t)n) : 0;
    return c != 0 ? c : (x->length > y->length) - (x->length < y->length);
}

// `fs::list(dir)`: the names of what is in the folder, without "." and "..", in the order of their bytes, into the
// vector<string> at `out`, which starts empty.
bool cplus_fs_list(const cplus_string* path, void** out, cplus_string* kind, cplus_string* problem) {
    const failing f = {"list", "there is no such folder", "this program is not allowed to read it", path, NULL, kind, problem};
    char* name = c_path(&f, path);
    if (!name) return false;
    struct stat info;
    if (stat(name, &info) != 0) {
        const int error = errno;
        free(name);
        return fail_with(&f, error);
    }
    if (!S_ISDIR(info.st_mode)) {
        free(name);
        return fail_as(&f, "not-directory", "it is a file, not a folder");
    }
    DIR* dir = opendir(name);
    const int opening = errno;
    free(name);
    if (!dir) return fail_with(&f, opening);
    cplus_string* names = NULL;
    int64_t count = 0, room = 0;
    int reading = 0;
    for (;;) {
        errno = 0;
        struct dirent* entry = readdir(dir);
        if (!entry) {
            reading = errno;
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (count == room) {
            room = room ? room * 2 : 16;
            names = cplus_realloc(names, sizeof(cplus_string) * (size_t)room);
        }
        names[count] = (cplus_string){NULL, 0, NULL};
        cplus_str_append(&names[count++], entry->d_name, (int64_t)strlen(entry->d_name));
    }
    closedir(dir);
    if (reading) {
        for (int64_t i = 0; i < count; ++i) cplus_str_release(names[i].owner);
        free(names);
        return fail_with(&f, reading);
    }
    if (count > 0) qsort(names, (size_t)count, sizeof(cplus_string), by_bytes);
    void* vec = cplus_vec_new((int64_t)sizeof(cplus_string), count);
    if (count > 0) memcpy((cplus_vec*)vec + 1, names, sizeof(cplus_string) * (size_t)count);  // the strings move into it
    free(names);
    *out = vec;
    return true;
}

// `fs::create_directory(path)` and, with `all`, `fs::create_directories(path)`, which makes the folders before it too. 1 when
// it made the folder, 0 when there was one already, as in C++, and -1 with the error.
int32_t cplus_fs_create_directory(const cplus_string* path, bool all, cplus_string* kind, cplus_string* problem) {
    const failing f = {"make the folder", "a folder before its end is not there", "this program is not allowed to make it", path, NULL, kind, problem};
    char* name = c_path(&f, path);
    if (!name) return -1;
    size_t length = strlen(name);
    while (length > 1 && name[length - 1] == '/') name[--length] = 0;  // `a/b/`
    int32_t made = 0;
    int error = 0;
    for (size_t end = all ? 1 : length; end <= length && !error; ++end) {
        if (end < length && name[end] != '/') continue;
        const char kept = name[end];
        name[end] = 0;
        struct stat info;
        if (mkdir(name, 0777) == 0) made = 1;
        else if (errno != EEXIST) error = errno;
        else if (stat(name, &info) != 0 || !S_ISDIR(info.st_mode)) error = end < length ? ENOTDIR : EEXIST;
        else made = 0;
        name[end] = kept;
    }
    free(name);
    if (!error) return made;
    if (error == EEXIST) return fail_as(&f, "exists", "there is a file of that name, which is not a folder"), -1;
    if (error == ENOTDIR) return fail_as(&f, "not-directory", "a part of the path before its end is a file, not a folder"), -1;
    return fail_with(&f, error), -1;
}

// `fs::rename(from, to)`: gives a file or a folder another name, and replaces a file that had that name.
bool cplus_fs_rename(const cplus_string* from, const cplus_string* to, cplus_string* kind, cplus_string* problem) {
    const failing f = {"rename", "it is not there, or a folder of the new name is not", "this program is not allowed to rename it", from, to, kind,
                       problem};
    char* old_name = c_path(&f, from);
    if (!old_name) return false;
    char* new_name = c_path(&f, to);
    if (!new_name) {
        free(old_name);
        return false;
    }
    const int done = rename(old_name, new_name);
    const int error = errno;
    free(old_name);
    free(new_name);
    if (done == 0) return true;
    if (error == ENOTEMPTY || error == EEXIST) return fail_as(&f, "not-empty", "the new name is a folder with something in it");
    if (error == EISDIR) return fail_as(&f, "is-directory", "the new name is a folder, and the old one is a file");
    if (error == ENOTDIR) return fail_as(&f, "not-directory", "the old name is a folder, and the new one is not, or a part of a path is a file");
    return fail_with(&f, error);
}

// `fs::file_size(path)`: the size of the file in bytes, following symbolic links.
bool cplus_fs_file_size(const cplus_string* path, int64_t* out, cplus_string* kind, cplus_string* problem) {
    const failing f = {"find the size of", "there is no such file", "this program is not allowed to look at it", path, NULL, kind, problem};
    char* name = c_path(&f, path);
    if (!name) return false;
    struct stat info;
    const int done = stat(name, &info);
    const int error = errno;
    free(name);
    if (done != 0) return fail_with(&f, error);
    if (S_ISDIR(info.st_mode)) return fail_with(&f, EISDIR);
    *out = (int64_t)info.st_size;
    return true;
}

// ---- fs::File --------------------------------------------------------------------

typedef struct {
    FILE* stream;  // null once it is closed
    char mode;     // 'r', 'w' or 'a', as it was opened
} fs_file;

// `fs::open(path, mode)`: the file for reading, "r", for writing from its start, "w", or for adding to its end, "a". Null, with
// the error, if it cannot be opened. Any other mode is a mistake in the program, which the compiler finds when it is written
// as a literal.
void* cplus_fs_open(const cplus_string* path, const cplus_string* mode, cplus_string* kind, cplus_string* problem) {
    const char m = mode->length == 1 ? mode->data[0] : 0;
    if (m != 'r' && m != 'w' && m != 'a') {
        cplus_string message = {NULL, 0, NULL};
        static const char start[] = "fs::open() was given the mode ";
        cplus_str_append(&message, start, (int64_t)sizeof start - 1);
        cplus_print_quoted_start(&message, mode->data, mode->length, 20);
        cplus_str_append(&message, ", which is not \"r\", \"w\" or \"a\"", 31);
        cplus_str_append(&message, "", 1);  // the zero byte at its end
        cplus_stop(message.data, "open it with \"r\" to read it, \"w\" to write it from its start, or \"a\" to add to its end");
    }
    const failing f = {"open", m == 'r' ? "there is no such file" : "a folder on the path is not there",
                       m == 'r' ? "this program is not allowed to read it" : "this program is not allowed to write it", path, NULL, kind, problem};
    FILE* stream = open_file(&f, m == 'r' ? "rb" : m == 'w' ? "wb" : "ab");
    if (!stream) return NULL;
    fs_file* file = cplus_malloc(sizeof *file);
    *file = (fs_file){stream, m};
    return file;
}

// The file, if it can be used to do this: it is open, and for reading or for writing, as it must be. An fs::File that a
// program built itself, as `fs::File(null, path)`, has no handle, and is as one that was closed.
static bool usable(const failing* f, const fs_file* file, bool writing) {
    if (!file || !file->stream) return fail_as(f, "closed", "the file was closed, with close()");
    if (writing && file->mode == 'r') return fail_as(f, "io", "it was opened with \"r\", which is for reading");
    if (!writing && file->mode != 'r')
        return fail_as(f, "io", file->mode == 'w' ? "it was opened with \"w\", which is for writing" : "it was opened with \"a\", which is for writing");
    return true;
}

#define READING(path) {"read", "there is no such file", "this program is not allowed to read it", path, NULL, kind, problem}
#define WRITING(path) {"write", "a folder on the path is not there", "this program is not allowed to write it", path, NULL, kind, problem}

// `f.readLine()`: the next line, as readLine() reads one: without its "\n" or "\r\n", and the last one even with no "\n" at
// its end. 1 for a line, 0 at the end of the file, and -1 with the error.
int32_t cplus_fs_file_read_line(void* handle, const cplus_string* path, cplus_string* out, cplus_string* kind, cplus_string* problem) {
    fs_file* file = handle;
    const failing f = READING(path);
    if (!usable(&f, file, false)) return -1;
    char* line = NULL;
    size_t capacity = 0;
    errno = 0;
    ssize_t length = getline(&line, &capacity, file->stream);
    if (length < 0) {
        const int error = errno;
        free(line);
        if (error == ENOMEM) cplus_malloc((size_t)-1);  // reports that there is no more, rather than taking it for the end
        if (ferror(file->stream)) return fail_with(&f, error ? error : EIO), -1;
        return 0;
    }
    if (length > 0 && line[length - 1] == '\n') {
        --length;
        if (length > 0 && line[length - 1] == '\r') --length;
    }
    cplus_str_append(out, line, (int64_t)length);
    free(line);
    return 1;
}

// `f.read(n)`: the next `n` bytes, or what is left when that is fewer, and "" at the end.
bool cplus_fs_file_read(void* handle, const cplus_string* path, int64_t n, cplus_string* out, cplus_string* kind, cplus_string* problem) {
    fs_file* file = handle;
    const failing f = READING(path);
    if (n < 0) {
        char message[128];
        snprintf(message, sizeof message, "read() on an fs::File was given the negative count %lld", (long long)n);
        cplus_stop(message, "give it how many bytes to read, or call readAll() for the rest of the file");
    }
    if (!usable(&f, file, false)) return false;
    char* chunk = cplus_malloc(CHUNK);
    int reading = 0;
    while (n > 0) {
        const size_t want = n < CHUNK ? (size_t)n : CHUNK;
        const size_t got = fread(chunk, 1, want, file->stream);
        cplus_str_append(out, chunk, (int64_t)got);
        n -= (int64_t)got;
        if (got < want) {
            if (ferror(file->stream)) reading = errno ? errno : EIO;
            break;
        }
    }
    free(chunk);
    return reading ? fail_with(&f, reading) : true;
}

// `f.readAll()`: the rest of the file.
bool cplus_fs_file_read_all(void* handle, const cplus_string* path, cplus_string* out, cplus_string* kind, cplus_string* problem) {
    fs_file* file = handle;
    const failing f = READING(path);
    return usable(&f, file, false) && read_to_end(&f, file->stream, out);
}

// `f.write(text)`. C's stdio may hold the bytes back, so a failure can come at flush() or close() instead.
bool cplus_fs_file_write(void* handle, const cplus_string* path, const cplus_string* text, cplus_string* kind, cplus_string* problem) {
    fs_file* file = handle;
    const failing f = WRITING(path);
    if (!usable(&f, file, true)) return false;
    if (text->length > 0 && fwrite(text->data, 1, (size_t)text->length, file->stream) != (size_t)text->length)
        return fail_with(&f, errno ? errno : EIO);
    return true;
}

// `f.flush()`: what was written, given to the system now.
bool cplus_fs_file_flush(void* handle, const cplus_string* path, cplus_string* kind, cplus_string* problem) {
    fs_file* file = handle;
    const failing f = WRITING(path);
    if (!file || !file->stream) return usable(&f, file, true);
    if (file->mode == 'r') return true;  // nothing was written
    return fflush(file->stream) == 0 ? true : fail_with(&f, errno ? errno : EIO);
}

// `f.close()`: what was held back is written, and a write that failed is reported. The file cannot be used after it.
bool cplus_fs_file_close(void* handle, const cplus_string* path, cplus_string* kind, cplus_string* problem) {
    fs_file* file = handle;
    const failing f = {"close", "there is no such file", "this program is not allowed to write it", path, NULL, kind, problem};
    if (!file || !file->stream) return fail_as(&f, "closed", "the file was closed already, with close()");
    const bool failed = ferror(file->stream) != 0 && file->mode != 'r';
    const int closing = fclose(file->stream) != 0 ? (errno ? errno : EIO) : failed ? EIO : 0;
    file->stream = NULL;
    return closing ? fail_with(&f, closing) : true;
}

// When the fs::File goes away: it is closed if it is still open, and what fails then is not reported.
void cplus_fs_file_free(void* handle) {
    fs_file* file = handle;
    if (file && file->stream) fclose(file->stream);
    free(file);
}
