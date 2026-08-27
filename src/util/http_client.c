#include "http_client.h"

#include <stdlib.h>
#include <string.h>

#include "../compat/csleep.h"
#include "string_util.h"

enum {
  MAX_RATE_LIMIT_RETRIES = 5,
  MAX_TRANSIENT_RETRIES = 5,
  REQUEST_TIMEOUT_SECONDS = 120,
  MAX_HEADERS = 4,
};

struct HttpClient {
  char *base_url;
  char *api_key;
  char *worker_uuid;
  // Built once; every request carries the same identity.
  char *auth_header;
};

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

  // An API key attributes work to an account; without one the server tracks
  // the anonymous UUID. Exactly one of the two is sent, never both.
  if (client->api_key) {
    client->auth_header =
        get_formatted_string("Authorization: Bearer %s", client->api_key);
  } else {
    client->auth_header =
        get_formatted_string("X-Worker-UUID: %s", client->worker_uuid);
  }
  return client;
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
                    const char *body, ChttpResponse *response,
                    ErrorStack *error_stack) {
  char *url = get_formatted_string("%s%s", client->base_url, path);

  const char *headers[MAX_HEADERS];
  int num_headers = 0;
  headers[num_headers++] = client->auth_header;
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
  int backoff_seconds = 1;

  while (true) {
    ErrorStack *attempt_errors = error_stack_create();
    chttp_request(&request, response, attempt_errors);

    if (!error_stack_is_empty(attempt_errors)) {
      // A transport failure -- DNS, connection refused, timeout. Worth
      // retrying a few times before giving up, since a contributor's link is
      // not necessarily stable.
      if (transient_retries < MAX_TRANSIENT_RETRIES) {
        transient_retries++;
        error_stack_destroy(attempt_errors);
        csleep_seconds(backoff_seconds);
        backoff_seconds *= 2;
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
      const int wait = response->retry_after_seconds > 0
                           ? response->retry_after_seconds
                           : 1;
      chttp_response_destroy(response);
      csleep_seconds(wait);
      continue;
    }

    if (response->status_code >= 500 &&
        transient_retries < MAX_TRANSIENT_RETRIES) {
      transient_retries++;
      chttp_response_destroy(response);
      csleep_seconds(backoff_seconds);
      backoff_seconds *= 2;
      continue;
    }

    free(url);
    return;
  }
}

void http_client_get(HttpClient *client, const char *path,
                     ChttpResponse *response, ErrorStack *error_stack) {
  perform(client, CHTTP_GET, path, NULL, response, error_stack);
}

void http_client_post_json(HttpClient *client, const char *path,
                           const char *body, ChttpResponse *response,
                           ErrorStack *error_stack) {
  perform(client, CHTTP_POST, path, body ? body : "{}", response, error_stack);
}
