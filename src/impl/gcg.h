
#ifndef GCG_H
#define GCG_H

#include "../def/gcg_defs.h"

#include "../ent/config.h"
#include "../ent/game_history.h"

gcg_parse_status_t parse_gcg(const char *gcg_filename, Config *config,
                             GameHistory *game_history);
gcg_parse_status_t parse_gcg_string(const char *input_gcg_string,
                                    Config *config, GameHistory *game_history);

#endif
