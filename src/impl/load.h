#ifndef LOAD_H
#define LOAD_H

#include "../ent/game_history.h"
#include "../util/io_util.h"
#include "config.h"

typedef struct DownloadGCGArgs {
  const char *source_identifier; // Game ID, URL, or local file path
  Config *config;
} DownloadGCGArgs;

void download_gcg(const DownloadGCGArgs *download_args,
                  GameHistory *game_history, ErrorStack *error_stack);

#endif