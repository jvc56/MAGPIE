#ifndef AUTOPLAY_RESULTS_H
#define AUTOPLAY_RESULTS_H

#include <stdbool.h>

#include "../def/autoplay_defs.h"

#include "error_stack.h"
#include "game.h"
#include "move.h"

#include "../util/string_util.h"

typedef struct AutoplayResults AutoplayResults;

AutoplayResults *autoplay_results_create(void);
AutoplayResults *
autoplay_results_create_empty_copy(const AutoplayResults *orig);
void autoplay_results_set_options(AutoplayResults *autoplay_results,
                                  const char *options_str,
                                  ErrorStack *error_stack);
void autoplay_results_reset_options(AutoplayResults *autoplay_results);
void autoplay_results_destroy(AutoplayResults *autoplay_results);
void autoplay_results_reset(AutoplayResults *autoplay_results);
void autoplay_results_add_move(AutoplayResults *autoplay_results,
                               const Game *game, const Move *move,
                               const Rack *leave);
void autoplay_results_add_game(AutoplayResults *autoplay_results,
                               const Game *game, uint64_t turns, bool divergent,
                               uint64_t seed);
void autoplay_results_finalize(AutoplayResults **autoplay_results_list,
                               int list_size, AutoplayResults *target);
char *autoplay_results_to_string(AutoplayResults *autoplay_results,
                                 bool human_readable, bool show_divergent);
void string_builder_add_winning_player_confidence(StringBuilder *sb,
                                                  double p0_win_pct,
                                                  double p1_win_pct,
                                                  uint64_t total_games);
void autoplay_results_set_write_buffer_size(AutoplayResults *autoplay_results,
                                            int write_buffer_size);
void autoplay_results_set_record_filepath(AutoplayResults *autoplay_results,
                                          const char *filepath);

#endif