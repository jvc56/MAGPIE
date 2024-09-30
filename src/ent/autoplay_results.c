#include "autoplay_results.h"

#include <pthread.h>
#include <stdlib.h>

#include "../def/autoplay_defs.h"

#include "game.h"
#include "move.h"
#include "stats.h"

#include "../str/move_string.h"
#include "../str/rack_string.h"

#include "../util/math_util.h"
#include "../util/util.h"

#define DEFAULT_WRITE_BUFFER_SIZE 1024

typedef enum {
  AUTOPLAY_RECORDER_TYPE_GAME,
  AUTOPLAY_RECORDER_TYPE_FJ,
  NUMBER_OF_AUTOPLAY_RECORDERS,
} autoplay_recorder_t;

typedef struct RecorderArgs {
  const Game *game;
  const Move *move;
  const Rack *leave;
  uint64_t number_of_turns;
  uint64_t seed;
  bool divergent;
  bool human_readable;
} RecorderArgs;

// Read-only data shared across all recorder types
typedef struct GenericSharedData {
  int write_buffer_size;
} GenericSharedData;

typedef struct Recorder Recorder;

typedef void (*recorder_reset_func_t)(Recorder *);
typedef void (*recorder_create_data_func_t)(Recorder *);
typedef void (*recorder_destroy_data_func_t)(Recorder *);
typedef void (*recorder_add_move_func_t)(Recorder *, const RecorderArgs *);
typedef void (*recorder_add_game_func_t)(Recorder *, const RecorderArgs *);
typedef void (*recorder_finalize_func_t)(Recorder **, int, Recorder *);
typedef char *(*recorder_str_func_t)(Recorder *, const RecorderArgs *);

struct Recorder {
  void *data;
  void *thread_shared_data;
  bool owns_thread_shared_data;
  const GenericSharedData *generic_shared_data;
  recorder_reset_func_t reset_func;
  recorder_destroy_data_func_t destroy_data_func;
  recorder_add_move_func_t add_move_func;
  recorder_add_game_func_t add_game_func;
  recorder_finalize_func_t finalize_func;
  recorder_str_func_t str_func;
};

struct AutoplayResults {
  uint64_t options;
  bool owns_generic_shared_data;
  GenericSharedData *generic_shared_data;
  Recorder *recorders[NUMBER_OF_AUTOPLAY_RECORDERS];
};

// Generic recorders

void add_move_noop(Recorder __attribute__((unused)) * recorder,
                   const RecorderArgs __attribute__((unused)) * args) {
  return;
}

char *get_str_noop(Recorder __attribute__((unused)) * recorder,
                   const RecorderArgs __attribute__((unused)) * args) {
  return NULL;
}

// Game Recorder

typedef struct GameData {
  uint64_t total_games;
  uint64_t total_turns;
  uint64_t p0_wins;
  uint64_t p0_losses;
  uint64_t p0_ties;
  uint64_t p0_firsts;
  Stat *p0_score;
  Stat *p1_score;
  Stat *turns;
  int game_end_reasons[NUMBER_OF_GAME_END_REASONS];
} GameData;

void game_data_reset(GameData *gd) {
  gd->total_games = 0;
  gd->total_turns = 0;
  gd->p0_wins = 0;
  gd->p0_losses = 0;
  gd->p0_ties = 0;
  gd->p0_firsts = 0;
  stat_reset(gd->p0_score);
  stat_reset(gd->p1_score);
  stat_reset(gd->turns);
  for (int i = 0; i < NUMBER_OF_GAME_END_REASONS; i++) {
    gd->game_end_reasons[i] = 0;
  }
}

GameData *game_data_create(void) {
  GameData *game_data = malloc_or_die(sizeof(GameData));
  game_data->p0_score = stat_create(true);
  game_data->p1_score = stat_create(true);
  game_data->turns = stat_create(true);
  game_data_reset(game_data);
  return game_data;
}

