#include "load.h"

#include "../ent/game_history.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "gcg.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WOOGLES_GCG_START "\"gcg\":\""
#define XTABLES_URL "cross-tables.com/annotated.php?u="
#define XTABLES_URL_LENGTH (sizeof(XTABLES_URL) - 1)
#define WOOGLES_URL "woogles.io/game/"
#define WOOGLES_URL_LENGTH (sizeof(WOOGLES_URL) - 1)
#define MAX_GAME_ID_LENGTH 30

char *get_xt_gcg_string(const char *identifier, ErrorStack *error_stack) {
  char game_id_str[MAX_GAME_ID_LENGTH + 1];
  // Check if this is a Cross-tables URL first
  char *xt_url_start = strstr(identifier, XTABLES_URL);
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
          return NULL;
        }
        game_id_str[game_id_str_len++] = url_char;
      } else {
        game_id_str[game_id_str_len] = '\0';
        break;
      }
      xt_url_start++;
    }
  } else {
    // Check if it's a numeric game id
    if (!is_all_digits_or_empty(identifier)) {
      return NULL;
    }
    const size_t game_id_str_len = string_length(identifier);
    if (game_id_str_len > MAX_GAME_ID_LENGTH) {
      error_stack_push(
          // FIXME: trigger this
          error_stack, ERROR_STATUS_XT_ID_MALFORMED,
          get_formatted_string(
              "xtables game id cannot be longer than %d characters",
              MAX_GAME_ID_LENGTH));
      return NULL;
    }
    strncpy(game_id_str, identifier, MAX_GAME_ID_LENGTH);
    game_id_str[game_id_str_len] = '\0';
  }

  printf("got game id str: >%s<\n", game_id_str);
  char *gcg_content = NULL;

  // Get first 3 digits for the path
  char first_three[4] = {0};
  for (int i = 0; i < 3 && game_id_str[i]; i++) {
    first_three[i] = game_id_str[i];
  }

  // Build the curl command
  char *curl_cmd = get_formatted_string(
      "curl -s -L \"https://cross-tables.com/annotated/selfgcg/%s/anno%s.gcg\"",
      first_three, game_id_str);

  gcg_content = get_process_output(curl_cmd);

  free(curl_cmd);

  return gcg_content;
}

char *get_woogles_gcg_string(const char *identifier, ErrorStack *error_stack) {
  char game_id_str[MAX_GAME_ID_LENGTH + 1];

  char *woogles_url_start = strstr(identifier, WOOGLES_URL);
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
          return NULL;
        }
        game_id_str[game_id_str_len++] = url_char;
      } else {
        game_id_str[game_id_str_len] = '\0';
        break;
      }
      woogles_url_start++;
    }
  } else {
    // Check if it's a standalone woogles game ID (alphanumeric, not all digits)
    for (int i = 0; identifier[i]; i++) {
      if (!isalnum(identifier[i])) {
        return NULL;
      }
    }
    const size_t game_id_str_len = string_length(identifier);
    if (game_id_str_len > MAX_GAME_ID_LENGTH) {
      error_stack_push(
          // FIXME: trigger this
          error_stack, ERROR_STATUS_WOOGLES_ID_MALFORMED,
          get_formatted_string(
              "woogles game id cannot be longer than %d characters",
              MAX_GAME_ID_LENGTH));
      return NULL;
    }
    strncpy(game_id_str, identifier, MAX_GAME_ID_LENGTH);
    game_id_str[game_id_str_len] = '\0';
  }

  char *gcg_content = NULL;

  // Use woogles API to get GCG content
  char *curl_cmd = get_formatted_string(
      "curl -s -H 'Content-Type: application/json' "
      "https://woogles.io/api/game_service.GameMetadataService/GetGCG "
      "-d '{\"game_id\":\"%s\"}'",
      game_id_str);

  char *response = get_process_output(curl_cmd);
  free(curl_cmd);

  if (!response) {
    error_stack_push(
        error_stack, ERROR_STATUS_WOOGLES_URL_MALFORMED,
        get_formatted_string(
            "Failed to get response from woogles API for ID: %s", identifier));
    return NULL;
  }

  // Parse JSON response to extract GCG content
  const char *gcg_start = strstr(response, WOOGLES_GCG_START);
  if (!gcg_start) {
    free(response);
    error_stack_push(
        error_stack, ERROR_STATUS_INVALID_WOOGLES_GCG_RESPONSE,
        get_formatted_string(
            "invalid or missing GCG content from woogles URL: %s", identifier));
    return NULL;
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
  gcg_content = json_unescape_string(raw_gcg);
  free(raw_gcg);
  free(response);
  return gcg_content;
}

char *get_url_gcg_string(const char *identifier, ErrorStack *error_stack) {
  if (!is_url(identifier)) {
    return NULL;
  }

  // It's a URL - try to download directly
  char *curl_cmd = get_formatted_string("curl -s -L \"%s\"", identifier);
  char *gcg_content = get_process_output(curl_cmd);
  free(curl_cmd);

  if (!gcg_content) {
    error_stack_push(error_stack, ERROR_STATUS_INVALID_GCG_URL,
                     get_formatted_string("failed to get response from URL: %s",
                                          identifier));
    return NULL;
  }
  return gcg_content;
}

char *get_local_gcg_string(const char *identifier, ErrorStack *error_stack) {
  // Check if file exists and is readable
  if (access(identifier, R_OK) != 0) {
    return NULL; // Not a valid local file - not our job
  }

  // Read file content directly
  char *gcg_content = get_string_from_file(identifier, error_stack);

  if (!error_stack_is_empty(error_stack)) {
    return NULL;
  }

  return gcg_content;
}

char *get_gcg_string(const DownloadGCGArgs *download_args,
                     ErrorStack *error_stack) {

  const char *identifier = download_args->source_identifier;

  // Try cross-tables first
  char *gcg_string = get_xt_gcg_string(identifier, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return NULL;
  }
  if (gcg_string) {
    return gcg_string;
  }

  // Try Woogles
  gcg_string = get_woogles_gcg_string(identifier, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return NULL;
  }
  if (gcg_string) {
    return gcg_string;
  }

  // Try local file
  gcg_string = get_local_gcg_string(identifier, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return NULL;
  }
  if (gcg_string) {
    return gcg_string;
  }

  // Try generic URL download
  gcg_string = get_url_gcg_string(identifier, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return NULL;
  }
  if (gcg_string) {
    return gcg_string;
  }

  // If we get here, nothing worked
  error_stack_push(
      error_stack, ERROR_STATUS_INVALID_GCG_SOURCE,
      get_formatted_string("Could not load GCG from any source: %s",
                           download_args->source_identifier));
  return NULL;
}

void download_gcg(const DownloadGCGArgs *download_args,
                  GameHistory *game_history, ErrorStack *error_stack) {

  // Get the GCG content from any available source
  char *gcg_content = get_gcg_string(download_args, error_stack);
  if (!gcg_content) {
    return; // Error already pushed to stack by get_gcg_string
  }
  // Parse the GCG content using the provided parser
  parse_gcg_string(gcg_content, download_args->config, game_history,
                   error_stack);
  // Clean up
  free(gcg_content);
}