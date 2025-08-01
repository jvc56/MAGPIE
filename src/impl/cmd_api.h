#ifndef CMD_API_H
#define CMD_API_H

// A string -> string API for code using magpie as a library.
//
// Callers get a handle to an opaque Magpie struct, and interact with it by
// passing it a command stirng, and parsing the resulting output.
// The Magpie struct retains and handles the internal game state, and the
// caller is responsible for freeing returned strings.
//
// clang-format off
// Example usage:
//    Magpie *mp = magpie_create("./data");
//    cmd_exit_code ret;
//    ret = magpie_run_sync(mp, "set -lex CSW21");
//    if (!ret == MAGPIE_SUCCESS) { // handle error }
//    ret = magpie_run_sync(mp, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 AQRTUYZ/ 0/0 0");
//    if (!ret == MAGPIE_SUCCESS) { // handle error }
//    ret = magpie_run_sync(mp, "generate");
//    if (ret == MAGPIE_SUCCESS) {
//      char* moves = magpie_get_last_command_output(mp);
//      // parse and handle the returned move list
//    } else {
//      char* error = magpie_get_and_clear_error(mp);
//      // display or handle error
//    }
// clang-format on

#include "../util/io_util.h"
#include "config.h"

// Opaque struct of magpie internal state
typedef struct Magpie Magpie;

typedef enum {
  MAGPIE_SUCCESS = 0,
  MAGPIE_ERROR = 1,
  MAGPIE_DID_NOT_RUN = 2
} cmd_exit_code;

Magpie *magpie_create(const char *data_paths);

void magpie_destroy(Magpie *mp);

cmd_exit_code magpie_run_sync(Magpie *mp, const char *command);

char *magpie_get_and_clear_error(Magpie *mp);

char *magpie_get_last_command_status_message(Magpie *mp);

char *magpie_get_last_command_output(const Magpie *mp);

#endif
