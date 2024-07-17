#ifndef AUTOPLAY_RESULTS_H
#define AUTOPLAY_RESULTS_H

#include <stdbool.h>

#include "../def/autoplay_defs.h"

#include "game.h"
#include "move.h"

typedef enum {
  AUTOPLAY_RECORDER_TYPE_GAME,
  AUTOPLAY_RECORDER_TYPE_LEAVEGEN,
  NUMBER_OF_AUTOPLAY_RECORDERS,
} autoplay_recorder_t;

typedef struct AutoplayResults AutoplayResults;

AutoplayResults *autoplay_results_create(void);
AutoplayResults *
autoplay_results_create_empty_copy(const AutoplayResults *orig);
autoplay_status_t
autoplay_results_set_options(AutoplayResults *autoplay_results,
                             const char *options_str);
void autoplay_results_reset_options(AutoplayResults *autoplay_results);
void autoplay_results_destroy(AutoplayResults *autoplay_results);
void autoplay_results_reset(AutoplayResults *autoplay_results);
void autoplay_results_add_move(AutoplayResults *autoplay_results,
                               const Move *move);
void autoplay_results_add_game(AutoplayResults *autoplay_results,
                               const Game *game);
void autoplay_results_combine(AutoplayResults **autoplay_results_list,
                              int list_size, AutoplayResults *target);
char *autoplay_results_to_string(AutoplayResults *autoplay_results,
                                 bool human_readable);

#endif