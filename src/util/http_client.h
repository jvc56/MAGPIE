#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

// Portable request construction and retry policy over the compat HTTP layer.
// Nothing here is platform-aware; see src/compat/chttp.h for that.

#include "../compat/chttp.h"
#include "io_util.h"
#include <stdbool.h>

typedef struct HttpClient HttpClient;

// `api_key` and `worker_uuid` may each be NULL. Neither string is retained;
// both are copied. If both are NULL, requests carry no identity header at
// all -- the normal state for a worker that has never contributed before, and
// the server assigns a UUID in response to its first claimed task (see
// http_client_set_worker_uuid).
HttpClient *http_client_create(const char *base_url, const char *api_key,
                               const char *worker_uuid);
void http_client_destroy(HttpClient *client);

// Records a worker UUID the server assigned mid-run so every later request
// identifies with it. No-op on a client with an API key, which never
// identifies by UUID.
void http_client_set_worker_uuid(HttpClient *client, const char *worker_uuid);

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