void game_data_destroy(GameData *gd) {
  if (!gd) {
    return;
  }
  stat_destroy(gd->p0_score);
  stat_destroy(gd->p1_score);
  stat_destroy(gd->turns);
  free(gd);
}

void game_data_add_game(GameData *gd, const RecorderArgs *args) {
  const Game *game = args->game;
  uint64_t turns = args->number_of_turns;
  int p0_game_score = player_get_score(game_get_player(game, 0));
  int p1_game_score = player_get_score(game_get_player(game, 1));
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
  stat_push(gd->turns, (double)turns, 1);
  gd->total_turns += turns;
  gd->game_end_reasons[game_get_game_end_reason(game)]++;
}

void string_builder_add_game_end_reasons(StringBuilder *sb,
                                         const GameData *gd) {
  for (int i = 0; i < NUMBER_OF_GAME_END_REASONS; i++) {
    string_builder_add_formatted_string(sb, "%d ", gd->game_end_reasons[i]);
  }
}

char *game_data_ucgi_str(const GameData *gd) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_formatted_string(
      sb, "autoplay games %lu %lu %lu %lu %lu %f %f %f %f ", gd->total_games,
      gd->p0_wins, gd->p0_losses, gd->p0_ties, gd->p0_firsts,
      stat_get_mean(gd->p0_score), stat_get_stdev(gd->p0_score),
      stat_get_mean(gd->p1_score), stat_get_stdev(gd->p1_score));
  string_builder_add_game_end_reasons(sb, gd);
  string_builder_add_string(sb, "\n");
  char *res = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  return res;
}

char *get_total_int_and_percentage_string(uint64_t total, double pct) {
  return get_formatted_string("%lu (%2.2f%%)", total, pct * 100.0);
}

char *get_total_float_and_percentage_string(double total, double pct) {
  return get_formatted_string("%2.1f (%2.2f%%)", total, pct * 100.0);
}

char *get_score_and_dev_string(double score, double stdev) {
  return get_formatted_string("%2.2f %2.2f", score, stdev);
}

void string_builder_add_winning_player_confidence(StringBuilder *sb,
                                                  double p0_total,
                                                  double p1_total,
                                                  uint64_t total_games) {
  // Apply a continuity correction from binomial to normal distribution
  // See
  // https://library.virginia.edu/data/articles/continuity-corrections-imperfect-responses-to-slight-problems#fn1
  // for more details.
  double p0_total_corrected_pct = (p0_total - 0.5) / total_games;
  double p1_total_corrected_pct = (p1_total - 0.5) / total_games;

  int winning_player_num = 0;
  double winning_player_total_corrected_pct;
  if (p0_total_corrected_pct >= 0.5) {
    winning_player_num = 1;
    winning_player_total_corrected_pct = p0_total_corrected_pct;
  } else if (p1_total_corrected_pct >= 0.5) {
    winning_player_num = 2;
    winning_player_total_corrected_pct = p1_total_corrected_pct;
  }

  if (winning_player_num > 0) {
    string_builder_add_formatted_string(
        sb, "Player %d is better with confidence: %f%%\n", winning_player_num,
        odds_that_player_is_better(winning_player_total_corrected_pct,
                                   total_games));
  } else {
    string_builder_add_string(
        sb, "Results too close to determine which player is better\n");
  }
}

