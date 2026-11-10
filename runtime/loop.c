// The event loop of async, on libuv. Only a program with `async` or `await` links this, and libuv with it.
//
// Each thread that uses async has a loop of its own. An async function that waits for a task that is not done gives
// its place to the loop. When the task is done, the function is put in the queue of the loop it waited on, and that
// loop goes on with it. A thread runs its loop when its function returns, and the main thread when `main` does, until
// nothing is left, and `wait()` runs it until its task is done.
//
// The rules of async let a function go on on another thread after it waits. This loop never does that, but nothing
// may count on it: see PLAN.md, 2.9.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "internal.h"

struct cplus_loop {
    uv_loop_t uv;
    uv_async_t wake;        // for a task finished elsewhere: it puts the waiting function in the queue, then wakes the loop
    pthread_mutex_t lock;   // for the queue, which other threads add to
    cplus_waiter* first;    // the functions that can go on, in the order their tasks were done
    cplus_waiter* last;
    int64_t waiting;        // the functions of this loop that are waiting for a task
    int64_t waitingRemote;  // the ones of those whose task is finished elsewhere, which the loop has to wait for
    bool running;
};

static __thread cplus_loop* current;
// What the next run to the end of a task waits for, when the program waits there by itself: 0 for an async `main`, 1 for the
// async function of a `spawn`, and -1 for a `wait()` that the program has.
static __thread int start = -1;

// The loop goes on with the functions in the queue. One that it goes on with can add more, which come after it.
static void drain(cplus_loop* loop) {
    for (;;) {
        pthread_mutex_lock(&loop->lock);
        cplus_waiter* w = loop->first;
        if (w) {
            loop->first = w->next;
            if (!loop->first) loop->last = NULL;
        }
        pthread_mutex_unlock(&loop->lock);
        if (!w) return;
        loop->waiting--;
        if (w->remote && --loop->waitingRemote == 0) uv_unref((uv_handle_t*)&loop->wake);
        void (*resume)(void*) = w->resume;
        void* handle = w->handle;
        free(w);
        resume(handle);
    }
}

static void on_wake(uv_async_t* wake) { drain((cplus_loop*)wake->data); }

static cplus_loop* this_loop(void) {
    if (current) return current;
    cplus_loop* loop = cplus_malloc(sizeof *loop);
    memset(loop, 0, sizeof *loop);
    if (uv_loop_init(&loop->uv) != 0 || uv_async_init(&loop->uv, &loop->wake, on_wake) != 0)
        cplus_stop("the system could not start an event loop for async", "this thread may have too many files open; close some, or start fewer threads");
    loop->wake.data = loop;
    uv_unref((uv_handle_t*)&loop->wake);  // only a function waiting for a task finished elsewhere keeps the loop going
    pthread_mutex_init(&loop->lock, NULL);
    current = loop;
    return loop;
}

static bool is_done(task_state* task) {
    pthread_mutex_lock(&task->lock);
    bool done = task->done;
    pthread_mutex_unlock(&task->lock);
    return done;
}

// Until `until` is done, or, without one, until nothing is left. What is left then is waiting for something that
// nothing can finish any more.
static void run(cplus_loop* loop, task_state* until) {
    const int waiter = until ? start : -1;
    start = -1;
    loop->running = true;
    for (;;) {
        drain(loop);
        if (until && is_done(until)) break;
        int alive = uv_run(&loop->uv, UV_RUN_ONCE);
        pthread_mutex_lock(&loop->lock);
        bool queued = loop->first != NULL;
        pthread_mutex_unlock(&loop->lock);
        if (alive || queued) continue;
        if (until && !is_done(until))
            cplus_stop(waiter == 0   ? "`main` waits for a task that nothing can finish any more"
                       : waiter == 1 ? "the async function of a `spawn` waits for a task that nothing can finish any more"
                                     : "`wait()` waits for a task that nothing can finish any more",
                       "an async function that waits, directly or through others, for its own task never goes on. Check what each `await` waits for");
        break;
    }
    loop->running = false;
}

// A task of an async function is finished by the loop of the thread it started on.
void cplus_task_bind(task_state* task) { task->loop = this_loop(); }

