#ifndef COMPAT_CHTTP_H
#define COMPAT_CHTTP_H

// The only platform-aware HTTP surface in MAGPIE. Everything above this --
// header construction, authentication, retry policy, JSON -- is portable and
// lives in src/util/http_client.
//
// Backends:
//   POSIX (Linux, BSD, macOS)  libcurl, resolved at runtime with dlopen
//   Windows                    WinHTTP, which ships with the OS
//   wasm                       a stub that reports the feature is unavailable
//
// libcurl is what a POSIX system provides: Linux has no OS-level HTTP API, and
// macOS ships libcurl as well, so one implementation covers both. Resolving it
// at runtime rather than linking it means a machine without libcurl still runs
// every offline MAGPIE command and only `contribute` reports the problem.

#include <stdbool.h>
#include <stddef.h>

#include "../util/io_util.h"

typedef enum {
  CHTTP_GET,
  CHTTP_POST,
} chttp_method_t;

typedef struct ChttpRequest {
  chttp_method_t method;
  const char *url;
  // "Name: value" strings.
  const char *const *headers;
  int num_headers;
  // NULL for GET. Not necessarily NUL-terminated safe to assume; body_length
  // is authoritative.
  const char *body;
  size_t body_length;
  int timeout_seconds;
} ChttpRequest;

typedef struct ChttpResponse {
  long status_code;
  // Always NUL-terminated for the convenience of JSON callers, but
  // body_length is authoritative: artifact downloads are binary and may
  // contain embedded zero bytes.
  char *body;
  size_t body_length;
  // Parsed from the Retry-After header; -1 when absent.
  int retry_after_seconds;
} ChttpResponse;

// Performs the request. On success `response` is populated and the caller must
// call chttp_response_destroy. On failure the error stack is pushed to and
// `response` is left zeroed.
//
// Certificate verification is always on and cannot be disabled.
void chttp_request(const ChttpRequest *request, ChttpResponse *response,
                   ErrorStack *error_stack);

void chttp_response_destroy(ChttpResponse *response);

// True when this build can make HTTP requests at all.
bool chttp_is_available(void);

#endif