char *game_data_human_readable_str(const GameData *gd, bool divergent) {
  uint64_t p0_wins = gd->p0_wins;
  double p0_win_pct = (double)p0_wins / gd->total_games;
  uint64_t p0_losses = gd->p0_losses;
  double p0_loss_pct = (double)p0_losses / gd->total_games;
  uint64_t p0_ties = gd->p0_ties;
  double p0_tie_pct = (double)p0_ties / gd->total_games;

  double p0_total = (double)gd->p0_wins + (double)gd->p0_ties / (double)2;
  double p0_total_pct = p0_total / (double)(gd->total_games);

  double p1_total = (double)(gd->total_games) - p0_total;
  double p1_total_pct = p1_total / (double)(gd->total_games);

  const int col_width = 25;
  StringBuilder *sb = string_builder_create();
  string_builder_add_string(sb, "\n");

  if (divergent) {
    string_builder_add_string(sb, "Divergent Games\n\n");
  } else {
    string_builder_add_string(sb, "All Games\n\n");
  }

  string_builder_add_formatted_string(sb, "Games Played: %d\n",
                                      gd->total_games);
  string_builder_add_formatted_string(sb, "Turns Played: %d\n",
                                      gd->total_turns);

  if (gd->total_games == 0) {
    string_builder_add_string(sb, "\n");
    char *no_games_ret_str = string_builder_dump(sb, NULL);
    string_builder_destroy(sb);
    return no_games_ret_str;
  }

  string_builder_add_formatted_string(sb, "Turns per Game: %0.2f %0.2f\n\n",
                                      stat_get_mean(gd->turns),
                                      stat_get_stdev(gd->turns));

  const char game_end_reason_strs[NUMBER_OF_GAME_END_REASONS][20] = {
      "None:", "Standard:", "Pass:"};

  for (int i = 0; i < NUMBER_OF_GAME_END_REASONS; i++) {
    string_builder_add_formatted_string(
        sb, "Game End Reason %-*s %d (%2.2f%%)\n", 10, game_end_reason_strs[i],
        gd->game_end_reasons[i],
        100 * ((double)gd->game_end_reasons[i] / gd->total_games));
  }

  string_builder_add_formatted_string(sb, "\n%-*s%-*s%-*s\n", col_width, "",
                                      col_width, "Player 1", col_width,
                                      "Player 2");

  char *p0_total_str =
      get_total_float_and_percentage_string(p0_total, p0_total_pct);
  char *p1_total_str =
      get_total_float_and_percentage_string(p1_total, p1_total_pct);
  string_builder_add_formatted_string(sb, "%-*s%-*s%-*s\n", col_width,
                                      "Total:", col_width, p0_total_str,
                                      col_width, p1_total_str);
  free(p1_total_str);
  free(p0_total_str);

  char *p0_win_str = get_total_int_and_percentage_string(p0_wins, p0_win_pct);
  char *p0_loss_str =
      get_total_int_and_percentage_string(p0_losses, p0_loss_pct);
  string_builder_add_formatted_string(
      sb,
      "%-*s%-*s%-*s\n"
      "%-*s%-*s%-*s\n",
      col_width, "Wins:", col_width, p0_win_str, col_width, p0_loss_str,
      col_width, "Losses:", col_width, p0_loss_str, col_width, p0_win_str);
  free(p0_win_str);
  free(p0_loss_str);

  char *p0_ties_str = get_total_int_and_percentage_string(p0_ties, p0_tie_pct);
  string_builder_add_formatted_string(sb, "%-*s%-*s%-*s\n", col_width,
                                      "Ties:", col_width, p0_ties_str,
                                      col_width, p0_ties_str);
  free(p0_ties_str);

  char *p0_score_str = get_score_and_dev_string(stat_get_mean(gd->p0_score),
                                                stat_get_stdev(gd->p0_score));
  char *p1_score_str = get_score_and_dev_string(stat_get_mean(gd->p1_score),
                                                stat_get_stdev(gd->p1_score));
  string_builder_add_formatted_string(sb, "%-*s%-*s%-*s\n", col_width,
                                      "Score:", col_width, p0_score_str,
                                      col_width, p1_score_str);
  free(p1_score_str);
  free(p0_score_str);

  string_builder_add_string(sb, "\n");

  string_builder_add_winning_player_confidence(sb, p0_total, p1_total,
                                               gd->total_games);
  string_builder_add_string(sb, "\n");

  char *ret_str = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  return ret_str;
}

char *game_data_str(const GameData *gd, const RecorderArgs *args) {
  if (args->human_readable) {
    return game_data_human_readable_str(gd, args->divergent);
  } else {
    return game_data_ucgi_str(gd);
  }
}

