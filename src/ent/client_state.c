#include "client_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../compat/cfile.h"
#include "../compat/crandom.h"
#include "../util/string_util.h"

enum {
  UUID_BYTES = 16,
  DEFAULT_IDLE_WAIT_SECONDS = 5,
};

// A v4 UUID in canonical 8-4-4-4-12 lowercase hex.
static char *generate_worker_uuid(ErrorStack *error_stack) {
  uint8_t bytes[UUID_BYTES];
  crandom_bytes(bytes, UUID_BYTES, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return NULL;
  }
  // Version 4 and the RFC 4122 variant.
  bytes[6] = (uint8_t)((bytes[6] & 0x0F) | 0x40);
  bytes[8] = (uint8_t)((bytes[8] & 0x3F) | 0x80);

  char *uuid = (char *)malloc_or_die(37);
  snprintf(uuid, 37,
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6],
           bytes[7], bytes[8], bytes[9], bytes[10], bytes[11], bytes[12],
           bytes[13], bytes[14], bytes[15]);
  return uuid;
}

static char *trim(char *text) {
  while (*text == ' ' || *text == '\t') {
    text++;
  }
  size_t length = string_length(text);
  while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\t' ||
                        text[length - 1] == '\r' || text[length - 1] == '\n')) {
    text[--length] = '\0';
  }
  return text;
}

static void append_uuid_line(const char *path, const char *uuid) {
  FILE *stream = fopen(path, "a");
  if (!stream) {
    return;
  }
  fprintf(stream, "\nuuid %s\n", uuid);
  fclose(stream);
}

ClientState *client_state_load(const char *path, ErrorStack *error_stack) {
  const char *settings_path =
      path ? path : CONTRIBUTE_SETTINGS_DEFAULT_FILENAME;

  char *contents = get_string_from_file(settings_path, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_reset(error_stack);
    error_stack_push(
        error_stack, ERROR_STATUS_CONTRIBUTE_SETTINGS_MISSING,
        get_formatted_string(
            "could not read contribution settings from '%s'. Create it with at "
            "least a 'server' line, for example:\n"
            "  server https://birdtest.example\n"
            "  apikey bt_...",
            settings_path));
    return NULL;
  }

  ClientState *state = (ClientState *)malloc_or_die(sizeof(ClientState));
  state->server_url = NULL;
  state->api_key = NULL;
  state->worker_uuid = NULL;
  state->threads = 0;
  state->max_tasks = 0;
  state->idle_wait_seconds = DEFAULT_IDLE_WAIT_SECONDS;
  state->settings_path = string_duplicate(settings_path);

  int line_number = 0;
  char *cursor = contents;
  while (cursor && *cursor) {
    char *newline = strchr(cursor, '\n');
    if (newline) {
      *newline = '\0';
    }
    line_number++;

    char *line = trim(cursor);
    if (*line != '\0' && *line != '#') {
      char *space = line;
      while (*space && *space != ' ' && *space != '\t') {
        space++;
      }
      const bool has_value = *space != '\0';
      if (has_value) {
        *space = '\0';
      }
      char *key = line;
      char *value = has_value ? trim(space + 1) : (char *)"";

      if (strings_equal(key, "server")) {
        free(state->server_url);
        state->server_url = string_duplicate(value);
      } else if (strings_equal(key, "apikey")) {
        free(state->api_key);
        state->api_key = string_duplicate(value);
      } else if (strings_equal(key, "uuid")) {
        free(state->worker_uuid);
        state->worker_uuid = string_duplicate(value);
      } else if (strings_equal(key, "threads")) {
        state->threads = atoi(value);
      } else if (strings_equal(key, "maxtasks")) {
        state->max_tasks = atoi(value);
      } else if (strings_equal(key, "idlewait")) {
        state->idle_wait_seconds = atoi(value);
      } else {
        // A typo'd 'apikey' must not silently downgrade someone to anonymous.
        error_stack_push(
            error_stack, ERROR_STATUS_CONTRIBUTE_SETTINGS_MALFORMED,
            get_formatted_string(
                "%s line %d: unknown setting '%s' (expected one of: server, "
                "apikey, uuid, threads, maxtasks, idlewait)",
                settings_path, line_number, key));
        free(contents);
        client_state_destroy(state);
        return NULL;
      }
    }

    cursor = newline ? newline + 1 : NULL;
  }
  free(contents);

  if (!state->server_url || string_length(state->server_url) == 0) {
    error_stack_push(
        error_stack, ERROR_STATUS_CONTRIBUTE_SETTINGS_MISSING,
        get_formatted_string("%s does not set 'server'", settings_path));
    client_state_destroy(state);
    return NULL;
  }

  if (state->api_key && string_length(state->api_key) == 0) {
    free(state->api_key);
    state->api_key = NULL;
  }

  // The file holds a bearer credential once an API key is present.
  if (state->api_key && cfile_is_world_readable(settings_path)) {
    ErrorStack *chmod_errors = error_stack_create();
    cfile_restrict_to_owner(settings_path, chmod_errors);
    error_stack_destroy(chmod_errors);
  }

  if (!state->worker_uuid || string_length(state->worker_uuid) == 0) {
    free(state->worker_uuid);
    state->worker_uuid = generate_worker_uuid(error_stack);
    if (!error_stack_is_empty(error_stack)) {
      client_state_destroy(state);
      return NULL;
    }
    // Appended rather than rewritten so the contributor's comments, ordering
    // and formatting survive.
    append_uuid_line(settings_path, state->worker_uuid);
  }

  if (state->idle_wait_seconds <= 0) {
    state->idle_wait_seconds = DEFAULT_IDLE_WAIT_SECONDS;
  }

  return state;
}

void client_state_destroy(ClientState *state) {
  if (!state) {
    return;
  }
  free(state->server_url);
  free(state->api_key);
  free(state->worker_uuid);
  free(state->settings_path);
  free(state);
}
