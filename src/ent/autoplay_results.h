#ifndef AUTOPLAY_RESULTS_H
#define AUTOPLAY_RESULTS_H

#include <stdbool.h>

#include "game.h"
#include "klv.h"
#include "move.h"

#include "../util/io_util.h"
#include "../util/string_util.h"

typedef enum {
  AUTOPLAY_RECORDER_TYPE_GAME,
  AUTOPLAY_RECORDER_TYPE_FJ,
  AUTOPLAY_RECORDER_TYPE_WIN_PCT,
  AUTOPLAY_RECORDER_TYPE_LEAVES,
  NUMBER_OF_AUTOPLAY_RECORDERS,
} autoplay_recorder_t;

typedef struct AutoplayResults AutoplayResults;

AutoplayResults *autoplay_results_create(void);
AutoplayResults *
autoplay_results_create_empty_copy(const AutoplayResults *orig);
void autoplay_results_set_options(AutoplayResults *autoplay_results,
                                  const char *options_str,
                                  ErrorStack *error_stack);
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
void autoplay_results_set_data_paths(AutoplayResults *autoplay_results,
                                     const char *data_paths);
void autoplay_results_set_ld(AutoplayResults *autoplay_results,
                             const LetterDistribution *ld);
void autoplay_results_set_klv(AutoplayResults *autoplay_results, KLV *klv);
uint64_t autoplay_results_build_option(autoplay_recorder_t recorder_type);
uint64_t autoplay_results_get_options(const AutoplayResults *autoplay_results);
void autoplay_results_set_players_data(AutoplayResults *autoplay_results,
                                       const PlayersData *players_data);
#endif