#ifndef LOAD_H
#define LOAD_H

#include "../ent/game_history.h"
#include "../util/io_util.h"
#include "config.h"

typedef struct GetGCGArgs {
  const char *source_identifier; // Game ID, URL, or local file path
} GetGCGArgs;

char *get_gcg(const GetGCGArgs *download_args, ErrorStack *error_stack);

#endif