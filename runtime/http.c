// The HTTP client and WebSockets, on libcurl. Only a program that imports http links this.
//
// libcurl makes the connections, with their TLS and proxies. The WebSocket protocol is written here, on the bare
// connection libcurl leaves, since many builds of libcurl, those of Debian and Ubuntu among them, leave its own out.

#define _GNU_SOURCE                // for memmem
#define CURL_DISABLE_DEPRECATION   // CURLOPT_PROTOCOLS, whose newer form older versions of libcurl do not have
#include <curl/curl.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

#include "internal.h"

// As many as JavaScript's fetch follows.
enum { MAX_REDIRECTS = 20 };

// How long closing a WebSocket waits for the other side, which must not be able to hold up a destructor for long.
enum { CLOSE_WAIT_MS = 2000 };

// How long a reply the program did not ask for, to a ping or a close, can take when the call has no time limit of its own.
enum { REPLY_WAIT_MS = 10000 };

// How long the server's answer to opening a WebSocket can be.
enum { MAX_HANDSHAKE = 64 * 1024 };

// How long waiting on a WebSocket goes before it looks again at whether another thread is closing it.
enum { WAIT_SLICE_MS = 100 };

// How long a reply to a ping waits for another thread to finish sending what it is sending. A connection so busy it takes longer than
// this is not one that a missing reply will upset.
enum { REPLY_LOCK_TRIES = 1000 };

static const int64_t NO_LIMIT = INT64_MAX;

// ---- Shared by both --------------------------------------------------------

// `what` says which kind of failure this is, as http::Error's `kind`.
__attribute__((format(printf, 4, 5)))
static bool fail_as(const char* what, cplus_string* kind, cplus_string* problem, const char* format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);
    kind->length = 0;
    problem->length = 0;
    cplus_str_append(kind, what, (int64_t)strlen(what));
    cplus_str_append(problem, text, (int64_t)strlen(text));
    return false;
}

// A C+ string as C text, or null if it has a null byte in it, which C would take for its end.
static char* c_text(const cplus_string* s) {
    if (s->length > 0 && memchr(s->data, 0, (size_t)s->length)) return NULL;
    char* text = cplus_malloc((size_t)s->length + 1);
    if (s->length > 0) memcpy(text, s->data, (size_t)s->length);
    text[s->length] = 0;
    return text;
}

static int64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

// A time limit of zero, or less, is no limit.
static int64_t deadline_after(double seconds) {
    if (!(seconds > 0) || seconds > 1e12) return NO_LIMIT;
    double ms = seconds * 1000;
    return now_ms() + (ms < 1 ? 1 : (int64_t)ms);
}

static pthread_once_t curl_once = PTHREAD_ONCE_INIT;
static pthread_key_t thread_handle_key;

static bool curl_started;

static void start_curl(void) {
    curl_global_init(CURL_GLOBAL_ALL);
    pthread_key_create(&thread_handle_key, NULL);
    __atomic_store_n(&curl_started, true, __ATOMIC_RELEASE);
}

// Called by thread.c as a thread ends, while the program is sure to be still running.
void cplus_http_thread_end(void) {
    if (!__atomic_load_n(&curl_started, __ATOMIC_ACQUIRE)) return;
    CURL* handle = pthread_getspecific(thread_handle_key);
    if (handle) curl_easy_cleanup(handle);
}

// Requests on one thread share a handle, as that is what keeps connections open from one request to the next.
static CURL* thread_handle(void) {
    pthread_once(&curl_once, start_curl);
    CURL* handle = pthread_getspecific(thread_handle_key);
    if (!handle) {
        handle = curl_easy_init();
        if (!handle) cplus_stop("libcurl could not start, which only running out of memory should cause", "this program asked for more memory than the system could give it");
        pthread_setspecific(thread_handle_key, handle);
    }
    return handle;
}

// What every connection has. Only http and https, or a URL from outside could read local files, among much else. Nothing
// here turns off checking certificates: `ca_file` is how to trust a server's own.
static void set_shared_options(CURL* handle, char* error_text, const char* ca_file) {
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);  // signals would go to the wrong thread
    curl_easy_setopt(handle, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, error_text);
    if (ca_file[0]) curl_easy_setopt(handle, CURLOPT_CAINFO, ca_file);
}

static bool curl_failure(CURLcode rc, const char* error_text, double timeout, cplus_string* kind, cplus_string* problem) {
    const char* detail = error_text[0] ? error_text : curl_easy_strerror(rc);
    switch (rc) {
    case CURLE_URL_MALFORMAT:
        return fail_as("bad-request", kind, problem, "this URL is not valid: %s", detail);
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
        return fail_as("dns", kind, problem, "could not find the server's address: %s", detail);
    case CURLE_COULDNT_CONNECT:
        return fail_as("connect", kind, problem, "could not connect to the server: %s", detail);
    case CURLE_OPERATION_TIMEDOUT:
        return fail_as("timeout", kind, problem, "this took longer than its timeout of %g seconds", timeout);
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_CIPHER:
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_ISSUER_ERROR:
    case CURLE_SSL_CRL_BADFILE:
    case CURLE_SSL_PINNEDPUBKEYNOTMATCH:
    case CURLE_SSL_INVALIDCERTSTATUS:
        return fail_as("tls", kind, problem, "could not make a secure connection: %s", detail);
    default:
        return fail_as("network", kind, problem, "the connection failed: %s", detail);
    }
}

// ---- HTTP ------------------------------------------------------------------

typedef struct {
    cplus_string* body;
    cplus_string* headers;
    int64_t max_bytes;
    bool too_large;
} exchange;

// Counts what the program gets, so a small compressed answer cannot grow into all of memory.
static size_t on_body(char* data, size_t size, size_t count, void* user) {
    exchange* x = user;
    size_t n = size * count;
    if ((int64_t)n > x->max_bytes - x->body->length) {
        x->too_large = true;
        return 0;
    }
    cplus_str_append(x->body, data, (int64_t)n);
    return n;
}

static bool is_blank(char c) { return c == ' ' || c == '\t'; }

