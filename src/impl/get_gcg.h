#ifndef LOAD_H
#define LOAD_H

#include "../ent/game_history.h"
#include "../util/io_util.h"

typedef enum {
  GCG_SOURCE_NONE,
  GCG_SOURCE_XT,
  GCG_SOURCE_WOOGLES,
  GCG_SOURCE_URL,
  GCG_SOURCE_LOCAL,
} gcg_source_t;

typedef struct GetGCGArgs {
  const char *source_identifier; // Game ID, URL, or local file path
} GetGCGArgs;

typedef struct GetGCGResult {
  char *gcg_string;
  // If the source is an xtables or woogles game, this is the game ID.
  // If the source is a URL, this is the everything after the last slash with
  // .gcg stripped if present. If the source is a local file, this is the file
  // path.
  char *basename_or_filepath;
  gcg_source_t source;
} GetGCGResult;

void get_gcg(const GetGCGArgs *get_args, GetGCGResult *result,
             ErrorStack *error_stack);
void get_gcg_reset_result(GetGCGResult *result);

#endif
