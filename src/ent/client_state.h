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
  // NULL until the server assigns one: an anonymous worker with no UUID yet
  // sends no identity at all on its first request. The server mints the UUID
  // (see client_state_set_worker_uuid) rather than the client, so contributor
  // identities cannot be forged or collided by picking a weak generator.
  char *worker_uuid;
  int threads;
  int max_tasks;
  int idle_wait_seconds;
  char *settings_path;
} ClientState;

// Reads the settings file.
ClientState *client_state_load(const char *path, ErrorStack *error_stack);
void client_state_destroy(ClientState *state);

// Records a worker UUID the server assigned during this run: updates the
// in-memory state and appends a single `uuid <value>` line to the settings
// file so later runs send it back. The file is otherwise never rewritten, so
// the contributor's comments, ordering and formatting survive.
void client_state_set_worker_uuid(ClientState *state, const char *uuid);

#endif
