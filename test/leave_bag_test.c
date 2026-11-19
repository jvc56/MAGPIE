// LAB (not for merge): leave values by the number of tiles in the bag.
//
// Plays static self-play games (stock leaves, no pool shift, no pre-endgame
// table, no PAT). At every turn whose bag holds a measured number of tiles,
// takes the mover's best plays by static equity, one per distinct leave, and
// plays each to the end of the game many times: the opponent's rack is dealt
// again from the tiles the mover cannot see, from a seed shared by the plays
// of the position, and both players then play their top static move until the
// game ends. A play's mean final spread less its score, compared across the
// plays of one position, measures what its leave is worth there.
//
// With exact=1 each leave also gets its exact value for the mover's unseen
// tiles: the mean value of the full racks over every draw that fills it, from
// the rack value table, as if the unseen tiles were all in the bag.
//
// Spec: first=<game>,games=<n>,threads=<t>,samples=<s>,cands=<k>,exact=<0|1>
// with an optional ,maxbag=<b>: measure only bags of at most b tiles.
// Lines printed:
//   P <game> <turn> <bag> <mover rack> <exact value of the empty leave>
//   C <game> <turn> <bag> <cand> <leave or -> <klv value> <score> <equity>
//     <tiles played> <exact value> <mean final spread> <sd> <wins>

#include "leave_bag_test.h"

#include "../src/def/equity_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/pool_shift.h"
#include "../src/ent/rack.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/str/rack_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  LB_SPEC_FIELDS = 6,
  LB_SPEC_FIELDS_WITH_MAX_BAG = 7,
  LB_NO_MAX_BAG = 1000,
  LB_MAX_CANDS = 32,
  LB_MAX_THREADS = 10,
  LB_MOVE_LIST_CAPACITY = 400,
  LB_MAX_PLAYOUT_MOVES = 400,
  LB_RACK_STRING = 64,
};

static const uint64_t LB_GAME_SEED = 77000000;

// The bag sizes measured: each one, or a band around a target.
static bool lb_bag_is_measured(int bag_tiles) {
  if (bag_tiles >= 1 && bag_tiles <= 7) {
    return true;
  }
  return (bag_tiles >= 9 && bag_tiles <= 11) ||
         (bag_tiles >= 13 && bag_tiles <= 15) ||
         (bag_tiles >= 18 && bag_tiles <= 22) ||
         (bag_tiles >= 37 && bag_tiles <= 43) ||
         (bag_tiles >= 57 && bag_tiles <= 63) ||
         (bag_tiles >= 77 && bag_tiles <= 86);
}

typedef struct LbShared {
  Config *config;
  const PoolShiftTable *exact_table;
  int first;
  int games;
  int samples;
  int cands;
  bool exact;
  int max_bag;
  int next_game;
  long long positions;
  long long playouts;
  pthread_mutex_t mutex;
} LbShared;

typedef struct LbWorker {
  LbShared *shared;
  Game *game;
  Game *playout;
} LbWorker;

static uint64_t lb_mix(uint64_t value) {
  value ^= value >> 33;
  value *= 0xff51afd7ed558ccdULL;
  value ^= value >> 33;
  value *= 0xc4ceb9fe1a85ec53ULL;
  value ^= value >> 33;
  return value;
}

static void lb_rack_string(const Rack *rack, const LetterDistribution *ld,
                           char *out) {
  if (rack_get_total_letters(rack) == 0) {
    (void)snprintf(out, LB_RACK_STRING, "-");
    return;
  }
  StringBuilder *sb = string_builder_create();
  string_builder_add_rack(sb, rack, ld, false);
  (void)snprintf(out, LB_RACK_STRING, "%s", string_builder_peek(sb));
  string_builder_destroy(sb);
}

static double lb_exact_value(const LbShared *shared, const Game *game,
                             int mover_index, const Rack *leave) {
  if (!shared->exact) {
    return NAN;
  }
  const LetterDistribution *ld = game_get_ld(game);
  int unseen_counts[MAX_ALPHABET_SIZE];
  const Rack *opponent_rack =
      player_get_rack(game_get_player(game, 1 - mover_index));
  for (int ml = 0; ml < ld_get_size(ld); ml++) {
    unseen_counts[ml] = bag_get_letter(game_get_bag(game), (MachineLetter)ml) +
                        rack_get_letter(opponent_rack, (MachineLetter)ml);
  }
  Equity value = 0;
  if (!pool_shift_get_exact_leave_value(shared->exact_table, unseen_counts,
                                        leave, &value)) {
    return NAN;
  }
  return equity_to_double(value);
}