typedef struct GameDataSets {
  GameData *all_games;
  GameData *divergent_games;
} GameDataSets;

void game_data_sets_reset(Recorder *recorder) {
  GameDataSets *sets = (GameDataSets *)recorder->data;
  game_data_reset(sets->all_games);
  game_data_reset(sets->divergent_games);
}

void game_data_sets_create(Recorder *recorder) {
  GameDataSets *sets = malloc_or_die(sizeof(GameDataSets));
  sets->all_games = game_data_create();
  sets->divergent_games = game_data_create();
  recorder->data = sets;
  recorder->thread_shared_data = NULL;
}

void game_data_sets_destroy(Recorder *recorder) {
  GameDataSets *sets = (GameDataSets *)recorder->data;
  game_data_destroy(sets->all_games);
  game_data_destroy(sets->divergent_games);
  free(sets);
}

void game_data_sets_add_game(Recorder *recorder, const RecorderArgs *args) {
  GameDataSets *sets = (GameDataSets *)recorder->data;
  game_data_add_game(sets->all_games, args);
  if (args->divergent) {
    game_data_add_game(sets->divergent_games, args);
  }
}

void game_data_sets_finalize_subset(Recorder **recorder_list,
                                    int recorder_list_size,
                                    Recorder *primary_recorder,
                                    bool divergent) {
  Stat **p0_score_stats =
      malloc_or_die((sizeof(Stat *)) * (recorder_list_size));
  Stat **p1_score_stats =
      malloc_or_die((sizeof(Stat *)) * (recorder_list_size));
  Stat **turns_stats = malloc_or_die((sizeof(Stat *)) * (recorder_list_size));

  GameDataSets *sets = (GameDataSets *)primary_recorder->data;
  GameData *gd_primary = sets->all_games;
  if (divergent) {
    gd_primary = sets->divergent_games;
  }

  for (int i = 0; i < recorder_list_size; i++) {
    GameDataSets *sets_i = (GameDataSets *)recorder_list[i]->data;
    GameData *gd_i = sets_i->all_games;
    if (divergent) {
      gd_i = sets_i->divergent_games;
    }
    gd_primary->total_games += gd_i->total_games;
    gd_primary->p0_wins += gd_i->p0_wins;
    gd_primary->p0_losses += gd_i->p0_losses;
    gd_primary->p0_ties += gd_i->p0_ties;
    gd_primary->p0_firsts += gd_i->p0_firsts;
    p0_score_stats[i] = gd_i->p0_score;
    p1_score_stats[i] = gd_i->p1_score;
    turns_stats[i] = gd_i->turns;
    gd_primary->total_turns += gd_i->total_turns;
    for (int j = 0; j < NUMBER_OF_GAME_END_REASONS; j++) {
      gd_primary->game_end_reasons[j] += gd_i->game_end_reasons[j];
    }
  }

  stats_combine(p0_score_stats, recorder_list_size, gd_primary->p0_score);
  stats_combine(p1_score_stats, recorder_list_size, gd_primary->p1_score);
  stats_combine(turns_stats, recorder_list_size, gd_primary->turns);
  free(p0_score_stats);
  free(p1_score_stats);
  free(turns_stats);
}

void game_data_sets_finalize(Recorder **recorder_list, int recorder_list_size,
                             Recorder *primary_recorder) {
  game_data_sets_finalize_subset(recorder_list, recorder_list_size,
                                 primary_recorder, false);
  game_data_sets_finalize_subset(recorder_list, recorder_list_size,
                                 primary_recorder, true);
}

char *game_data_sets_str(Recorder *recorder, const RecorderArgs *args) {
  GameDataSets *sets = (GameDataSets *)recorder->data;
  StringBuilder *sb = string_builder_create();

  char *all_game_str = game_data_str(sets->all_games, args);
  string_builder_add_string(sb, all_game_str);
  free(all_game_str);

  if (args->divergent) {
    char *divergent_games_str = game_data_str(sets->divergent_games, args);
    string_builder_add_string(sb, divergent_games_str);
    free(divergent_games_str);
  }

  char *str = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  return str;
}

