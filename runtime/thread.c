// Threads: what `spawn`, `Task` and `Mutex` are written on. Only a program that uses them links this.

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "internal.h"

// A thread has as much stack as the main thread usually has. The memory is only used as it is reached.
enum { THREAD_STACK = 8 * 1024 * 1024 };

// As large as the one the main thread's crash handler runs on.
enum { CRASH_STACK = 64 * 1024 };

// ---- Starting threads ------------------------------------------------------

// How many threads are running. The program does not end while any is, so that what they are doing gets done, and so
// that nothing is left to look like a leak.
static pthread_mutex_t running_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t running_changed = PTHREAD_COND_INITIALIZER;
static int64_t running;

typedef struct {
    void (*entry)(void*);
    void* closure;
} thread_start;

// Frees what http.c keeps for this thread, if the program has http at all. It must happen before this thread counts as
// finished: otherwise the program could end, and libcurl with it, while the thread is still using it.
void cplus_http_thread_end(void) __attribute__((weak));

// What loop.c does, in a program with async: what is pending on this thread's event loop is done before the thread
// counts as finished, a task's waiters are woken, and `wait()` runs the loop for a task that is finished on it.
void cplus_loop_finish(void) __attribute__((weak));
void cplus_loop_wake(cplus_waiter* waiters) __attribute__((weak));
bool cplus_loop_run_until(task_state* task) __attribute__((weak));

static void* thread_main(void* given) {
    thread_start start = *(thread_start*)given;
    free(given);
#ifdef CPLUS_OWN_CRASH_HANDLER
    // Running out of stack can only be reported from another stack, and each thread has to have one of its own.
    stack_t crash = {.ss_sp = cplus_malloc(CRASH_STACK), .ss_size = CRASH_STACK};
    sigaltstack(&crash, NULL);
#endif
    start.entry(start.closure);
    if (cplus_loop_finish) cplus_loop_finish();
    if (cplus_http_thread_end) cplus_http_thread_end();
#ifdef CPLUS_OWN_CRASH_HANDLER
    stack_t none = {.ss_flags = SS_DISABLE};
    sigaltstack(&none, NULL);
    free(crash.ss_sp);
#endif
    pthread_mutex_lock(&running_lock);
    if (--running == 0) pthread_cond_broadcast(&running_changed);
    pthread_mutex_unlock(&running_lock);
    return NULL;
}

void cplus_thread_start(void (*entry)(void*), void* closure) {
    thread_start* start = cplus_malloc(sizeof *start);
    start->entry = entry;
    start->closure = closure;
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attributes, THREAD_STACK);
    pthread_mutex_lock(&running_lock);
    running++;
    pthread_mutex_unlock(&running_lock);
    pthread_t thread;
    int result = pthread_create(&thread, &attributes, thread_main, start);
    pthread_attr_destroy(&attributes);
    if (result != 0)
        cplus_stop("the system could not start another thread",
                   "this program has too many running at once; wait for some of them with `t.wait()` before starting more");
}

// When `main` is done.
void cplus_threads_finish(void) {
    pthread_mutex_lock(&running_lock);
    while (running > 0) pthread_cond_wait(&running_changed, &running_lock);
    pthread_mutex_unlock(&running_lock);
}

// ---- Tasks -----------------------------------------------------------------

// The type is in internal.h, which loop.c shares.
task_state* cplus_task_new(void) {
    task_state* task = cplus_malloc(sizeof *task);
    pthread_mutex_init(&task->lock, NULL);
    pthread_cond_init(&task->changed, NULL);
    task->done = false;
    task->waited = false;
    task->count = 1;
    task->loop = NULL;
    task->waiters = NULL;
    return task;
}

void cplus_task_hold(task_state* task) { __atomic_add_fetch(&task->count, 1, __ATOMIC_RELAXED); }

