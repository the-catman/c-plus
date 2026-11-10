#ifndef CPLUS_HTTP_H
#define CPLUS_HTTP_H

// One request, with its redirects, which http.c and fetch.c share: the first sends it and waits, the second sends it
// on the thread's event loop. Each round is one request on the wire. Only a file that has libcurl includes this.

#define CURL_DISABLE_DEPRECATION
#include <curl/curl.h>

#include "internal.h"

typedef struct {
    cplus_string* body;
    cplus_string* headers;
    int64_t max_bytes;
    bool too_large;
} http_exchange;

typedef struct {
    char* current;  // the URL of this round
    char* verb;
    char* ca;
    const cplus_string* headers;
    const cplus_string* body;  // libcurl reads it while a round runs, so it must stay as it is until the request is over
    double timeout;
    int64_t deadline;
    bool follow;
    bool left_origin;
    bool body_gone;
    int redirects;
    http_exchange x;
    struct curl_slist* list;
    char error_text[CURL_ERROR_SIZE];
    int32_t* status;
    cplus_string* out_url;
    cplus_string* kind;
    cplus_string* problem;
} http_request;

// False if the request cannot be sent at all, as `kind` and `problem` then say. Either way http_request_end frees it.
bool http_request_begin(http_request* r, const cplus_string* method, const cplus_string* url, const cplus_string* headers, const cplus_string* body,
                        double timeout, int64_t max_bytes, bool follow, const cplus_string* ca_file, int32_t* status, cplus_string* out_body,
                        cplus_string* out_url, cplus_string* out_headers, cplus_string* kind, cplus_string* problem);
// Before each round. It resets the handle, which keeps its open connections.
void http_request_setup(http_request* r, CURL* handle);
// After each round: what is next.
enum { HTTP_ANSWERED, HTTP_FAILED, HTTP_REDIRECTED };
int http_request_after(http_request* r, CURL* handle, CURLcode rc);
void http_request_end(http_request* r);

#endif
