#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

// Portable request construction and retry policy over the compat HTTP layer.
// Nothing here is platform-aware; see src/compat/chttp.h for that.

#include <stdbool.h>

#include "../compat/chttp.h"
#include "io_util.h"

typedef struct HttpClient HttpClient;

// `api_key` may be NULL, in which case requests identify with the worker UUID.
// Neither string is retained; both are copied.
HttpClient *http_client_create(const char *base_url, const char *api_key,
                               const char *worker_uuid);
void http_client_destroy(HttpClient *client);

// `path` is appended to the base URL. `response` must be destroyed by the
// caller with chttp_response_destroy on success.
//
// Retries are applied uniformly: 429 honours Retry-After up to 5 times, 5xx and
// transport failures back off exponentially, and other 4xx are returned to the
// caller untouched. A 2xx or an unretryable status is not an error -- callers
// decide what a given status means for them.
void http_client_get(HttpClient *client, const char *path,
                     ChttpResponse *response, ErrorStack *error_stack);
void http_client_post_json(HttpClient *client, const char *path,
                           const char *body, ChttpResponse *response,
                           ErrorStack *error_stack);

#endif
