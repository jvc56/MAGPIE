
#ifndef GCG_H
#define GCG_H

#include "../ent/game_history.h"
#include "../util/io_util.h"

typedef struct GCGParser GCGParser;

GCGParser *gcg_parser_create(const char *gcg_string, GameHistory *game_history,
                             const char *existing_p0_lexicon,
                             ErrorStack *error_stack);
void gcg_parser_destroy(GCGParser *gcg_parser);

void parse_gcg_settings(GCGParser *gcg_parser, ErrorStack *error_stack);
void parse_gcg_events(GCGParser *gcg_parser, Game *game,
                      ErrorStack *error_stack);
void write_gcg(const char *gcg_filename, const LetterDistribution *ld,
               const GameHistory *game_history, ErrorStack *error_stack);

#endif