// Keeps the headers of the last response as `name:value` lines, for http::__parseHeaders. libcurl calls this for each line,
// and for each response on the way: `HTTP/` starts another.
static size_t on_header(char* data, size_t size, size_t count, void* user) {
    exchange* x = user;
    size_t total = size * count, n = total;
    if (n >= 5 && memcmp(data, "HTTP/", 5) == 0) {
        x->headers->length = 0;
        return total;
    }
    if ((int64_t)n > x->max_bytes - x->headers->length) {
        x->too_large = true;
        return 0;
    }
    while (n > 0 && (data[n - 1] == '\r' || data[n - 1] == '\n' || is_blank(data[n - 1]))) n--;
    if (n == 0) return total;
    if (is_blank(data[0])) {  // a line that goes on from the one before, which HTTP has since forbidden
        if (x->headers->length == 0) return total;
        size_t start = 0;
        while (start < n && is_blank(data[start])) start++;
        x->headers->length--;  // its line break
        cplus_str_append(x->headers, " ", 1);
        cplus_str_append(x->headers, data + start, (int64_t)(n - start));
        cplus_str_append(x->headers, "\n", 1);
        return total;
    }
    const char* colon = memchr(data, ':', n);
    if (!colon) return total;
    size_t name = (size_t)(colon - data), start = name + 1;
    while (start < n && is_blank(data[start])) start++;
    cplus_str_append(x->headers, data, (int64_t)name + 1);
    cplus_str_append(x->headers, data + start, (int64_t)(n - start));
    cplus_str_append(x->headers, "\n", 1);
    return total;
}

static bool name_is(const char* name, size_t length, const char* wanted) {
    return strlen(wanted) == length && strncasecmp(name, wanted, length) == 0;
}

static struct curl_slist* add_header(struct curl_slist* list, const char* text) {
    struct curl_slist* longer = curl_slist_append(list, text);
    if (!longer) cplus_malloc((size_t)-1);  // reports that there is none
    return longer;
}

// The program's headers, `name: value` lines that http::__headerLines has already checked, as libcurl wants them.
static struct curl_slist* request_headers(const cplus_string* lines, bool sends_body, bool left_origin, bool body_gone) {
    struct curl_slist* list = NULL;
    bool has_type = false, has_expect = false;
    const char* at = lines->data;
    const char* end = lines->length > 0 ? at + lines->length : at;
    while (at < end) {
        const char* stop = memchr(at, '\r', (size_t)(end - at));
        size_t length = (size_t)(stop - at);
        size_t name = (size_t)((const char*)memchr(at, ':', length) - at);
        const char* next = stop + 2;
        has_type = has_type || name_is(at, name, "content-type");
        has_expect = has_expect || name_is(at, name, "expect");
        bool secret = name_is(at, name, "authorization") || name_is(at, name, "cookie") || name_is(at, name, "proxy-authorization") ||
                      name_is(at, name, "host");
        if ((left_origin && secret) || (body_gone && strncasecmp(at, "content-", name < 8 ? name : 8) == 0 && name >= 8)) {
            at = next;
            continue;
        }
        char* text = cplus_malloc(length + 1);
        memcpy(text, at, length);
        if (length == name + 2) {  // no value: libcurl would take `name:` as leaving that header out
            text[name] = ';';
            text[name + 1] = 0;
        } else {
            text[length] = 0;
        }
        list = add_header(list, text);
        free(text);
        at = next;
    }
    // libcurl would wait for a second before sending a large body, and would call any body a filled-in form.
    if (!has_expect) list = add_header(list, "Expect:");
    if (sends_body && !has_type) list = add_header(list, "Content-Type:");
    return list;
}

static bool is_http_url(const char* url) { return strncasecmp(url, "http://", 7) == 0 || strncasecmp(url, "https://", 8) == 0; }

static bool same_part(CURLU* a, CURLU* b, CURLUPart part) {
    char* x = NULL;
    char* y = NULL;
    bool same = curl_url_get(a, part, &x, CURLU_DEFAULT_PORT) == CURLUE_OK && curl_url_get(b, part, &y, CURLU_DEFAULT_PORT) == CURLUE_OK &&
                strcasecmp(x, y) == 0;
    curl_free(x);
    curl_free(y);
    return same;
}

// As a browser sees it: the scheme, the host and the port. What cannot be read counts as different.
static bool same_origin(const char* a, const char* b) {
    CURLU* x = curl_url();
    CURLU* y = curl_url();
    bool same = x && y && curl_url_set(x, CURLUPART_URL, a, 0) == CURLUE_OK && curl_url_set(y, CURLUPART_URL, b, 0) == CURLUE_OK &&
                same_part(x, y, CURLUPART_SCHEME) && same_part(x, y, CURLUPART_HOST) && same_part(x, y, CURLUPART_PORT);
    curl_url_cleanup(x);
    curl_url_cleanup(y);
    return same;
}

// As fetch does: a 303 makes anything a GET, and a 301 or 302 makes a POST one, which every browser has always done.
static bool redirect_becomes_get(long status, const char* method) {
    if (status == 303) return strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0;
    return (status == 301 || status == 302) && strcmp(method, "POST") == 0;
}

static bool is_followed_status(long status) { return status == 301 || status == 302 || status == 303 || status == 307 || status == 308; }