static double lb_spread(const Game *game, int player_index) {
  return equity_to_double(
      player_get_score(game_get_player(game, player_index)) -
      player_get_score(game_get_player(game, 1 - player_index)));
}

static void lb_measure(LbWorker *worker, MoveList *all_moves,
                       MoveList *top_move, int game_idx, int turn) {
  LbShared *shared = worker->shared;
  Game *game = worker->game;
  const LetterDistribution *ld = game_get_ld(game);
  const int mover_index = game_get_player_on_turn_index(game);
  const Player *mover = game_get_player(game, mover_index);
  const Rack *rack = player_get_rack(mover);
  const KLV *klv = player_get_klv(mover);
  const int bag_tiles = bag_get_letters(game_get_bag(game));

  const MoveGenArgs args = {
      .game = game,
      .move_list = all_moves,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
  };
  generate_moves(&args);
  SortedMoveList *sorted = sorted_move_list_create(all_moves);
  Move cands[LB_MAX_CANDS];
  Rack leaves[LB_MAX_CANDS];
  Equity klv_values[LB_MAX_CANDS];
  int num_cands = 0;
  for (int idx = 0; idx < sorted->count && num_cands < shared->cands; idx++) {
    const Move *move = sorted->moves[idx];
    if (move_get_type(move) == GAME_EVENT_PASS) {
      continue;
    }
    Rack leave;
    rack_copy(&leave, rack);
    const Equity klv_value = get_leave_value_for_move(klv, move, &leave);
    bool seen = false;
    for (int cand_idx = 0; cand_idx < num_cands; cand_idx++) {
      if (racks_are_equal(&leaves[cand_idx], &leave)) {
        seen = true;
        break;
      }
    }
    if (seen) {
      continue;
    }
    move_copy(&cands[num_cands], move);
    rack_copy(&leaves[num_cands], &leave);
    klv_values[num_cands] = klv_value;
    num_cands++;
  }
  sorted_move_list_destroy(sorted);
  if (num_cands < 2) {
    return;
  }

  StringBuilder *out = string_builder_create();
  char rack_string[LB_RACK_STRING];
  lb_rack_string(rack, ld, rack_string);
  Rack empty;
  rack_set_dist_size_and_reset(&empty, ld_get_size(ld));
  string_builder_add_formatted_string(
      out, "P %d %d %d %s %.3f\n", game_idx, turn, bag_tiles, rack_string,
      lb_exact_value(shared, game, mover_index, &empty));
  long long playouts = 0;
  for (int cand_idx = 0; cand_idx < num_cands; cand_idx++) {
    double sum = 0;
    double sum_sq = 0;
    double wins = 0;
    for (int sample_idx = 0; sample_idx < shared->samples; sample_idx++) {
      Game *playout = worker->playout;
      game_copy(playout, game);
      return_rack_to_bag(playout, 1 - mover_index);
      const uint64_t seed =
          lb_mix(((uint64_t)game_idx << 32) ^ ((uint64_t)turn << 16) ^
                 (uint64_t)sample_idx);
      game_seed(playout, seed);
      draw_to_full_rack(playout, 1 - mover_index);
      play_move(&cands[cand_idx], playout, NULL);
      int moves = 0;
      while (!game_over(playout) && moves < LB_MAX_PLAYOUT_MOVES) {
        play_move(get_top_equity_move(playout, top_move), playout, NULL);
        moves++;
      }
      const double spread = lb_spread(playout, mover_index);
      sum += spread;
      sum_sq += spread * spread;
      if (spread > 0) {
        wins += 1;
      } else if (spread == 0) {
        wins += 0.5;
      }
      playouts++;
    }
    const double mean = sum / shared->samples;
    const double variance = sum_sq / shared->samples - mean * mean;
    char leave_string[LB_RACK_STRING];
    lb_rack_string(&leaves[cand_idx], ld, leave_string);
    string_builder_add_formatted_string(
        out, "C %d %d %d %d %s %.3f %d %.3f %d %.3f %.3f %.3f %.4f\n",
        game_idx, turn, bag_tiles, cand_idx, leave_string,
        equity_to_double(klv_values[cand_idx]),
        equity_to_int(move_get_score(&cands[cand_idx])),
        equity_to_double(move_get_equity(&cands[cand_idx])),
        move_get_tiles_played(&cands[cand_idx]),
        lb_exact_value(shared, game, mover_index, &leaves[cand_idx]), mean,
        sqrt(variance > 0 ? variance : 0), wins / shared->samples);
  }
  pthread_mutex_lock(&shared->mutex);
  (void)fputs(string_builder_peek(out), stdout);
  (void)fflush(stdout);
  shared->positions++;
  shared->playouts += playouts;
  pthread_mutex_unlock(&shared->mutex);
  string_builder_destroy(out);
}