// FJ recorders
#define MAX_NUMBER_OF_MOVES 100
#define FJ_FILENAME "fj_log.csv"

typedef struct FJMove {
  int unseen_counts[MAX_ALPHABET_SIZE];
  Rack leave;
  Move move;
  int score_diff;
  int unseen_total;
  int player_index;
} FJMove;

typedef struct FJData {
  StringBuilder *sb;
  FJMove moves[MAX_NUMBER_OF_MOVES];
  int move_count;
} FJData;

typedef struct FJSharedData {
  pthread_mutex_t fh_mutex;
  FILE *fh;
} FJSharedData;

void fj_data_reset_fh(FJSharedData *shared_data) {
  if (shared_data->fh) {
    fclose(shared_data->fh);
    shared_data->fh = NULL;
  }
  shared_data->fh = fopen(FJ_FILENAME, "w");
  if (!shared_data->fh) {
    log_fatal("Error opening fj file for writing: %s\n", FJ_FILENAME);
  }
}

void fj_data_reset(Recorder *recorder) {
  FJData *fj_data = (FJData *)recorder->data;
  string_builder_clear(fj_data->sb);
  if (recorder->owns_thread_shared_data) {
    fj_data_reset_fh(recorder->thread_shared_data);
  }
  for (int i = 0; i < MAX_NUMBER_OF_MOVES; i++) {
    for (int j = 0; j < MAX_ALPHABET_SIZE; j++) {
      fj_data->moves[i].unseen_counts[j] = 0;
    }
  }
  fj_data->move_count = 0;
}

void fj_data_create(Recorder *recorder) {
  FJData *data = malloc_or_die(sizeof(FJData));
  data->sb = string_builder_create();
  FJSharedData *shared_data = NULL;
  // If this recorder is not the owner, the thread shared data will be
  // assigned in the recorder_create function.
  if (recorder->owns_thread_shared_data) {
    shared_data = malloc_or_die(sizeof(FJSharedData));
    pthread_mutex_init(&shared_data->fh_mutex, NULL);
    shared_data->fh = NULL;
  }
  recorder->data = data;
  recorder->thread_shared_data = shared_data;
  fj_data_reset(recorder);
}

void fj_data_destroy(Recorder *recorder) {
  FJData *fj_data = (FJData *)recorder->data;
  string_builder_destroy(fj_data->sb);
  if (recorder->owns_thread_shared_data) {
    FJSharedData *shared_data = (FJSharedData *)recorder->thread_shared_data;
    fclose(shared_data->fh);
    shared_data->fh = NULL;
    free(shared_data);
  }
  free(fj_data);
}

void fj_data_add_move(Recorder *recorder, const RecorderArgs *args) {
  FJData *fj_data = (FJData *)recorder->data;
  const Game *game = args->game;
  const Bag *bag = game_get_bag(game);
  if (fj_data->move_count >= MAX_NUMBER_OF_MOVES) {
    return;
  }
  FJMove *fj_move = &fj_data->moves[fj_data->move_count];
  const Rack *leave = args->leave;
  rack_copy(&fj_move->leave, leave);
  move_copy(&fj_move->move, args->move);
  fj_move->player_index = game_get_player_on_turn_index(game);
  const Player *player = game_get_player(game, fj_move->player_index);
  const Player *opponent = game_get_player(game, 1 - fj_move->player_index);
  fj_move->score_diff = player_get_score(player) - player_get_score(opponent);
  const int num_tiles_in_bag = bag_get_tiles(bag);
  if (num_tiles_in_bag == 0) {
    fj_move->unseen_total = rack_get_total_letters(player_get_rack(opponent));
  } else {
    fj_move->unseen_total = bag_get_tiles(bag) + (RACK_SIZE);
  }
  bag_increment_unseen_count(bag, fj_move->unseen_counts);
  rack_increment_unseen_count(player_get_rack(opponent),
                              fj_move->unseen_counts);
  fj_data->move_count++;
  return;
}