// Redirects are followed here, rather than by libcurl, since libcurl keeps a method it was told by name, so a PUT
// answered with a 303 would be sent again, to the place that was only meant to be read.
bool cplus_http_send(const cplus_string* method, const cplus_string* url, const cplus_string* headers, const cplus_string* body,
                     double timeout, int64_t max_bytes, bool follow, const cplus_string* ca_file, int32_t* status,
                     cplus_string* out_body, cplus_string* out_url, cplus_string* out_headers, cplus_string* kind, cplus_string* problem) {
    char* current = c_text(url);
    if (!current) return fail_as("bad-request", kind, problem, "this URL has a null byte in it");
    char* ca = c_text(ca_file);
    if (!ca) {
        free(current);
        return fail_as("bad-request", kind, problem, "caFile has a null byte in it");
    }
    char* verb = c_text(method);
    bool done = false;
    if (!verb) {
        fail_as("bad-request", kind, problem, "the method has a null byte in it");
    } else if (!is_http_url(current)) {
        fail_as("bad-request", kind, problem, "a URL for http must start with http:// or https://");
    } else {
        CURL* handle = thread_handle();
        exchange x = {out_body, out_headers, max_bytes, false};
        int64_t deadline = deadline_after(timeout);
        bool left_origin = false, body_gone = false;
        for (int redirects = 0;; redirects++) {
            char error_text[CURL_ERROR_SIZE] = "";
            out_body->length = 0;
            out_headers->length = 0;
            curl_easy_reset(handle);  // this keeps open connections
            set_shared_options(handle, error_text, ca);
            curl_easy_setopt(handle, CURLOPT_URL, current);
            curl_easy_setopt(handle, CURLOPT_USERAGENT, "cplus");
            curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "");  // whatever libcurl can decode
            curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, on_body);
            curl_easy_setopt(handle, CURLOPT_WRITEDATA, &x);
            curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, on_header);
            curl_easy_setopt(handle, CURLOPT_HEADERDATA, &x);
            if (deadline != NO_LIMIT) {
                int64_t left = deadline - now_ms();
                curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, left < 1 ? 1L : (long)left);
            }
            bool sends_body = !body_gone && (body->length > 0 || strcmp(verb, "POST") == 0 || strcmp(verb, "PUT") == 0 || strcmp(verb, "PATCH") == 0);
            if (strcmp(verb, "HEAD") == 0) {
                curl_easy_setopt(handle, CURLOPT_NOBODY, 1L);
            } else if (sends_body) {
                curl_easy_setopt(handle, CURLOPT_POST, 1L);
                curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body->length);
                curl_easy_setopt(handle, CURLOPT_POSTFIELDS, body->length > 0 ? body->data : "");  // without any, libcurl would read standard input
                curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, verb);
            } else {
                curl_easy_setopt(handle, CURLOPT_HTTPGET, 1L);
                curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, verb);
            }
            struct curl_slist* list = request_headers(headers, sends_body, left_origin, body_gone);
            curl_easy_setopt(handle, CURLOPT_HTTPHEADER, list);
            CURLcode rc = curl_easy_perform(handle);
            curl_easy_setopt(handle, CURLOPT_HTTPHEADER, NULL);
            curl_slist_free_all(list);
            if (x.too_large) {
                fail_as("too-large", kind, problem, "the response is larger than maxBytes, which is %lld bytes: raise maxBytes if you expected this", (long long)max_bytes);
                break;
            }
            if (rc == CURLE_URL_MALFORMAT && redirects > 0) {  // not the program's mistake, but the server's
                fail_as("redirect", kind, problem, "the server redirected this to a URL that is not valid");
                break;
            }
            if (rc != CURLE_OK) {
                curl_failure(rc, error_text, timeout, kind, problem);
                break;
            }
            long code = 0;
            char* next = NULL;
            curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &code);
            curl_easy_getinfo(handle, CURLINFO_REDIRECT_URL, &next);
            if (!follow || !next || !is_followed_status(code)) {
                char* final_url = NULL;
                curl_easy_getinfo(handle, CURLINFO_EFFECTIVE_URL, &final_url);
                if (final_url) cplus_str_append(out_url, final_url, (int64_t)strlen(final_url));
                *status = (int32_t)code;
                done = true;
                break;
            }
            if (redirects == MAX_REDIRECTS) {
                fail_as("redirect", kind, problem, "the server redirected this more than %d times", MAX_REDIRECTS);
                break;
            }
            if (!is_http_url(next)) {
                fail_as("redirect", kind, problem, "the server redirected this to something other than http or https");
                break;
            }
            left_origin = left_origin || !same_origin(current, next);
            if (redirect_becomes_get(code, verb)) {
                free(verb);
                verb = cplus_malloc(4);
                memcpy(verb, "GET", 4);
                body_gone = true;
            }
            char* copy = cplus_malloc(strlen(next) + 1);
            strcpy(copy, next);
            free(current);
            current = copy;
        }
        curl_easy_reset(handle);  // it stays open after this, and must not go on pointing at what is on this stack
    }
    free(verb);
    free(ca);
    free(current);
    return done;
}

// ---- WebSockets: RFC 6455 --------------------------------------------------

enum { OP_CONTINUE = 0, OP_TEXT = 1, OP_BINARY = 2, OP_CLOSE = 8, OP_PING = 9, OP_PONG = 10 };

// The close codes that say why a connection failed. 1006 never goes over the wire: it says there was no close at all.
enum { CLOSE_NORMAL = 1000, CLOSE_PROTOCOL = 1002, CLOSE_NO_CODE = 1005, CLOSE_ABNORMAL = 1006, CLOSE_BAD_TEXT = 1007, CLOSE_TOO_LARGE = 1009 };

// A class reference can be shared with another thread, and what a WebSocket is for is one thread waiting for messages while another
// sends them. libcurl must not see two calls on one handle at once. On a bare connection neither of its calls ever waits, so `io` is held
// only around each one, and waiting happens outside it. `reading` and `writing` keep whole messages and frames together, and are never
// held the other way around: a reader takes `writing` to answer a ping, and nothing that holds `writing` takes `reading`.
typedef struct {
    pthread_mutex_t io;       // `curl`, `closing`, `close_code` and `close_reason`
    pthread_mutex_t reading;  // what has come in, and the message being put together
    pthread_mutex_t writing;  // what goes out
    CURL* curl;               // null once the connection is gone
    bool closing;             // a close has begun, so whatever else is waiting stops
    curl_socket_t socket;
    int64_t max_bytes;
    unsigned char* in;  // what came in: in[used] to in[filled] has not been looked at
    size_t used, filled, capacity;
    char* message;  // the frames so far of a message, which can come in many
    size_t message_length, message_capacity;
    int message_opcode;  // of the first of them: 0 between messages
    int close_code;      // 0 while open
    char close_reason[124];
    size_t close_reason_length;
    char error_text[CURL_ERROR_SIZE];  // libcurl keeps writing here for as long as the connection lasts
} websocket;

typedef struct {
    bool fin;
    int opcode;
    const unsigned char* payload;
    size_t length;
} frame;

static void end_connection(websocket* ws) {
    pthread_mutex_lock(&ws->io);
    if (ws->curl) curl_easy_cleanup(ws->curl);
    ws->curl = NULL;
    pthread_mutex_unlock(&ws->io);
}

// Whether the connection has gone, or another thread has begun to close it.
static bool is_over(websocket* ws) {
    pthread_mutex_lock(&ws->io);
    bool over = !ws->curl || ws->closing;
    pthread_mutex_unlock(&ws->io);
    return over;
}

