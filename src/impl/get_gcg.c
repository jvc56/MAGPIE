#include "get_gcg.h"

#include "../compat/chttp.h"
#include "../ent/data_filepaths.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WOOGLES_GCG_START "\"gcg\":\""
#define XTABLES_URL "cross-tables.com/annotated.php?u="
#define WOOGLES_URL "woogles.io/game/"

enum {
  XTABLES_URL_LENGTH = sizeof(XTABLES_URL) - 1,
  WOOGLES_URL_LENGTH = sizeof(WOOGLES_URL) - 1,
  MAX_GAME_ID_LENGTH = 30,
};

// Fetches a URL and returns the body, or NULL on any failure. Replaces three
// sites that shelled out to the `curl` binary through popen: those built shell
// command strings partly out of caller-supplied identifiers, and depended on
// curl being installed. The compat HTTP layer needs neither.
static char *fetch_url(const char *url, const char *body,
                       const char *const *headers, int num_headers) {
  ChttpRequest request;
  request.method = body ? CHTTP_POST : CHTTP_GET;
  request.url = url;
  request.headers = headers;
  request.num_headers = num_headers;
  request.body = body;
  request.body_length = body ? string_length(body) : 0;
  request.timeout_seconds = 60;

  ChttpResponse response;
  ErrorStack *errors = error_stack_create();
  chttp_request(&request, &response, errors);
  const bool failed = !error_stack_is_empty(errors);
  error_stack_reset(errors);
  error_stack_destroy(errors);
  if (failed) {
    return NULL;
  }
  if (response.status_code < 200 || response.status_code >= 300) {
    chttp_response_destroy(&response);
    return NULL;
  }
  char *content = response.body;
  // Ownership moves to the caller.
  response.body = NULL;
  chttp_response_destroy(&response);
  return content;
}

// Returns the basename of path with the .gcg extension stripped if present.
static char *get_file_id(const char *path) {
  const char *slash = strrchr(path, '/');
  const char *basename_start = slash ? slash + 1 : path;
  const size_t basename_len = string_length(basename_start);
  const size_t gcg_ext_len = string_length(GCG_EXTENSION);
  if (has_suffix(GCG_EXTENSION, basename_start)) {
    return get_substring(basename_start, 0, (int)(basename_len - gcg_ext_len));
  }
  return string_duplicate(basename_start);
}

// Populates result for a cross-tables game. basename is set to the numeric
// game ID. Does nothing (without error) if identifier does not match
// cross-tables format.
static void get_xt_gcg(const char *identifier, GetGCGResult *result,
                       ErrorStack *error_stack) {
  char game_id_str[MAX_GAME_ID_LENGTH + 1] = {0};
  // Check if this is a Cross-tables URL first
  const char *xt_url_start = strstr(identifier, XTABLES_URL);
  if (xt_url_start) {
    size_t game_id_str_len = 0;
    xt_url_start += XTABLES_URL_LENGTH;
    while (*xt_url_start != '\0') {
      const char url_char = *xt_url_start;
      if (isdigit(url_char)) {
        if (game_id_str_len >= MAX_GAME_ID_LENGTH) {
          error_stack_push(
              error_stack, ERROR_STATUS_XT_URL_MALFORMED,
              get_formatted_string(
                  "xtables game id cannot be longer than %d characters",
                  MAX_GAME_ID_LENGTH));
          return;
        }
        game_id_str[game_id_str_len++] = url_char;
      } else {
        break;
      }
      xt_url_start++;
    }
  } else {
    // Check if it's a numeric game id
    if (!is_all_digits_or_empty(identifier)) {
      return;
    }
    const size_t game_id_str_len = string_length(identifier);
    if (game_id_str_len > MAX_GAME_ID_LENGTH) {
      error_stack_push(
          error_stack, ERROR_STATUS_XT_ID_MALFORMED,
          get_formatted_string(
              "xtables game id cannot be longer than %d characters",
              MAX_GAME_ID_LENGTH));
      return;
    }
    strncpy(game_id_str, identifier, MAX_GAME_ID_LENGTH);
    game_id_str[game_id_str_len] = '\0';
  }

  // Get first 3 digits for the path
  char first_three[4] = {0};
  for (int i = 0; i < 3 && game_id_str[i]; i++) {
    first_three[i] = game_id_str[i];
  }

  char *url = get_formatted_string(
      "https://cross-tables.com/annotated/selfgcg/%s/anno%s.gcg", first_three,
      game_id_str);
  char *gcg_content = fetch_url(url, NULL, NULL, 0);
  free(url);

  if (!gcg_content) {
    error_stack_push(error_stack, ERROR_STATUS_INVALID_GCG_URL,
                     get_formatted_string(
                         "failed to download GCG from cross-tables for ID: %s",
                         game_id_str));
    return;
  }
  result->gcg_string = gcg_content;
  result->basename_or_filepath = string_duplicate(game_id_str);
  result->source = GCG_SOURCE_XT;
}