void fj_write_buffer_to_output(Recorder *recorder, bool always_flush) {
  FJData *fj_data = (FJData *)recorder->data;
  FJSharedData *shared_data = (FJSharedData *)recorder->thread_shared_data;
  const GenericSharedData *generic_shared_data = recorder->generic_shared_data;
  int str_len = string_builder_length(fj_data->sb);
  if (str_len > 0 &&
      (always_flush || str_len >= generic_shared_data->write_buffer_size)) {
    pthread_mutex_lock(&shared_data->fh_mutex);
    if (fputs(string_builder_peek(fj_data->sb), shared_data->fh) == EOF) {
      fclose(shared_data->fh);
      log_fatal("Error writing to fj file of index: %d\n", index);
    }
    pthread_mutex_unlock(&shared_data->fh_mutex);
    string_builder_clear(fj_data->sb);
  }
}

void fj_data_add_game(Recorder *recorder, const RecorderArgs *args) {
  FJData *fj_data = (FJData *)recorder->data;
  const Game *game = args->game;
  const LetterDistribution *ld = game_get_ld(game);
  const Board *board = game_get_board(game);
  double player_one_result = 0.5;
  int player_one_score = player_get_score(game_get_player(game, 0));
  int player_two_score = player_get_score(game_get_player(game, 1));
  if (player_one_score < player_two_score) {
    player_one_result = 0;
  } else if (player_one_score > player_two_score) {
    player_one_result = 1;
  }
  const int dist_size =
      rack_get_dist_size(player_get_rack(game_get_player(game, 0)));
  for (int i = 0; i < fj_data->move_count; i++) {
    FJMove *fj_move = &fj_data->moves[i];
    const double player_result =
        fj_move->player_index * (1 - player_one_result) +
        (1 - fj_move->player_index) * player_one_result;
    string_builder_add_formatted_string(fj_data->sb, "%lu,", args->seed);
    string_builder_add_move(fj_data->sb, board, &fj_move->move, ld);
    string_builder_add_char(fj_data->sb, ',');
    string_builder_add_formatted_string(fj_data->sb, "%d,",
                                        move_get_score(&fj_move->move));
    string_builder_add_rack(fj_data->sb, &fj_move->leave, ld, false);
    string_builder_add_formatted_string(fj_data->sb, ",%d,%d,%.1f",
                                        player_result, fj_move->score_diff,
                                        fj_move->unseen_total);
    for (int ml = 0; ml < dist_size; ml++) {
      string_builder_add_formatted_string(fj_data->sb, ",%d",
                                          fj_move->unseen_counts[ml]);
      fj_move->unseen_counts[ml] = 0;
    }
    string_builder_add_char(fj_data->sb, '\n');
  }
  fj_data->move_count = 0;
  fj_write_buffer_to_output(recorder, false);
}

void fj_data_finalize(Recorder **recorders, int num_recorders,
                      Recorder __attribute__((unused)) * primary_recorder) {
  for (int i = 0; i < num_recorders; i++) {
    Recorder *recorder = recorders[i];
    FJData *fj_data = (FJData *)recorder->data;
    fj_data->move_count = 0;
    fj_write_buffer_to_output(recorder, true);
  }
}

// Generic recorder and autoplay results functions