static void set_close_code(websocket* ws, int code) {
    pthread_mutex_lock(&ws->io);
    ws->close_code = code;
    pthread_mutex_unlock(&ws->io);
}

static void set_close_reason(websocket* ws, const unsigned char* reason, size_t length) {
    pthread_mutex_lock(&ws->io);
    memcpy(ws->close_reason, reason, length);
    ws->close_reason_length = length;
    pthread_mutex_unlock(&ws->io);
}

// `writing`, if it can be had soon, for a reply that must not leave the reader stuck behind a sender that is stuck itself.
static bool lock_writing_soon(websocket* ws) {
    for (int tries = 0; tries < REPLY_LOCK_TRIES; tries++) {
        if (pthread_mutex_trylock(&ws->writing) == 0) return true;
        usleep(1000);
    }
    return false;
}

// `closer` is for what closing or opening the connection does itself, and which does not stop for it.
// 1 when ready, 0 when the time ran out, -1 on failure or when the connection is over.
static int wait_for_socket(websocket* ws, short events, int64_t deadline, bool closer) {
    for (;;) {
        if (!closer && is_over(ws)) return -1;
        int wait = WAIT_SLICE_MS;
        if (deadline != NO_LIMIT) {
            int64_t left = deadline - now_ms();
            if (left <= 0) return 0;
            if (left < wait) wait = (int)left;
        }
        struct pollfd p = {ws->socket, events, 0};
        int got = poll(&p, 1, wait);
        if (got > 0) return 1;
        if (got < 0 && errno != EINTR) return -1;
    }
}

// 1 when all of it went, 0 when the time ran out, -1 on failure.
static int write_all(websocket* ws, const void* data, size_t length, int64_t deadline, bool closer) {
    const char* at = data;
    while (length > 0) {
        size_t sent = 0;
        pthread_mutex_lock(&ws->io);
        CURLcode rc = ws->curl ? curl_easy_send(ws->curl, at, length, &sent) : CURLE_SEND_ERROR;
        pthread_mutex_unlock(&ws->io);
        if (rc == CURLE_AGAIN) {
            int ready = wait_for_socket(ws, POLLOUT, deadline, closer);
            if (ready <= 0) return ready;
            continue;
        }
        if (rc != CURLE_OK) return -1;
        at += sent;
        length -= sent;
    }
    return 1;
}

// 1 when more came in, 0 when the time ran out, -1 on failure, -2 when the other side ended the connection.
static int read_more(websocket* ws, int64_t deadline, bool closer) {
    if (ws->used > 0) {
        memmove(ws->in, ws->in + ws->used, ws->filled - ws->used);
        ws->filled -= ws->used;
        ws->used = 0;
    }
    if (ws->capacity - ws->filled < 4096) {  // grows with what has come, never with what a frame says will
        size_t capacity = ws->capacity ? ws->capacity * 2 : 16384;
        ws->in = cplus_realloc(ws->in, capacity);
        ws->capacity = capacity;
    }
    for (;;) {
        size_t got = 0;
        pthread_mutex_lock(&ws->io);
        CURLcode rc = ws->curl ? curl_easy_recv(ws->curl, ws->in + ws->filled, ws->capacity - ws->filled, &got) : CURLE_RECV_ERROR;
        pthread_mutex_unlock(&ws->io);
        if (rc == CURLE_AGAIN) {
            int ready = wait_for_socket(ws, POLLIN, deadline, closer);
            if (ready <= 0) return ready;
            continue;
        }
        if (rc != CURLE_OK) return -1;
        if (got == 0) return -2;
        ws->filled += got;
        return 1;
    }
}

// What a client sends is masked, with a key the server cannot guess, for the sake of proxies that would otherwise take a
// frame for a request of their own.
static int send_frame(websocket* ws, int opcode, const void* payload, size_t length, int64_t deadline, bool closer) {
    unsigned char key[4];
    if (getentropy(key, sizeof key) != 0) return -1;
    size_t head = length < 126 ? 6 : length < 65536 ? 8 : 14;
    if (length > SIZE_MAX - head) return -1;
    unsigned char* out = cplus_malloc(head + length);
    out[0] = (unsigned char)(0x80 | opcode);
    if (length < 126) {
        out[1] = (unsigned char)(0x80 | length);
    } else if (length < 65536) {
        out[1] = 0x80 | 126;
        out[2] = (unsigned char)(length >> 8);
        out[3] = (unsigned char)length;
    } else {
        out[1] = 0x80 | 127;
        for (int i = 0; i < 8; i++) out[2 + i] = (unsigned char)((uint64_t)length >> (56 - 8 * i));
    }
    memcpy(out + head - 4, key, 4);
    const unsigned char* in = payload;
    for (size_t i = 0; i < length; i++) out[head + i] = in[i] ^ key[i & 3];
    int result = write_all(ws, out, head + length, deadline, closer);
    free(out);
    return result;
}

// Gives up on the connection, since the other side broke the protocol, telling it why if that can still be done.
static int32_t give_up(websocket* ws, int code, const char* what, const char* why, cplus_string* kind, cplus_string* problem) {
    unsigned char body[2] = {(unsigned char)(code >> 8), (unsigned char)code};
    if (lock_writing_soon(ws)) {
        send_frame(ws, OP_CLOSE, body, 2, now_ms() + 1000, false);
        pthread_mutex_unlock(&ws->writing);
    }
    set_close_code(ws, code);
    end_connection(ws);
    fail_as(what, kind, problem, "%s", why);
    return -1;
}

