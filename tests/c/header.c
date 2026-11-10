// The functions of tests/c/header.h.
#include "header.h"

#include <stdlib.h>
#include <string.h>

int header_counter = 3;

int header_add(int a, int b) { return a + b; }
long header_length(const char* text) { return (long)strlen(text); }
const char* header_greeting(void) { return "hello from C"; }
char* header_copy(const char* text) {
    char* copy = malloc(strlen(text) + 1);
    strcpy(copy, text);
    return copy;
}
void header_free(char* text) { free(text); }
int header_count() { return header_counter; }
header_point_t header_move(header_point_t p, int dx, int dy) {
    p.x += dx;
    p.y += dy;
    return p;
}
void header_fill(struct header_named* n, const char* name) {
    strncpy(n->name, name, sizeof n->name - 1);
    n->label = "filled";
    n->weight = 2.5;
    n->where.x = 1;
    n->where.y = 2;
    n->flag = true;
}
long header_sum(const struct header_list* list) {
    long total = 0;
    for (; list; list = list->next) total += list->value;
    return total;
}
struct header_opaque {
    int n;
};
header_opaque* header_open(int n) {
    header_opaque* h = malloc(sizeof *h);
    h->n = n;
    return h;
}
int header_value(const header_opaque* h) { return h->n; }
void header_close(header_opaque* h) { free(h); }
int header_call_back(header_callback f, void* data) { return f == NULL && data == NULL ? 1 : 0; }
int header_words_sum(struct header_words w) { return w.class + w.new + w.string; }
int header_union_int(union header_either* e) { return e->i; }
int header_keyword_params(int string, int new, int private) { return string * 100 + new * 10 + private; }
int header_sum_values(const int* values, size_t count) {
    int total = 0;
    for (size_t i = 0; i < count; i++) total += values[i];
    return total;
}
int header_strings(const char* const* texts, int count) {
    int total = 0;
    for (int i = 0; i < count; i++) total += (int)strlen(texts[i]);
    return total;
}
const unsigned char* header_bytes(void) { return (const unsigned char*)"\x01\x02"; }
int header_first_byte(const unsigned char* bytes) { return bytes[0]; }
int header_count_items(const struct header_flexible* f) { return f ? f->count : -1; }