static void *lb_worker_run(void *arg) {
  LbWorker *worker = (LbWorker *)arg;
  LbShared *shared = worker->shared;
  MoveList *all_moves = move_list_create(LB_MOVE_LIST_CAPACITY);
  MoveList *top_move = move_list_create(1);
  while (true) {
    pthread_mutex_lock(&shared->mutex);
    const int game_idx = shared->first + shared->next_game++;
    pthread_mutex_unlock(&shared->mutex);
    if (game_idx >= shared->first + shared->games) {
      break;
    }
    Game *game = worker->game;
    game_reset(game);
    game_seed(game, LB_GAME_SEED + (uint64_t)game_idx);
    draw_starting_racks(game);
    int turn = 0;
    while (!game_over(game)) {
      const int bag_tiles = bag_get_letters(game_get_bag(game));
      if (lb_bag_is_measured(bag_tiles) && bag_tiles <= shared->max_bag) {
        lb_measure(worker, all_moves, top_move, game_idx, turn);
      }
      play_move(get_top_equity_move(game, top_move), game, NULL);
      turn++;
    }
  }
  move_list_destroy(top_move);
  move_list_destroy(all_moves);
  return NULL;
}

void leave_bag_run_spec(const char *spec) {
  int first = 0;
  int games = 0;
  int threads = 1;
  int samples = 0;
  int cands = 0;
  int exact = 0;
  int max_bag = LB_NO_MAX_BAG;
  const int num_fields = sscanf(
      spec, "first=%d,games=%d,threads=%d,samples=%d,cands=%d,exact=%d,"
            "maxbag=%d",
      &first, &games, &threads, &samples, &cands, &exact, &max_bag);
  if ((num_fields != LB_SPEC_FIELDS &&
       num_fields != LB_SPEC_FIELDS_WITH_MAX_BAG) ||
      threads < 1 || threads > LB_MAX_THREADS || samples < 1 || cands < 2 ||
      cands > LB_MAX_CANDS) {
    log_fatal("leavebag: bad spec: %s", spec);
  }
  Config *config = config_create_or_die(
      "set -lex CSW24 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -threads 1 -savesettings false -poolshift 0 "
      "-pegadjust none -pat none");
  Config *exact_config = NULL;
  Game *exact_game = NULL;
  LbShared shared;
  memset(&shared, 0, sizeof(shared));
  shared.config = config;
  shared.first = first;
  shared.games = games;
  shared.samples = samples;
  shared.cands = cands;
  shared.exact = exact != 0;
  shared.max_bag = max_bag;
  if (shared.exact) {
    exact_config = config_create_or_die(
        "set -lex CSW24 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
        "-numplays 1 -threads 1 -savesettings false -poolshift exact "
        "-pegadjust none -pat none");
    exact_game = config_game_create(exact_config);
    shared.exact_table =
        player_get_pool_shift_table(game_get_player(exact_game, 0));
    if (shared.exact_table == NULL ||
        shared.exact_table->exact_values == NULL) {
      log_fatal("leavebag: no rack value table");
    }
  }
  pthread_mutex_init(&shared.mutex, NULL);
  LbWorker workers[LB_MAX_THREADS];
  pthread_t thread_ids[LB_MAX_THREADS];
  for (int thread_idx = 0; thread_idx < threads; thread_idx++) {
    workers[thread_idx].shared = &shared;
    workers[thread_idx].game = config_game_create(config);
    workers[thread_idx].playout = game_duplicate(workers[thread_idx].game);
  }
  for (int thread_idx = 0; thread_idx < threads; thread_idx++) {
    pthread_create(&thread_ids[thread_idx], NULL, lb_worker_run,
                   &workers[thread_idx]);
  }
  for (int thread_idx = 0; thread_idx < threads; thread_idx++) {
    pthread_join(thread_ids[thread_idx], NULL);
  }
  printf("LB done games=%d positions=%lld playouts=%lld\n", games,
         shared.positions, shared.playouts);
  for (int thread_idx = 0; thread_idx < threads; thread_idx++) {
    game_destroy(workers[thread_idx].playout);
    game_destroy(workers[thread_idx].game);
  }
  if (exact_game != NULL) {
    game_destroy(exact_game);
  }
  if (exact_config != NULL) {
    config_destroy(exact_config);
  }
  config_destroy(config);
}
