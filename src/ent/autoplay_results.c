#include "autoplay_results.h"

#include <stdlib.h>

#include "../def/autoplay_defs.h"

#include "game.h"
#include "move.h"
#include "stats.h"

#include "../util/util.h"

typedef struct Recorder Recorder;

typedef void (*recorder_reset_func_t)(Recorder *);
typedef void (*recorder_create_data_func_t)(Recorder *, const Recorder *);
typedef void (*recorder_destroy_data_func_t)(Recorder *);
typedef void (*recorder_add_move_func_t)(Recorder *, const Move *);
typedef void (*recorder_add_game_func_t)(Recorder *, const Game *);
typedef void (*recorder_combine_func_t)(Recorder **, int, Recorder *);
typedef char *(*recorder_str_func_t)(Recorder *, bool);

struct Recorder {
  void *data;
  void *shared_data;
  bool owns_shared_data;
  recorder_reset_func_t reset_func;
  recorder_destroy_data_func_t destroy_data_func;
  recorder_add_move_func_t add_move_func;
  recorder_add_game_func_t add_game_func;
  recorder_combine_func_t combine_func;
  recorder_str_func_t str_func;
};

struct AutoplayResults {
  uint64_t options;
  Recorder *recorders[NUMBER_OF_AUTOPLAY_RECORDERS];
};

// Generic recorders

void add_move_noop(Recorder __attribute__((unused)) * recorder,
                   const Move __attribute__((unused)) * move) {
  return;
}

void add_game_noop(Recorder __attribute__((unused)) * recorder,
                   const Game __attribute__((unused)) * game) {
  return;
}

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

void game_data_reset(Recorder *recorder) {
  GameData *gd = (GameData *)recorder->data;
  gd->total_games = 0;
  gd->p0_wins = 0;
  gd->p0_losses = 0;
  gd->p0_ties = 0;
  gd->p0_firsts = 0;
  stat_reset(gd->p0_score);
  stat_reset(gd->p1_score);
}

void game_data_create(Recorder *recorder, const Recorder
                                              __attribute__((unused)) *
                                              primary_recorder) {
  GameData *game_data = malloc_or_die(sizeof(GameData));
  game_data->p0_score = stat_create(true);
  game_data->p1_score = stat_create(true);
  recorder->data = game_data;
  recorder->shared_data = NULL;
  game_data_reset(recorder);
}

void game_data_destroy(Recorder *recorder) {
  if (!recorder) {
    return;
  }
  GameData *gd = (GameData *)recorder->data;
  stat_destroy(gd->p0_score);
  stat_destroy(gd->p1_score);
  free(gd);
}

