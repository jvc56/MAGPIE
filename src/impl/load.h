#ifndef LOAD_H
#define LOAD_H

#include "../ent/game_history.h"
#include "../util/io_util.h"

typedef struct DownloadGCGOptions {
  const char *source_identifier;  // Game ID, URL, or local file path
  const char *lexicon;            // Optional lexicon override
  void *config;
} DownloadGCGOptions;

// helper functions for .gcg sources
char *get_xt_gcg_string(const char *identifier, ErrorStack *error_stack);
char *get_woogles_gcg_string(const char *identifier, ErrorStack *error_stack);
char *get_local_gcg_string(const char *identifier, ErrorStack *error_stack);

// main dispatcher function
char *get_gcg_string(const DownloadGCGOptions *options, ErrorStack *error_stack);

void download_gcg(const DownloadGCGOptions *options, GameHistory *game_history,
                  ErrorStack *error_stack);

#endif