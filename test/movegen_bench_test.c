// CPU baseline for the GPU movegen experiment.
//
// Two benches:
//   1. Single rack (AABDELT) repeated — replicates Cesar's VsMatt baseline,
//      measures hot-cache per-call cost.
//   2. Many distinct racks against the same VsMatt board — the workload the
//      GPU experiment will target. Iterates all *reachable* size-7 racks
//      given the position: per-letter cap = bag_distribution - tiles_on_board.
//      For VsMatt that's the universe of racks the on-turn player could
//      actually hold given the tiles already played.
//
// Usage: ./bin/magpie_test movegenbench
// Env vars:
//   MGBENCH_ITERS    iters for single-rack bench (default 200000; lower for
//   ALL) MGBENCH_N_RACKS  cap for many-racks bench (default 200000; "0" =
//   full 3.2M)

#include "movegen_bench_test.h"

#include "../src/def/board_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/ent/board.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/impl/config.h"
#include "../src/impl/move_gen.h"
#include "test_util.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// VsMatt position from Cesar's 2018 Lake George game vs Matt Graham, turn 10.
// Same board as VS_MATT in test_constants.h, but with the trailing
// "-lex NWL20;" stripped so the active config's lexicon controls the run.
#define VS_MATT_NO_LEX                                                         \
  "7ZEP1F3/1FLUKY3R1R3/5EX2A1U3/2SCARIEST1I3/9TOT3/6GO1LO4/6OR1ETA3/"          \
  "6JABS1b3/5QI4A3/5I1N3N3/3ReSPOND1D3/1HOE3V3O3/1ENCOMIA3N3/7T7/3VENGED6 "    \
  "/ 0/0 0"

static double monotonic_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts); // NOLINT(misc-include-cleaner)
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static long bench_iters_for(move_record_t record) {
  const char *env = getenv("MGBENCH_ITERS");
  if (env != NULL) {
    return strtol(env, NULL, 10);
  }
  // ALL is ~50x slower than BEST; scale so wall time is similar.
  return (record == MOVE_RECORD_BEST) ? 200000 : 5000;
}

static void run_one_bench(Config *config, move_record_t record,
                          const char *label) {
  Game *game = config_game_create(config);
  MoveList *move_list = move_list_create(10000);
  const LetterDistribution *ld = game_get_ld(game);

  load_cgp_or_die(game, VS_MATT_NO_LEX);
  Player *player = game_get_player(game, game_get_player_on_turn_index(game));
  rack_set_to_string(ld, player_get_rack(player), "AABDELT");

  const MoveGenArgs args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = record,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };

  // Warmup so cache state is steady.
  for (int i = 0; i < 1000; i++) {
    generate_moves(&args);
  }

  const long iters = bench_iters_for(record);
  const double t0 = monotonic_seconds();
  for (long i = 0; i < iters; i++) {
    generate_moves(&args);
  }
  const double dt = monotonic_seconds() - t0;
  printf("  %-32s %8ld iters  %7.3fs  %10.0f pos/s\n", label, iters, dt,
         (double)iters / dt);
  (void)fflush(stdout);

  move_list_destroy(move_list);
  game_destroy(game);
}

// Recursive enumeration: fills counts[letter_idx..alpha-1] with non-negative
// integers summing to `remaining`, capped per letter by the *available*
// tile budget — i.e. the bag's tile count minus what's already on the board.
// This is the universe of racks the on-turn player could actually hold given
// the current position. At each leaf, sets the player rack and runs movegen,
// accumulating a checksum to defeat dead-code elimination.
typedef struct RackBenchCtx {
  const MoveGenArgs *args;
  Rack *rack;
  uint8_t
      available[MAX_ALPHABET_SIZE]; // per-letter cap after subtracting board
  long target;                      // stop after this many leaves; 0 = no cap
  long visited;                     // leaves processed so far
  uint64_t checksum;                // sum of move_list_get_count() values
} RackBenchCtx;

static void enumerate_racks(RackBenchCtx *ctx, int letter_idx, int alpha,
                            int remaining) {
  if (ctx->target > 0 && ctx->visited >= ctx->target) {
    return;
  }
  Rack *rack = ctx->rack;
  const int bag_max = ctx->available[letter_idx];
  if (letter_idx == alpha - 1) {
    if (remaining > bag_max) {
      return; // not a reachable rack
    }
    rack->array[letter_idx] = (uint16_t)remaining;
    rack->number_of_letters = 7;
    generate_moves(ctx->args);
    ctx->checksum += (uint64_t)move_list_get_count(ctx->args->move_list);
    ctx->visited++;
    return;
  }
  const int max_c = remaining < bag_max ? remaining : bag_max;
  for (int c = 0; c <= max_c; c++) {
    rack->array[letter_idx] = (uint16_t)c;
    enumerate_racks(ctx, letter_idx + 1, alpha, remaining - c);
    if (ctx->target > 0 && ctx->visited >= ctx->target) {
      return;
    }
  }
}

