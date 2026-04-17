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
  char *basename;
  gcg_source_t source;
} GetGCGResult;

void get_gcg(const GetGCGArgs *get_args, GetGCGResult *result,
             ErrorStack *error_stack);

#endif
