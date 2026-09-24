#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

// Portable request construction and retry policy over the compat HTTP layer.
// Nothing here is platform-aware; see src/compat/chttp.h for that.

#include "../compat/chttp.h"
#include "../def/contribute_defs.h"
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
// transport failures back off exponentially (see http_client_backoff_seconds),
// and other 4xx are returned to the caller untouched. A 2xx or an unretryable
// status is not an error -- callers decide what a given status means for them.
void http_client_get(HttpClient *client, const char *path,
                     ChttpResponse *response, ErrorStack *error_stack);
void http_client_post_json(HttpClient *client, const char *path,
                           const char *body, ChttpResponse *response,
                           ErrorStack *error_stack);

// As http_client_post_json, but a transport failure or a 5xx is retried for as
// long as it lasts, at the back-off's ceiling once it gets there. For the one
// request that loses nothing by waiting: a task claim. A submission keeps the
// finite budget, because the claim it answers lapses on the server anyway.
void http_client_post_json_persistent(HttpClient *client, const char *path,
                                      const char *body, ChttpResponse *response,
                                      ErrorStack *error_stack);

// Called before each wait of a transient retry, with the number of the retry
// (from 0), the seconds about to be waited, and `context`. Optional; NULL
// clears it. The client itself prints nothing: it has no terminal to print to,
// and a caller that retries for minutes owes its user a line saying so.
typedef void (*http_client_retry_listener_t)(void *context, int retry_idx,
                                             int wait_seconds);
void http_client_set_retry_listener(HttpClient *client,
                                    http_client_retry_listener_t listener,
                                    void *context);

// One attempt, with no retry of a transport failure or a 5xx. For a request
// whose own schedule is the retry: the heartbeat goes out every thirty seconds
// whatever happened to the last one, and a heartbeat that spent minutes backing
// off would hold up the task's submission, which waits for the heartbeat
// thread to stop.
void http_client_post_json_once(HttpClient *client, const char *path,
                                const char *body, ChttpResponse *response,
                                ErrorStack *error_stack);

// Seconds to wait before retry number `retry_idx` (from 0) of a transport
// failure or a 5xx: 1, 2, 4, ... and never more than
// HTTP_CLIENT_MAX_BACKOFF_SECONDS. The budget itself is in
// src/def/contribute_defs.h.
int http_client_backoff_seconds(int retry_idx);

#endif
