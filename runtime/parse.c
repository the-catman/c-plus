// stoi, stol, stod and stof, for a program that uses them: see kStoiSource in compiler/Builtins.h. All of the text must be
// the number, in decimal, with only spaces around it. What C's strtol and strtod take besides, such as `0x1F`, `nan(1)` or
// a number followed by other text, is turned down here, before they see it. The text is read by its length, so a zero byte
// in it is one more character that is not a digit.

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

// How much of the text an error message quotes.
enum { MOST_QUOTED = 40 };

// The spaces of C's isspace, whatever the locale.
static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

// Where the text starts and ends without the spaces around it.
static void trim(const cplus_string* text, int64_t* start, int64_t* end) {
    *start = 0;
    *end = text->length;
    while (*start < *end && is_space(text->data[*start])) ++*start;
    while (*end > *start && is_space(text->data[*end - 1])) --*end;
}

static bool fail(const cplus_string* text, const char* why, cplus_string* problem) {
    cplus_print_quoted_start(problem, text->data, text->length, MOST_QUOTED);
    cplus_str_append(problem, why, (int64_t)strlen(why));
    return false;
}

// Whether s[at, end) is `name`, in any case.
static bool is_named(const char* s, int64_t at, int64_t end, const char* name) {
    int64_t length = (int64_t)strlen(name);
    if (end - at != length) return false;
    for (int64_t i = 0; i < length; ++i)
        if ((s[at + i] | 0x20) != name[i]) return false;  // a capital letter, made small
    return true;
}

// Whether s[at, end) is a number for strtod: [sign] digits [. [digits]] or [sign] . digits, then [e [sign] digits]; or [sign]
// inf, infinity or nan, in any case, which is what to_string prints for them. `named` says which of the two it is.
static bool is_floating(const char* s, int64_t at, int64_t end, bool* named) {
    *named = false;
    if (at < end && (s[at] == '+' || s[at] == '-')) ++at;
    if (is_named(s, at, end, "inf") || is_named(s, at, end, "infinity") || is_named(s, at, end, "nan")) {
        *named = true;
        return true;
    }
    int64_t digits = 0;
    for (; at < end && is_digit(s[at]); ++at) ++digits;
    if (at < end && s[at] == '.')
        for (++at; at < end && is_digit(s[at]); ++at) ++digits;
    if (digits == 0) return false;
    if (at < end && (s[at] == 'e' || s[at] == 'E')) {
        ++at;
        if (at < end && (s[at] == '+' || s[at] == '-')) ++at;
        int64_t exponent = 0;
        for (; at < end && is_digit(s[at]); ++at) ++exponent;
        if (exponent == 0) return false;
    }
    return at == end;
}

// A whole number, of at most `largest`, or one more than that when it is negative.
static bool read_whole(const cplus_string* text, uint64_t largest, const char* too_large, int64_t* out, cplus_string* problem) {
    int64_t start, end;
    trim(text, &start, &end);
    const char* s = text->data;
    int64_t at = start;
    bool negative = false;
    if (at < end && (s[at] == '+' || s[at] == '-')) negative = s[at++] == '-';
    bool whole = at < end;
    uint64_t magnitude = 0;
    for (; at < end && whole; ++at) {
        whole = is_digit(s[at]);
        if (!whole) break;
        if (magnitude <= (UINT64_MAX - 9) / 10) magnitude = magnitude * 10 + (uint64_t)(s[at] - '0');
        else magnitude = UINT64_MAX;  // too large for any type, but the rest must still be digits
    }
    if (!whole) {
        bool named;
        if (is_floating(s, start, end, &named) && !named)
            return fail(text, " is not a whole number: stod reads one with a fraction or an exponent", problem);
        return fail(text, " is not a whole number", problem);
    }
    if (magnitude > largest + (negative ? 1 : 0)) return fail(text, too_large, problem);
    *out = !negative ? (int64_t)magnitude : magnitude == 0 ? 0 : -(int64_t)(magnitude - 1) - 1;
    return true;
}

bool cplus_parse_int(const cplus_string* text, int32_t* out, cplus_string* problem) {
    int64_t value;
    if (!read_whole(text, INT32_MAX, " does not fit in an int", &value, problem)) return false;
    *out = (int32_t)value;
    return true;
}

bool cplus_parse_long(const cplus_string* text, int64_t* out, cplus_string* problem) {
    return read_whole(text, INT64_MAX, " does not fit in a long", out, problem);
}

// The text as C's strtod or strtof wants it, without the spaces around it and with a zero byte at its end, which the caller
// frees. Null if it is not a number.
static char* floating_text(const cplus_string* text) {
    int64_t start, end;
    trim(text, &start, &end);
    bool named;
    if (!is_floating(text->data, start, end, &named)) return NULL;
    char* copy = cplus_malloc((size_t)(end - start) + 1);
    memcpy(copy, text->data + start, (size_t)(end - start));
    copy[end - start] = 0;
    return copy;
}

// Too small a number is rounded, to zero at last, where C++ would throw. Only too large a one is an error.
bool cplus_parse_double(const cplus_string* text, double* out, cplus_string* problem) {
    char* copy = floating_text(text);
    if (!copy) return fail(text, " is not a number", problem);
    errno = 0;
    double value = strtod(copy, NULL);
    const bool too_large = errno == ERANGE && isinf(value);
    free(copy);
    if (too_large) return fail(text, " does not fit in a double", problem);
    *out = value;
    return true;
}

bool cplus_parse_float(const cplus_string* text, float* out, cplus_string* problem) {
    char* copy = floating_text(text);
    if (!copy) return fail(text, " is not a number", problem);
    errno = 0;
    float value = strtof(copy, NULL);
    const bool too_large = errno == ERANGE && isinf(value);
    free(copy);
    if (too_large) return fail(text, " does not fit in a float", problem);
    *out = value;
    return true;
}
