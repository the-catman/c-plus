// The string methods that build a new string or change one in place, beyond those of runtime.c: split, trim, join,
// replace_all, rfind, insert, erase and replace. What they make shares bytes with what they were given, by the rules
// of substr in runtime.c, and a string they change gets new bytes unless nothing else holds its old ones.

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

// The spaces of C's isspace, whatever the locale.
static int is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }

// A part of `s` as one of the strings for cplus_str_concat, which reads it, or shares it when it is the only one.
static cplus_string part_of(const cplus_string* s, int64_t pos, int64_t count) {
    return (cplus_string){count > 0 ? s->data + pos : NULL, count, s->owner};
}

// `s.split(sep)`: the parts between the separators, from left to right, so that "a,,b" gives ["a", "", "b"] and ""
// gives [""]. An empty separator would be found everywhere, so it stops the program.
void* cplus_str_split(const cplus_string* s, const cplus_string* sep, const char* file, int32_t line, int32_t column) {
    if (sep->length == 0) cplus_trap("split() was given an empty separator, which is between every two bytes", file, line, column);
    int64_t count = 1;
    for (int64_t at = cplus_str_find(s->data, s->length, sep->data, sep->length, 0); at >= 0;
         at = cplus_str_find(s->data, s->length, sep->data, sep->length, at + sep->length))
        ++count;
    void* vec = cplus_vec_new((int64_t)sizeof(cplus_string), count);
    cplus_string* parts = (cplus_string*)((cplus_vec*)vec + 1);
    int64_t start = 0;
    for (int64_t i = 0; i < count; ++i) {
        int64_t at = i + 1 < count ? cplus_str_find(s->data, s->length, sep->data, sep->length, start) : s->length;
        cplus_str_substr(&parts[i], s, start, at - start, file, line, column);
        start = at + sep->length;
    }
    return vec;
}

// `s.trim()`, `s.trim_start()` and `s.trim_end()`, as `sides` is 3, 1 or 2: the string without the spaces at its start,
// its end, or both.
void cplus_str_trim(cplus_string* out, const cplus_string* s, int32_t sides) {
    int64_t start = 0, end = s->length;
    if (sides & 1)
        while (start < end && is_space(s->data[start])) ++start;
    if (sides & 2)
        while (end > start && is_space(s->data[end - 1])) --end;
    cplus_str_substr(out, s, start, end - start, "", 0, 0);
}

// `parts.join(sep)`: the strings of the vector, with `sep` between each two.
void cplus_str_join(cplus_string* out, void* vec, const cplus_string* sep) {
    int64_t count = vec ? ((cplus_vec*)vec)->size : 0;
    if (count == 0) {
        *out = (cplus_string){NULL, 0, NULL};
        return;
    }
    const cplus_string* items = (const cplus_string*)((cplus_vec*)vec + 1);
    cplus_string* parts = cplus_malloc(sizeof(cplus_string) * (size_t)(2 * count - 1));
    for (int64_t i = 0; i < count; ++i) {
        parts[2 * i] = items[i];
        if (i + 1 < count) parts[2 * i + 1] = *sep;
    }
    cplus_str_concat(out, parts, 2 * count - 1);
    free(parts);
}

// `s.replace_all(from, to)`: every `from`, from left to right, each after the one before, made `to`. An empty `from`
// would be found everywhere, so it stops the program.
void cplus_str_replace_all(cplus_string* out, const cplus_string* s, const cplus_string* from, const cplus_string* to,
                           const char* file, int32_t line, int32_t column) {
    if (from->length == 0) cplus_trap("replace_all() was given an empty string to replace, which is between every two bytes", file, line, column);
    int64_t found = 0;
    for (int64_t at = cplus_str_find(s->data, s->length, from->data, from->length, 0); at >= 0;
         at = cplus_str_find(s->data, s->length, from->data, from->length, at + from->length))
        ++found;
    if (found == 0) {
        *out = *s;
        cplus_retain(out->owner);
        return;
    }
    cplus_string* parts = cplus_malloc(sizeof(cplus_string) * (size_t)(2 * found + 1));
    int64_t start = 0, count = 0, kept = 0, keptAt = 0;
    for (int64_t i = 0; i <= found; ++i) {
        int64_t at = i < found ? cplus_str_find(s->data, s->length, from->data, from->length, start) : s->length;
        if (at > start) ++kept, keptAt = count;
        parts[count++] = part_of(s, start, at - start);
        if (i < found) parts[count++] = *to;
        start = at + from->length;
    }
    if (to->length == 0 && kept == 1)  // one part of `s` is left, which substr shares or copies, as it does for any part
        cplus_str_substr(out, s, parts[keptAt].data - s->data, parts[keptAt].length, file, line, column);
    else
        cplus_str_concat(out, parts, count);
    free(parts);
}

// `s.rfind(text, from)`: the position of the last `text` that starts at or before `from`, or -1 (string::npos). A `from`
// of string::npos, or past the end, looks at the whole string.
int64_t cplus_str_rfind(const char* s, int64_t length, const char* needle, int64_t needle_length, int64_t from) {
    if (needle_length > length) return -1;
    int64_t at = length - needle_length;
    if (from >= 0 && from < at) at = from;
    for (; at >= 0; --at)
        if (needle_length == 0 || memcmp(s + at, needle, (size_t)needle_length) == 0) return at;
    return -1;
}

// `s.insert(pos, text)`, `s.erase(pos, count)` and `s.replace(pos, count, text)` on the string stored at `s`: the
// `count` bytes from `pos` become `text`. As in C++, a count past the end, or string::npos (-1), takes the rest. A
// position past the end, or another negative count, stops the program, as it throws in C++.
void cplus_str_splice(cplus_string* s, int64_t pos, int64_t count, const cplus_string* text, const char* method, const char* file,
                      int32_t line, int32_t column) {
    char message[160];
    if (pos < 0 || pos > s->length) {
        snprintf(message, sizeof message, "%s() position %" PRId64 " is out of range for a string of size %" PRId64, method, pos, s->length);
        cplus_trap(message, file, line, column);
    }
    int64_t rest = s->length - pos;
    if (count == -1 || count > rest) count = rest;
    if (count < 0) {
        snprintf(message, sizeof message, "%s() was given the negative length %" PRId64, method, count);
        cplus_trap(message, file, line, column);
    }
    const int64_t length = text ? text->length : 0;
    if (pos + count == s->length) {  // at the end, which is changed as pop_back and += change it
        s->length = pos;
        if (length > 0) cplus_str_append(s, text->data, length);
        if (s->length == 0) {
            cplus_str_release(s->owner);
            *s = (cplus_string){NULL, 0, NULL};
        }
        return;
    }
    cplus_string joined;
    if (pos == 0 && length == 0) {  // what is left is the end of the string, which substr shares or copies
        cplus_str_substr(&joined, s, count, rest - count, file, line, column);
    } else {
        cplus_string parts[3] = {part_of(s, 0, pos), length > 0 ? *text : (cplus_string){NULL, 0, NULL}, part_of(s, pos + count, rest - count)};
        cplus_str_concat(&joined, parts, 3);
    }
    cplus_str_release(s->owner);  // after joining, since `text` may lie in it
    *s = joined;
}
