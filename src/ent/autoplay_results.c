#include "autoplay_results.h"

#include <stdlib.h>

#include "game.h"
#include "move.h"
#include "stats.h"

#include "../util/util.h"

typedef enum {
  AUTOPLAY_RECORDER_TYPE_GAME,
  AUTOPLAY_RECORDER_TYPE_LEAVEGEN,
  NUMBER_OF_AUTOPLAY_RECORDERS,
} autoplay_recorder_t;

#define AUTOPLAY_RECORDER_GAME_OPTION 1 << AUTOPLAY_RECORDER_TYPE_GAME
#define AUTOPLAY_RECORDER_LEAVEGEN_OPTION 1 << AUTOPLAY_RECORDER_TYPE_LEAVEGEN

typedef void (*recorder_reset_func_t)(void *);
typedef void *(*recorder_create_func_t)();
typedef void (*recorder_destroy_func_t)(void *);
typedef void (*recorder_add_move_func_t)(void *, const Move *);
typedef void (*recorder_add_game_func_t)(void *, const Game *);
typedef void (*recorder_combine_func_t)(void *, const void *);

typedef struct Recorder {
  void *data;
  recorder_reset_func_t reset_func;
  recorder_create_func_t create_func;
  recorder_destroy_func_t destroy_func;
  recorder_add_move_func_t add_move_func;
  recorder_add_game_func_t add_game_func;
  recorder_combine_func_t combine_func;
} Recorder;

struct AutoplayResults {
  Recorder recorders[NUMBER_OF_AUTOPLAY_RECORDERS];
};

// Generic recorders

void add_move_noop(void *data, const Move *move) { return; }

void add_game_noop(void *data, const Game *game) { return; }

// Game Recorder

typedef struct GameData {
  int total_games;
  int p0_wins;
  int p0_losses;
  int p0_ties;
  int p0_firsts;
  Stat *p0_score;
  Stat *p1_score;
} GameData;

void game_data_reset(void *data) {
  GameData *gd = (GameData *)data;
  gd->total_games = 0;
  gd->p0_wins = 0;
  gd->p0_losses = 0;
  gd->p0_ties = 0;
  gd->p0_firsts = 0;
  stat_reset(gd->p0_score);
  stat_reset(gd->p1_score);
}

void *game_data_create() {
  GameData *game_data = malloc_or_die(sizeof(GameData));
  game_data->p0_score = stat_create();
  game_data->p1_score = stat_create();
  game_data_reset(game_data);
  return (void *)game_data;
}

void game_data_destroy(void *data) {
  if (!data) {
    return;
  }
  GameData *gd = (GameData *)data;
  stat_destroy(gd->p0_score);
  stat_destroy(gd->p1_score);
  free(gd);
}

void game_data_add_game(void *data, const Game *game) {
  int p0_game_score = player_get_score(game_get_player(game, 0));
  int p1_game_score = player_get_score(game_get_player(game, 1));
  GameData *gd = (GameData *)data;
  gd->total_games++;
  if (p0_game_score > p0_game_score) {
    gd->p0_wins++;
  } else if (p0_game_score > p0_game_score) {
    gd->p0_losses++;
  } else {
    gd->p0_ties++;
  }
  if (game_get_starting_player_index(game) == 0) {
    gd->p0_firsts++;
  }
  stat_push(gd->p0_score, (double)p0_game_score, 1);
  stat_push(gd->p1_score, (double)p1_game_score, 1);
}

void game_data_combine(void *data1, const void *data2) {
  GameData *gd1 = (GameData *)data1;
  const GameData *gd2 = (const GameData *)data2;
}

void autoplay_results_add(const AutoplayResults *result_to_add,
                          AutoplayResults *result_to_be_updated) {
  // Stats are combined elsewhere
  result_to_be_updated->p0_firsts += result_to_add->p0_firsts;
  result_to_be_updated->p0_wins += result_to_add->p0_wins;
  result_to_be_updated->p0_losses += result_to_add->p0_losses;
  result_to_be_updated->p0_ties += result_to_add->p0_ties;
  result_to_be_updated->total_games += result_to_add->total_games;
}
