#ifndef CLIENT_STATE_H
#define CLIENT_STATE_H

// Contribution settings, read from a file rather than the command line.
//
// An API key on a command line ends up in shell history and in `ps` output,
// and contribution settings have no business mixed into settings.txt alongside
// board layouts and simulation parameters. So `contribute` takes only an
// optional path to this file, defaulting to contribute.txt in the working
// directory.
//
// The default file is optional, and so is every setting in any file: no
// contribute.txt in the working directory is every setting at its default
// (birdtest.org, anonymous, cores - 1 threads, no task limit), and a file
// states only what it changes. A file named on the command line must exist:
// a typo in its name would otherwise quietly start a new anonymous worker. What
// a file does state is held to its rules -- an unknown or miscased setting, or
// a malformed value, is refused, never passed over for the default. Nothing
// defaulted is ever written into the file, so a later change of default reaches
// every contributor whose file does not override it.

#include "../util/io_util.h"
#include <stdbool.h>

#define CONTRIBUTE_SETTINGS_DEFAULT_FILENAME "contribute.txt"
// The server contributed to when the settings file names none.
#define CONTRIBUTE_DEFAULT_SERVER "https://birdtest.org"
// The first line of a settings file MAGPIE creates (to save the identity the
// server issued an anonymous worker that had no file).
#define CONTRIBUTE_SETTINGS_HEADER                                             \
  "# magpie contribute settings; see " CONTRIBUTE_DEFAULT_SERVER

typedef struct ClientState {
  char *server_url;
  // NULL when contributing anonymously, in which case worker_uuid identifies.
  char *api_key;
  // NULL until the server assigns one: an anonymous worker with no UUID yet
  // sends no identity at all on its first request. The server mints the UUID
  // (see client_state_set_worker_uuid) rather than the client, so contributor
  // identities cannot be forged or collided by picking a weak generator.
  char *worker_uuid;
  // 0 (or less) for the default, cores - 1: see contribute_state_create.
  int threads;
  int max_tasks;
  int idle_wait_seconds;
  char *settings_path;
  // Whether the settings file was there to read. A missing one is every
  // setting at its default, and is created only to save an issued identity.
  bool settings_file_found;
  // Whether the file stated `server`, `maxtasks` and `idlewait` (a positive
  // one): the start of a run says which settings are defaults.
  bool server_stated;
  bool max_tasks_stated;
  bool idle_wait_stated;
  // The line of a comment that holds `apikey` then a key, or 0: what
  // appending `apikey bt_...` to a last comment line with no newline makes.
  // A comment is never refused -- a key may be commented out on purpose --
  // but a run with no apikey set says which line to look at.
  int commented_key_line;
} ClientState;

// Reads the settings file at `path`, or CONTRIBUTE_SETTINGS_DEFAULT_FILENAME
// when `path` is NULL. With `path` NULL, a file that is not there is every
// setting at its default; a `path` that is not there is an error. So is a file
// that cannot be read, or that holds an unknown setting or a malformed value
// (NULL, and a message on error_stack that never quotes a value that could be
// an API key).
ClientState *client_state_load(const char *path, ErrorStack *error_stack);
void client_state_destroy(ClientState *state);

// Whether `uuid` is a UUID in its canonical form: 36 characters, hex digits
// (either case) in groups of 8-4-4-4-12 separated by hyphens. What the server
// sends is checked against this before it is used or written anywhere -- it
// becomes a request header and a line of the settings file, and a newline in it
// wrote settings of the server's choosing (a `server` line every later run
// obeyed).
bool client_state_is_worker_uuid(const char *uuid);

// Records a worker UUID the server assigned during this run: updates the
// in-memory state and appends a single `uuid <value>` line to the settings
// file so later runs send it back -- creating the file, holding
// CONTRIBUTE_SETTINGS_HEADER and that line alone, when there is none. The file
// is otherwise never rewritten, so the contributor's comments, ordering and
// formatting survive, and no defaulted setting is ever written into it. The
// caller has checked `uuid` with client_state_is_worker_uuid. Returns false
// when the file could not be written: the identity then lasts only this run.
bool client_state_set_worker_uuid(ClientState *state, const char *uuid);

#endif
