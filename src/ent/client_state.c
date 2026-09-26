#include "client_state.h"

#include "../util/io_util.h"
#include "../util/string_util.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  DEFAULT_IDLE_WAIT_SECONDS = 5,
};

// Whether a comment line holds `apikey`, blanks, then `bt_`, wherever it
// sits in the comment (passes 17 and 18 refused shapes of this and either
// refused a comment written on purpose or missed a key run on from one; the
// run now says so instead, and refuses nothing).
static bool comment_holds_a_key(const char *line) {
  for (const char *at = strstr(line, "apikey"); at;
       at = strstr(at + 1, "apikey")) {
    const char *after = at + strlen("apikey");
    const size_t blanks = strspn(after, " \t");
    if (blanks > 0 && strncmp(after + blanks, "bt_", 3) == 0) {
      return true;
    }
  }
  return false;
}

// Parses an integer-valued setting, pushing a message naming the file, line
// and key on failure rather than atoi's silent zero.
static int parse_setting_int(const char *value, const char *key,
                             const char *settings_path, int line_number,
                             ErrorStack *error_stack) {
  ErrorStack *conversion_errors = error_stack_create();
  int result = string_to_int(value, conversion_errors);
  // string_to_int truncates a long: 4294967296 read as 0, "no limit".
  errno = 0;
  const long wide = strtol(value, NULL, 10);
  const bool out_of_range = errno == ERANGE || wide > INT_MAX || wide < INT_MIN;
  if (!error_stack_is_empty(conversion_errors) || out_of_range) {
    error_stack_push(
        error_stack, ERROR_STATUS_CONTRIBUTE_SETTINGS_MALFORMED,
        // The value is not shown: a key appended to a last line that had no
        // newline ends up here, and would be printed (the audit's pass 16).
        get_formatted_string("%s line %d: '%s' is not a whole number from "
                             "-2147483648 to 2147483647",
                             settings_path, line_number, key));
    result = 0;
  }
  error_stack_destroy(conversion_errors);
  return result;
}

static bool append_uuid_line(const char *path, const char *uuid) {
  FILE *stream = fopen(path, "ae");
  if (!stream) {
    return false;
  }
  const bool written = fprintf(stream, "\nuuid %s\n", uuid) > 0;
  return fclose(stream) == 0 && written;
}

