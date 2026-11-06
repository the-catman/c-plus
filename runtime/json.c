// JSON, read and written with the descriptors of types: see reflect.h. Only a program that imports json links this.

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

// How deep a value can go. It keeps hostile input from using up the stack, and it is how a class object that refers
// back to itself is caught, if nothing else catches it first.
enum { MAX_DEPTH = 512 };

// How many bytes the valid UTF-8 character at `s` takes, or 0 if it is not one.
static int utf8_length(const unsigned char* s, int64_t left) {
    unsigned char c = s[0];
    if (c < 0x80) return 1;
    if (c < 0xC2) return 0;
    if (c < 0xE0) return left >= 2 && (s[1] & 0xC0) == 0x80 ? 2 : 0;
    if (c < 0xF0) {
        if (left < 3 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return 0;
        if ((c == 0xE0 && s[1] < 0xA0) || (c == 0xED && s[1] >= 0xA0)) return 0;  // too long a form, or half a surrogate pair
        return 3;
    }
    if (c < 0xF5) {
        if (left < 4 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) return 0;
        if ((c == 0xF0 && s[1] < 0x90) || (c == 0xF4 && s[1] >= 0x90)) return 0;  // too long a form, or past the last character
        return 4;
    }
    return 0;
}

// ---- Writing ---------------------------------------------------------------

typedef struct {
    cplus_string* out;
    int indent;  // spaces for each level, or 0 for all on one line
    int depth;
    const void* objects[MAX_DEPTH];  // the class objects being written, to find one that holds itself
    int object_count;
} writer;

static void put(writer* w, const char* text) { cplus_str_append(w->out, text, (int64_t)strlen(text)); }

static void new_line(writer* w) {
    if (!w->indent) return;
    put(w, "\n");
    for (int i = 0; i < w->depth * w->indent; i++) cplus_str_append(w->out, " ", 1);
}

static void write_char_escape(writer* w, unsigned char c) {
    char escaped[8];
    snprintf(escaped, sizeof escaped, "\\u%04x", c);
    put(w, escaped);
}

// What a C+ string holds is bytes, and JSON text is UTF-8, so what is not UTF-8 becomes the character for that.
static void write_string(writer* w, const char* data, int64_t size) {
    put(w, "\"");
    int64_t run = 0;  // from here on, what can be copied as it is
    for (int64_t i = 0; i < size;) {
        unsigned char c = (unsigned char)data[i];
        const char* short_form = c == '"' ? "\\\"" : c == '\\' ? "\\\\" : c == '\n' ? "\\n" : c == '\r' ? "\\r" : c == '\t' ? "\\t"
                                 : c == '\b' ? "\\b" : c == '\f' ? "\\f" : NULL;
        int length = short_form || c < 0x20 ? 0 : utf8_length((const unsigned char*)data + i, size - i);
        if (length) {
            i += length;
            continue;
        }
        cplus_str_append(w->out, data + run, i - run);
        if (short_form) put(w, short_form);
        else if (c < 0x20) write_char_escape(w, c);
        else put(w, "\\ufffd");
        run = ++i;
    }
    cplus_str_append(w->out, data + run, size - run);
    put(w, "\"");
}

static void write_value(writer* w, const cplus_type* t, const char* place);

_Noreturn static void stop_too_deep(void) {
    cplus_stop("a value is nested too deeply to write as JSON",
               "more than 512 levels may mean a very long chain of objects. Write it as a vector, which takes one level");
}

static void enter(writer* w) {
    if (++w->depth > MAX_DEPTH) stop_too_deep();
}

static void write_fields(writer* w, const cplus_type* t, const char* base) {
    put(w, "{");
    enter(w);
    bool any = false;
    for (int64_t i = 0; i < t->field_count; i++) {
        const cplus_field* f = &t->fields[i];
        if (!(f->flags & CPLUS_FIELD_PUBLIC) || f->type->kind == CPLUS_KIND_WEAK) continue;
        if (any) put(w, ",");
        any = true;
        new_line(w);
        write_string(w, f->name, (int64_t)strlen(f->name));
        put(w, w->indent ? ": " : ":");
        write_value(w, f->type, base + f->offset);
    }
    w->depth--;
    if (any) new_line(w);
    put(w, "}");
}

static void write_object(writer* w, const char* place) {
    char* object = *(char* const*)place;
    if (!object) {  // only a unique reference that was moved away, or an optional
        put(w, "null");
        return;
    }
    // Whatever the object really is, as when it is printed.
    const cplus_type* real = (*(const cplus_type* const* const*)(object + 8))[CPLUS_VTABLE_TYPE];
    const char* start = cplus_object_start(object);
    for (int i = 0; i < w->object_count; i++)
        if (w->objects[i] == start)
            cplus_stop("an object refers to itself, directly or through others, so it cannot be written as JSON",
                       "JSON has no way to say that an object is the same one again. A weak reference is left out of JSON, which breaks a circle");
    if (w->object_count == MAX_DEPTH) stop_too_deep();
    w->objects[w->object_count++] = start;
    write_fields(w, real, start);
    w->object_count--;
}

static void write_map(writer* w, const cplus_type* t, const char* place) {
    const cplus_vec* entries = *(cplus_vec* const*)place;
    const cplus_vec* hashes = *(cplus_vec* const*)(place + t->length);
    const cplus_field* key = &t->elem->fields[0];
    const cplus_field* value = &t->elem->fields[1];
    put(w, "{");
    enter(w);
    bool any = false;
    for (int64_t i = 0; entries && i < entries->size; i++) {
        if (((const int64_t*)(hashes + 1))[i] < 0) continue;
        const char* entry = (const char*)(entries + 1) + i * t->elem->size;
        if (any) put(w, ",");
        any = true;
        new_line(w);
        if (key->type->kind == CPLUS_KIND_STRING) {
            const cplus_string* text = (const cplus_string*)(entry + key->offset);
            write_string(w, text->data, text->length);
        } else {  // a number, which an object can only have as a string
            char number[32];
            int64_t n = key->type->kind == CPLUS_KIND_INT ? *(const int32_t*)(entry + key->offset) : *(const int64_t*)(entry + key->offset);
            snprintf(number, sizeof number, "\"%" PRId64 "\"", n);
            put(w, number);
        }
        put(w, w->indent ? ": " : ":");
        write_value(w, value->type, entry + value->offset);
    }
    w->depth--;
    if (any) new_line(w);
    put(w, "}");
}

static void write_list(writer* w, const cplus_type* elem, const char* first, int64_t count) {
    put(w, "[");
    enter(w);
    for (int64_t i = 0; i < count; i++) {
        if (i) put(w, ",");
        new_line(w);
        write_value(w, elem, first + i * elem->size);
    }
    w->depth--;
    if (count) new_line(w);
    put(w, "]");
}

// A json::Value: what kind of value it holds, and each kind's field. See compiler/Builtins.h.
enum { VALUE_KIND, VALUE_FLAG, VALUE_NUMBER, VALUE_IS_WHOLE, VALUE_WHOLE, VALUE_TEXT, VALUE_ITEMS, VALUE_FIELDS };
enum { IS_NULL, IS_BOOL, IS_NUMBER, IS_STRING, IS_ARRAY, IS_OBJECT };

static void write_json_value(writer* w, const cplus_type* t, const char* place) {
    const cplus_field* f = t->fields;
    switch (*(const int32_t*)(place + f[VALUE_KIND].offset)) {
        case IS_BOOL: put(w, place[f[VALUE_FLAG].offset] ? "true" : "false"); return;
        case IS_NUMBER: {
            if (place[f[VALUE_IS_WHOLE].offset]) write_value(w, f[VALUE_WHOLE].type, place + f[VALUE_WHOLE].offset);
            else write_value(w, f[VALUE_NUMBER].type, place + f[VALUE_NUMBER].offset);
            return;
        }
        case IS_STRING: write_value(w, f[VALUE_TEXT].type, place + f[VALUE_TEXT].offset); return;
        case IS_ARRAY: write_value(w, f[VALUE_ITEMS].type, place + f[VALUE_ITEMS].offset); return;
        case IS_OBJECT: write_value(w, f[VALUE_FIELDS].type, place + f[VALUE_FIELDS].offset); return;
        default: put(w, "null"); return;
    }
}

static void write_value(writer* w, const cplus_type* t, const char* place) {
    char number[32];
    switch (t->kind) {
        case CPLUS_KIND_BOOL: put(w, *place ? "true" : "false"); return;
        case CPLUS_KIND_INT:
            snprintf(number, sizeof number, "%" PRId32, *(const int32_t*)place);
            put(w, number);
            return;
        case CPLUS_KIND_LONG:
            snprintf(number, sizeof number, "%" PRId64, *(const int64_t*)place);
            put(w, number);
            return;
        // JSON has no NaN or infinity, and these are what a JavaScript program would write.
        case CPLUS_KIND_FLOAT: {
            float x = *(const float*)place;
            if (isfinite(x)) cplus_print_f32(w->out, x);
            else put(w, "null");
            return;
        }
        case CPLUS_KIND_DOUBLE: {
            double x = *(const double*)place;
            if (isfinite(x)) cplus_print_f64(w->out, x);
            else put(w, "null");
            return;
        }
        case CPLUS_KIND_CHAR: {
            unsigned char c = (unsigned char)*place;
            if (c < 0x80) {
                write_string(w, place, 1);
            } else {  // a byte that is not a character by itself: what it counts as, as a char, 0 to 255
                put(w, "\"");
                write_char_escape(w, c);
                put(w, "\"");
            }
            return;
        }
        case CPLUS_KIND_STRING: {
            const cplus_string* s = (const cplus_string*)place;
            write_string(w, s->data, s->length);
            return;
        }
        case CPLUS_KIND_VECTOR: {
            const cplus_vec* v = *(cplus_vec* const*)place;
            write_list(w, t->elem, (const char*)(v + 1), v ? v->size : 0);
            return;
        }
        case CPLUS_KIND_ARRAY: write_list(w, t->elem, place, t->length); return;
        case CPLUS_KIND_OPTIONAL:
            if (t->length < 0) write_value(w, t->elem, place);
            else if (*place) write_value(w, t->elem, place + t->length);
            else put(w, "null");
            return;
        case CPLUS_KIND_STRUCT: write_fields(w, t, place); return;
        case CPLUS_KIND_CLASS: write_object(w, place); return;
        case CPLUS_KIND_UNIQUE: write_object(w, place); return;
        case CPLUS_KIND_MAP: write_map(w, t, place); return;
        case CPLUS_KIND_JSON_VALUE: write_json_value(w, t, place); return;
        case CPLUS_KIND_WEAK: put(w, "null"); return;
        default:
            cplus_stop("a pointer cannot be written as JSON, since it only means something to this program",
                       "make it a private field, which is left out of JSON");
    }
}

void cplus_json_write(const void* value, const cplus_type* type, int32_t indent, cplus_string* out) {
    writer w = {.out = out, .indent = indent};
    write_value(&w, type, value);
}

// ---- Reading ---------------------------------------------------------------

typedef struct {
    const char* start;
    const char* at;
    const char* end;
    int depth;
    char path[256];  // where in the value this is, such as `users[2].name`, for an error
    size_t path_size;
    cplus_string* problem;
    int32_t* line;
    int32_t* column;
    bool failed;
} reader;

// Says what is wrong, once: the first thing that is wrong is what caused the rest.
static bool fail_at(reader* r, const char* at, const char* format, ...) {
    if (r->failed) return false;
    r->failed = true;
    int line = 1, column = 1;
    for (const char* p = r->start; p < at; p++) {
        if (*p == '\n') {
            line++;
            column = 1;
        } else {
            column++;
        }
    }
    *r->line = line;
    *r->column = column;
    char message[512];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof message, format, args);
    va_end(args);
    char text[1024];
    if (r->path_size)
        snprintf(text, sizeof text, "line %d, column %d: in %s: %s", line, column, r->path, message);
    else
        snprintf(text, sizeof text, "line %d, column %d: %s", line, column, message);
    cplus_str_append(r->problem, text, (int64_t)strlen(text));
    return false;
}