void game_data_add_game(Recorder *recorder, const Game *game) {
  int p0_game_score = player_get_score(game_get_player(game, 0));
  int p1_game_score = player_get_score(game_get_player(game, 1));
  GameData *gd = (GameData *)recorder->data;
  gd->total_games++;
  if (p0_game_score > p1_game_score) {
    gd->p0_wins++;
  } else if (p1_game_score > p0_game_score) {
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

void game_data_combine(Recorder **recorder_list, int recorder_list_size,
                       Recorder *primary_recorder) {
  Stat **p0_score_stats =
      malloc_or_die((sizeof(Stat *)) * (recorder_list_size));
  Stat **p1_score_stats =
      malloc_or_die((sizeof(Stat *)) * (recorder_list_size));

  GameData *gd_primary = (GameData *)primary_recorder->data;

  for (int i = 0; i < recorder_list_size; i++) {
    GameData *gd_i = (GameData *)recorder_list[i]->data;
    gd_primary->total_games += gd_i->total_games;
    gd_primary->p0_wins += gd_i->p0_wins;
    gd_primary->p0_losses += gd_i->p0_losses;
    gd_primary->p0_ties += gd_i->p0_ties;
    gd_primary->p0_firsts += gd_i->p0_firsts;
    p0_score_stats[i] = gd_i->p0_score;
    p1_score_stats[i] = gd_i->p1_score;
  }

  stats_combine(p0_score_stats, recorder_list_size, gd_primary->p0_score);
  stats_combine(p1_score_stats, recorder_list_size, gd_primary->p1_score);
  free(p0_score_stats);
  free(p1_score_stats);
}

char *game_data_ucgi_str(const GameData *gd) {
  return get_formatted_string(
      "autoplay games %d %d %d %d %d %f %f %f %f\n", gd->total_games,
      gd->p0_wins, gd->p0_losses, gd->p0_ties, gd->p0_firsts,
      stat_get_mean(gd->p0_score), stat_get_stdev(gd->p0_score),
      stat_get_mean(gd->p1_score), stat_get_stdev(gd->p1_score));
}

char *game_data_human_readable_str(const GameData *gd) {
  int p0_wins = gd->p0_wins;
  double p0_win_pct = (double)p0_wins / gd->total_games;
  int p0_losses = gd->p0_losses;
  double p0_loss_pct = (double)p0_losses / gd->total_games;
  int p1_wins = gd->total_games - p0_wins;
  double p1_win_pct = (double)p1_wins / gd->total_games;
  int p1_losses = gd->total_games - p0_losses;
  double p1_loss_pct = (double)p1_losses / gd->total_games;
  int p0_ties = gd->p0_ties;
  double p0_tie_pct = (double)p0_ties / gd->total_games;

  StringBuilder *sb = string_builder_create();
  string_builder_add_string(sb, "Player 1");
  string_builder_add_formatted_string(sb, "%10s", "");
  string_builder_add_string(sb, "Player 2");
  string_builder_add_formatted_string(sb, "%10s", "");
  string_builder_add_formatted_string(sb, "Wins    %d %f %d %f %d", p0_wins,
                                      p0_win_pct, p1_wins, p1_win_pct,
                                      gd->total_games);
  string_builder_add_formatted_string(sb, "Losses  %d %f %d %f %d", p0_losses,
                                      p0_loss_pct, p1_losses, p1_loss_pct,
                                      gd->total_games);
  string_builder_add_formatted_string(sb, "Ties    %d %f %d %f %d", p0_ties,
                                      p0_tie_pct, p0_ties, p0_tie_pct, p0_ties);
  string_builder_add_formatted_string(
      sb, "Score   %f %f %f %f", stat_get_mean(gd->p0_score),
      stat_get_stdev(gd->p0_score), stat_get_mean(gd->p1_score),
      stat_get_stdev(gd->p1_score));
  char *ret_str = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  return ret_str;
}

char *game_data_str(Recorder *recorder, bool human_readable) {
  GameData *gd = (GameData *)recorder->data;
  if (human_readable) {
    return game_data_human_readable_str(gd);
  } else {
    return game_data_ucgi_str(gd);
  }
}

// Generic recorder and autoplay results functions

Recorder *recorder_create(const Recorder *primary_recorder,
                          recorder_reset_func_t reset_func,
                          recorder_create_data_func_t create_data_func,
                          recorder_destroy_data_func_t destroy_data_func,
                          recorder_add_move_func_t add_move_func,
                          recorder_add_game_func_t add_game_func,
                          recorder_combine_func_t combine_func,
                          recorder_str_func_t str_func) {
  Recorder *recorder = malloc_or_die(sizeof(Recorder));
  recorder->reset_func = reset_func;
  recorder->destroy_data_func = destroy_data_func;
  recorder->add_move_func = add_move_func;
  recorder->add_game_func = add_game_func;
  recorder->combine_func = combine_func;
  recorder->str_func = str_func;
  recorder->owns_shared_data = !primary_recorder;
  create_data_func(recorder, primary_recorder);

  return recorder;
}

void recorder_destroy(Recorder *recorder) {
  if (!recorder) {
    return;
  }
  recorder->destroy_data_func(recorder);
  free(recorder);
}

void recorder_reset(Recorder *recorder) { recorder->reset_func(recorder); }

void recorder_add_move(Recorder *recorder, const Move *move) {
  recorder->add_move_func(recorder, move);
}

void recorder_add_game(Recorder *recorder, const Game *game) {
  recorder->add_game_func(recorder, game);
}

void recorder_combine(Recorder **recorder_list, int list_size,
                      Recorder *primary_recorder) {
  primary_recorder->combine_func(recorder_list, list_size, primary_recorder);
}

char *recorder_str(Recorder *recorder, bool human_readable) {
  return recorder->str_func(recorder, human_readable);
}

AutoplayResults *autoplay_results_create() {
  AutoplayResults *autoplay_results = malloc_or_die(sizeof(AutoplayResults));
  for (int i = 0; i < NUMBER_OF_AUTOPLAY_RECORDERS; i++) {
    autoplay_results->recorders[i] = NULL;
  }
  return autoplay_results;
}

uint64_t autoplay_results_build_option(autoplay_recorder_t recorder_type) {
  return (uint64_t)1 << recorder_type;
}

void autoplay_results_set_recorder(
    AutoplayResults *autoplay_results, uint64_t options,
    const AutoplayResults *primary_autoplay_results,
    autoplay_recorder_t recorder_type, recorder_reset_func_t reset_func,
    recorder_create_data_func_t create_data_func,
    recorder_destroy_data_func_t destroy_data_func,
    recorder_add_move_func_t add_move_func,
    recorder_add_game_func_t add_game_func,
    recorder_combine_func_t combine_func, recorder_str_func_t str_func) {
  if (options & autoplay_results_build_option(recorder_type)) {
    if (!autoplay_results->recorders[recorder_type]) {
      const Recorder *primary_recorder = NULL;
      if (primary_autoplay_results) {
        primary_recorder = primary_autoplay_results->recorders[recorder_type];
      }
      autoplay_results->recorders[recorder_type] = recorder_create(
          primary_recorder, reset_func, create_data_func, destroy_data_func,
          add_move_func, add_game_func, combine_func, str_func);
    }
  } else {
    recorder_destroy(autoplay_results->recorders[recorder_type]);
    autoplay_results->recorders[recorder_type] = NULL;
  }
}

void autoplay_results_set_options_int(AutoplayResults *autoplay_results,
                                      uint64_t options,
                                      const AutoplayResults *primary) {
  autoplay_results_set_recorder(
      autoplay_results, options, primary, AUTOPLAY_RECORDER_TYPE_GAME,
      game_data_reset, game_data_create, game_data_destroy, add_move_noop,
      game_data_add_game, game_data_combine, game_data_str);
  autoplay_results->options = options;
}

autoplay_status_t autoplay_results_set_options_with_splitter(
    AutoplayResults *autoplay_results, const StringSplitter *split_options) {
  int number_of_options = string_splitter_get_number_of_items(split_options);

  if (number_of_options == 0) {
    return AUTOPLAY_STATUS_EMPTY_OPTIONS;
  }

  uint64_t options = 0;
  autoplay_status_t status = AUTOPLAY_STATUS_SUCCESS;
  for (int i = 0; i < number_of_options; i++) {
    const char *option_str = string_splitter_get_item(split_options, i);
    if (has_iprefix(option_str, "games")) {
      options |= autoplay_results_build_option(AUTOPLAY_RECORDER_TYPE_GAME);
    } else {
      status = AUTOPLAY_STATUS_INVALID_OPTIONS;
      break;
    }
  }

  if (status == AUTOPLAY_STATUS_SUCCESS) {
    autoplay_results_set_options_int(autoplay_results, options, NULL);
  }

  return status;
}

autoplay_status_t
autoplay_results_set_options(AutoplayResults *autoplay_results,
                             const char *options_str) {
  if (is_string_empty_or_null(options_str)) {
    return AUTOPLAY_STATUS_EMPTY_OPTIONS;
  }
  StringSplitter *split_options = split_string(options_str, ',', true);
  autoplay_status_t status = autoplay_results_set_options_with_splitter(
      autoplay_results, split_options);
  string_splitter_destroy(split_options);
  return status;
}

void autoplay_results_reset_options(AutoplayResults *autoplay_results) {
  autoplay_results_set_options_int(autoplay_results, 0, NULL);
}

AutoplayResults *
autoplay_results_create_empty_copy(const AutoplayResults *orig) {
  AutoplayResults *autoplay_results = autoplay_results_create();
  autoplay_results_set_options_int(autoplay_results, orig->options, orig);
  return autoplay_results;
}

void autoplay_results_destroy(AutoplayResults *autoplay_results) {
  for (int i = 0; i < NUMBER_OF_AUTOPLAY_RECORDERS; i++) {
    recorder_destroy(autoplay_results->recorders[i]);
  }
  free(autoplay_results);
}

void autoplay_results_reset(AutoplayResults *autoplay_results) {
  for (int i = 0; i < NUMBER_OF_AUTOPLAY_RECORDERS; i++) {
    if (autoplay_results->recorders[i]) {
      recorder_reset(autoplay_results->recorders[i]);
    }
  }
}

void autoplay_results_add_move(AutoplayResults *autoplay_results,
                               const Move *move) {
  for (int i = 0; i < NUMBER_OF_AUTOPLAY_RECORDERS; i++) {
    if (autoplay_results->recorders[i]) {
      recorder_add_move(autoplay_results->recorders[i], move);
    }
  }
}

void autoplay_results_add_game(AutoplayResults *autoplay_results,
                               const Game *game) {
  for (int i = 0; i < NUMBER_OF_AUTOPLAY_RECORDERS; i++) {
    if (autoplay_results->recorders[i]) {
      recorder_add_game(autoplay_results->recorders[i], game);
    }
  }
}

void autoplay_results_combine(AutoplayResults **autoplay_results_list,
                              int list_size, AutoplayResults *primary) {
  Recorder **recorder_list = malloc_or_die(sizeof(Recorder *) * list_size);
  for (int i = 0; i < NUMBER_OF_AUTOPLAY_RECORDERS; i++) {
    if (!autoplay_results_list[0]->recorders[i]) {
      continue;
    }
    for (int j = 0; j < list_size; j++) {
      recorder_list[j] = autoplay_results_list[j]->recorders[i];
    }
    recorder_combine(recorder_list, list_size, primary->recorders[i]);
  }
  free(recorder_list);
}

char *autoplay_results_to_string(AutoplayResults *autoplay_results,
                                 bool human_readable) {
  StringBuilder *ar_sb = string_builder_create();
  for (int i = 0; i < NUMBER_OF_AUTOPLAY_RECORDERS; i++) {
    if (!autoplay_results->recorders[i]) {
      continue;
    }
    char *rec_str =
        recorder_str(autoplay_results->recorders[i], human_readable);
    string_builder_add_string(ar_sb, rec_str);
    free(rec_str);
  }
  char *ar_str = string_builder_dump(ar_sb, NULL);
  string_builder_destroy(ar_sb);
  return ar_str;
}