bool client_state_is_worker_uuid(const char *uuid) {
  if (!uuid || strlen(uuid) != 36) {
    return false;
  }
  for (int i = 0; i < 36; i++) {
    const char c = uuid[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') {
        return false;
      }
    } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                 (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }
  return true;
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
  state->commented_key_line = 0;

  int line_number = 0;
  // The last uuid line's, which is the one that counts.
  int uuid_line = 0;
  char *cursor = contents;
  // A UTF-8 byte-order mark, which some editors write first: read as part of
  // the first word, it made `server` an unknown setting.
  if ((unsigned char)cursor[0] == 0xEF && (unsigned char)cursor[1] == 0xBB &&
      (unsigned char)cursor[2] == 0xBF) {
    cursor += 3;
  }
  while (cursor && *cursor) {
    char *newline = strchr(cursor, '\n');
    if (newline) {
      *newline = '\0';
    }
    line_number++;

    char *line = trim(cursor);
    // A key appended to a last comment line with no newline was swallowed by
    // the comment, and the run went anonymous without a word.
    if (*line == '#' && comment_holds_a_key(line)) {
      state->commented_key_line = line_number;
    }
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
      const char *const value = has_value ? trim(space + 1) : "";

      if (strings_equal(key, "server")) {
        // A space is two settings on one line -- an `apikey` appended to a
        // last line with no newline -- and would be printed as the server.
        if (strpbrk(value, " \t")) {
          error_stack_push(
              error_stack, ERROR_STATUS_CONTRIBUTE_SETTINGS_MALFORMED,
              get_formatted_string("%s line %d: 'server' holds a space; put "
                                   "each setting on a line of its own",
                                   settings_path, line_number));
        }
        free(state->server_url);
        state->server_url = string_duplicate(value);
      } else if (strings_equal(key, "apikey")) {
        // A key is `bt_` and letters, digits and underscores: a space was two
        // settings on one line, and anything else (a no-break space) a key
        // the server refuses. Not quoted.
        bool well_formed = strncmp(value, "bt_", 3) == 0;
        for (const char *c = value; well_formed && *c; c++) {
          well_formed = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                        (*c >= '0' && *c <= '9') || *c == '_';
        }
        if (value[0] != '\0' && !well_formed) {
          error_stack_push(
              error_stack, ERROR_STATUS_CONTRIBUTE_SETTINGS_MALFORMED,
              get_formatted_string("%s line %d: 'apikey' is not a key (bt_ "
                                   "then letters and digits); put each "
                                   "setting on a line of its own",
                                   settings_path, line_number));
        }
        free(state->api_key);
        state->api_key = string_duplicate(value);
      } else if (strings_equal(key, "uuid")) {
        // Checked once the file is read: the last one counts.
        free(state->worker_uuid);
        state->worker_uuid = string_duplicate(value);
        uuid_line = line_number;
      } else if (strings_equal(key, "threads")) {
        state->threads = parse_setting_int(value, key, settings_path,
                                           line_number, error_stack);
      } else if (strings_equal(key, "maxtasks")) {
        state->max_tasks = parse_setting_int(value, key, settings_path,
                                             line_number, error_stack);
        // 0 is "no limit"; a negative count stopped the run after one task.
        if (state->max_tasks < 0) {
          error_stack_push(
              error_stack, ERROR_STATUS_CONTRIBUTE_SETTINGS_MALFORMED,
              get_formatted_string("%s line %d: 'maxtasks' must be 0 (no "
                                   "limit) or more",
                                   settings_path, line_number));
        }
      } else if (strings_equal(key, "idlewait")) {
        state->idle_wait_seconds = parse_setting_int(value, key, settings_path,
                                                     line_number, error_stack);
      } else {
        // A typo'd 'apikey' must not silently downgrade someone to anonymous.
        // The word is shown only if it could be a setting's name: a pasted
        // key alone on its line is not one, and was printed.
        bool shown = strlen(key) <= 12;
        for (const char *c = key; shown && *c; c++) {
          shown = *c >= 'a' && *c <= 'z';
        }
        // A setting's name in the wrong case is shown, with why.
        static const char *const names[] = {"server",  "apikey",   "uuid",
                                            "threads", "maxtasks", "idlewait"};
        bool wrong_case = false;
        for (size_t n = 0; n < sizeof(names) / sizeof(names[0]); n++) {
          wrong_case = wrong_case || strings_iequal(key, names[n]);
        }
        shown = shown || wrong_case;
        error_stack_push(
            error_stack, ERROR_STATUS_CONTRIBUTE_SETTINGS_MALFORMED,
            get_formatted_string(
                "%s line %d: unknown setting%s%s%s%s (expected one of: server, "
                "apikey, uuid, threads, maxtasks, idlewait)",
                settings_path, line_number, shown ? " '" : "", shown ? key : "",
                shown ? "'" : "",
                wrong_case ? "; settings are lowercase" : ""));
        free(contents);
        client_state_destroy(state);
        return NULL;
      }
      if (!error_stack_is_empty(error_stack)) {
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

  if (state->worker_uuid && string_length(state->worker_uuid) == 0) {
    free(state->worker_uuid);
    state->worker_uuid = NULL;
  }

  // Held to the form the server issues: a line cut short when a full disk
  // interrupted its save was sent as the identity, refused by the server, and
  // every later run ended the same way. Empty still means none.
  if (state->worker_uuid && !client_state_is_worker_uuid(state->worker_uuid)) {
    error_stack_push(
        error_stack, ERROR_STATUS_CONTRIBUTE_SETTINGS_MALFORMED,
        get_formatted_string(
            "%s line %d: 'uuid' is not a UUID (8-4-4-4-12 hex digits): "
            "correct it, or delete the line to be issued a new one",
            settings_path, uuid_line));
    client_state_destroy(state);
    return NULL;
  }

  if (state->idle_wait_seconds <= 0) {
    state->idle_wait_seconds = DEFAULT_IDLE_WAIT_SECONDS;
  }

  return state;
}

bool client_state_set_worker_uuid(ClientState *state, const char *uuid) {
  free(state->worker_uuid);
  state->worker_uuid = string_duplicate(uuid);
  // Appended rather than rewritten so the contributor's comments, ordering
  // and formatting survive.
  return append_uuid_line(state->settings_path, uuid);
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
