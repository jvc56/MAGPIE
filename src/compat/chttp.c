#include "chttp.h"

#include <stdlib.h>
#include <string.h>

#include "../util/io_util.h"
#include "../util/string_util.h"

static void chttp_response_reset(ChttpResponse *response) {
  response->status_code = 0;
  response->body = NULL;
  response->body_length = 0;
  response->retry_after_seconds = -1;
}

void chttp_response_destroy(ChttpResponse *response) {
  if (!response) {
    return;
  }
  free(response->body);
  chttp_response_reset(response);
}

// ---------------------------------------------------------------------------
#if defined(__wasm__)
// ---------------------------------------------------------------------------

bool chttp_is_available(void) { return false; }

void chttp_request(const ChttpRequest *request, ChttpResponse *response,
                   ErrorStack *error_stack) {
  (void)request;
  chttp_response_reset(response);
  error_stack_push(
      error_stack, ERROR_STATUS_HTTP_UNAVAILABLE,
      string_duplicate("HTTP requests are not available in wasm builds"));
}

// ---------------------------------------------------------------------------
#elif defined(_WIN32)
// ---------------------------------------------------------------------------

#include <windows.h>
// winhttp.h must follow windows.h.
#include <winhttp.h>

bool chttp_is_available(void) { return true; }

// Widens a UTF-8 string for the WinHTTP API. Caller frees.
static wchar_t *widen(const char *text) {
  const int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
  if (length <= 0) {
    return NULL;
  }
  wchar_t *wide = (wchar_t *)malloc_or_die(sizeof(wchar_t) * (size_t)length);
  MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, length);
  return wide;
}