// 1 with a whole frame, whose payload stays where it is until read_more, 0 when more must come first, -1 when the
// frame breaks a rule, with why.
static int next_frame(websocket* ws, frame* f, int* code, const char** why) {
    const unsigned char* p = ws->in + ws->used;
    size_t have = ws->filled - ws->used;
    if (have < 2) return 0;
    *code = CLOSE_PROTOCOL;
    int opcode = p[0] & 0x0F;
    bool fin = (p[0] & 0x80) != 0;
    uint64_t length = p[1] & 0x7F;
    size_t head = 2;
    if (p[0] & 0x70) return *why = "the server sent a frame with reserved bits set, and none were agreed on", -1;
    if (p[1] & 0x80) return *why = "the server sent a masked frame, which only a client may do", -1;
    if (opcode >= 3 && opcode != OP_CLOSE && opcode != OP_PING && opcode != OP_PONG) return *why = "the server sent a frame of an unknown kind", -1;
    if (opcode >= 8 && (!fin || length > 125)) return *why = "the server sent a close, ping or pong in pieces, or longer than 125 bytes", -1;
    if (length == 126) {
        if (have < 4) return 0;
        length = ((uint64_t)p[2] << 8) | p[3];
        head = 4;
        if (length < 126) return *why = "the server wrote the length of a frame in more bytes than it takes", -1;
    } else if (length == 127) {
        if (have < 10) return 0;
        length = 0;
        for (int i = 0; i < 8; i++) length = (length << 8) | p[2 + i];
        head = 10;
        if (length >> 63) return *why = "the server sent a frame with an impossible length", -1;
        if (length < 65536) return *why = "the server wrote the length of a frame in more bytes than it takes", -1;
    }
    if (opcode < 8 && (ws->max_bytes < 0 || length > (uint64_t)ws->max_bytes || (int64_t)length > ws->max_bytes - (int64_t)ws->message_length)) {
        *code = CLOSE_TOO_LARGE;
        return *why = "the server sent a message larger than maxBytes: raise maxBytes if you expected this", -1;
    }
    if (have - head < length) return 0;
    f->fin = fin;
    f->opcode = opcode;
    f->payload = p + head;
    f->length = (size_t)length;
    ws->used += head + (size_t)length;
    return 1;
}

static bool is_utf8(const char* text, size_t length) {
    for (size_t i = 0; i < length;) {
        int n = cplus_utf8_length((const unsigned char*)text + i, (int64_t)(length - i));
        if (!n) return false;
        i += (size_t)n;
    }
    return true;
}

static void add_to_message(websocket* ws, const frame* f) {
    if (ws->message_capacity - ws->message_length < f->length) {
        size_t capacity = (ws->message_length + f->length) * 2;
        ws->message = cplus_realloc(ws->message, capacity);
        ws->message_capacity = capacity;
    }
    if (f->length > 0) memcpy(ws->message + ws->message_length, f->payload, f->length);
    ws->message_length += f->length;
}

static bool is_sendable_close_code(int code) { return (code >= 1000 && code <= 1003) || (code >= 1007 && code <= 1014) || (code >= 3000 && code <= 4999); }

static int32_t receive_message(websocket* ws, cplus_string* out, double timeout, cplus_string* kind, cplus_string* problem) {
    if (is_over(ws)) return 0;
    int64_t deadline = deadline_after(timeout);
    for (;;) {
        frame f;
        int code = 0;
        const char* why = NULL;
        int got = next_frame(ws, &f, &code, &why);
        if (got < 0) return give_up(ws, code, code == CLOSE_TOO_LARGE ? "too-large" : "protocol", why, kind, problem);
        if (got == 0) {
            int more = read_more(ws, deadline, false);
            if (more == 1) continue;
            if (more == 0) {  // what has come stays, for the next call
                fail_as("timeout", kind, problem, "nothing came in the %g seconds of this WebSocket's timeout", timeout);
                return -1;
            }
            if (is_over(ws)) return 0;  // another thread closed it, or is doing so
            set_close_code(ws, CLOSE_ABNORMAL);
            end_connection(ws);
            fail_as("network", kind, problem, more == -2 ? "the connection ended without a WebSocket close" : "the connection failed");
            return -1;
        }
        if (f.opcode == OP_PING) {
            int64_t reply_by = deadline != NO_LIMIT ? deadline : now_ms() + REPLY_WAIT_MS;
            int replied = 1;
            if (lock_writing_soon(ws)) {
                replied = send_frame(ws, OP_PONG, f.payload, f.length, reply_by, false);
                pthread_mutex_unlock(&ws->writing);
            }
            if (replied != 1) {
                if (is_over(ws)) return 0;
                set_close_code(ws, CLOSE_ABNORMAL);
                end_connection(ws);
                fail_as("network", kind, problem, "the connection failed while answering a ping");
                return -1;
            }
        } else if (f.opcode == OP_CLOSE) {
            int close_code = CLOSE_NO_CODE;
            if (f.length == 1) return give_up(ws, CLOSE_PROTOCOL, "protocol", "the server sent a close of one byte, which cannot hold a code", kind, problem);
            if (f.length >= 2) {
                close_code = (f.payload[0] << 8) | f.payload[1];
                if (!is_sendable_close_code(close_code)) return give_up(ws, CLOSE_PROTOCOL, "protocol", "the server sent a close with a code that is not allowed", kind, problem);
                if (!is_utf8((const char*)f.payload + 2, f.length - 2)) return give_up(ws, CLOSE_BAD_TEXT, "protocol", "the server sent a close whose reason is not valid UTF-8", kind, problem);
                set_close_reason(ws, f.payload + 2, f.length - 2);
            }
            if (lock_writing_soon(ws)) {
                send_frame(ws, OP_CLOSE, f.payload, f.length >= 2 ? 2 : 0, now_ms() + 1000, false);
                pthread_mutex_unlock(&ws->writing);
            }
            set_close_code(ws, close_code);
            end_connection(ws);
            return 0;
        } else if (f.opcode != OP_PONG) {
            if (f.opcode == OP_CONTINUE && !ws->message_opcode) return give_up(ws, CLOSE_PROTOCOL, "protocol", "the server continued a message it had not started", kind, problem);
            if (f.opcode != OP_CONTINUE && ws->message_opcode) return give_up(ws, CLOSE_PROTOCOL, "protocol", "the server started a message before ending the last one", kind, problem);
            if (f.opcode != OP_CONTINUE) ws->message_opcode = f.opcode;
            add_to_message(ws, &f);
            if (f.fin) {
                int opcode = ws->message_opcode;
                size_t length = ws->message_length;
                ws->message_opcode = 0;
                ws->message_length = 0;
                if (opcode == OP_TEXT && !is_utf8(ws->message, length)) return give_up(ws, CLOSE_BAD_TEXT, "protocol", "the server sent a text message that is not valid UTF-8", kind, problem);
                cplus_str_append(out, ws->message, (int64_t)length);
                return opcode == OP_TEXT ? 1 : 2;
            }
        }
        // So that a stream of pings or pieces cannot hold this call past its time.
        if (deadline != NO_LIMIT && now_ms() >= deadline) {
            fail_as("timeout", kind, problem, "no whole message came in the %g seconds of this WebSocket's timeout", timeout);
            return -1;
        }
    }
}