static void skip_space(reader* r) {
    while (r->at < r->end && (*r->at == ' ' || *r->at == '\n' || *r->at == '\r' || *r->at == '\t')) r->at++;
}

// What comes next, for an error.
static const char* next_thing(reader* r) {
    skip_space(r);
    if (r->at == r->end) return "the end of the text";
    size_t left = (size_t)(r->end - r->at);
    const char* c = r->at;
    switch (*c) {
        case '{': return "an object";
        case '[': return "an array";
        case '"': return "a string";
        default: break;
    }
    if ((left >= 4 && memcmp(c, "true", 4) == 0) || (left >= 5 && memcmp(c, "false", 5) == 0)) return "a bool";
    if (left >= 4 && memcmp(c, "null", 4) == 0) return "null";
    if (*c == '-' && left > 1) c++;
    return *c >= '0' && *c <= '9' ? "a number" : "something that is not JSON";
}

static bool expected(reader* r, const char* what) { return fail_at(r, r->at, "expected %s, but found %s", what, next_thing(r)); }

// Adds to where this is, for an error, and gives what to come back to.
static size_t path_add(reader* r, const char* format, const char* name, int64_t index) {
    size_t mark = r->path_size;
    size_t room = sizeof r->path - mark;
    int wrote = name ? snprintf(r->path + mark, room, format, mark ? "." : "", name) : snprintf(r->path + mark, room, format, index);
    if (wrote > 0) r->path_size += (size_t)wrote < room ? (size_t)wrote : room - 1;
    return mark;
}

