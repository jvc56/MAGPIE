#include "http_client.h"

#include "../compat/chttp.h"
#include "../compat/ctime.h"
#include "io_util.h"
#include "string_util.h"
#include <stdlib.h>
#include <string.h>

enum {
  MAX_RATE_LIMIT_RETRIES = 5,
  REQUEST_TIMEOUT_SECONDS = 120,
  MAX_HEADERS = 4,
};

// How long a request rides out a server that is not answering: a transport
// failure or a 5xx is retried HTTP_CLIENT_MAX_TRANSIENT_RETRIES times, waiting
// 1, 2, 4, ... seconds and never more than HTTP_CLIENT_MAX_BACKOFF_SECONDS
// between attempts -- about fifteen minutes in all.
//
// It was five retries, 31 seconds, and giving up ends the contribute run. A
// routine birdtest deployment is longer than that: the service is a single
// instance whose old task stops before the new one starts, so every deploy is
// a minute or more of connection failures and 503s from the load balancer.
// Every contributor that asked for a task in that window stopped contributing
// until a person noticed and started it again, and one that was *submitting*
// lost the finished task as well. The budget is sized to outlast a deployment,
// a database failover or a short maintenance window with room to spare; a
// server that is really gone still ends the run, a quarter of an hour later.
int http_client_backoff_seconds(int retry_idx) {
  int seconds = 1;
  for (int doubling_idx = 0;
       doubling_idx < retry_idx && seconds < HTTP_CLIENT_MAX_BACKOFF_SECONDS;
       doubling_idx++) {
    seconds *= 2;
  }
  return seconds < HTTP_CLIENT_MAX_BACKOFF_SECONDS
             ? seconds
             : HTTP_CLIENT_MAX_BACKOFF_SECONDS;
}

struct HttpClient {
  char *base_url;
  char *api_key;
  char *worker_uuid;
  // Rebuilt whenever the identity changes (see http_client_set_worker_uuid).
  // NULL when neither an API key nor a worker UUID is known yet, in which
  // case a request carries no identity header at all.
  char *auth_header;
};

static void rebuild_auth_header(HttpClient *client) {
  free(client->auth_header);
  // An API key attributes work to an account; without one the server tracks
  // the anonymous UUID. Exactly one of the two is sent, never both. A worker
  // that has neither yet -- it has never contributed before, and the server
  // rather than the client assigns the UUID -- sends no identity header, and
  // the server hands one back with the first task (see
  // http_client_set_worker_uuid).
  if (client->api_key) {
    client->auth_header =
        get_formatted_string("Authorization: Bearer %s", client->api_key);
  } else if (client->worker_uuid) {
    client->auth_header =
        get_formatted_string("X-Worker-UUID: %s", client->worker_uuid);
  } else {
    client->auth_header = NULL;
  }
}

HttpClient *http_client_create(const char *base_url, const char *api_key,
                               const char *worker_uuid) {
  HttpClient *client = (HttpClient *)malloc_or_die(sizeof(HttpClient));
  client->base_url = string_duplicate(base_url);
  // Trailing slashes would produce a double slash when joined with a path.
  size_t length = string_length(client->base_url);
  while (length > 0 && client->base_url[length - 1] == '/') {
    client->base_url[--length] = '\0';
  }
  client->api_key = api_key ? string_duplicate(api_key) : NULL;
  client->worker_uuid = worker_uuid ? string_duplicate(worker_uuid) : NULL;
  client->auth_header = NULL;
  rebuild_auth_header(client);
  return client;
}

void http_client_set_worker_uuid(HttpClient *client, const char *worker_uuid) {
  free(client->worker_uuid);
  client->worker_uuid = string_duplicate(worker_uuid);
  rebuild_auth_header(client);
}

void http_client_destroy(HttpClient *client) {
  if (!client) {
    return;
  }
  free(client->base_url);
  free(client->api_key);
  free(client->worker_uuid);
  free(client->auth_header);
  free(client);
}

static void perform(HttpClient *client, chttp_method_t method, const char *path,
                    const char *body, int max_transient_retries,
                    ChttpResponse *response, ErrorStack *error_stack) {
  char *url = get_formatted_string("%s%s", client->base_url, path);

  const char *headers[MAX_HEADERS];
  int num_headers = 0;
  if (client->auth_header) {
    headers[num_headers++] = client->auth_header;
  }
  if (body) {
    headers[num_headers++] = "Content-Type: application/json";
  }

  ChttpRequest request;
  request.method = method;
  request.url = url;
  request.headers = headers;
  request.num_headers = num_headers;
  request.body = body;
  request.body_length = body ? string_length(body) : 0;
  request.timeout_seconds = REQUEST_TIMEOUT_SECONDS;

  int rate_limit_retries = 0;
  int transient_retries = 0;

  while (true) {
    ErrorStack *attempt_errors = error_stack_create();
    chttp_request(&request, response, attempt_errors);

    if (!error_stack_is_empty(attempt_errors)) {
      // A transport failure -- DNS, connection refused, timeout. A
      // contributor's link is not necessarily stable, and neither is the
      // server's: see http_client_backoff_seconds.
      if (transient_retries < max_transient_retries) {
        error_stack_destroy(attempt_errors);
        ctime_nap(http_client_backoff_seconds(transient_retries));
        transient_retries++;
        continue;
      }
      char *message = error_stack_get_string_and_reset(attempt_errors);
      error_stack_destroy(attempt_errors);
      error_stack_push(error_stack, ERROR_STATUS_HTTP_REQUEST_FAILED, message);
      free(url);
      return;
    }
    error_stack_destroy(attempt_errors);

    // The worker endpoints are rate limited per identity and a task costs at
    // least two requests, so 429 is reached in normal operation. It is
    // backoff, not an error.
    if (response->status_code == 429 &&
        rate_limit_retries < MAX_RATE_LIMIT_RETRIES) {
      rate_limit_retries++;
      const int wait =
          response->retry_after_seconds > 0 ? response->retry_after_seconds : 1;
      chttp_response_destroy(response);
      ctime_nap(wait);
      continue;
    }

    if (response->status_code >= 500 &&
        transient_retries < max_transient_retries) {
      chttp_response_destroy(response);
      ctime_nap(http_client_backoff_seconds(transient_retries));
      transient_retries++;
      continue;
    }

    free(url);
    return;
  }
}

void http_client_get(HttpClient *client, const char *path,
                     ChttpResponse *response, ErrorStack *error_stack) {
  perform(client, CHTTP_GET, path, NULL, HTTP_CLIENT_MAX_TRANSIENT_RETRIES,
          response, error_stack);
}

void http_client_post_json(HttpClient *client, const char *path,
                           const char *body, ChttpResponse *response,
                           ErrorStack *error_stack) {
  perform(client, CHTTP_POST, path, body ? body : "{}",
          HTTP_CLIENT_MAX_TRANSIENT_RETRIES, response, error_stack);
}

void http_client_post_json_once(HttpClient *client, const char *path,
                                const char *body, ChttpResponse *response,
                                ErrorStack *error_stack) {
  perform(client, CHTTP_POST, path, body ? body : "{}",
          /*max_transient_retries=*/0, response, error_stack);
}