// 1 with a text message in `out`, 2 with a binary one, 0 when the connection closed as it should, -1 on failure.
int32_t cplus_ws_receive(websocket* ws, cplus_string* out, double timeout, cplus_string* kind, cplus_string* problem) {
    pthread_mutex_lock(&ws->reading);
    int32_t result = receive_message(ws, out, timeout, kind, problem);
    pthread_mutex_unlock(&ws->reading);
    return result;
}

bool cplus_ws_send(websocket* ws, const cplus_string* data, bool binary, double timeout, cplus_string* kind, cplus_string* problem) {
    if (is_over(ws)) return fail_as("closed", kind, problem, "this WebSocket is closed");
    if (!binary && !is_utf8(data->data, (size_t)data->length))
        return fail_as("bad-request", kind, problem, "this text is not valid UTF-8, which a text message must be: use sendBinary for other bytes");
    pthread_mutex_lock(&ws->writing);
    int sent = send_frame(ws, binary ? OP_BINARY : OP_TEXT, data->data, (size_t)data->length, deadline_after(timeout), false);
    pthread_mutex_unlock(&ws->writing);
    if (sent == 1) return true;
    if (is_over(ws)) return fail_as("closed", kind, problem, "this WebSocket is closed");
    // Part of the frame may have gone, so nothing else can be sent on this connection.
    set_close_code(ws, CLOSE_ABNORMAL);
    end_connection(ws);
    if (sent == 0) return fail_as("timeout", kind, problem, "sending took longer than this WebSocket's timeout of %g seconds", timeout);
    return fail_as("network", kind, problem, "the connection failed while sending");
}

int32_t cplus_ws_close_code(websocket* ws);

// Sends a close, then waits a little for the other side's, dropping what else comes. What another thread is waiting for stops, by
// seeing `closing`: a sender with an error, and a reader with null.
void cplus_ws_close(websocket* ws) {
    pthread_mutex_lock(&ws->io);
    bool first = ws->curl && !ws->closing;
    if (first) ws->closing = true;
    pthread_mutex_unlock(&ws->io);
    if (!first) return;
    int64_t deadline = now_ms() + CLOSE_WAIT_MS;
    unsigned char body[2] = {CLOSE_NORMAL >> 8, CLOSE_NORMAL & 0xFF};
    int code = CLOSE_ABNORMAL;
    pthread_mutex_lock(&ws->writing);
    int sent = send_frame(ws, OP_CLOSE, body, 2, deadline, true);
    pthread_mutex_unlock(&ws->writing);
    pthread_mutex_lock(&ws->reading);
    if (sent == 1) {
        for (;;) {
            frame f;
            int ignored = 0;
            const char* why = NULL;
            int got = next_frame(ws, &f, &ignored, &why);
            if (got < 0) break;
            if (got == 0) {
                if (read_more(ws, deadline, true) != 1) break;
                continue;
            }
            if (f.opcode == OP_CLOSE) {
                code = f.length >= 2 ? (f.payload[0] << 8) | f.payload[1] : CLOSE_NO_CODE;
                if (f.length > 2 && is_utf8((const char*)f.payload + 2, f.length - 2)) set_close_reason(ws, f.payload + 2, f.length - 2);
                break;
            }
            if (now_ms() >= deadline) break;
        }
    }
    // A thread that was reading may have got the other side's close by now, and with it the real code.
    if (cplus_ws_close_code(ws) == 0) set_close_code(ws, code);
    end_connection(ws);
    pthread_mutex_unlock(&ws->reading);
}

static void free_websocket(websocket* ws) {
    pthread_mutex_destroy(&ws->io);
    pthread_mutex_destroy(&ws->reading);
    pthread_mutex_destroy(&ws->writing);
    free(ws->in);
    free(ws->message);
    free(ws);
}

void cplus_ws_free(websocket* ws) {
    cplus_ws_close(ws);
    free_websocket(ws);
}

int32_t cplus_ws_close_code(websocket* ws) {
    pthread_mutex_lock(&ws->io);
    int32_t code = ws->close_code;
    pthread_mutex_unlock(&ws->io);
    return code;
}

void cplus_ws_close_reason(websocket* ws, cplus_string* out) {
    pthread_mutex_lock(&ws->io);
    cplus_str_append(out, ws->close_reason, (int64_t)ws->close_reason_length);
    pthread_mutex_unlock(&ws->io);
}

static void base64(const unsigned char* in, size_t length, char* out) {
    static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0;
    for (; i + 3 <= length; i += 3) {
        unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8) | in[i + 2];
        *out++ = digits[v >> 18];
        *out++ = digits[(v >> 12) & 63];
        *out++ = digits[(v >> 6) & 63];
        *out++ = digits[v & 63];
    }
    if (length - i == 1) {
        unsigned v = (unsigned)in[i] << 16;
        *out++ = digits[v >> 18];
        *out++ = digits[(v >> 12) & 63];
        *out++ = '=';
        *out++ = '=';
    } else if (length - i == 2) {
        unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8);
        *out++ = digits[v >> 18];
        *out++ = digits[(v >> 12) & 63];
        *out++ = digits[(v >> 6) & 63];
        *out++ = '=';
    }
    *out = 0;
}