// Let go of by the Task, or by what was to finish it.
void cplus_task_free(task_state* task) {
    if (__atomic_sub_fetch(&task->count, 1, __ATOMIC_ACQ_REL) != 0) return;
    pthread_cond_destroy(&task->changed);
    pthread_mutex_destroy(&task->lock);
    free(task);
}

// What the thread leaves in the Task is written before this, and read after cplus_task_wait, so this is what keeps those apart.
void cplus_task_finish(task_state* task) {
    pthread_mutex_lock(&task->lock);
    task->done = true;
    cplus_waiter* waiters = task->waiters;
    task->waiters = NULL;
    pthread_cond_broadcast(&task->changed);
    pthread_mutex_unlock(&task->lock);
    if (waiters) cplus_loop_wake(waiters);  // there are only waiters in a program with async, which has loop.c
}

void cplus_task_wait(task_state* task) {
    pthread_mutex_lock(&task->lock);
    task->waited = true;
    bool done = task->done;
    pthread_mutex_unlock(&task->lock);
    // A task that this thread's loop finishes is waited for by running the loop until it is done.
    if (!done && task->loop && cplus_loop_run_until) cplus_loop_run_until(task);
    pthread_mutex_lock(&task->lock);
    while (!task->done) pthread_cond_wait(&task->changed, &task->lock);
    pthread_mutex_unlock(&task->lock);
}

bool cplus_task_is_done(task_state* task) {
    pthread_mutex_lock(&task->lock);
    bool done = task->done;
    pthread_mutex_unlock(&task->lock);
    return done;
}

bool cplus_task_was_waited_for(task_state* task) {
    pthread_mutex_lock(&task->lock);
    bool waited = task->waited;
    pthread_mutex_unlock(&task->lock);
    return waited;
}

// An error that no one can ever see any more, since nothing waited for its task. As with one that gets out of main, the
// program fails: `text` is what the error prints as. Nothing else is cleaned up, as other threads may be using it.
void cplus_task_lost_error(const cplus_string* text) {
    cplus_fatal_begin();
    fprintf(stderr, "uncaught error in a task that nothing waited for: %.*s\n  = help: keep the task, and catch it with `try { t.wait(); } catch (Error e) { ... }`\n",
            (int)text->length, text->data);
    fflush(stderr);
    _Exit(1);
}

// ---- Mutex -----------------------------------------------------------------

// The kind that checks: a thread that locks one twice would otherwise wait for itself forever without a word.
pthread_mutex_t* cplus_mutex_new(void) {
    pthread_mutex_t* mutex = cplus_malloc(sizeof *mutex);
    pthread_mutexattr_t attributes;
    pthread_mutexattr_init(&attributes);
    pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init(mutex, &attributes);
    pthread_mutexattr_destroy(&attributes);
    return mutex;
}

void cplus_mutex_free(pthread_mutex_t* mutex) {
    pthread_mutex_destroy(mutex);
    free(mutex);
}

void cplus_mutex_lock(pthread_mutex_t* mutex) {
    if (pthread_mutex_lock(mutex) == EDEADLK)
        cplus_stop("this thread locked a Mutex it has already locked, so it would wait for itself forever",
                   "unlock it first, or use `LockGuard g = LockGuard(m);`, which unlocks it at the end of the block");
}

void cplus_mutex_unlock(pthread_mutex_t* mutex) {
    if (pthread_mutex_unlock(mutex) == EPERM)
        cplus_stop("this thread unlocked a Mutex it has not locked",
                   "only the thread that locked a Mutex can unlock it, once for each lock");
}

bool cplus_mutex_try_lock(pthread_mutex_t* mutex) { return pthread_mutex_trylock(mutex) == 0; }

// ---- Printing --------------------------------------------------------------

// So that what one thread prints in one call stays together. It is the lock stdio already has, which a thread can take
// again, so it does not matter that what is printed goes through stdio in many pieces.
void cplus_print_lock(void) { flockfile(stdout); }
void cplus_print_unlock(void) { funlockfile(stdout); }