// The same for a key of a map.
static void path_add_key(reader* r, const cplus_field* key, const char* entry) {
    size_t room = sizeof r->path - r->path_size;
    int wrote;
    if (key->type->kind == CPLUS_KIND_STRING) {
        const cplus_string* text = (const cplus_string*)(entry + key->offset);
        wrote = snprintf(r->path + r->path_size, room, "[\"%.*s\"]", text->length > 40 ? 40 : (int)text->length, text->data);
    } else {
        int64_t n = key->type->kind == CPLUS_KIND_INT ? *(const int32_t*)(entry + key->offset) : *(const int64_t*)(entry + key->offset);
        wrote = snprintf(r->path + r->path_size, room, "[%" PRId64 "]", n);
    }
    if (wrote > 0) r->path_size += (size_t)wrote < room ? (size_t)wrote : room - 1;
}

static void path_back(reader* r, size_t mark) {
    r->path_size = mark;
    r->path[mark] = 0;
}

static bool go_deeper(reader* r) {
    if (++r->depth > MAX_DEPTH) return fail_at(r, r->at, "this is nested too deeply, past %d levels", MAX_DEPTH);
    return true;
}

static bool at_word(reader* r, const char* word) {
    size_t size = strlen(word);
    if ((size_t)(r->end - r->at) < size || memcmp(r->at, word, size) != 0) return false;
    r->at += size;
    return true;
}