// Populates result for a Woogles game. basename_or_filepath is set to the
// alphanumeric game ID. Does nothing (without error) if identifier does not
// match Woogles format.
static void get_woogles_gcg(const char *identifier, GetGCGResult *result,
                            ErrorStack *error_stack) {
  char game_id_str[MAX_GAME_ID_LENGTH + 1] = {0};

  const char *woogles_url_start = strstr(identifier, WOOGLES_URL);
  if (woogles_url_start) {
    size_t game_id_str_len = 0;
    woogles_url_start += WOOGLES_URL_LENGTH;
    while (*woogles_url_start != '\0') {
      const char url_char = *woogles_url_start;
      if (isalnum(url_char)) {
        if (game_id_str_len >= MAX_GAME_ID_LENGTH) {
          error_stack_push(
              error_stack, ERROR_STATUS_WOOGLES_URL_MALFORMED,
              get_formatted_string(
                  "woogles game id cannot be longer than %d characters",
                  MAX_GAME_ID_LENGTH));
          return;
        }
        game_id_str[game_id_str_len++] = url_char;
      } else {
        break;
      }
      woogles_url_start++;
    }
  } else {
    // Check if it's a standalone woogles game ID (alphanumeric, not all digits)
    for (int i = 0; identifier[i]; i++) {
      if (!isalnum(identifier[i])) {
        return;
      }
    }
    const size_t game_id_str_len = string_length(identifier);
    if (game_id_str_len > MAX_GAME_ID_LENGTH) {
      error_stack_push(
          error_stack, ERROR_STATUS_WOOGLES_ID_MALFORMED,
          get_formatted_string(
              "woogles game id cannot be longer than %d characters",
              MAX_GAME_ID_LENGTH));
      return;
    }
    strncpy(game_id_str, identifier, MAX_GAME_ID_LENGTH);
    game_id_str[game_id_str_len] = '\0';
  }

  char *request_body =
      get_formatted_string("{\"game_id\":\"%s\"}", game_id_str);
  static const char *const headers[] = {"Content-Type: application/json"};
  char *response = fetch_url(
      "https://woogles.io/api/game_service.GameMetadataService/GetGCG",
      request_body, headers, 1);
  free(request_body);

  if (!response) {
    error_stack_push(
        error_stack, ERROR_STATUS_WOOGLES_URL_MALFORMED,
        get_formatted_string(
            "failed to get response from woogles API for ID: %s", identifier));
    return;
  }

  // Parse JSON response to extract GCG content
  const char *gcg_start = strstr(response, WOOGLES_GCG_START);
  if (!gcg_start) {
    free(response);
    error_stack_push(
        error_stack, ERROR_STATUS_INVALID_WOOGLES_GCG_RESPONSE,
        get_formatted_string(
            "invalid or missing GCG content from woogles URL: %s", identifier));
    return;
  }
  gcg_start += string_length(WOOGLES_GCG_START);
  const char *gcg_end = gcg_start;
  while (*gcg_end && *gcg_end != '"') {
    gcg_end++;
  }
  long gcg_len = gcg_end - gcg_start;
  char *raw_gcg = malloc_or_die(gcg_len + 1);
  strncpy(raw_gcg, gcg_start, gcg_len);
  raw_gcg[gcg_len] = '\0';

  // Unescape JSON string using utility function
  result->gcg_string = json_unescape_string(raw_gcg);
  free(raw_gcg);
  free(response);
  result->basename_or_filepath = string_duplicate(game_id_str);
  result->source = GCG_SOURCE_WOOGLES;
}