// For `await`: false when the task is done already. Otherwise the function waits, and the loop goes on with
// `resume(handle)` once the task is done.
bool cplus_task_await(task_state* task, void (*resume)(void*), void* handle) {
    cplus_loop* loop = this_loop();
    pthread_mutex_lock(&task->lock);
    task->waited = true;
    if (task->done) {
        pthread_mutex_unlock(&task->lock);
        return false;
    }
    cplus_waiter* w = cplus_malloc(sizeof *w);
    w->resume = resume;
    w->handle = handle;
    w->loop = loop;
    w->remote = task->loop != loop;
    w->next = NULL;
    cplus_waiter** end = &task->waiters;
    while (*end) end = &(*end)->next;
    *end = w;
    pthread_mutex_unlock(&task->lock);
    loop->waiting++;
    if (w->remote && loop->waitingRemote++ == 0) uv_ref((uv_handle_t*)&loop->wake);
    return true;
}

// When a task is done, from whichever thread finished it. Each loop wakes up for its own.
void cplus_loop_wake(cplus_waiter* waiters) {
    while (waiters) {
        cplus_waiter* w = waiters;
        waiters = w->next;
        w->next = NULL;
        cplus_loop* loop = w->loop;
        pthread_mutex_lock(&loop->lock);
        if (loop->last) loop->last->next = w;
        else loop->first = w;
        loop->last = w;
        pthread_mutex_unlock(&loop->lock);
        uv_async_send(&loop->wake);
    }
}

// Before the program waits for the task of an async `main`, or of the async function of a `spawn`, which it does by itself.
void cplus_loop_waits_for_start(int what) { start = what; }

// For `wait()` on a task: one that this thread's loop finishes is waited for by running the loop until it is done.
bool cplus_loop_run_until(task_state* task) {
    cplus_loop* loop = current;
    if (!loop || task->loop != loop) return false;
    if (loop->running)
        cplus_stop("`wait()` was called on a task while this thread's event loop is running it, so it would wait forever",
                   "in an async function, wait with `await t`. A plain function that async code calls, and that waits, can be made async too, and waited for with `await`");
    run(loop, task);
    return true;
}

static void free_handle(uv_handle_t* handle) { free(handle); }

// When `main` returns, or the function of a thread: what is pending is done, and the loop then closes.
void cplus_loop_finish(void) {
    cplus_loop* loop = current;
    if (!loop) return;
    run(loop, NULL);
    if (loop->waiting > 0) {
        char message[200];
        snprintf(message, sizeof message, "the program ended while %lld async function%s still waited for a task that nothing can finish any more",
                 (long long)loop->waiting, loop->waiting == 1 ? "" : "s");
        cplus_stop(message, "an async function that waits, directly or through others, for its own task never goes on. Check what each `await` waits for");
    }
    uv_close((uv_handle_t*)&loop->wake, NULL);
    uv_run(&loop->uv, UV_RUN_DEFAULT);
    uv_loop_close(&loop->uv);
    pthread_mutex_destroy(&loop->lock);
    free(loop);
    current = NULL;
}

// ---- Timers ----------------------------------------------------------------

typedef struct {
    uv_timer_t timer;  // first, so the handle is the whole
    task_state* task;
} delay_timer;

static void on_timer(uv_timer_t* timer) {
    delay_timer* t = (delay_timer*)timer;
    cplus_task_finish(t->task);
    cplus_task_free(t->task);
    uv_close((uv_handle_t*)timer, free_handle);
}

// For `delay(ms)`: the timer finishes the task, which it holds until then. A time below 0 is 0, as in JavaScript.
void cplus_timer_start(int64_t milliseconds, task_state* task) {
    cplus_loop* loop = this_loop();
    task->loop = loop;
    cplus_task_hold(task);
    delay_timer* t = cplus_malloc(sizeof *t);
    t->task = task;
    uv_timer_init(&loop->uv, &t->timer);
    uv_update_time(&loop->uv);  // the time from when it is started, which the loop may not have looked at for a while
    uv_timer_start(&t->timer, on_timer, milliseconds < 0 ? 0 : (uint64_t)milliseconds, 0);
}
