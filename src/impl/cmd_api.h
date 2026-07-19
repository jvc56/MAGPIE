#ifndef CMD_API_H
#define CMD_API_H

// A string -> string API for programs embedding magpie as a library.
//
// Callers get a handle to an opaque Magpie struct and interact with it by
// passing it command strings and parsing the resulting output. The Magpie
// struct retains the internal game state between commands, and the caller
// is responsible for freeing all returned strings with free().
//
// Command output is machine readable by default. Use
// magpie_run_sync_human_readable to get output formatted for display
// instead. In either case, an explicit -hr argument in the command string
// overrides the default for that command only.
//
// clang-format off
// Example usage:
//    Magpie *mp = magpie_create("./data");
//    if (magpie_has_error(mp)) { /* handle creation error */ }
//    cmd_exit_code ret;
//    ret = magpie_run_sync(mp, "set -lex CSW21");
//    if (ret != MAGPIE_SUCCESS) { /* handle error */ }
//    ret = magpie_run_sync(mp, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 AQRTUYZ/ 0/0 0");
//    if (ret != MAGPIE_SUCCESS) { /* handle error */ }
//    ret = magpie_run_sync(mp, "generate");
//    if (ret == MAGPIE_SUCCESS) {
//      char *moves = magpie_get_last_command_output(mp);
//      // parse and handle the returned move list
//      free(moves);
//    } else {
//      char *error = magpie_get_and_clear_error(mp);
//      // display or handle error
//      free(error);
//    }
//    magpie_destroy(mp);
// clang-format on

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque struct of magpie internal state
typedef struct Magpie Magpie;

typedef enum {
  MAGPIE_SUCCESS = 0,
  MAGPIE_ERROR = 1,
  MAGPIE_DID_NOT_RUN = 2
} cmd_exit_code;

// Status of the most recent command. Values mirror thread_control_status_t.
typedef enum {
  MAGPIE_THREAD_STATUS_UNINITIALIZED = 0,
  MAGPIE_THREAD_STATUS_STARTED = 1,
  MAGPIE_THREAD_STATUS_USER_INTERRUPT = 2,
  MAGPIE_THREAD_STATUS_FINISHED = 3
} magpie_thread_status;

// Creates a Magpie instance loading data from the ':'-separated paths in
// data_paths. Never returns NULL; if creation fails, the returned handle
// carries the error (check with magpie_has_error) and all subsequent
// commands return MAGPIE_DID_NOT_RUN.
Magpie *magpie_create(const char *data_paths);

void magpie_destroy(Magpie *mp);

// Runs a command to completion, storing its output in machine-readable
// form. Retrieve the output with magpie_get_last_command_output.
cmd_exit_code magpie_run_sync(Magpie *mp, const char *command);

// Same as magpie_run_sync, but stores output formatted for human display.
cmd_exit_code magpie_run_sync_human_readable(Magpie *mp, const char *command);

// Returns true if an error is pending on the error stack.
bool magpie_has_error(const Magpie *mp);

// Returns the pending error messages (empty string if none) and clears the
// error stack.
char *magpie_get_and_clear_error(Magpie *mp);

// Returns the status of the currently executing or most recently finished
// command; returns an empty string if no command has been loaded.
char *magpie_get_last_command_status_message(Magpie *mp);

char *magpie_get_last_command_output(const Magpie *mp);

// Signals the currently running command to stop.
void magpie_stop_current_command(Magpie *mp);

magpie_thread_status magpie_get_thread_status(const Magpie *mp);

#ifdef __cplusplus
}
#endif

#endif