// Fill out[ml] with bag_distribution[ml] minus the count of letter ml placed
// on the board (blanks-on-board count toward BLANK_MACHINE_LETTER).
static void compute_available_tiles(const Game *game, uint8_t *out) {
  const LetterDistribution *ld = game_get_ld(game);
  const Board *board = game_get_board(game);
  const int alpha = ld_get_size(ld);
  for (int ml = 0; ml < alpha; ml++) {
    out[ml] = (uint8_t)ld_get_dist(ld, (MachineLetter)ml);
  }
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      MachineLetter ml = board_get_letter(board, row, col);
      if (ml == ALPHABET_EMPTY_SQUARE_MARKER) {
        continue;
      }
      // Blanked tile: count against BLANK_MACHINE_LETTER (= 0).
      const MachineLetter ml_for_budget =
          (ml & BLANK_MASK) ? BLANK_MACHINE_LETTER : ml;
      if (out[ml_for_budget] > 0) {
        out[ml_for_budget]--;
      }
    }
  }
}

static void run_rack_batch_bench(Config *config, move_record_t record,
                                 long target, const char *label) {
  Game *game = config_game_create(config);
  MoveList *move_list = move_list_create(10000);

  load_cgp_or_die(game, VS_MATT_NO_LEX);
  Player *player = game_get_player(game, game_get_player_on_turn_index(game));
  Rack *rack = player_get_rack(player);
  rack_reset(rack);

  const MoveGenArgs args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = record,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };

  RackBenchCtx ctx = {
      .args = &args,
      .rack = rack,
      .available = {0},
      .target = target,
      .visited = 0,
      .checksum = 0,
  };
  compute_available_tiles(game, ctx.available);

  const int alpha = ld_get_size(game_get_ld(game)); // 27 for English
  const double t0 = monotonic_seconds();
  enumerate_racks(&ctx, 0, alpha, 7);
  const double dt = monotonic_seconds() - t0;

  printf("  %-32s %8ld racks %7.3fs  %10.0f racks/s  (sum=%llu)\n", label,
         ctx.visited, dt, (double)ctx.visited / dt,
         (unsigned long long)ctx.checksum);
  (void)fflush(stdout);

  move_list_destroy(move_list);
  game_destroy(game);
}

void test_movegen_bench(void) {
  printf("VsMatt + AABDELT, single position, single thread\n");
  printf("  %-32s %8s %8s  %10s\n", "configuration", "iters", "time", "pos/s");
  printf("  %-32s %8s %8s  %10s\n", "-----", "-----", "----", "-----");

  Config *cfg_wmp = config_create_or_die(
      "set -lex CSW24 -wmp true -s1 equity -s2 equity -r1 best -r2 best "
      "-numplays 1");
  run_one_bench(cfg_wmp, MOVE_RECORD_BEST, "CSW24  wmp=on   record=best");
  run_one_bench(cfg_wmp, MOVE_RECORD_ALL, "CSW24  wmp=on   record=all");
  config_destroy(cfg_wmp);

  Config *cfg_nowmp = config_create_or_die(
      "set -lex CSW24 -wmp false -s1 equity -s2 equity -r1 best -r2 best "
      "-numplays 1");
  run_one_bench(cfg_nowmp, MOVE_RECORD_BEST, "CSW24  wmp=off  record=best");
  run_one_bench(cfg_nowmp, MOVE_RECORD_ALL, "CSW24  wmp=off  record=all");
  config_destroy(cfg_nowmp);

  Config *cfg_nwl = config_create_or_die(
      "set -lex NWL20 -wmp true -s1 equity -s2 equity -r1 best -r2 best "
      "-numplays 1");
  run_one_bench(cfg_nwl, MOVE_RECORD_BEST, "NWL20  wmp=on   record=best");
  config_destroy(cfg_nwl);

  // ---- Many-racks-one-position bench ----
  // Default cap of 200k racks keeps wall time short; pass MGBENCH_N_RACKS=0
  // to enumerate all C(33,7) = 4,272,048 size-7 racks.
  const char *n_env = getenv("MGBENCH_N_RACKS");
  const long n_racks = (n_env != NULL) ? strtol(n_env, NULL, 10) : 200000;
  printf("\nVsMatt + every-rack-of-size-7, single thread\n");
  printf("  %-32s %8s %8s  %10s\n", "configuration", "racks", "time",
         "racks/s");
  printf("  %-32s %8s %8s  %10s\n", "-----", "-----", "----", "-----");

  Config *cfg_b1 = config_create_or_die(
      "set -lex CSW24 -wmp true -s1 equity -s2 equity -r1 best -r2 best "
      "-numplays 1");
  run_rack_batch_bench(cfg_b1, MOVE_RECORD_BEST, n_racks,
                       "CSW24  wmp=on   record=best");
  config_destroy(cfg_b1);

  Config *cfg_b2 = config_create_or_die(
      "set -lex CSW24 -wmp false -s1 equity -s2 equity -r1 best -r2 best "
      "-numplays 1");
  run_rack_batch_bench(cfg_b2, MOVE_RECORD_BEST, n_racks,
                       "CSW24  wmp=off  record=best");
  config_destroy(cfg_b2);
}
