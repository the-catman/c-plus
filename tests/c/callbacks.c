// What tests/c/callbacks.h declares.
#include "callbacks.h"

#include <pthread.h>

int cb_each(int count, cb_visit visit, void* data) {
    for (int i = 1; i <= count; i++) {
        int r = visit(data, i);
        if (r) return r;
    }
    return 0;
}

void cb_words(cb_text f, void* data) {
    f("one", data);
    f("two", data);
    f("three", data);
}

int cb_use_ops(const struct cb_ops* ops) {
    int total = ops->add(2, 3);
    total += (int)ops->scale(1.5);
    if (ops->test('x', 200, 60000)) total += 100;
    return total;
}

static cb_visit kept;
cb_visit cb_keep(cb_visit visit) {
    cb_visit before = kept;
    kept = visit;
    return before;
}
int cb_call_kept(void* data, int value) { return kept ? kept(data, value) : -1; }

struct run {
    cb_visit visit;
    void* data;
    int result;
};
static void* run_it(void* given) {
    struct run* r = given;
    r->result = r->visit(r->data, 7);
    return NULL;
}
int cb_on_thread(cb_visit visit, void* data) {
    struct run r = {visit, data, 0};
    pthread_t thread;
    if (pthread_create(&thread, NULL, run_it, &r) != 0) return -1;
    pthread_join(thread, NULL);
    return r.result;
}

int cb_double(void* data, int value) {
    (void)data;
    return value * 2;
}

size_t cb_feed(size_t (*write)(char*, size_t, size_t, void*), void* data) {
    char chunk[] = {'a', 0, 'b'};
    return write(chunk, 1, 3, data) + write(chunk, 1, 1, data);
}
size_t cb_write(char* bytes, size_t size, size_t count, void* data) {
    (void)bytes;
    (void)data;
    return size * count;
}
