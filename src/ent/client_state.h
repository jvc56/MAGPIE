#ifndef CLIENT_STATE_H
#define CLIENT_STATE_H

// Contribution settings, read from a file rather than the command line.
//
// An API key on a command line ends up in shell history and in `ps` output,
// and contribution settings have no business mixed into settings.txt alongside
// board layouts and simulation parameters. So `contribute` takes only an
// optional path to this file, defaulting to contribute.txt in the working
// directory.

#include "../util/io_util.h"

#define CONTRIBUTE_SETTINGS_DEFAULT_FILENAME "contribute.txt"

typedef struct ClientState {
  char *server_url;
  // NULL when contributing anonymously, in which case worker_uuid identifies.
  char *api_key;
  char *worker_uuid;
  int threads;
  int max_tasks;
  int idle_wait_seconds;
  char *settings_path;
} ClientState;

// Reads the settings file. If it contains no `uuid`, one is generated and a
// single line is appended -- the file is otherwise never rewritten, so
// comments and formatting the contributor put there survive.
ClientState *client_state_load(const char *path, ErrorStack *error_stack);
void client_state_destroy(ClientState *state);

#endif
