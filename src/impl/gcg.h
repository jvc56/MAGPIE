
#ifndef GCG_H
#define GCG_H

#include "../ent/game_history.h"
#include "../util/io_util.h"
#include "config.h"

void parse_gcg(const char *gcg_filename, Config *config,
               GameHistory *game_history, ErrorStack *error_stack);
void parse_gcg_string(const char *input_gcg_string, Config *config,
                      GameHistory *game_history, ErrorStack *error_stack);
void write_gcg(const char *gcg_filename, const LetterDistribution *ld,
               GameHistory *game_history, ErrorStack *error_stack);

#endif
