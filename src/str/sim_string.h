
#ifndef SIM_STRING_H
#define SIM_STRING_H

#include "../def/letter_distribution_defs.h"
#include "../ent/game.h"
#include "../ent/sim_results.h"
#include "../ent/thread_control.h"
#include "../util/string_util.h"

void string_builder_add_simmed_play_ply_counts(StringBuilder *sb,
                                               const Board *board,
                                               const LetterDistribution *ld,
                                               const SimmedPlay *simmed_play,
                                               const int ply_index);
char *sim_results_get_string(const Game *game, SimResults *sim_results,
                             int max_num_display_plays,
                             int max_num_display_plies, int filter_row,
                             int filter_col, const MachineLetter *prefix_mls,
                             int prefix_len, bool exclude_tile_placement_moves,
                             bool use_ucgi_format,
                             const char *game_board_string);
void sim_results_print(ThreadControl *thread_control, const Game *game,
                       SimResults *sim_results, int max_num_display_plays,
                       int max_num_display_plies, bool use_ucgi_format,
                       const char *game_board_string);

#endif