Recorder *recorder_create(const Recorder *primary_recorder,
                          recorder_reset_func_t reset_func,
                          recorder_create_data_func_t create_data_func,
                          recorder_destroy_data_func_t destroy_data_func,
                          recorder_add_move_func_t add_move_func,
                          recorder_add_game_func_t add_game_func,
                          recorder_finalize_func_t finalize_func,
                          recorder_str_func_t str_func) {
  Recorder *recorder = malloc_or_die(sizeof(Recorder));
  recorder->reset_func = reset_func;
  recorder->destroy_data_func = destroy_data_func;
  recorder->add_move_func = add_move_func;
  recorder->add_game_func = add_game_func;
  recorder->finalize_func = finalize_func;
  recorder->str_func = str_func;
  recorder->owns_thread_shared_data = !primary_recorder;
  create_data_func(recorder);
  // If this recorder owns the shared data, then it was already created
  // in the create_data_func call above. If not, we need to copy the pointer
  // to the shared data from the primary recorder.
  if (primary_recorder) {
    recorder->thread_shared_data = primary_recorder->thread_shared_data;
  }

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

void recorder_add_move(Recorder *recorder, const RecorderArgs *args) {
  recorder->add_move_func(recorder, args);
}

void recorder_add_game(Recorder *recorder, const RecorderArgs *args) {
  recorder->add_game_func(recorder, args);
}

void recorder_finalize(Recorder **recorder_list, int list_size,
                       Recorder *primary_recorder) {
  primary_recorder->finalize_func(recorder_list, list_size, primary_recorder);
}

char *recorder_str(Recorder *recorder, bool human_readable,
                   bool show_divergent) {
  RecorderArgs args;
  args.human_readable = human_readable;
  args.divergent = show_divergent;
  return recorder->str_func(recorder, &args);
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
    recorder_finalize_func_t finalize_func, recorder_str_func_t str_func) {
  if (options & autoplay_results_build_option(recorder_type)) {
    if (!autoplay_results->recorders[recorder_type]) {
      const Recorder *primary_recorder = NULL;
      if (primary_autoplay_results) {
        primary_recorder = primary_autoplay_results->recorders[recorder_type];
      }
      autoplay_results->recorders[recorder_type] = recorder_create(
          primary_recorder, reset_func, create_data_func, destroy_data_func,
          add_move_func, add_game_func, finalize_func, str_func);
      autoplay_results->recorders[recorder_type]->generic_shared_data =
          autoplay_results->generic_shared_data;
    }
  } else {
    recorder_destroy(autoplay_results->recorders[recorder_type]);
    autoplay_results->recorders[recorder_type] = NULL;
  }
}

void copy_generic_shared_data(AutoplayResults *dst,
                              const AutoplayResults *src) {
  dst->generic_shared_data->write_buffer_size =
      src->generic_shared_data->write_buffer_size;
}

void autoplay_results_set_options_int(AutoplayResults *autoplay_results,
                                      uint64_t options,
                                      const AutoplayResults *primary) {
  autoplay_results_set_recorder(
      autoplay_results, options, primary, AUTOPLAY_RECORDER_TYPE_GAME,
      game_data_sets_reset, game_data_sets_create, game_data_sets_destroy,
      add_move_noop, game_data_sets_add_game, game_data_sets_finalize,
      game_data_sets_str);
  autoplay_results_set_recorder(
      autoplay_results, options, primary, AUTOPLAY_RECORDER_TYPE_FJ,
      fj_data_reset, fj_data_create, fj_data_destroy, fj_data_add_move,
      fj_data_add_game, fj_data_finalize, get_str_noop);
  autoplay_results->options = options;
  if (primary) {
    copy_generic_shared_data(autoplay_results, primary);
  }
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
    } else if (has_iprefix(option_str, "fj")) {
      options |= autoplay_results_build_option(AUTOPLAY_RECORDER_TYPE_FJ);
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

GenericSharedData *create_generic_shared_data(void) {
  GenericSharedData *generic_shared_data =
      malloc_or_die(sizeof(GenericSharedData));
  generic_shared_data->write_buffer_size = DEFAULT_WRITE_BUFFER_SIZE;
  return generic_shared_data;
}

void destroy_generic_shared_data(GenericSharedData *generic_shared_data) {
  free(generic_shared_data);
}

void autoplay_results_add_generic_shared_data(
    AutoplayResults *autoplay_results) {
  autoplay_results->generic_shared_data = create_generic_shared_data();
  autoplay_results->owns_generic_shared_data = true;
}

AutoplayResults *autoplay_results_create_internal(void) {
  AutoplayResults *autoplay_results = malloc_or_die(sizeof(AutoplayResults));
  for (int i = 0; i < NUMBER_OF_AUTOPLAY_RECORDERS; i++) {
    autoplay_results->recorders[i] = NULL;
  }
  return autoplay_results;
}

AutoplayResults *autoplay_results_create_without_generic_shared_data(void) {
  return autoplay_results_create_internal();
}

AutoplayResults *autoplay_results_create(void) {
  AutoplayResults *autoplay_results = autoplay_results_create_internal();
  autoplay_results_add_generic_shared_data(autoplay_results);
  return autoplay_results;
}

AutoplayResults *
autoplay_results_create_empty_copy(const AutoplayResults *orig) {
  AutoplayResults *autoplay_results =
      autoplay_results_create_without_generic_shared_data();
  autoplay_results->generic_shared_data = orig->generic_shared_data;
  autoplay_results->owns_generic_shared_data = false;
  autoplay_results_set_options_int(autoplay_results, orig->options, orig);
  return autoplay_results;
}

void autoplay_results_destroy(AutoplayResults *autoplay_results) {
  if (!autoplay_results) {
    return;
  }
  for (int i = 0; i < NUMBER_OF_AUTOPLAY_RECORDERS; i++) {
    recorder_destroy(autoplay_results->recorders[i]);
  }
  if (autoplay_results->owns_generic_shared_data) {
    destroy_generic_shared_data(autoplay_results->generic_shared_data);
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
                               const Game *game, const Move *move,
                               const Rack *leave) {
  RecorderArgs args;
  args.game = game;
  args.move = move;
  args.leave = leave;
  for (int i = 0; i < NUMBER_OF_AUTOPLAY_RECORDERS; i++) {
    if (autoplay_results->recorders[i]) {
      recorder_add_move(autoplay_results->recorders[i], &args);
    }
  }
}

void autoplay_results_add_game(AutoplayResults *autoplay_results,
                               const Game *game, uint64_t turns, bool divergent,
                               uint64_t seed) {
  RecorderArgs args;
  args.game = game;
  args.number_of_turns = turns;
  args.divergent = divergent;
  args.seed = seed;
  for (int i = 0; i < NUMBER_OF_AUTOPLAY_RECORDERS; i++) {
    if (autoplay_results->recorders[i]) {
      recorder_add_game(autoplay_results->recorders[i], &args);
    }
  }
}

void autoplay_results_finalize(AutoplayResults **autoplay_results_list,
                               int list_size, AutoplayResults *primary) {
  Recorder **recorder_list = malloc_or_die(sizeof(Recorder *) * list_size);
  for (int i = 0; i < NUMBER_OF_AUTOPLAY_RECORDERS; i++) {
    if (!autoplay_results_list[0]->recorders[i]) {
      continue;
    }
    for (int j = 0; j < list_size; j++) {
      recorder_list[j] = autoplay_results_list[j]->recorders[i];
    }
    recorder_finalize(recorder_list, list_size, primary->recorders[i]);
  }
  free(recorder_list);
}

char *autoplay_results_to_string(AutoplayResults *autoplay_results,
                                 bool human_readable, bool show_divergent) {
  StringBuilder *ar_sb = string_builder_create();
  for (int i = 0; i < NUMBER_OF_AUTOPLAY_RECORDERS; i++) {
    if (!autoplay_results->recorders[i]) {
      continue;
    }
    char *rec_str = recorder_str(autoplay_results->recorders[i], human_readable,
                                 show_divergent);
    if (rec_str) {
      string_builder_add_string(ar_sb, rec_str);
      free(rec_str);
    }
  }
  char *ar_str = string_builder_dump(ar_sb, NULL);
  string_builder_destroy(ar_sb);
  return ar_str;
}

void autoplay_results_set_write_buffer_size(AutoplayResults *autoplay_results,
                                            int write_buffer_size) {
  autoplay_results->generic_shared_data->write_buffer_size = write_buffer_size;
}