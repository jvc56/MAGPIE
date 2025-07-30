#ifndef LOAD_H
#define LOAD_H

#include "../ent/game_history.h"
#include "../util/io_util.h"

typedef struct DownloadGCGOptions {
  const char *source_identifier;  // Game ID, URL, or local file path
  const char *lexicon;            // Optional lexicon override
  const char *ld;                 // Optional letter distribution override
  void *config;                   // Config* - optional, will create default if NULL
} DownloadGCGOptions;

void download_gcg(const DownloadGCGOptions *options, GameHistory *game_history,
                  ErrorStack *error_stack);

#endif