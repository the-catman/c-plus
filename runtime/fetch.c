// http::fetch: requests on the thread's event loop, through libcurl's multi interface. Only a program that calls
// http::fetch links this, and libcurl and libuv with it.
//
// Each loop has a multi handle of its own, which keeps the connections its requests open, as the one handle of a thread
// does for http::send. libcurl says which sockets to watch and when to call it back, and libuv watches them. A request
// that is redirected goes round again on its handle. When it is over, it finishes its task, and the async function
// waiting for it goes on.

#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "http.h"
#include "internal.h"

typedef struct watched watched;

typedef struct {
    CURLM* multi;
    uv_loop_t* uv;
    uv_timer_t timer;  // when libcurl wants to be called back, whatever its sockets do
    watched* sockets;  // all that are watched, so the loop can stop watching them at its end
    bool ending;
} fetch_loop;

// A socket of libcurl's that libuv watches. Neither keeps the loop going: what does is a request that is not over, which
// holds the loop. A connection kept open for later is not one.
struct watched {
    uv_poll_t poll;  // first, so the handle is the whole
    curl_socket_t socket;
    fetch_loop* owner;
    watched* prev;
    watched* next;
};

typedef struct {
    http_request request;
    CURL* handle;
    task_state* task;
    bool* ok;
} fetch;

static __thread fetch_loop* here;

static void free_watched(uv_handle_t* handle) { free(handle); }

static void free_loop(uv_handle_t* timer) { free(timer->data); }

static void forget(watched* w) {
    if (w->prev) w->prev->next = w->next;
    else w->owner->sockets = w->next;
    if (w->next) w->next->prev = w->prev;
    uv_poll_stop(&w->poll);
    uv_close((uv_handle_t*)&w->poll, free_watched);
}

static void finish(fetch* f, bool ok) {
    *f->ok = ok;
    curl_easy_cleanup(f->handle);
    http_request_end(&f->request);
    task_state* task = f->task;
    free(f);
    cplus_loop_release();
    cplus_task_finish(task);
    cplus_task_free(task);
}

// What libcurl has finished. A request that was redirected goes round again.
static void take_finished(fetch_loop* loop) {
    CURLMsg* message;
    int left;
    while ((message = curl_multi_info_read(loop->multi, &left))) {
        if (message->msg != CURLMSG_DONE) continue;
        CURL* handle = message->easy_handle;
        CURLcode rc = message->data.result;  // read before the handle is removed, which ends the message
        fetch* f = NULL;
        curl_easy_getinfo(handle, CURLINFO_PRIVATE, (char**)&f);
        curl_multi_remove_handle(loop->multi, handle);
        int state = http_request_after(&f->request, handle, rc);
        if (state == HTTP_REDIRECTED) {
            http_request_setup(&f->request, handle);
            curl_easy_setopt(handle, CURLOPT_PRIVATE, f);
            if (curl_multi_add_handle(loop->multi, handle) == CURLM_OK) continue;
            state = HTTP_FAILED;
            cplus_string* kind = f->request.kind;
            cplus_string* problem = f->request.problem;
            kind->length = problem->length = 0;
            cplus_str_append(kind, "network", 7);
            cplus_str_append(problem, "libcurl could not go on with the redirect", 41);
        }
        finish(f, state == HTTP_ANSWERED);
    }
}

static void on_ready(uv_poll_t* poll, int status, int events) {
    watched* w = (watched*)poll;
    fetch_loop* loop = w->owner;  // libcurl may stop watching this socket, and so free `w`
    int flags = 0;
    if (status < 0) flags = CURL_CSELECT_ERR;
    if (events & UV_READABLE) flags |= CURL_CSELECT_IN;
    if (events & UV_WRITABLE) flags |= CURL_CSELECT_OUT;
    int running = 0;
    curl_multi_socket_action(loop->multi, w->socket, flags, &running);
    take_finished(loop);
}

static int on_socket(CURL* handle, curl_socket_t socket, int what, void* user, void* socket_data) {
    (void)handle;
    fetch_loop* loop = user;
    watched* w = socket_data;
    if (loop->ending) return 0;
    if (what == CURL_POLL_REMOVE) {
        if (w) {
            curl_multi_assign(loop->multi, socket, NULL);
            forget(w);
        }
        return 0;
    }
    if (!w) {
        w = cplus_malloc(sizeof *w);
        memset(w, 0, sizeof *w);
        if (uv_poll_init_socket(loop->uv, &w->poll, socket) != 0) {
            free(w);
            return -1;  // libcurl then fails the request
        }
        uv_unref((uv_handle_t*)&w->poll);
        w->socket = socket;
        w->owner = loop;
        w->next = loop->sockets;
        if (loop->sockets) loop->sockets->prev = w;
        loop->sockets = w;
        curl_multi_assign(loop->multi, socket, w);
    }
    int events = 0;
    if (what & CURL_POLL_IN) events |= UV_READABLE;
    if (what & CURL_POLL_OUT) events |= UV_WRITABLE;
    uv_poll_start(&w->poll, events, on_ready);
    return 0;
}