void chttp_request(const ChttpRequest *request, ChttpResponse *response,
                   ErrorStack *error_stack) {
  chttp_response_reset(response);

  wchar_t *wide_url = widen(request->url);
  if (!wide_url) {
    error_stack_push(error_stack, ERROR_STATUS_HTTP_REQUEST_FAILED,
                     get_formatted_string("could not encode url %s",
                                          request->url));
    return;
  }

  URL_COMPONENTS components;
  memset(&components, 0, sizeof(components));
  components.dwStructSize = sizeof(components);
  components.dwHostNameLength = (DWORD)-1;
  components.dwUrlPathLength = (DWORD)-1;
  components.dwExtraInfoLength = (DWORD)-1;
  components.dwSchemeLength = (DWORD)-1;

  HINTERNET session = NULL;
  HINTERNET connection = NULL;
  HINTERNET handle = NULL;
  wchar_t *host = NULL;
  wchar_t *path = NULL;
  bool failed = true;

  if (!WinHttpCrackUrl(wide_url, 0, 0, &components)) {
    error_stack_push(
        error_stack, ERROR_STATUS_HTTP_REQUEST_FAILED,
        get_formatted_string("could not parse url %s", request->url));
    goto cleanup;
  }

  host = (wchar_t *)malloc_or_die(sizeof(wchar_t) *
                                  ((size_t)components.dwHostNameLength + 1));
  memcpy(host, components.lpszHostName,
         sizeof(wchar_t) * components.dwHostNameLength);
  host[components.dwHostNameLength] = L'\0';

  const DWORD path_length =
      components.dwUrlPathLength + components.dwExtraInfoLength;
  path = (wchar_t *)malloc_or_die(sizeof(wchar_t) * ((size_t)path_length + 1));
  memcpy(path, components.lpszUrlPath, sizeof(wchar_t) * path_length);
  path[path_length] = L'\0';

  session = WinHttpOpen(L"MAGPIE", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    error_stack_push(error_stack, ERROR_STATUS_HTTP_REQUEST_FAILED,
                     string_duplicate("WinHttpOpen failed"));
    goto cleanup;
  }

  const DWORD timeout_ms = (DWORD)request->timeout_seconds * 1000;
  WinHttpSetTimeouts(session, (int)timeout_ms, (int)timeout_ms, (int)timeout_ms,
                     (int)timeout_ms);

  connection = WinHttpConnect(session, host, components.nPort, 0);
  if (!connection) {
    error_stack_push(error_stack, ERROR_STATUS_HTTP_REQUEST_FAILED,
                     string_duplicate("WinHttpConnect failed"));
    goto cleanup;
  }

  const DWORD flags =
      (components.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
  handle = WinHttpOpenRequest(connection,
                              request->method == CHTTP_POST ? L"POST" : L"GET",
                              path, NULL, WINHTTP_NO_REFERER,
                              WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
  if (!handle) {
    error_stack_push(error_stack, ERROR_STATUS_HTTP_REQUEST_FAILED,
                     string_duplicate("WinHttpOpenRequest failed"));
    goto cleanup;
  }

  for (int i = 0; i < request->num_headers; i++) {
    wchar_t *wide_header = widen(request->headers[i]);
    if (wide_header) {
      WinHttpAddRequestHeaders(handle, wide_header, (DWORD)-1,
                               WINHTTP_ADDREQ_FLAG_ADD);
      free(wide_header);
    }
  }

  if (!WinHttpSendRequest(handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                          (LPVOID)request->body, (DWORD)request->body_length,
                          (DWORD)request->body_length, 0) ||
      !WinHttpReceiveResponse(handle, NULL)) {
    error_stack_push(
        error_stack, ERROR_STATUS_HTTP_REQUEST_FAILED,
        get_formatted_string("request to %s failed", request->url));
    goto cleanup;
  }

  DWORD status = 0;
  DWORD status_size = sizeof(status);
  WinHttpQueryHeaders(handle,
                      WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                      WINHTTP_NO_HEADER_INDEX);
  response->status_code = (long)status;

  DWORD retry_after = 0;
  DWORD retry_after_size = sizeof(retry_after);
  if (WinHttpQueryHeaders(handle,
                          WINHTTP_QUERY_RETRY_AFTER | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &retry_after,
                          &retry_after_size, WINHTTP_NO_HEADER_INDEX)) {
    response->retry_after_seconds = (int)retry_after;
  }

  size_t capacity = 4096;
  size_t length = 0;
  char *body = (char *)malloc_or_die(capacity);
  DWORD available = 0;
  while (WinHttpQueryDataAvailable(handle, &available) && available > 0) {
    if (length + available + 1 > capacity) {
      while (length + available + 1 > capacity) {
        capacity *= 2;
      }
      body = (char *)realloc_or_die(body, capacity);
    }
    DWORD read = 0;
    if (!WinHttpReadData(handle, body + length, available, &read)) {
      break;
    }
    length += read;
  }
  body[length] = '\0';
  response->body = body;
  response->body_length = length;
  failed = false;

cleanup:
  if (handle) {
    WinHttpCloseHandle(handle);
  }
  if (connection) {
    WinHttpCloseHandle(connection);
  }
  if (session) {
    WinHttpCloseHandle(session);
  }
  free(host);
  free(path);
  free(wide_url);
  if (failed) {
    chttp_response_destroy(response);
  }
}

// ---------------------------------------------------------------------------
#else
// ---------------------------------------------------------------------------

#include <dlfcn.h>

// Only the handful of libcurl entry points the client needs, resolved lazily.
// Types are declared locally so no curl headers are required to build.

typedef void CURL;
typedef int CURLcode;
struct curl_slist;

enum {
  CURLOPT_URL = 10002,
  CURLOPT_WRITEFUNCTION = 20011,
  CURLOPT_WRITEDATA = 10001,
  CURLOPT_POSTFIELDS = 10015,
  CURLOPT_POSTFIELDSIZE_LARGE = 30120,
  CURLOPT_HTTPHEADER = 10023,
  CURLOPT_TIMEOUT = 13,
  CURLOPT_FOLLOWLOCATION = 52,
  CURLOPT_MAXREDIRS = 68,
  CURLOPT_SSL_VERIFYPEER = 64,
  CURLOPT_SSL_VERIFYHOST = 81,
  CURLOPT_USERAGENT = 10018,
  CURLOPT_POST = 47,
  CURLOPT_NOSIGNAL = 99,
  CURLINFO_RESPONSE_CODE = 2097154,
  CURLINFO_RETRY_AFTER = 6291508,
};

typedef CURL *(*curl_easy_init_fn)(void);
typedef CURLcode (*curl_easy_setopt_fn)(CURL *, int, ...);
typedef CURLcode (*curl_easy_perform_fn)(CURL *);
typedef CURLcode (*curl_easy_getinfo_fn)(CURL *, int, ...);
typedef void (*curl_easy_cleanup_fn)(CURL *);
typedef struct curl_slist *(*curl_slist_append_fn)(struct curl_slist *,
                                                   const char *);
typedef void (*curl_slist_free_all_fn)(struct curl_slist *);
typedef const char *(*curl_easy_strerror_fn)(CURLcode);

static struct {
  bool attempted;
  void *handle;
  curl_easy_init_fn easy_init;
  curl_easy_setopt_fn easy_setopt;
  curl_easy_perform_fn easy_perform;
  curl_easy_getinfo_fn easy_getinfo;
  curl_easy_cleanup_fn easy_cleanup;
  curl_slist_append_fn slist_append;
  curl_slist_free_all_fn slist_free_all;
  curl_easy_strerror_fn easy_strerror;
} curl_api;

static bool load_curl(void) {
  if (curl_api.attempted) {
    return curl_api.handle != NULL;
  }
  curl_api.attempted = true;

  // Distributions and macOS name the library differently; try each in turn.
  static const char *const candidates[] = {
      "libcurl.so.4", "libcurl.so", "libcurl.4.dylib", "libcurl.dylib",
  };
  for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
    curl_api.handle = dlopen(candidates[i], RTLD_LAZY | RTLD_LOCAL);
    if (curl_api.handle) {
      break;
    }
  }
  if (!curl_api.handle) {
    return false;
  }

#define LOAD_SYMBOL(field, name, type)                                         \
  curl_api.field = (type)dlsym(curl_api.handle, name);                         \
  if (!curl_api.field) {                                                       \
    curl_api.handle = NULL;                                                    \
    return false;                                                              \
  }

  LOAD_SYMBOL(easy_init, "curl_easy_init", curl_easy_init_fn)
  LOAD_SYMBOL(easy_setopt, "curl_easy_setopt", curl_easy_setopt_fn)
  LOAD_SYMBOL(easy_perform, "curl_easy_perform", curl_easy_perform_fn)
  LOAD_SYMBOL(easy_getinfo, "curl_easy_getinfo", curl_easy_getinfo_fn)
  LOAD_SYMBOL(easy_cleanup, "curl_easy_cleanup", curl_easy_cleanup_fn)
  LOAD_SYMBOL(slist_append, "curl_slist_append", curl_slist_append_fn)
  LOAD_SYMBOL(slist_free_all, "curl_slist_free_all", curl_slist_free_all_fn)
  LOAD_SYMBOL(easy_strerror, "curl_easy_strerror", curl_easy_strerror_fn)
#undef LOAD_SYMBOL

  return true;
}

bool chttp_is_available(void) { return load_curl(); }

typedef struct ResponseBuffer {
  char *data;
  size_t length;
  size_t capacity;
} ResponseBuffer;

static size_t write_callback(void *contents, size_t size, size_t count,
                             void *user_data) {
  ResponseBuffer *buffer = (ResponseBuffer *)user_data;
  const size_t total = size * count;
  if (buffer->length + total + 1 > buffer->capacity) {
    while (buffer->length + total + 1 > buffer->capacity) {
      buffer->capacity *= 2;
    }
    buffer->data = (char *)realloc_or_die(buffer->data, buffer->capacity);
  }
  memcpy(buffer->data + buffer->length, contents, total);
  buffer->length += total;
  buffer->data[buffer->length] = '\0';
  return total;
}

void chttp_request(const ChttpRequest *request, ChttpResponse *response,
                   ErrorStack *error_stack) {
  chttp_response_reset(response);

  if (!load_curl()) {
    error_stack_push(
        error_stack, ERROR_STATUS_HTTP_UNAVAILABLE,
        string_duplicate(
            "libcurl was not found. Install it to use network features "
            "(Debian/Ubuntu: libcurl4, Fedora: libcurl, macOS: preinstalled). "
            "All offline MAGPIE commands work without it."));
    return;
  }

  CURL *handle = curl_api.easy_init();
  if (!handle) {
    error_stack_push(error_stack, ERROR_STATUS_HTTP_REQUEST_FAILED,
                     string_duplicate("curl_easy_init failed"));
    return;
  }

  struct curl_slist *headers = NULL;
  for (int i = 0; i < request->num_headers; i++) {
    headers = curl_api.slist_append(headers, request->headers[i]);
  }

  ResponseBuffer buffer;
  buffer.capacity = 4096;
  buffer.length = 0;
  buffer.data = (char *)malloc_or_die(buffer.capacity);
  buffer.data[0] = '\0';

  curl_api.easy_setopt(handle, CURLOPT_URL, request->url);
  curl_api.easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_callback);
  curl_api.easy_setopt(handle, CURLOPT_WRITEDATA, &buffer);
  curl_api.easy_setopt(handle, CURLOPT_TIMEOUT, (long)request->timeout_seconds);
  curl_api.easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
  curl_api.easy_setopt(handle, CURLOPT_MAXREDIRS, 5L);
  curl_api.easy_setopt(handle, CURLOPT_USERAGENT, "MAGPIE");
  // Without this libcurl installs signal handlers, which is hostile inside a
  // process that already manages its own threads.
  curl_api.easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
  // Certificate verification is deliberately not configurable.
  curl_api.easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_api.easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);
  if (headers) {
    curl_api.easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
  }
  if (request->method == CHTTP_POST) {
    curl_api.easy_setopt(handle, CURLOPT_POST, 1L);
    curl_api.easy_setopt(handle, CURLOPT_POSTFIELDS,
                         request->body ? request->body : "");
    curl_api.easy_setopt(handle, CURLOPT_POSTFIELDSIZE_LARGE,
                         (long long)request->body_length);
  }

  const CURLcode code = curl_api.easy_perform(handle);
  if (code != 0) {
    error_stack_push(error_stack, ERROR_STATUS_HTTP_REQUEST_FAILED,
                     get_formatted_string("request to %s failed: %s",
                                          request->url,
                                          curl_api.easy_strerror(code)));
    free(buffer.data);
    if (headers) {
      curl_api.slist_free_all(headers);
    }
    curl_api.easy_cleanup(handle);
    return;
  }

  long status = 0;
  curl_api.easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
  response->status_code = status;

  long long retry_after = 0;
  if (curl_api.easy_getinfo(handle, CURLINFO_RETRY_AFTER, &retry_after) == 0 &&
      retry_after > 0) {
    response->retry_after_seconds = (int)retry_after;
  }

  response->body = buffer.data;
  response->body_length = buffer.length;

  if (headers) {
    curl_api.slist_free_all(headers);
  }
  curl_api.easy_cleanup(handle);
}

#endif