static uint32_t rotate_left(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static void sha1_block(uint32_t h[5], const unsigned char* block) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) | ((uint32_t)block[i * 4 + 2] << 8) | block[i * 4 + 3];
    for (int i = 16; i < 80; i++) w[i] = rotate_left(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f = i < 20 ? (b & c) | (~b & d) : i < 40 ? b ^ c ^ d : i < 60 ? (b & c) | (b & d) | (c & d) : b ^ c ^ d;
        uint32_t k = i < 20 ? 0x5A827999 : i < 40 ? 0x6ED9EBA1 : i < 60 ? 0x8F1BBCDC : 0xCA62C1D6;
        uint32_t t = rotate_left(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotate_left(b, 30);
        b = a;
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

// SHA-1 is what the protocol asks for here. It shows the server understood the request, and nothing relies on it for security.
static void sha1(const unsigned char* data, size_t length, unsigned char out[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    size_t i = 0;
    for (; i + 64 <= length; i += 64) sha1_block(h, data + i);
    unsigned char last[128] = {0};
    size_t rest = length - i;
    memcpy(last, data + i, rest);
    last[rest] = 0x80;
    size_t end = rest < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)length * 8;
    for (int k = 0; k < 8; k++) last[end - 1 - (size_t)k] = (unsigned char)(bits >> (8 * k));
    sha1_block(h, last);
    if (end == 128) sha1_block(h, last + 64);
    for (int k = 0; k < 5; k++)
        for (int j = 0; j < 4; j++) out[k * 4 + j] = (unsigned char)(h[k] >> (24 - 8 * j));
}

// In `name: value` lines that end in \r\n.
static bool find_header(const char* lines, size_t length, const char* wanted, const char** value, size_t* value_length) {
    const char* at = lines;
    const char* end = lines + length;
    size_t wanted_length = strlen(wanted);
    while (at < end) {
        const char* stop = memmem(at, (size_t)(end - at), "\r\n", 2);
        if (!stop) stop = end;
        const char* colon = memchr(at, ':', (size_t)(stop - at));
        if (colon && (size_t)(colon - at) == wanted_length && strncasecmp(at, wanted, wanted_length) == 0) {
            const char* a = colon + 1;
            const char* b = stop;
            while (a < b && is_blank(*a)) a++;
            while (b > a && is_blank(b[-1])) b--;
            *value = a;
            *value_length = (size_t)(b - a);
            return true;
        }
        at = stop + 2;
    }
    return false;
}

// Whether a list with commas between its items has this one in it.
static bool has_item(const char* list, size_t length, const char* item, size_t item_length) {
    const char* at = list;
    const char* end = list + length;
    while (at < end) {
        const char* comma = memchr(at, ',', (size_t)(end - at));
        const char* a = at;
        const char* b = comma ? comma : end;
        while (a < b && is_blank(*a)) a++;
        while (b > a && is_blank(b[-1])) b--;
        if ((size_t)(b - a) == item_length && strncasecmp(a, item, item_length) == 0) return true;
        at = comma ? comma + 1 : end;
    }
    return false;
}

// Whether any of the lines of this header has the item, as a header that comes twice is the one list, in two parts.
static bool header_has_item(const char* lines, size_t length, const char* wanted, const char* item) {
    const char* at = lines;
    const char* end = lines + length;
    while (at < end) {
        const char* stop = memmem(at, (size_t)(end - at), "\r\n", 2);
        if (!stop) stop = end;
        const char* value;
        size_t value_length;
        if (find_header(at, (size_t)(stop - at), wanted, &value, &value_length) && has_item(value, value_length, item, strlen(item))) return true;
        at = stop + 2;
    }
    return false;
}

// Reads the server's answer up to where frames start, which may already be there, and checks everything it has to say.
static bool read_handshake(websocket* ws, const char* accept, const cplus_string* asked, int64_t deadline, double timeout, cplus_string* kind, cplus_string* problem) {
    const char* blank;
    for (;;) {
        size_t have = ws->filled - ws->used;
        blank = have ? memmem(ws->in + ws->used, have, "\r\n\r\n", 4) : NULL;
        if (blank) break;
        if (have > MAX_HANDSHAKE) return fail_as("handshake", kind, problem, "the server's answer to opening a WebSocket is too long");
        int more = read_more(ws, deadline, true);
        if (more == 0) return fail_as("timeout", kind, problem, "this took longer than its timeout of %g seconds", timeout);
        if (more < 0) return fail_as("handshake", kind, problem, "the server ended the connection instead of opening a WebSocket");
    }
    const char* text = (const char*)ws->in + ws->used;
    size_t length = (size_t)(blank - text) + 2;  // keeps the last line's own end
    ws->used += (size_t)(blank - text) + 4;
    if (length < 14 || strncmp(text, "HTTP/1.", 7) != 0 || text[8] != ' ')
        return fail_as("handshake", kind, problem, "the server's answer to opening a WebSocket is not HTTP");
    int status = (text[9] - '0') * 100 + (text[10] - '0') * 10 + (text[11] - '0');
    if (status != 101) return fail_as("handshake", kind, problem, "the server answered with status %d instead of opening a WebSocket", status);
    const char* headers = memmem(text, length, "\r\n", 2) + 2;
    size_t headers_length = length - (size_t)(headers - text);
    const char* value;
    size_t value_length;
    if (!header_has_item(headers, headers_length, "upgrade", "websocket") || !header_has_item(headers, headers_length, "connection", "upgrade"))
        return fail_as("handshake", kind, problem, "the server answered with status 101, but not with an upgrade to a WebSocket");
    if (!find_header(headers, headers_length, "sec-websocket-accept", &value, &value_length) || value_length != strlen(accept) || memcmp(value, accept, value_length) != 0)
        return fail_as("handshake", kind, problem, "the server's Sec-WebSocket-Accept does not match what was sent, so it may not understand WebSockets");
    if (find_header(headers, headers_length, "sec-websocket-extensions", &value, &value_length))
        return fail_as("handshake", kind, problem, "the server chose an extension, and none were offered");
    if (find_header(headers, headers_length, "sec-websocket-protocol", &value, &value_length)) {
        const char* offered;
        size_t offered_length;
        if (!asked->length || !find_header(asked->data, (size_t)asked->length, "sec-websocket-protocol", &offered, &offered_length) ||
            !has_item(offered, offered_length, value, value_length))
            return fail_as("handshake", kind, problem, "the server chose a subprotocol that was not offered");
    }
    return true;
}

static char* url_part(CURLU* url, CURLUPart part) {
    char* text = NULL;
    return curl_url_get(url, part, &text, 0) == CURLUE_OK ? text : NULL;
}

// The request that opens a WebSocket, of `url` with its scheme made http or https.
static char* handshake_request(CURLU* url, const char* key, const cplus_string* headers) {
    char* path = url_part(url, CURLUPART_PATH);
    char* query = url_part(url, CURLUPART_QUERY);
    char* host = url_part(url, CURLUPART_HOST);
    char* port = url_part(url, CURLUPART_PORT);
    const char* ignored;
    size_t ignored_length;
    bool has_agent = headers->length && find_header(headers->data, (size_t)headers->length, "user-agent", &ignored, &ignored_length);
    size_t room = strlen(path ? path : "/") + (query ? strlen(query) : 0) + (host ? strlen(host) : 0) + (port ? strlen(port) : 0) + strlen(key) + (size_t)headers->length + 256;
    char* text = cplus_malloc(room);
    int n = snprintf(text, room,
                     "GET %s%s%s HTTP/1.1\r\nHost: %s%s%s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n%s",
                     path ? path : "/", query ? "?" : "", query ? query : "", host ? host : "", port ? ":" : "", port ? port : "", key,
                     has_agent ? "" : "User-Agent: cplus\r\n");
    if (headers->length > 0) memcpy(text + n, headers->data, (size_t)headers->length);
    memcpy(text + n + headers->length, "\r\n", 3);
    curl_free(path);
    curl_free(query);
    curl_free(host);
    curl_free(port);
    return text;
}

static void connect_error(CURLUcode rc, cplus_string* kind, cplus_string* problem) {
    fail_as("bad-request", kind, problem, "this URL is not valid: %s", curl_url_strerror(rc));
}

// Null on failure.
websocket* cplus_ws_connect(const cplus_string* url, const cplus_string* headers, double timeout, int64_t max_bytes, const cplus_string* ca_file,
                            cplus_string* kind, cplus_string* problem) {
    pthread_once(&curl_once, start_curl);
    char* text = c_text(url);
    if (!text) return fail_as("bad-request", kind, problem, "this URL has a null byte in it"), NULL;
    bool secure = strncasecmp(text, "wss://", 6) == 0;
    if (!secure && strncasecmp(text, "ws://", 5) != 0) {
        free(text);
        return fail_as("bad-request", kind, problem, "a URL for a WebSocket must start with ws:// or wss://"), NULL;
    }
    const char* rest = text + (secure ? 6 : 5);
    char* real = cplus_malloc(strlen(rest) + 9);
    strcpy(real, secure ? "https://" : "http://");
    strcat(real, rest);
    free(text);

    char* ca = c_text(ca_file);
    CURLU* parsed = curl_url();
    websocket* ws = NULL;
    bool opened = false;
    if (!ca) {
        fail_as("bad-request", kind, problem, "caFile has a null byte in it");
    } else if (!parsed) {
        fail_as("network", kind, problem, "libcurl could not start");
    } else {
        CURLUcode url_rc = curl_url_set(parsed, CURLUPART_URL, real, 0);
        char* user = url_rc == CURLUE_OK ? url_part(parsed, CURLUPART_USER) : NULL;
        if (url_rc != CURLUE_OK) {
            connect_error(url_rc, kind, problem);
        } else if (user) {
            fail_as("bad-request", kind, problem, "a URL for a WebSocket cannot hold a name and password, which nothing would send: use an Authorization header");
        } else {
            ws = cplus_malloc(sizeof *ws);
            memset(ws, 0, sizeof *ws);
            pthread_mutex_init(&ws->io, NULL);
            pthread_mutex_init(&ws->reading, NULL);
            pthread_mutex_init(&ws->writing, NULL);
            ws->max_bytes = max_bytes;
            ws->curl = curl_easy_init();
            int64_t deadline = deadline_after(timeout);
            if (!ws->curl) {
                fail_as("network", kind, problem, "libcurl could not start");
            } else {
                set_shared_options(ws->curl, ws->error_text, ca);
                curl_easy_setopt(ws->curl, CURLOPT_URL, real);
                curl_easy_setopt(ws->curl, CURLOPT_CONNECT_ONLY, 1L);
                curl_easy_setopt(ws->curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
                curl_easy_setopt(ws->curl, CURLOPT_SSL_ENABLE_ALPN, 0L);  // what comes after TLS is HTTP/1.1, whatever else the server knows
                curl_easy_setopt(ws->curl, CURLOPT_HTTPPROXYTUNNEL, 1L);  // what comes after is not HTTP a proxy could read
                curl_easy_setopt(ws->curl, CURLOPT_TCP_KEEPALIVE, 1L);    // finds out, if slowly, that a silent server has gone
                curl_easy_setopt(ws->curl, CURLOPT_TCP_KEEPIDLE, 60L);
                curl_easy_setopt(ws->curl, CURLOPT_TCP_KEEPINTVL, 60L);
                if (deadline != NO_LIMIT) {
                    int64_t left = deadline - now_ms();
                    curl_easy_setopt(ws->curl, CURLOPT_TIMEOUT_MS, left < 1 ? 1L : (long)left);
                }
                CURLcode rc = curl_easy_perform(ws->curl);
                if (rc != CURLE_OK) {
                    curl_failure(rc, ws->error_text, timeout, kind, problem);
                } else if (curl_easy_getinfo(ws->curl, CURLINFO_ACTIVESOCKET, &ws->socket) != CURLE_OK || ws->socket == CURL_SOCKET_BAD) {
                    fail_as("network", kind, problem, "libcurl did not leave a connection to use");
                } else {
                    unsigned char random[16];
                    char key[32], accept[32];
                    if (getentropy(random, sizeof random) != 0) {
                        fail_as("network", kind, problem, "the system gave no random numbers, which opening a WebSocket needs");
                    } else {
                        base64(random, sizeof random, key);
                        char joined[128];
                        snprintf(joined, sizeof joined, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);  // the protocol's own constant
                        unsigned char digest[20];
                        sha1((const unsigned char*)joined, strlen(joined), digest);
                        base64(digest, sizeof digest, accept);
                        char* request = handshake_request(parsed, key, headers);
                        int wrote = write_all(ws, request, strlen(request), deadline, true);
                        free(request);
                        if (wrote == 0) fail_as("timeout", kind, problem, "this took longer than its timeout of %g seconds", timeout);
                        else if (wrote < 0) fail_as("network", kind, problem, "the connection failed while opening a WebSocket");
                        else opened = read_handshake(ws, accept, headers, deadline, timeout, kind, problem);
                    }
                }
            }
        }
        curl_free(user);
    }
    free(real);
    free(ca);
    curl_url_cleanup(parsed);
    if (!opened && ws) {
        end_connection(ws);
        free_websocket(ws);
        ws = NULL;
    }
    return ws;
}