static void on_timer(uv_timer_t* timer) {
    fetch_loop* loop = timer->data;
    int running = 0;
    curl_multi_socket_action(loop->multi, CURL_SOCKET_TIMEOUT, 0, &running);
    take_finished(loop);
}

static int on_timeout(CURLM* multi, long milliseconds, void* user) {
    (void)multi;
    fetch_loop* loop = user;
    if (loop->ending) return 0;
    if (milliseconds < 0) uv_timer_stop(&loop->timer);
    else uv_timer_start(&loop->timer, on_timer, (uint64_t)milliseconds, 0);  // never called back from in here, as libcurl asks
    return 0;
}

static fetch_loop* this_fetch_loop(void) {
    if (here) return here;
    fetch_loop* loop = cplus_malloc(sizeof *loop);
    memset(loop, 0, sizeof *loop);
    loop->uv = (uv_loop_t*)cplus_loop_uv();
    loop->multi = curl_multi_init();
    if (!loop->multi) cplus_stop("libcurl could not start, which only running out of memory should cause", "this program asked for more memory than the system could give it");
    curl_multi_setopt(loop->multi, CURLMOPT_SOCKETFUNCTION, on_socket);
    curl_multi_setopt(loop->multi, CURLMOPT_SOCKETDATA, loop);
    curl_multi_setopt(loop->multi, CURLMOPT_TIMERFUNCTION, on_timeout);
    curl_multi_setopt(loop->multi, CURLMOPT_TIMERDATA, loop);
    uv_timer_init(loop->uv, &loop->timer);
    uv_unref((uv_handle_t*)&loop->timer);
    loop->timer.data = loop;
    here = loop;
    return loop;
}

// For http::fetch: it finishes `task` once the request is over, with `*ok` saying whether it was answered. Until then what
// it is given must stay as it is, which the frame of the async function waiting for it sees to.
void cplus_http_fetch(const cplus_string* method, const cplus_string* url, const cplus_string* headers, const cplus_string* body, double timeout,
                      int64_t max_bytes, bool follow, const cplus_string* ca_file, int32_t* status, cplus_string* out_body, cplus_string* out_url,
                      cplus_string* out_headers, cplus_string* kind, cplus_string* problem, bool* ok, task_state* task) {
    cplus_task_bind(task);
    cplus_task_hold(task);
    cplus_loop_hold();
    fetch* f = cplus_malloc(sizeof *f);
    memset(f, 0, sizeof *f);
    f->task = task;
    f->ok = ok;
    if (!http_request_begin(&f->request, method, url, headers, body, timeout, max_bytes, follow, ca_file, status, out_body, out_url, out_headers,
                            kind, problem)) {
        f->handle = NULL;
        finish(f, false);
        return;
    }
    fetch_loop* loop = this_fetch_loop();
    f->handle = curl_easy_init();
    if (!f->handle) cplus_stop("libcurl could not start, which only running out of memory should cause", "this program asked for more memory than the system could give it");
    http_request_setup(&f->request, f->handle);
    curl_easy_setopt(f->handle, CURLOPT_PRIVATE, f);
    if (curl_multi_add_handle(loop->multi, f->handle) != CURLM_OK) {
        kind->length = problem->length = 0;
        cplus_str_append(kind, "network", 7);
        cplus_str_append(problem, "libcurl could not start the request", 35);
        finish(f, false);
    }
}

// When the loop has nothing left to do, before it closes: the connections kept open for later are closed.
void cplus_fetch_loop_end(void) {
    fetch_loop* loop = here;
    if (!loop) return;
    loop->ending = true;  // libcurl's callbacks from here on are for what is being taken apart
    while (loop->sockets) forget(loop->sockets);
    curl_multi_cleanup(loop->multi);
    uv_timer_stop(&loop->timer);
    uv_close((uv_handle_t*)&loop->timer, free_loop);  // the loop runs once more, for what is closing
    here = NULL;
}