static int hex_digit(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static bool read_four_hex(reader* r, unsigned* out) {
    if (r->end - r->at < 4) return fail_at(r, r->at, "'\\u' must be followed by four hex digits");
    unsigned value = 0;
    for (int i = 0; i < 4; i++) {
        int digit = hex_digit(r->at[i]);
        if (digit < 0) return fail_at(r, r->at, "'\\u' must be followed by four hex digits");
        value = value * 16 + (unsigned)digit;
    }
    r->at += 4;
    *out = value;
    return true;
}

static void append_utf8(cplus_string* out, unsigned c) {
    char bytes[4];
    int size;
    if (c < 0x80) {
        bytes[0] = (char)c;
        size = 1;
    } else if (c < 0x800) {
        bytes[0] = (char)(0xC0 | (c >> 6));
        bytes[1] = (char)(0x80 | (c & 0x3F));
        size = 2;
    } else if (c < 0x10000) {
        bytes[0] = (char)(0xE0 | (c >> 12));
        bytes[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        bytes[2] = (char)(0x80 | (c & 0x3F));
        size = 3;
    } else {
        bytes[0] = (char)(0xF0 | (c >> 18));
        bytes[1] = (char)(0x80 | ((c >> 12) & 0x3F));
        bytes[2] = (char)(0x80 | ((c >> 6) & 0x3F));
        bytes[3] = (char)(0x80 | (c & 0x3F));
        size = 4;
    }
    cplus_str_append(out, bytes, size);
}

// Adds a string to `out`, at its opening quote. What is added belongs to `out`, even if this fails.
static bool read_string_into(reader* r, cplus_string* out) {
    const char* opening = r->at++;
    const char* run = r->at;
    while (r->at < r->end) {
        unsigned char c = (unsigned char)*r->at;
        if (c == '"') {
            cplus_str_append(out, run, r->at - run);
            r->at++;
            return true;
        }
        if (c < 0x20) return fail_at(r, r->at, "a string cannot hold a control character as it is; write it as \\n, \\t or \\u00XX");
        if (c != '\\') {
            int length = utf8_length((const unsigned char*)r->at, r->end - r->at);
            if (!length) return fail_at(r, r->at, "this is not valid UTF-8, which all JSON text must be");
            r->at += length;
            continue;
        }
        cplus_str_append(out, run, r->at - run);
        const char* escape = r->at++;
        if (r->at == r->end) break;
        char e = *r->at++;
        const char* plain = e == '"' ? "\"" : e == '\\' ? "\\" : e == '/' ? "/" : e == 'b' ? "\b" : e == 'f' ? "\f"
                            : e == 'n' ? "\n" : e == 'r' ? "\r" : e == 't' ? "\t" : NULL;
        if (plain) {
            cplus_str_append(out, plain, 1);
        } else if (e == 'u') {
            unsigned c1;
            if (!read_four_hex(r, &c1)) return false;
            if (c1 >= 0xDC00 && c1 <= 0xDFFF) return fail_at(r, escape, "this is the second half of a surrogate pair, without the first");
            if (c1 >= 0xD800 && c1 <= 0xDBFF) {
                unsigned c2;
                if (r->end - r->at < 2 || r->at[0] != '\\' || r->at[1] != 'u')
                    return fail_at(r, escape, "this is the first half of a surrogate pair, without the second");
                r->at += 2;
                if (!read_four_hex(r, &c2)) return false;
                if (c2 < 0xDC00 || c2 > 0xDFFF) return fail_at(r, escape, "this is the first half of a surrogate pair, without the second");
                c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
            }
            append_utf8(out, c1);
        } else {
            return fail_at(r, escape, "this is not an escape that JSON has; a backslash goes before one of \" \\ / b f n r t u");
        }
        run = r->at;
    }
    return fail_at(r, opening, "this string has no closing quote");
}

// Finds a number, as JSON writes it, without working out its value.
static bool scan_number(reader* r, bool* whole) {
    const char* p = r->at;
    const char* end = r->end;
    *whole = true;
    if (p < end && *p == '-') p++;
    if (p == end || *p < '0' || *p > '9') return expected(r, "a number");
    if (*p == '0') {
        p++;
        if (p < end && *p >= '0' && *p <= '9') return fail_at(r, r->at, "a number cannot start with 0 in JSON");
    } else {
        while (p < end && *p >= '0' && *p <= '9') p++;
    }
    if (p < end && *p == '.') {
        *whole = false;
        p++;
        if (p == end || *p < '0' || *p > '9') return fail_at(r, p, "expected a digit after the '.' of a number");
        while (p < end && *p >= '0' && *p <= '9') p++;
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
        *whole = false;
        p++;
        if (p < end && (*p == '+' || *p == '-')) p++;
        if (p == end || *p < '0' || *p > '9') return fail_at(r, p, "expected a digit in the exponent of a number");
        while (p < end && *p >= '0' && *p <= '9') p++;
    }
    r->at = p;
    return true;
}

static bool read_double(reader* r, double* out, const char* type) {
    const char* begin = r->at;
    bool whole;
    if (!scan_number(r, &whole)) return false;
    char small[64];
    size_t size = (size_t)(r->at - begin);
    char* text = size < sizeof small ? small : malloc(size + 1);
    if (!text) cplus_stop("out of memory", "");
    memcpy(text, begin, size);
    text[size] = 0;
    *out = strtod(text, NULL);
    bool fits = isfinite(*out);
    if (text != small) free(text);
    if (!fits) return fail_at(r, begin, "this number is too large for %s", type);
    return true;
}

static bool read_integer(reader* r, int64_t* out, int64_t lowest, int64_t highest, const char* type) {
    const char* begin = r->at;
    bool whole;
    if (!scan_number(r, &whole)) return false;
    size_t size = (size_t)(r->at - begin);
    char text[64];
    if (size >= sizeof text) return fail_at(r, begin, "this number does not fit in %s", type);
    memcpy(text, begin, size);
    text[size] = 0;
    long long value;
    if (whole) {
        errno = 0;
        value = strtoll(text, NULL, 10);
        if (errno == ERANGE) return fail_at(r, begin, "%s does not fit in %s", text, type);
    } else {  // `1.0` and `1e3` are whole numbers too, and `1.5` is what cannot be kept
        double d = strtod(text, NULL);
        if (d != trunc(d)) return fail_at(r, begin, "%s has a fraction, which %s cannot hold", text, type);
        if (!(d >= -9007199254740992.0 && d <= 9007199254740992.0)) return fail_at(r, begin, "%s does not fit in %s exactly", text, type);
        value = (long long)d;
    }
    if (value < lowest || value > highest) return fail_at(r, begin, "%s does not fit in %s", text, type);
    *out = value;
    return true;
}

static bool skip_value(reader* r);

static bool skip_object_or_list(reader* r, char close) {
    const char* opening = r->at++;
    if (!go_deeper(r)) return false;
    skip_space(r);
    if (r->at < r->end && *r->at == close) {
        r->at++;
        r->depth--;
        return true;
    }
    while (true) {
        skip_space(r);
        if (close == '}') {
            if (r->at == r->end || *r->at != '"') return expected(r, "a string as a key");
            cplus_string ignored = {NULL, 0, NULL};
            bool ok = read_string_into(r, &ignored);
            cplus_str_release(ignored.owner);
            if (!ok) return false;
            skip_space(r);
            if (r->at == r->end || *r->at != ':') return expected(r, "':' after the key");
            r->at++;
        }
        if (!skip_value(r)) return false;
        skip_space(r);
        if (r->at == r->end) return fail_at(r, opening, close == '}' ? "this object has no closing '}'" : "this array has no closing ']'");
        if (*r->at == ',') {
            r->at++;
            continue;
        }
        if (*r->at != close) return expected(r, close == '}' ? "',' or '}'" : "',' or ']'");
        r->at++;
        r->depth--;
        return true;
    }
}

static bool skip_value(reader* r) {
    skip_space(r);
    if (r->at == r->end) return expected(r, "a value");
    switch (*r->at) {
        case '{': return skip_object_or_list(r, '}');
        case '[': return skip_object_or_list(r, ']');
        case '"': {
            cplus_string ignored = {NULL, 0, NULL};
            bool ok = read_string_into(r, &ignored);
            cplus_str_release(ignored.owner);
            return ok;
        }
        default:
            if (at_word(r, "true") || at_word(r, "false") || at_word(r, "null")) return true;
            if (*r->at == '-' || (*r->at >= '0' && *r->at <= '9')) {
                bool whole;
                return scan_number(r, &whole);
            }
            return expected(r, "a value");
    }
}

static void clear_place(const cplus_type* t, void* place) {
    if (t->release) t->release(place);
    memset(place, 0, (size_t)t->size);
}

// Makes the value that is replaced, with its defaults, in memory that holds nothing yet.
static void start_value(const cplus_type* t, void* place) {
    memset(place, 0, (size_t)t->size);
    if (t->make && t->kind != CPLUS_KIND_CLASS) t->make(place);
}

static bool read_value(reader* r, const cplus_type* t, char* place);

static bool read_fields(reader* r, const cplus_type* t, char* base) {
    const char* opening = r->at;
    skip_space(r);
    if (r->at == r->end || *r->at != '{') return fail_at(r, r->at, "expected an object for a '%s', but found %s", t->name, next_thing(r));
    opening = r->at++;
    if (!go_deeper(r)) return false;
    bool small[32] = {false};
    bool* seen = t->field_count <= 32 ? small : calloc((size_t)t->field_count, 1);
    if (!seen) cplus_stop("out of memory", "");
    bool ok = false;
    const char* closing = NULL;
    skip_space(r);
    if (r->at < r->end && *r->at == '}') {
        closing = r->at++;
        ok = true;
    }
    while (!ok) {
        skip_space(r);
        if (r->at == r->end || *r->at != '"') {
            expected(r, r->at == r->end ? "'}' or a key" : "a string as a key");
            break;
        }
        cplus_string key = {NULL, 0, NULL};
        const cplus_field* field = NULL;
        bool read = read_string_into(r, &key);
        for (int64_t i = 0; read && i < t->field_count; i++) {
            const cplus_field* f = &t->fields[i];
            if ((f->flags & CPLUS_FIELD_PUBLIC) && strlen(f->name) == (size_t)key.length && memcmp(f->name, key.data, (size_t)key.length) == 0) {
                field = f;
                if (seen[i] && f->type->kind != CPLUS_KIND_WEAK) {  // the last of two with one name is the one that counts, as in JavaScript
                    clear_place(f->type, base + f->offset);
                    start_value(f->type, base + f->offset);
                }
                seen[i] = true;
                break;
            }
        }
        cplus_str_release(key.owner);
        if (!read) break;
        skip_space(r);
        if (r->at == r->end || *r->at != ':') {
            expected(r, "':' after the key");
            break;
        }
        r->at++;
        // A key that is not a field is left out: what sends the JSON can know of more than this program.
        if (!field || field->type->kind == CPLUS_KIND_WEAK) {
            if (!skip_value(r)) break;
        } else {
            size_t mark = path_add(r, "%s%s", field->name, 0);
            if (!read_value(r, field->type, base + field->offset)) break;
            path_back(r, mark);
        }
        skip_space(r);
        if (r->at == r->end) {
            fail_at(r, opening, "this object has no closing '}'");
            break;
        }
        if (*r->at == ',') {
            r->at++;
            continue;
        }
        if (*r->at != '}') {
            expected(r, "',' or '}'");
            break;
        }
        closing = r->at++;
        ok = true;
    }
    for (int64_t i = 0; ok && i < t->field_count; i++)
        if ((t->fields[i].flags & CPLUS_FIELD_REQUIRED) && !seen[i]) {
            fail_at(r, closing, "this '%s' has no \"%s\", which it needs. A field that can be left out has a default value, or is optional", t->name, t->fields[i].name);
            ok = false;
        }
    if (seen != small) free(seen);
    if (ok) r->depth--;
    return ok;
}

static bool read_vector(reader* r, const cplus_type* t, char* place) {
    const cplus_type* elem = t->elem;
    skip_space(r);
    if (r->at == r->end || *r->at != '[') return expected(r, "an array");
    const char* opening = r->at++;
    if (!go_deeper(r)) return false;
    cplus_vec* buffer = NULL;
    bool ok = false;
    skip_space(r);
    if (r->at < r->end && *r->at == ']') {
        r->at++;
        ok = true;
    }
    while (!ok) {
        int64_t index = buffer ? buffer->size : 0;
        buffer = cplus_vec_reserve(buffer, elem->size, index + 1, elem->retain, elem->release);
        char* slot = (char*)(buffer + 1) + index * elem->size;
        start_value(elem, slot);
        buffer->size = index + 1;
        size_t mark = path_add(r, "[%" PRId64 "]", NULL, index);
        if (!read_value(r, elem, slot)) break;
        path_back(r, mark);
        skip_space(r);
        if (r->at == r->end) {
            fail_at(r, opening, "this array has no closing ']'");
            break;
        }
        if (*r->at == ',') {
            r->at++;
            continue;
        }
        if (*r->at != ']') {
            expected(r, "',' or ']'");
            break;
        }
        r->at++;
        ok = true;
    }
    if (!ok) {
        if (buffer) cplus_vec_release(buffer, elem->size, elem->release);
        return false;
    }
    clear_place(t, place);
    *(cplus_vec**)place = buffer;
    r->depth--;
    return true;
}

static bool read_array(reader* r, const cplus_type* t, char* place) {
    const cplus_type* elem = t->elem;
    skip_space(r);
    if (r->at == r->end || *r->at != '[') return expected(r, "an array");
    const char* opening = r->at++;
    if (!go_deeper(r)) return false;
    int64_t count = 0;
    skip_space(r);
    bool closed = r->at < r->end && *r->at == ']';
    while (!closed) {
        if (count == t->length) return fail_at(r, r->at, "'%s' holds %" PRId64 " elements, but this array has more", t->name, t->length);
        size_t mark = path_add(r, "[%" PRId64 "]", NULL, count);
        if (!read_value(r, elem, place + count * elem->size)) return false;
        path_back(r, mark);
        count++;
        skip_space(r);
        if (r->at == r->end) return fail_at(r, opening, "this array has no closing ']'");
        if (*r->at == ',') {
            r->at++;
            skip_space(r);
            continue;
        }
        if (*r->at != ']') return expected(r, "',' or ']'");
        closed = true;
    }
    if (count != t->length)
        return fail_at(r, opening, "'%s' holds %" PRId64 " elements, but this array has %" PRId64, t->name, t->length, count);
    r->at++;
    r->depth--;
    return true;
}

// A key is what an object has as a string, and a map of numbers has them written as numbers inside it.
static bool read_map_key(reader* r, const cplus_field* key, char* entry) {
    const char* begin = r->at;
    cplus_string* text = (cplus_string*)(entry + key->offset);
    if (key->type->kind == CPLUS_KIND_STRING) return read_string_into(r, text);
    cplus_string digits = {NULL, 0, NULL};
    bool ok = read_string_into(r, &digits);
    if (ok) {
        char number[32];
        char* stop = NULL;
        ok = digits.length > 0 && digits.length < (int64_t)sizeof number;
        long long value = 0;
        if (ok) {
            memcpy(number, digits.data, (size_t)digits.length);
            number[digits.length] = 0;
            errno = 0;
            value = strtoll(number, &stop, 10);
            ok = errno == 0 && *stop == 0 && number[0] != ' ';
        }
        if (ok && key->type->kind == CPLUS_KIND_INT) {
            ok = value >= INT32_MIN && value <= INT32_MAX;
            if (ok) *(int32_t*)(entry + key->offset) = (int32_t)value;
        } else if (ok) {
            *(int64_t*)(entry + key->offset) = value;
        }
        if (!ok) fail_at(r, begin, "this key must be a whole number that fits in %s", key->type->kind == CPLUS_KIND_INT ? "an int" : "a long");
    }
    cplus_str_release(digits.owner);
    return ok;
}

static bool read_map(reader* r, const cplus_type* t, char* place) {
    const cplus_type* entry_type = t->elem;
    const cplus_field* key = &entry_type->fields[0];
    const cplus_field* value = &entry_type->fields[1];
    skip_space(r);
    if (r->at == r->end || *r->at != '{') return expected(r, "an object");
    const char* opening = r->at++;
    if (!go_deeper(r)) return false;
    clear_place(t, place);
    start_value(t, place);
    char* entry = malloc((size_t)entry_type->size);
    if (!entry) cplus_stop("out of memory", "");
    bool ok = false;
    skip_space(r);
    if (r->at < r->end && *r->at == '}') {
        r->at++;
        ok = true;
    }
    while (!ok) {
        skip_space(r);
        if (r->at == r->end || *r->at != '"') {
            expected(r, r->at == r->end ? "'}' or a key" : "a string as a key");
            break;
        }
        start_value(entry_type, entry);
        bool read = read_map_key(r, key, entry);
        if (read) {
            skip_space(r);
            if (r->at == r->end || *r->at != ':') read = expected(r, "':' after the key");
            else r->at++;
        }
        size_t mark = r->path_size;
        if (read) {
            path_add_key(r, key, entry);
            read = read_value(r, value->type, entry + value->offset);
        }
        if (read) path_back(r, mark);
        if (!read) {
            if (entry_type->release) entry_type->release(entry);
            break;
        }
        t->put(place, entry);  // the last of two with one key is the one that counts, as in JavaScript
        skip_space(r);
        if (r->at == r->end) {
            fail_at(r, opening, "this object has no closing '}'");
            break;
        }
        if (*r->at == ',') {
            r->at++;
            continue;
        }
        if (*r->at != '}') {
            expected(r, "',' or '}'");
            break;
        }
        r->at++;
        ok = true;
    }
    free(entry);
    if (ok) r->depth--;
    return ok;
}

// A value of any shape, into a json::Value that holds nothing.
static bool read_json_value(reader* r, const cplus_type* t, char* place) {
    const cplus_field* f = t->fields;
    int32_t* kind = (int32_t*)(place + f[VALUE_KIND].offset);
    skip_space(r);
    if (r->at == r->end) return expected(r, "a value");
    char c = *r->at;
    if (c == '{') {
        *kind = IS_OBJECT;
        return read_value(r, f[VALUE_FIELDS].type, place + f[VALUE_FIELDS].offset);
    }
    if (c == '[') {
        *kind = IS_ARRAY;
        return read_value(r, f[VALUE_ITEMS].type, place + f[VALUE_ITEMS].offset);
    }
    if (c == '"') {
        *kind = IS_STRING;
        return read_value(r, f[VALUE_TEXT].type, place + f[VALUE_TEXT].offset);
    }
    if (at_word(r, "null")) return true;
    if (at_word(r, "true") || at_word(r, "false")) {
        *kind = IS_BOOL;
        place[f[VALUE_FLAG].offset] = r->at[-1] == 'e' && r->at[-2] == 'u';
        return true;
    }
    if (c != '-' && (c < '0' || c > '9')) return expected(r, "a value");
    const char* begin = r->at;
    *kind = IS_NUMBER;
    double* number = (double*)(place + f[VALUE_NUMBER].offset);
    if (!read_double(r, number, "a double")) return false;
    // The exact number too, if it is whole: a double cannot hold every long, which an id may need.
    char text[32];
    size_t size = (size_t)(r->at - begin);
    bool whole = size < sizeof text;
    for (size_t i = 0; whole && i < size; i++) whole = begin[i] == '-' || (begin[i] >= '0' && begin[i] <= '9');
    if (whole) {
        memcpy(text, begin, size);
        text[size] = 0;
        errno = 0;
        long long value = strtoll(text, NULL, 10);
        if (errno == 0) {
            place[f[VALUE_IS_WHOLE].offset] = 1;
            *(int64_t*)(place + f[VALUE_WHOLE].offset) = value;
        }
    }
    return true;
}

// Replaces the value at `place`, which is a value already. If this fails, it still is one, for its owner to release.
static bool read_value(reader* r, const cplus_type* t, char* place) {
    skip_space(r);
    int64_t number;
    double fraction;
    switch (t->kind) {
        case CPLUS_KIND_BOOL:
            if (at_word(r, "true")) *place = 1;
            else if (at_word(r, "false")) *place = 0;
            else return expected(r, "a bool");
            return true;
        case CPLUS_KIND_INT:
            if (!read_integer(r, &number, INT32_MIN, INT32_MAX, "an int")) return false;
            *(int32_t*)place = (int32_t)number;
            return true;
        case CPLUS_KIND_LONG:
            if (!read_integer(r, &number, INT64_MIN, INT64_MAX, "a long")) return false;
            *(int64_t*)place = number;
            return true;
        case CPLUS_KIND_FLOAT: {
            const char* begin = r->at;
            if (!read_double(r, &fraction, "a float")) return false;
            if (!isfinite((float)fraction)) return fail_at(r, begin, "this number is too large for a float");
            *(float*)place = (float)fraction;
            return true;
        }
        case CPLUS_KIND_DOUBLE:
            if (!read_double(r, &fraction, "a double")) return false;
            *(double*)place = fraction;
            return true;
        case CPLUS_KIND_CHAR: {
            if (r->at == r->end || *r->at != '"') return expected(r, "a string of one char");
            const char* begin = r->at;
            cplus_string text = {NULL, 0, NULL};
            bool ok = read_string_into(r, &text);
            if (ok) {
                const unsigned char* b = (const unsigned char*)text.data;
                if (text.length == 1) *place = (char)b[0];
                else if (text.length == 2 && b[0] >= 0xC2 && b[0] <= 0xC3) *place = (char)(((b[0] & 3) << 6) | (b[1] & 0x3F));  // up to 255
                else ok = fail_at(r, begin, "expected a string of one char, but this has more");
            }
            cplus_str_release(text.owner);
            return ok;
        }
        case CPLUS_KIND_STRING: {
            if (r->at == r->end || *r->at != '"') return expected(r, "a string");
            cplus_string text = {NULL, 0, NULL};
            if (!read_string_into(r, &text)) {
                cplus_str_release(text.owner);
                return false;
            }
            cplus_string* old = (cplus_string*)place;
            cplus_str_release(old->owner);
            *old = text;
            return true;
        }
        case CPLUS_KIND_VECTOR: return read_vector(r, t, place);
        case CPLUS_KIND_ARRAY: return read_array(r, t, place);
        case CPLUS_KIND_OPTIONAL:
            if (at_word(r, "null")) {
                clear_place(t, place);
                return true;
            }
            if (t->length < 0) return read_value(r, t->elem, place);
            if (!*place) {
                start_value(t->elem, place + t->length);
                *place = 1;
            }
            return read_value(r, t->elem, place + t->length);
        case CPLUS_KIND_STRUCT: return read_fields(r, t, place);
        case CPLUS_KIND_CLASS:
            if (r->at == r->end || *r->at != '{') return fail_at(r, r->at, "expected an object for a '%s', but found %s", t->name, next_thing(r));
            clear_place(t, place);
            t->make(place);
            return read_fields(r, t, *(char**)place);
        case CPLUS_KIND_UNIQUE: return read_value(r, t->elem, place);
        case CPLUS_KIND_MAP: return read_map(r, t, place);
        case CPLUS_KIND_JSON_VALUE:
            clear_place(t, place);
            start_value(t, place);
            return read_json_value(r, t, place);
        case CPLUS_KIND_WEAK: return skip_value(r);
        default: return fail_at(r, r->at, "a '%s' cannot be read from JSON", t->name);
    }
}

// Fills `out`, which holds zero bytes. On failure, `problem` says why, and `out` still is a value to release.
int32_t cplus_json_read(const cplus_string* text, const cplus_type* type, void* out, cplus_string* problem, int32_t* line, int32_t* column) {
    reader r = {.start = text->data, .at = text->data, .end = text->data + text->length, .problem = problem, .line = line, .column = column};
    start_value(type, out);
    if (!read_value(&r, type, out)) return 0;
    skip_space(&r);
    if (r.at != r.end) {
        fail_at(&r, r.at, "expected the end of the text, but found %s", next_thing(&r));
        return 0;
    }
    return 1;
}