// Populates result for a generic URL. basename_or_filepath is set to everything
// after the last '/' with .gcg stripped. Does nothing (without error) if
// identifier is not a URL.
static void get_url_gcg(const char *identifier, GetGCGResult *result,
                        ErrorStack *error_stack) {
  if (!is_url(identifier)) {
    return;
  }

  char *gcg_content = fetch_url(identifier, NULL, NULL, 0);

  if (!gcg_content) {
    error_stack_push(error_stack, ERROR_STATUS_INVALID_GCG_URL,
                     get_formatted_string("failed to get response from URL: %s",
                                          identifier));
    return;
  }

  result->gcg_string = gcg_content;
  result->basename_or_filepath = get_file_id(identifier);
  result->source = GCG_SOURCE_URL;
}

// Populates result for a local file. basename_or_filepath is set to the
// full filepath (including .gcg extension if present). Does nothing (without
// error) if the file cannot be found.
static void get_local_gcg(const char *identifier, GetGCGResult *result,
                          ErrorStack *error_stack) {
  char *identifier_with_possible_ext = NULL;
  if (access(identifier, R_OK) != 0) {
    if (!has_suffix(GCG_EXTENSION, identifier)) {
      // Try adding .gcg extension
      StringBuilder *sb = string_builder_create();
      string_builder_add_string(sb, identifier);
      string_builder_add_string(sb, GCG_EXTENSION);
      if (access(string_builder_peek(sb), R_OK) == 0) {
        identifier_with_possible_ext =
            string_builder_dump_and_destroy(sb, NULL);
      } else {
        string_builder_destroy(sb);
      }
    }
  } else {
    identifier_with_possible_ext = string_duplicate(identifier);
  }

  if (!identifier_with_possible_ext) {
    return;
  }

  char *gcg_content =
      get_string_from_file(identifier_with_possible_ext, error_stack);

  if (error_stack_is_empty(error_stack)) {
    result->gcg_string = gcg_content;
    result->basename_or_filepath =
        string_duplicate(identifier_with_possible_ext);
    result->source = GCG_SOURCE_LOCAL;
  }
  free(identifier_with_possible_ext);
}

void get_gcg_reset_result(GetGCGResult *result) {
  result->source = GCG_SOURCE_NONE;
  free(result->gcg_string);
  result->gcg_string = NULL;
  free(result->basename_or_filepath);
  result->basename_or_filepath = NULL;
}

// Assumes the GetGCGResult is either zero-initialized or was populated
// from a previous call to get_gcg.
void get_gcg(const GetGCGArgs *get_args, GetGCGResult *result,
             ErrorStack *error_stack) {
  const char *identifier = get_args->source_identifier;

  get_gcg_reset_result(result);

  get_xt_gcg(identifier, result, error_stack);
  if (!error_stack_is_empty(error_stack) || result->source != GCG_SOURCE_NONE) {
    return;
  }

  get_woogles_gcg(identifier, result, error_stack);
  if (!error_stack_is_empty(error_stack) || result->source != GCG_SOURCE_NONE) {
    return;
  }

  get_local_gcg(identifier, result, error_stack);
  if (!error_stack_is_empty(error_stack) || result->source != GCG_SOURCE_NONE) {
    return;
  }

  get_url_gcg(identifier, result, error_stack);
  if (!error_stack_is_empty(error_stack) || result->source != GCG_SOURCE_NONE) {
    return;
  }

  error_stack_push(
      error_stack, ERROR_STATUS_INVALID_GCG_SOURCE,
      get_formatted_string("could not load GCG from any source: %s",
                           get_args->source_identifier));
}
