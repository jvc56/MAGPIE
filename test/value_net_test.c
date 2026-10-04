#include "value_net_test.h"

#include "../src/compat/cpthread.h"
#include "../src/compat/ctime.h"
#include "../src/compat/endian_io.h"
#include "../src/def/bai_defs.h"
#include "../src/def/cpthread_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/def/value_net_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/game_timer.h"
#include "../src/ent/inference_args.h"
#include "../src/ent/inference_results.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/sim_args.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/stats.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/value_net.h"
#include "../src/ent/win_pct.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/play_chooser.h"
#include "../src/impl/simmer.h"
#include "../src/impl/value_net_coreml.h"
#include "../src/impl/value_net_features.h"
#include "../src/impl/value_net_metal.h"
#include "../src/impl/value_net_player.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  VNT_MAX_THROUGHPUT_THREADS = 32,
  VALUE_NET_PARITY_ROWS = 64,
  VALUE_NET_PARITY_REPEATS = 20,
  VNT_MAX_TURNS = 256,
  // Distillation records (vnt_distill): the board's 0/1 planes as bits,
  // padded to a multiple of 8 bytes.
  VNT_DISTILL_BOARD_BYTES = ((VALUE_NET_BOARD_FLOATS + 63) / 64) * 8,
  VNT_DISTILL_CANDIDATES = 50,
  VNT_DISTILL_PROGRESS_GAMES = 200,
  // A pass's static equity in distillation records and exploration (its
  // equity is a sentinel, not points).
  VNT_DISTILL_PASS_EQUITY = -1000,
};

// One candidate move at one decision of a teacher self-play game, as
// tools/value_net/distill reads it (little-endian, no padding).
typedef struct VntDistillRecord {
  // Input plane c, square q is bit c * 225 + q, most significant bit of
  // each byte first (numpy's unpackbits order).
  uint8_t board_bits[VNT_DISTILL_BOARD_BYTES];
  float scalars[VALUE_NET_SCALARS];
  // The teacher's outputs for this row.
  float value;
  float spread;
  // The mover's spread in points just after the move, and its static
  // equity in points (VNT_DISTILL_PASS_EQUITY for a pass).
  float spread_after;
  float equity;
  uint32_t game_id;
  uint16_t turn;
  // This move's rank by static equity, and the decision's candidate count.
  uint16_t candidate;
  uint16_t candidates;
  uint16_t bag;
  uint16_t tiles_played;
  // Whether this candidate was played.
  uint16_t chosen;
} VntDistillRecord;

static_assert(sizeof(VntDistillRecord) == VNT_DISTILL_BOARD_BYTES +
                                              (VALUE_NET_SCALARS * 4) + 16 + 4 +
                                              12,
              "VntDistillRecord must have no padding");

enum {
  // Letters in an opponent record's rack and leave: the blank, then A-Z.
  VNT_OPPONENT_LETTERS = 28,
  VNT_OPPONENT_MOVED = 1,
  VNT_OPPONENT_BINGO = 2,
  VNT_OPPONENT_EXCHANGE = 4,
  VNT_OPPONENT_PASS = 8,
};

// One decision's hidden information, written beside distillation records
// when opp=1 (<out>.opp2, <out>.t<k>.opp2 with several threads): the
// opponent's rack at the decision and the leave they kept from their last
// move (their rack before it less the tiles it played or exchanged),
// counts by machine letter (the blank is 0). Joined with the records by
// game_id and turn. Little-endian, no padding.
typedef struct VntOpponentRecord {
  uint32_t game_id;
  uint16_t turn;
  // VNT_OPPONENT_* bits: the opponent has moved (else leave is empty), and
  // whether that move was a bingo, an exchange or a pass.
  uint16_t flags;
  uint8_t rack[VNT_OPPONENT_LETTERS];
  uint8_t leave[VNT_OPPONENT_LETTERS];
  // The opponent's last move's score and tiles played or exchanged.
  int16_t last_score;
  uint16_t last_tiles;
  // Those tiles, counts by machine letter (a blank played as a letter
  // counts as the blank).
  uint8_t played[VNT_OPPONENT_LETTERS];
} VntOpponentRecord;

static_assert(sizeof(VntOpponentRecord) ==
                  4 + 2 + 2 + (3 * VNT_OPPONENT_LETTERS) + 2 + 2,
              "VntOpponentRecord must have no padding");

static uint64_t vnt_mix(uint64_t value) {
  value += UINT64_C(0x9e3779b97f4a7c15);
  value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
  return value ^ (value >> 31);
}

static value_net_backend_t vnt_parse_backend(const char *name) {
  if (strings_equal(name, "cpu")) {
    return VALUE_NET_BACKEND_CPU;
  }
  if (strings_equal(name, "fp16")) {
    return VALUE_NET_BACKEND_METAL_FP16;
  }
  if (strings_equal(name, "ane")) {
    return VALUE_NET_BACKEND_COREML;
  }
  if (strings_equal(name, "anegpu")) {
    return VALUE_NET_BACKEND_COREML_AND_METAL;
  }
  if (!strings_equal(name, "fp32")) {
    log_fatal("backend must be cpu, fp32, fp16, ane or anegpu, got %s", name);
  }
  return VALUE_NET_BACKEND_METAL_FP32;
}

// The top static-equity move for the player on turn.
static const Move *vnt_static_move(const Game *game, MoveList *list) {
  move_list_reset(list);
  const MoveGenArgs args = {
      .game = game,
      .move_list = list,
      .move_record_type = MOVE_RECORD_BEST,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&args);
  return move_list_get_move(list, 0);
}

// Reads count little-endian floats from path.
static float *vnt_read_floats(const char *path, size_t count) {
  FILE *stream = fopen_or_die(path, "rb");
  uint32_t *bits = malloc_or_die(sizeof(uint32_t) * count);
  if (!fread_le_uint32s(bits, count, stream)) {
    log_fatal("could not read %zu floats from %s", count, path);
  }
  (void)fclose(stream);
  float *values = malloc_or_die(sizeof(float) * count);
  memcpy(values, bits, sizeof(float) * count);
  free(bits);
  return values;
}

// Compares a backend's outputs with the parity set's reference outputs and
// prints the largest differences.
static void vnt_report(const char *backend, int rows, const float *value,
                       const float *spread, const float *ref_value,
                       const float *ref_spread, double seconds) {
  double max_value = 0.0;
  double max_spread = 0.0;
  for (int row = 0; row < rows; row++) {
    max_value = fmax(max_value, fabs((double)value[row] - ref_value[row]));
    max_spread = fmax(max_spread, fabs((double)spread[row] - ref_spread[row]));
  }
  printf("value_net_parity backend=%s rows=%d max_abs_value_diff=%.3g "
         "max_abs_spread_diff=%.3g seconds=%.3f\n",
         backend, rows, max_value, max_spread, seconds);
}

// "parity:<dir>:<parity_dir>[:<rows>]": the CPU forward pass on the
// parity set's input rows against its reference outputs.
static void vnt_parity(const char *dir, const char *parity_dir, int rows) {
  ErrorStack *error_stack = error_stack_create();
  ValueNet *net = value_net_create(dir, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("could not load the value net from %s", dir);
  }
  char *path = get_formatted_string("%s/board.f32", parity_dir);
  float *board = vnt_read_floats(path, (size_t)VALUE_NET_PARITY_ROWS *
                                           VALUE_NET_BOARD_FLOATS);
  free(path);
  path = get_formatted_string("%s/scalars.f32", parity_dir);
  float *scalars =
      vnt_read_floats(path, (size_t)VALUE_NET_PARITY_ROWS * VALUE_NET_SCALARS);
  free(path);
  path = get_formatted_string("%s/value.f32", parity_dir);
  float *ref_value = vnt_read_floats(path, VALUE_NET_PARITY_ROWS);
  free(path);
  path = get_formatted_string("%s/spread.f32", parity_dir);
  float *ref_spread = vnt_read_floats(path, VALUE_NET_PARITY_ROWS);
  free(path);
  float value[VALUE_NET_PARITY_ROWS];
  float spread[VALUE_NET_PARITY_ROWS];
  int64_t start = ctimer_monotonic_ns();
  value_net_evaluate_cpu(net, rows, board, scalars, value, spread);
  vnt_report("cpu", rows, value, spread, ref_value, ref_spread,
             (double)(ctimer_monotonic_ns() - start) / 1e9);
  for (int half = 0; half <= 1; half++) {
    ValueNetMetal *metal = value_net_metal_create(net, half, 1, error_stack);
    if (metal == NULL) {
      error_stack_print_and_reset(error_stack);
      continue;
    }
    // The first run compiles the graph for this batch size.
    start = ctimer_monotonic_ns();
    value_net_metal_evaluate(metal, rows, board, scalars, value, spread);
    const double first = (double)(ctimer_monotonic_ns() - start) / 1e9;
    start = ctimer_monotonic_ns();
    for (int repeat = 0; repeat < VALUE_NET_PARITY_REPEATS; repeat++) {
      value_net_metal_evaluate(metal, rows, board, scalars, value, spread);
    }
    const double seconds = (double)(ctimer_monotonic_ns() - start) / 1e9 /
                           VALUE_NET_PARITY_REPEATS;
    vnt_report(half ? "metal_fp16" : "metal_fp32", rows, value, spread,
               ref_value, ref_spread, seconds);
    printf("value_net_metal first_call_seconds=%.3f\n", first);
    value_net_metal_destroy(metal);
  }
  free(board);
  free(scalars);
  free(ref_value);
  free(ref_spread);
  value_net_destroy(net);
  error_stack_destroy(error_stack);
}

// The win% a sim gave move, or -1 if move was not among its plays.
static double vnt_sim_win_pct(const SimResults *results, const Move *move) {
  for (int play_idx = 0; play_idx < sim_results_get_number_of_plays(results);
       play_idx++) {
    const SimmedPlay *play = sim_results_get_simmed_play(results, play_idx);
    if (compare_moves_without_equity(simmed_play_get_move(play), move, true) ==
        -1) {
      return stat_get_mean(simmed_play_get_win_pct_stat(play));
    }
  }
  return -1.0;
}

// One sim of the top root_cands static plays for iterations iterations
// (round robin, ranked by win%) from seed, its first nn_plies rollout plies
// the value net's when player is set, scored by the net at the horizon with
// leaf; results is filled.
static void vnt_simcompare_sim(const Config *config, Game *game, MoveList *root,
                               ValueNetPlayer *player, int plies, int threads,
                               uint64_t iterations, int reply_cands, int batch,
                               int nn_plies, bool leaf, uint64_t seed,
                               SimResults *results, ErrorStack *error_stack) {
  ThreadControl *control = thread_control_create();
  thread_control_set_status(control, THREAD_CONTROL_STATUS_STARTED);
  SimArgs sim_args;
  sim_args_fill(plies, root, move_list_get_count(root), NULL,
                config_get_win_pcts(config), NULL, control, game, false, false,
                threads, 0, move_list_get_count(root), plies, seed, iterations,
                1, 0.0, BAI_THRESHOLD_NONE, 0.0, BAI_SAMPLING_RULE_ROUND_ROBIN,
                0.0, 1.0, 0.0, 100.0, false, NULL, &sim_args);
  if (player != NULL) {
    sim_args.rollout_value_net_evaluate = value_net_player_evaluate_rows;
    sim_args.rollout_value_net_context = player;
    sim_args.rollout_value_net_candidates = reply_cands;
    sim_args.rollout_value_net_batch = batch;
    sim_args.rollout_value_net_plies = nn_plies;
    sim_args.rollout_value_net_leaf = leaf;
  }
  simulate_without_ctx(&sim_args, results, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("sim failed");
  }
  thread_control_destroy(control);
}

// "simcompare:<model_dir>:<backend>:<plies>:<root_cands>:<threads>:
// <iterations>:<reply_cands>:<batch>:<nn_plies>:<positions>:<out>[:leaf]": on
// positions from static NWL23 games (even turns 2..20 with tiles in the
// bag), a sim with static rollouts and one whose first nn_plies rollout
// plies are the value net's (reply_cands candidates, batch iterations per
// evaluation), from the same seed, plus a second static sim from another
// seed as the sampling-noise baseline; all of the top root_cands static
// plays for iterations iterations each (round robin, ranked by win%).
// Writes <out>.csv with the picks and the win% the first two sims give both
// of theirs, and prints how often the static and net picks agree and how
// often the two static picks do. leaf 1 scores the net sim's rollouts by
// the net at the horizon.
static void vnt_simcompare(const StringSplitter *fields) {
  const int field_count = string_splitter_get_number_of_items(fields);
  if (field_count != 12 && field_count != 13) {
    log_fatal("simcompare needs 11 or 12 fields");
  }
  const bool leaf = field_count == 13 &&
                    strtol(string_splitter_get_item(fields, 12), NULL, 10) > 0;
  const char *model_dir = string_splitter_get_item(fields, 1);
  const char *backend_name = string_splitter_get_item(fields, 2);
  const int plies = (int)strtol(string_splitter_get_item(fields, 3), NULL, 10);
  const int root_cands =
      (int)strtol(string_splitter_get_item(fields, 4), NULL, 10);
  const int threads =
      (int)strtol(string_splitter_get_item(fields, 5), NULL, 10);
  const uint64_t iterations =
      strtoull(string_splitter_get_item(fields, 6), NULL, 10);
  const int reply_cands =
      (int)strtol(string_splitter_get_item(fields, 7), NULL, 10);
  const int batch = (int)strtol(string_splitter_get_item(fields, 8), NULL, 10);
  const int nn_plies =
      (int)strtol(string_splitter_get_item(fields, 9), NULL, 10);
  const int positions =
      (int)strtol(string_splitter_get_item(fields, 10), NULL, 10);
  const char *out = string_splitter_get_item(fields, 11);
  Config *config = config_create_or_die(
      "set -lex NWL23 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -threads 1");
  ErrorStack *error_stack = error_stack_create();
  config_load_win_pcts(config, error_stack);
  // Pure win%, as the sims.
  ValueNetPlayer *player =
      value_net_player_create(model_dir, vnt_parse_backend(backend_name), 0,
                              1.0, 0.0, 100.0, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("simcompare setup failed");
  }
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  MoveList *static_list = move_list_create(1);
  MoveList *root = move_list_create(root_cands);
  char *path = get_formatted_string("%s.csv", out);
  FILE *csv = fopen_or_die(path, "w");
  free(path);
  fprintf(csv, "position,game,turn,bag,agree,static_agree,static_pick,"
               "net_pick,static2_pick,static_sim_static_pick,"
               "static_sim_net_pick,net_sim_net_pick,net_sim_static_pick\n");
  StringBuilder *names[3] = {string_builder_create(), string_builder_create(),
                             string_builder_create()};
  int done = 0;
  int agreed = 0;
  int static_agreed = 0;
  double static_gap_sum = 0.0;
  double net_gap_sum = 0.0;
  for (uint64_t game_idx = 0; done < positions; game_idx++) {
    game_reset(game);
    game_seed(game, vnt_mix(UINT64_C(4141) ^ game_idx));
    draw_starting_racks(game);
    for (int turn = 0; !game_over(game) && done < positions; turn++) {
      const int bag = bag_get_letters(game_get_bag(game));
      if (turn >= 2 && turn <= 20 && turn % 2 == 0 && bag > 0) {
        move_list_reset(root);
        const MoveGenArgs args = {
            .game = game,
            .move_list = root,
            .move_record_type = MOVE_RECORD_ALL,
            .move_sort_type = MOVE_SORT_EQUITY,
            .override_kwg = NULL,
            .eq_margin_movegen = 0,
            .target_equity = EQUITY_MAX_VALUE,
            .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
        };
        generate_moves(&args);
        move_list_sort_moves(root);
        // static (seed 7), net (seed 7), static (seed 8).
        SimResults *results[3];
        Move picks[3];
        for (int sim_idx = 0; sim_idx < 3; sim_idx++) {
          results[sim_idx] = sim_results_create(0.0);
          vnt_simcompare_sim(config, game, root, sim_idx == 1 ? player : NULL,
                             plies, threads, iterations, reply_cands, batch,
                             nn_plies, leaf, sim_idx == 2 ? 8 : 7,
                             results[sim_idx], error_stack);
          move_copy(&picks[sim_idx],
                    sim_results_get_best_move(results[sim_idx]));
          string_builder_clear(names[sim_idx]);
          string_builder_add_move(names[sim_idx], game_get_board(game),
                                  &picks[sim_idx], ld, false);
        }
        const bool agree =
            compare_moves_without_equity(&picks[0], &picks[1], true) == -1;
        const bool static_agree =
            compare_moves_without_equity(&picks[0], &picks[2], true) == -1;
        const double static_static = vnt_sim_win_pct(results[0], &picks[0]);
        const double static_net = vnt_sim_win_pct(results[0], &picks[1]);
        const double net_net = vnt_sim_win_pct(results[1], &picks[1]);
        const double net_static = vnt_sim_win_pct(results[1], &picks[0]);
        fprintf(csv, "%d,%llu,%d,%d,%d,%d,%s,%s,%s,%.5f,%.5f,%.5f,%.5f\n", done,
                (unsigned long long)game_idx, turn, bag, agree ? 1 : 0,
                static_agree ? 1 : 0, string_builder_peek(names[0]),
                string_builder_peek(names[1]), string_builder_peek(names[2]),
                static_static, static_net, net_net, net_static);
        (void)fflush(csv);
        agreed += agree ? 1 : 0;
        static_agreed += static_agree ? 1 : 0;
        static_gap_sum += static_static - static_net;
        net_gap_sum += net_net - net_static;
        done++;
        if (done % 25 == 0) {
          printf("simcompare positions=%d agree=%.3f static_agree=%.3f\n", done,
                 (double)agreed / done, (double)static_agreed / done);
          (void)fflush(stdout);
        }
        for (int sim_idx = 0; sim_idx < 3; sim_idx++) {
          sim_results_destroy(results[sim_idx]);
        }
      }
      Move move;
      move_copy(&move, vnt_static_move(game, static_list));
      play_move(&move, game, NULL);
    }
  }
  printf("simcompare backend=%s plies=%d nn_plies=%d iterations=%llu "
         "positions=%d agree=%.3f static_agree=%.3f "
         "mean_gap_static_sim=%.4f mean_gap_net_sim=%.4f\n",
         backend_name, plies, nn_plies, (unsigned long long)iterations, done,
         (double)agreed / done, (double)static_agreed / done,
         static_gap_sum / done, net_gap_sum / done);
  (void)fclose(csv);
  for (int sim_idx = 0; sim_idx < 3; sim_idx++) {
    string_builder_destroy(names[sim_idx]);
  }
  move_list_destroy(root);
  move_list_destroy(static_list);
  value_net_player_destroy(player);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// Correlation of xs and ys (n values each); 0 when either is constant.
static double vnt_correlation(const double *xs, const double *ys, int n) {
  double mx = 0.0;
  double my = 0.0;
  for (int idx = 0; idx < n; idx++) {
    mx += xs[idx] / n;
    my += ys[idx] / n;
  }
  double sxy = 0.0;
  double sxx = 0.0;
  double syy = 0.0;
  for (int idx = 0; idx < n; idx++) {
    sxy += (xs[idx] - mx) * (ys[idx] - my);
    sxx += (xs[idx] - mx) * (xs[idx] - mx);
    syy += (ys[idx] - my) * (ys[idx] - my);
  }
  return sxx > 0.0 && syy > 0.0 ? sxy / sqrt(sxx * syy) : 0.0;
}

// "leafcheck:<model_dir>:<backend>:<cands>:<threads>:<iterations>:
// <positions>": a consistency check of value net sims. On positions from
// static NWL23 games (even turns 2..20 with tiles in the bag), the top cands
// static plays get the net's own win% (1 + value) / 2 and the mean win% of
// three 2-ply sims of iterations iterations, round robin: net rollouts
// scored by the net at the horizon, net rollouts scored by the win% table,
// and static rollouts. Prints, for each sim against the net's win%, the mean
// difference and absolute difference, the correlation of both within each
// position (each centered on its position's mean) and how often their best
// plays agree.
static void vnt_leafcheck(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 7) {
    log_fatal("leafcheck needs 6 fields");
  }
  const char *model_dir = string_splitter_get_item(fields, 1);
  const char *backend_name = string_splitter_get_item(fields, 2);
  const int cands = (int)strtol(string_splitter_get_item(fields, 3), NULL, 10);
  const int threads =
      (int)strtol(string_splitter_get_item(fields, 4), NULL, 10);
  const uint64_t iterations =
      strtoull(string_splitter_get_item(fields, 5), NULL, 10);
  const int positions =
      (int)strtol(string_splitter_get_item(fields, 6), NULL, 10);
  Config *config = config_create_or_die(
      "set -lex NWL23 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -threads 1");
  ErrorStack *error_stack = error_stack_create();
  config_load_win_pcts(config, error_stack);
  ValueNetPlayer *player =
      value_net_player_create(model_dir, vnt_parse_backend(backend_name), 0,
                              1.0, 0.0, 100.0, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("leafcheck setup failed");
  }
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *game = config_get_game(config);
  Game *scratch = game_duplicate(game);
  MoveList *static_list = move_list_create(1);
  MoveList *root = move_list_create(cands);
  float *board =
      malloc_or_die(sizeof(float) * (size_t)cands * VALUE_NET_BOARD_FLOATS);
  float *scalars =
      malloc_or_die(sizeof(float) * (size_t)cands * VALUE_NET_SCALARS);
  float *values = malloc_or_die(sizeof(float) * (size_t)cands);
  const int max_rows = positions * cands;
  double *net = malloc_or_die(sizeof(double) * (size_t)max_rows);
  double *sims[3];
  for (int sim_idx = 0; sim_idx < 3; sim_idx++) {
    sims[sim_idx] = malloc_or_die(sizeof(double) * (size_t)max_rows);
  }
  double *centered_net = malloc_or_die(sizeof(double) * (size_t)max_rows);
  double *centered_sim = malloc_or_die(sizeof(double) * (size_t)max_rows);
  int best_agree[3] = {0, 0, 0};
  ValueNetHistory history;
  value_net_history_reset(&history);
  int rows = 0;
  int done = 0;
  for (uint64_t game_idx = 0; done < positions; game_idx++) {
    game_reset(game);
    game_seed(game, vnt_mix(UINT64_C(5151) ^ game_idx));
    draw_starting_racks(game);
    for (int turn = 0; !game_over(game) && done < positions; turn++) {
      const int bag = bag_get_letters(game_get_bag(game));
      if (turn >= 2 && turn <= 20 && turn % 2 == 0 && bag > 0) {
        move_list_reset(root);
        const MoveGenArgs args = {
            .game = game,
            .move_list = root,
            .move_record_type = MOVE_RECORD_ALL,
            .move_sort_type = MOVE_SORT_EQUITY,
            .override_kwg = NULL,
            .eq_margin_movegen = 0,
            .target_equity = EQUITY_MAX_VALUE,
            .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
        };
        generate_moves(&args);
        move_list_sort_moves(root);
        const int count = move_list_get_count(root);
        for (int move_idx = 0; move_idx < count; move_idx++) {
          value_net_features_for_move(
              game, move_list_get_move(root, move_idx), &history, scratch,
              board + ((size_t)move_idx * VALUE_NET_BOARD_FLOATS),
              scalars + ((size_t)move_idx * VALUE_NET_SCALARS));
        }
        value_net_player_evaluate_rows(player, count, board, scalars, values,
                                       NULL);
        for (int sim_idx = 0; sim_idx < 3; sim_idx++) {
          SimResults *results = sim_results_create(0.0);
          vnt_simcompare_sim(config, game, root, sim_idx < 2 ? player : NULL, 2,
                             threads, iterations, 8, 8, 2, sim_idx == 0, 7,
                             results, error_stack);
          for (int move_idx = 0; move_idx < count; move_idx++) {
            sims[sim_idx][rows + move_idx] =
                vnt_sim_win_pct(results, move_list_get_move(root, move_idx));
          }
          sim_results_destroy(results);
        }
        int net_best = 0;
        int sim_best[3] = {0, 0, 0};
        for (int move_idx = 0; move_idx < count; move_idx++) {
          net[rows + move_idx] = (1.0 + (double)values[move_idx]) / 2.0;
          if (net[rows + move_idx] > net[rows + net_best]) {
            net_best = move_idx;
          }
          for (int sim_idx = 0; sim_idx < 3; sim_idx++) {
            if (sims[sim_idx][rows + move_idx] >
                sims[sim_idx][rows + sim_best[sim_idx]]) {
              sim_best[sim_idx] = move_idx;
            }
          }
        }
        printf("leafcheck position=%d bag=%d net:", done, bag);
        for (int move_idx = 0; move_idx < count; move_idx++) {
          printf(" %.3f", net[rows + move_idx]);
        }
        printf(" | netleaf:");
        for (int move_idx = 0; move_idx < count; move_idx++) {
          printf(" %.3f", sims[0][rows + move_idx]);
        }
        printf("\n");
        (void)fflush(stdout);
        for (int sim_idx = 0; sim_idx < 3; sim_idx++) {
          best_agree[sim_idx] += sim_best[sim_idx] == net_best ? 1 : 0;
        }
        rows += count;
        done++;
      }
      Move move;
      move_copy(&move, vnt_static_move(game, static_list));
      play_move(&move, game, NULL);
    }
  }
  const char *sim_names[3] = {"net rollouts, net leaf",
                              "net rollouts, table leaf",
                              "static rollouts, table leaf"};
  for (int sim_idx = 0; sim_idx < 3; sim_idx++) {
    double diff = 0.0;
    double abs_diff = 0.0;
    for (int row = 0; row < rows; row++) {
      diff += (sims[sim_idx][row] - net[row]) / rows;
      abs_diff += fabs(sims[sim_idx][row] - net[row]) / rows;
    }
    // Center each position's values on its mean.
    int row = 0;
    for (int position = 0; position < done; position++) {
      const int count = rows / done;
      double net_mean = 0.0;
      double sim_mean = 0.0;
      for (int idx = 0; idx < count; idx++) {
        net_mean += net[row + idx] / count;
        sim_mean += sims[sim_idx][row + idx] / count;
      }
      for (int idx = 0; idx < count; idx++) {
        centered_net[row + idx] = net[row + idx] - net_mean;
        centered_sim[row + idx] = sims[sim_idx][row + idx] - sim_mean;
      }
      row += count;
    }
    printf("leafcheck %s vs net: mean_diff=%+.4f mean_abs_diff=%.4f "
           "corr=%.3f within_position_corr=%.3f best_agree=%.2f\n",
           sim_names[sim_idx], diff, abs_diff,
           vnt_correlation(sims[sim_idx], net, rows),
           vnt_correlation(centered_sim, centered_net, rows),
           (double)best_agree[sim_idx] / done);
  }
  free(board);
  free(scalars);
  free(values);
  free(net);
  for (int sim_idx = 0; sim_idx < 3; sim_idx++) {
    free(sims[sim_idx]);
  }
  free(centered_net);
  free(centered_sim);
  game_destroy(scratch);
  move_list_destroy(static_list);
  move_list_destroy(root);
  value_net_player_destroy(player);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// "priorfit:<model_dir>:<backend>:<threads>:<iterations>:<positions>:<out>
// [:hist[:infer]]":
// data for fitting the net prior (PlayChooserStrategy.
// sim_net_prior_iterations) as games use it. On positions from static
// NWL23 games (even turns 2..20 with tiles in the bag), with PAT, the 8
// plays of the PAT top 64 the net rates best by MAGPIE's utility (1, 0.5,
// 100) are simmed 2 plies, net rollouts (PAT top 8) scored by the net at
// the horizon, iterations iterations each, round robin; with hist 1, the
// net sees each player's history from the game, and with infer 1, opponent
// racks are drawn from an inference of their last move. Writes <out>.csv:
// each play's net utility and its sim's mean, variance and iterations.
static void vnt_priorfit(const StringSplitter *fields) {
  const int field_count = string_splitter_get_number_of_items(fields);
  if (field_count < 7 || field_count > 9) {
    log_fatal("priorfit needs 6 to 8 fields");
  }
  // infer 1: opponent racks drawn from an inference of their last move.
  const bool use_inference =
      field_count == 9 &&
      strtol(string_splitter_get_item(fields, 8), NULL, 10) > 0;
  // hist 1: each player's value net history from the game, as in games;
  // otherwise empty.
  const bool use_history =
      field_count == 8 &&
      strtol(string_splitter_get_item(fields, 7), NULL, 10) > 0;
  const char *model_dir = string_splitter_get_item(fields, 1);
  const char *backend_name = string_splitter_get_item(fields, 2);
  const int threads =
      (int)strtol(string_splitter_get_item(fields, 3), NULL, 10);
  const uint64_t iterations =
      strtoull(string_splitter_get_item(fields, 4), NULL, 10);
  const int positions =
      (int)strtol(string_splitter_get_item(fields, 5), NULL, 10);
  const char *out = string_splitter_get_item(fields, 6);
  enum { POOL = 64, KEEP = 8 };
  Config *config = config_create_or_die(
      "set -lex NWL23 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -threads 1 -pat NWL23");
  ErrorStack *error_stack = error_stack_create();
  config_load_win_pcts(config, error_stack);
  ValueNetPlayer *player =
      value_net_player_create(model_dir, vnt_parse_backend(backend_name), 0,
                              1.0, 0.5, 100.0, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("priorfit setup failed");
  }
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *game = config_get_game(config);
  Game *scratch = game_duplicate(game);
  Game *before_last_move = game_duplicate(game);
  Move last_move;
  InferenceResults *inference_results = inference_results_create(NULL);
  const int ld_size = ld_get_size(game_get_ld(game));
  Rack played_tiles;
  Rack target_known_rack;
  Rack nontarget_known_rack;
  MoveList *static_list = move_list_create(1);
  MoveList *pool = move_list_create(POOL);
  MoveList *root = move_list_create(KEEP);
  float *board = malloc_or_die(sizeof(float) * POOL * VALUE_NET_BOARD_FLOATS);
  float *scalars = malloc_or_die(sizeof(float) * POOL * VALUE_NET_SCALARS);
  float values[POOL];
  float spreads[POOL];
  double utility[POOL];
  int order[POOL];
  ValueNetHistory histories[2];
  char *path = get_formatted_string("%s.csv", out);
  FILE *csv = fopen_or_die(path, "w");
  free(path);
  fprintf(csv, "position,bag,rank,prior,mean,variance,iterations\n");
  int done = 0;
  for (uint64_t game_idx = 0; done < positions; game_idx++) {
    game_reset(game);
    game_seed(game, vnt_mix(UINT64_C(6161) ^ game_idx));
    draw_starting_racks(game);
    value_net_history_reset(&histories[0]);
    value_net_history_reset(&histories[1]);
    move_set_as_pass(&last_move);
    for (int turn = 0; !game_over(game) && done < positions; turn++) {
      const int mover = game_get_player_on_turn_index(game);
      ValueNetHistory empty_history;
      value_net_history_reset(&empty_history);
      const ValueNetHistory *own =
          use_history ? &histories[mover] : &empty_history;
      const ValueNetHistory *replier =
          use_history ? &histories[1 - mover] : &empty_history;
      const int bag = bag_get_letters(game_get_bag(game));
      if (turn >= 2 && turn <= 20 && turn % 2 == 0 && bag > 0) {
        move_list_reset(pool);
        const MoveGenArgs args = {
            .game = game,
            .move_list = pool,
            .move_record_type = MOVE_RECORD_ALL,
            .move_sort_type = MOVE_SORT_EQUITY,
            .override_kwg = NULL,
            .eq_margin_movegen = 0,
            .target_equity = EQUITY_MAX_VALUE,
            .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
        };
        generate_moves(&args);
        move_list_sort_moves(pool);
        const int count = move_list_get_count(pool);
        for (int move_idx = 0; move_idx < count; move_idx++) {
          const Move *move = move_list_get_move(pool, move_idx);
          value_net_features_for_move(
              game, move, own, scratch,
              board + ((size_t)move_idx * VALUE_NET_BOARD_FLOATS),
              scalars + ((size_t)move_idx * VALUE_NET_SCALARS));
        }
        value_net_player_evaluate_rows(player, count, board, scalars, values,
                                       spreads);
        for (int move_idx = 0; move_idx < count; move_idx++) {
          utility[move_idx] =
              value_net_utility(values[move_idx], spreads[move_idx],
                                value_net_spread_after_move(
                                    game, move_list_get_move(pool, move_idx)),
                                1.0, 0.5, 100.0);
          int pos = move_idx;
          while (pos > 0 && utility[order[pos - 1]] < utility[move_idx]) {
            order[pos] = order[pos - 1];
            pos--;
          }
          order[pos] = move_idx;
        }
        const int keep = count < KEEP ? count : KEEP;
        if (keep >= 2) {
          move_list_reset(root);
          for (int rank = 0; rank < keep; rank++) {
            move_list_add_move(root, move_list_get_move(pool, order[rank]));
          }
          ThreadControl *control = thread_control_create();
          thread_control_set_status(control, THREAD_CONTROL_STATUS_STARTED);
          // The opponent's last move: tiles played, or tiles exchanged.
          bool infer = use_inference;
          int num_exchanged = 0;
          rack_set_dist_size_and_reset(&played_tiles, ld_size);
          rack_set_dist_size_and_reset(&target_known_rack, ld_size);
          rack_set_dist_size_and_reset(&nontarget_known_rack, ld_size);
          if (move_get_type(&last_move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
            for (int tile_idx = 0; tile_idx < move_get_tiles_length(&last_move);
                 tile_idx++) {
              const MachineLetter ml = move_get_tile(&last_move, tile_idx);
              if (ml != PLAYED_THROUGH_MARKER) {
                rack_add_letter(&played_tiles,
                                get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml);
              }
            }
          } else if (move_get_type(&last_move) == GAME_EVENT_EXCHANGE &&
                     bag + RACK_SIZE >= RACK_SIZE * 2) {
            num_exchanged = move_get_tiles_played(&last_move);
          } else {
            infer = false;
          }
          InferenceArgs inference_args;
          if (infer) {
            rack_copy(&nontarget_known_rack,
                      player_get_rack(game_get_player(game, mover)));
            infer_args_fill(&inference_args, 20, 0, NULL, before_last_move,
                            threads, 0, 0, control, false, true, 1 - mover,
                            move_get_score(&last_move), num_exchanged,
                            &played_tiles, &target_known_rack,
                            &nontarget_known_rack);
          }
          SimArgs sim_args;
          sim_args_fill(
              2, root, keep, NULL, config_get_win_pcts(config),
              infer ? inference_results : NULL, control, game, infer, false,
              threads, 0, keep, 2, vnt_mix(UINT64_C(77) ^ (uint64_t)done),
              iterations * (uint64_t)keep, 1, 0.0, BAI_THRESHOLD_NONE, 0.0,
              BAI_SAMPLING_RULE_ROUND_ROBIN, 0.0, 1.0, 0.5, 100.0, false,
              infer ? &inference_args : NULL, &sim_args);
          sim_args.rollout_value_net_evaluate = value_net_player_evaluate_rows;
          sim_args.rollout_value_net_context = player;
          sim_args.rollout_value_net_candidates = 8;
          sim_args.rollout_value_net_batch = 8;
          sim_args.rollout_value_net_plies = 2;
          sim_args.rollout_value_net_leaf = true;
          sim_args.rollout_value_net_history = *replier;
          sim_args.rollout_value_net_own_history = *own;
          SimResults *results = sim_results_create(0.0);
          simulate_without_ctx(&sim_args, results, error_stack);
          if (!error_stack_is_empty(error_stack)) {
            error_stack_print_and_reset(error_stack);
            log_fatal("priorfit sim failed");
          }
          thread_control_destroy(control);
          for (int rank = 0; rank < keep; rank++) {
            const Move *move = move_list_get_move(pool, order[rank]);
            for (int play_idx = 0;
                 play_idx < sim_results_get_number_of_plays(results);
                 play_idx++) {
              const SimmedPlay *play =
                  sim_results_get_simmed_play(results, play_idx);
              if (compare_moves_without_equity(simmed_play_get_move(play), move,
                                               true) != -1) {
                continue;
              }
              const Stat *stat = simmed_play_get_utility_stat(play);
              fprintf(csv, "%d,%d,%d,%.6f,%.6f,%.8f,%llu\n", done, bag, rank,
                      utility[order[rank]], stat_get_mean(stat),
                      stat_get_variance(stat),
                      (unsigned long long)stat_get_num_samples(stat));
            }
          }
          (void)fflush(csv);
          if (infer) {
            StringBuilder *last_sb = string_builder_create();
            string_builder_add_move(last_sb, game_get_board(before_last_move),
                                    &last_move, game_get_ld(game), false);
            printf(
                "priorfit position=%d last_move=%s inferred_leaves=%llu\n",
                done, string_builder_peek(last_sb),
                (unsigned long long)sim_results_get_num_infer_leaves(results));
            string_builder_destroy(last_sb);
          }
          sim_results_destroy(results);
          done++;
          if (done % 10 == 0) {
            printf("priorfit positions=%d\n", done);
            (void)fflush(stdout);
          }
        }
      }
      Move move;
      move_copy(&move, vnt_static_move(game, static_list));
      value_net_history_record_opponent_move(&histories[1 - mover], &move);
      game_copy(before_last_move, game);
      move_copy(&last_move, &move);
      play_move(&move, game, NULL);
    }
  }
  (void)fclose(csv);
  free(board);
  free(scalars);
  game_destroy(scratch);
  game_destroy(before_last_move);
  inference_results_destroy(inference_results);
  move_list_destroy(static_list);
  move_list_destroy(pool);
  move_list_destroy(root);
  value_net_player_destroy(player);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// "xsprobe": prints MAGPIE's cross sets in both directions around CAT
// played across at 8H, to pin down which direction holds which constraint.
static void vnt_cross_set_probe(void) {
  Config *config = config_create_or_die(
      "set -lex NWL23 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/7CAT5/15/15/15/15/15/15/15 / 0/0 0");
  const Game *game = config_get_game(config);
  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  const int squares[3][2] = {{7, 10}, {6, 7}, {7, 8}};
  for (int idx = 0; idx < 3; idx++) {
    for (int dir = 0; dir < 2; dir++) {
      const uint64_t cross_set =
          board_get_cross_set(board, squares[idx][0], squares[idx][1], dir, 0);
      printf("square (%d,%d) dir %d:", squares[idx][0], squares[idx][1], dir);
      for (int ml = 1; ml <= 26; ml++) {
        if ((cross_set >> ml) & 1) {
          char *letter = ld_ml_to_hl(ld, (MachineLetter)ml);
          printf("%s", letter);
          free(letter);
        }
      }
      printf("\n");
    }
  }
  config_destroy(config);
}

typedef enum {
  VNT_PLAYER_NN,
  VNT_PLAYER_STATIC,
  VNT_PLAYER_SIM,
  VNT_PLAYER_SIM_NN,
} vnt_player_t;

static vnt_player_t vnt_parse_player(const char *name) {
  if (strings_equal(name, "nn")) {
    return VNT_PLAYER_NN;
  }
  if (strings_equal(name, "static")) {
    return VNT_PLAYER_STATIC;
  }
  if (strings_equal(name, "sim")) {
    return VNT_PLAYER_SIM;
  }
  if (!strings_equal(name, "simnn")) {
    log_fatal("player must be nn, static, sim or simnn, got %s", name);
  }
  return VNT_PLAYER_SIM_NN;
}

// The value of key=value among fields[first..], player-specific keys
// (<key>_a / <key>_b) first; fallback when absent.
static const char *vnt_option(const StringSplitter *fields, int first,
                              const char *key, int player_idx,
                              const char *fallback) {
  char *specific =
      get_formatted_string("%s_%c=", key, player_idx == 0 ? 'a' : 'b');
  char *shared = get_formatted_string("%s=", key);
  const char *found = NULL;
  for (int pass = 0; pass < 2 && found == NULL; pass++) {
    const char *prefix = pass == 0 ? specific : shared;
    for (int idx = first; idx < string_splitter_get_number_of_items(fields);
         idx++) {
      const char *item = string_splitter_get_item(fields, idx);
      if (has_prefix(prefix, item)) {
        found = item + strlen(prefix);
        break;
      }
    }
  }
  free(specific);
  free(shared);
  return found != NULL ? found : fallback;
}

// vnt_option as a number; fallback when absent.
static double vnt_option_double(const StringSplitter *fields, int first,
                                const char *key, int player_idx,
                                double fallback) {
  const char *text = vnt_option(fields, first, key, player_idx, NULL);
  return text != NULL ? strtod(text, NULL) : fallback;
}

// "games:<model_dir>:<backend>:<lexicon>:<pairs>:<seed>:<worker>:<workers>:
// <out>[:key=value...]": game pairs between players a and b (a=, b=: nn,
// static, sim with static rollouts, simnn with value net plays on the
// first nnplies= rollout plies, default 1; default nn vs static). Sim
// players take plies=, cands= (root candidates), ms= (per move) and iters=
// (cap), rcands= and batch= (value net plays: candidates per position and
// iterations per evaluation). clock= gives each player a game clock in
// seconds, from which a sim player without ms= budgets each move
// (PlayChooser; after its flag falls it plays static). A sim player plays
// endgames static, or with egplies= > 0 by an endgame search that many
// plies deep; with pre=static it plays static until the bag is empty. leaf=1
// scores a simnn player's rollouts by the net at the horizon when it chose
// the final ply (nnplies= equal to plies=). pool= > 0 has a simnn player sim
// the cands= plays its net rates best among the top pool= static plays, and
// prior= > 0 counts each one's net utility as that many sim iterations.
// infer=1 draws a sim player's opponent racks from an inference of the
// opponent's last move (imargin= its equity margin in points, default 0).
// uwin=, uspread= and uscale= set the utility (as -uwin, -uspread,
// -uspreadscale; MAGPIE's defaults otherwise) that an nn player and each sim,
// value net replies included, rank by. model= sets the net's directory (default
// <model_dir>). pat=<name> ranks a player's moves by static equity with that
// PAT term (one file for both players). An nn player scores the top cands=
// static moves (default 50); with rescore=<dir> it cascades: the net at <dir>
// rescores its top rescore_top= (default 5) candidates and decides. Any key may
// be given per player as <key>_a or <key>_b. Both games of a pair use one seed
// and the players swap seats, so each seat draws the same tiles. Writes
// <out>.games.csv and per-decision timing to <out>.moves.csv.
static void vnt_games(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) < 9) {
    log_fatal("games needs at least 8 fields");
  }
  const char *model_dir = string_splitter_get_item(fields, 1);
  const value_net_backend_t backend =
      vnt_parse_backend(string_splitter_get_item(fields, 2));
  const char *lexicon = string_splitter_get_item(fields, 3);
  const long pairs = strtol(string_splitter_get_item(fields, 4), NULL, 10);
  const uint64_t seed = strtoull(string_splitter_get_item(fields, 5), NULL, 10);
  const long worker = strtol(string_splitter_get_item(fields, 6), NULL, 10);
  const long workers = strtol(string_splitter_get_item(fields, 7), NULL, 10);
  const char *out = string_splitter_get_item(fields, 8);
  // A player with pat=<name> ranks its moves by static equity with that PAT
  // term; both seats load the one file and each turn switches it on for the
  // player to move only (the players swap seats).
  const char *pat_names[2] = {vnt_option(fields, 9, "pat", 0, NULL),
                              vnt_option(fields, 9, "pat", 1, NULL)};
  if (pat_names[0] != NULL && pat_names[1] != NULL &&
      !strings_equal(pat_names[0], pat_names[1])) {
    log_fatal("pat_a and pat_b must name the same file");
  }
  const char *pat_name = pat_names[0] != NULL ? pat_names[0] : pat_names[1];
  char *settings = get_formatted_string(
      "set -lex %s -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -threads 1%s%s%s",
      lexicon, pat_name != NULL ? " -pat " : "",
      pat_name != NULL ? pat_name : "",
      vnt_option_double(fields, 9, "tables", 0, 0.0) > 0
          ? " -rit true -wit true -ritmmap true"
          : "");
  Config *config = config_create_or_die(settings);
  free(settings);
  ErrorStack *error_stack = error_stack_create();
  config_load_win_pcts(config, error_stack);
  // The game exists from here on (players' choosers keep pointers to its
  // copies).
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  vnt_player_t kinds[2];
  double utility_w_winpct[2];
  double utility_w_spread[2];
  double utility_spread_scale[2];
  for (int player_idx = 0; player_idx < 2; player_idx++) {
    kinds[player_idx] =
        vnt_parse_player(vnt_option(fields, 9, player_idx == 0 ? "a" : "b", 2,
                                    player_idx == 0 ? "nn" : "static"));
    utility_w_winpct[player_idx] = vnt_option_double(
        fields, 9, "uwin", player_idx, config_get_utility_w_winpct(config));
    utility_w_spread[player_idx] = vnt_option_double(
        fields, 9, "uspread", player_idx, config_get_utility_w_spread(config));
    utility_spread_scale[player_idx] =
        vnt_option_double(fields, 9, "uscale", player_idx,
                          config_get_utility_spread_scale(config));
  }
  // Each player using the net has its own, with its own utility.
  ValueNetPlayer *players[2] = {NULL, NULL};
  for (int player_idx = 0; player_idx < 2; player_idx++) {
    if (kinds[player_idx] == VNT_PLAYER_NN ||
        kinds[player_idx] == VNT_PLAYER_SIM_NN) {
      // An nn player's cands= caps its candidates (default 50); a sim
      // player's is its root candidates.
      const int candidates =
          kinds[player_idx] == VNT_PLAYER_NN
              ? (int)vnt_option_double(fields, 9, "cands", player_idx, 0.0)
              : 0;
      players[player_idx] = value_net_player_create(
          vnt_option(fields, 9, "model", player_idx, model_dir), backend,
          candidates, utility_w_winpct[player_idx],
          utility_w_spread[player_idx], utility_spread_scale[player_idx],
          error_stack);
    }
  }
  // An nn player with rescore=<model_dir> cascades (see
  // value_net_player_set_rescorer).
  ValueNetPlayer *rescorers[2] = {NULL, NULL};
  for (int player_idx = 0; player_idx < 2; player_idx++) {
    const char *rescore_dir =
        vnt_option(fields, 9, "rescore", player_idx, NULL);
    if (kinds[player_idx] != VNT_PLAYER_NN || players[player_idx] == NULL ||
        rescore_dir == NULL) {
      continue;
    }
    rescorers[player_idx] = value_net_player_create(
        rescore_dir, backend, 0, utility_w_winpct[player_idx],
        utility_w_spread[player_idx], utility_spread_scale[player_idx],
        error_stack);
    if (rescorers[player_idx] != NULL) {
      value_net_player_set_rescorer(
          players[player_idx], rescorers[player_idx],
          (int)vnt_option_double(fields, 9, "rescore_top", player_idx, 5.0));
    }
  }
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("could not set up the players");
  }
  // The replier's and the sim player's own histories for each sim player's
  // rollouts, refreshed before each of its decisions.
  ValueNetHistory rollout_histories[2];
  ValueNetHistory rollout_own_histories[2];
  // Each player's game clock in seconds (clock=; 0: untimed), run for every
  // decision and budgeted from by sim players without ms= (PlayChooser).
  double clock_seconds[2];
  // The position before the last move and the move, for sim players'
  // opponent rack inference (infer=1).
  Game *before_last_move = game_duplicate(config_get_game(config));
  Move last_move;
  move_set_as_pass(&last_move);
  GameTimer game_timer;
  game_timer_reset(&game_timer, 0.0);
  PlayChooser *choosers[2] = {NULL, NULL};
  for (int player_idx = 0; player_idx < 2; player_idx++) {
    value_net_history_reset(&rollout_histories[player_idx]);
    value_net_history_reset(&rollout_own_histories[player_idx]);
    clock_seconds[player_idx] =
        vnt_option_double(fields, 9, "clock", player_idx, 0.0);
    if (kinds[player_idx] != VNT_PLAYER_SIM &&
        kinds[player_idx] != VNT_PLAYER_SIM_NN) {
      continue;
    }
    const double ms =
        strtod(vnt_option(fields, 9, "ms", player_idx, "0"), NULL);
    const int endgame_plies =
        (int)vnt_option_double(fields, 9, "egplies", player_idx, 0.0);
    const int candidate_pool =
        (int)vnt_option_double(fields, 9, "pool", player_idx, 0.0);
    // pre=static plays static while tiles remain (with egplies= for the
    // endgame).
    const bool pre_static = strings_equal(
        vnt_option(fields, 9, "pre", player_idx, "sim"), "static");
    const PlayChooserStrategy strategy = {
        .pre_endgame_eval =
            pre_static ? PLAY_CHOOSER_EVAL_STATIC : PLAY_CHOOSER_EVAL_SIM,
        // egplies= > 0 solves endgames that many plies deep (within the
        // move's budget); 0 plays them static.
        .endgame_eval = endgame_plies > 0 ? PLAY_CHOOSER_EVAL_ENDGAME
                                          : PLAY_CHOOSER_EVAL_STATIC,
        .endgame_plies = endgame_plies,
        .sim_plies = (int)strtol(
            vnt_option(fields, 9, "plies", player_idx, "2"), NULL, 10),
        .sim_max_candidates = (int)strtol(
            vnt_option(fields, 9, "cands", player_idx, "15"), NULL, 10),
        .sim_max_iterations =
            strtoull(vnt_option(fields, 9, "iters", player_idx, "0"), NULL, 10),
        // ms= is a flat budget; otherwise a clock budgets each move, and an
        // iteration cap alone gets an ample time budget.
        .fixed_seconds_per_move =
            ms > 0 ? ms / 1000.0
                   : (clock_seconds[player_idx] > 0 ? 0.0 : 600.0),
        .game_timer = &game_timer,
        .win_pcts = config_get_win_pcts(config),
        .num_threads = (int)strtol(
            vnt_option(fields, 9, "threads", player_idx, "1"), NULL, 10),
        .utility_w_winpct = utility_w_winpct[player_idx],
        .utility_w_spread = utility_w_spread[player_idx],
        .utility_spread_scale = utility_spread_scale[player_idx],
        .seed = seed + (uint64_t)player_idx,
        .rollout_value_net_evaluate = kinds[player_idx] == VNT_PLAYER_SIM_NN
                                          ? value_net_player_evaluate_rows
                                          : NULL,
        .rollout_value_net_context =
            kinds[player_idx] == VNT_PLAYER_SIM_NN ? players[player_idx] : NULL,
        .rollout_value_net_candidates = (int)strtol(
            vnt_option(fields, 9, "rcands", player_idx, "15"), NULL, 10),
        .rollout_value_net_batch = (int)strtol(
            vnt_option(fields, 9, "batch", player_idx, "8"), NULL, 10),
        .rollout_value_net_plies = (int)strtol(
            vnt_option(fields, 9, "nnplies", player_idx, "1"), NULL, 10),
        .rollout_value_net_leaf =
            vnt_option_double(fields, 9, "leaf", player_idx, 0.0) > 0,
        .sim_candidate_value_net_evaluate =
            kinds[player_idx] == VNT_PLAYER_SIM_NN && candidate_pool > 0
                ? value_net_player_evaluate_rows
                : NULL,
        .sim_candidate_value_net_context =
            kinds[player_idx] == VNT_PLAYER_SIM_NN ? players[player_idx] : NULL,
        .sim_candidate_pool = candidate_pool,
        .sim_net_prior_iterations =
            vnt_option_double(fields, 9, "prior", player_idx, 0.0),
        .sim_inference_game =
            vnt_option_double(fields, 9, "infer", player_idx, 0.0) > 0
                ? before_last_move
                : NULL,
        .sim_inference_move = &last_move,
        .sim_inference_margin = double_to_equity(
            vnt_option_double(fields, 9, "imargin", player_idx, 0.0)),
        .rollout_value_net_history = &rollout_histories[player_idx],
        .rollout_value_net_own_history = &rollout_own_histories[player_idx],
    };
    choosers[player_idx] = play_chooser_create(&strategy);
  }
  MoveList *static_list = move_list_create(1);
  char *path = get_formatted_string("%s.games.csv", out);
  FILE *games_out = fopen_or_die(path, "w");
  free(path);
  path = get_formatted_string("%s.moves.csv", out);
  FILE *moves_out = fopen_or_die(path, "w");
  free(path);
  (void)fprintf(games_out,
                "pair,game,a_seat,a_score,b_score,a_spread,a_win,turns,"
                "a_seconds,b_seconds\n");
  (void)fprintf(moves_out, "pair,game,turn,player,bag,total_ms,sim_iterations,"
                           "move,static_move,static_agree\n");
  StringBuilder *move_names[2] = {string_builder_create(),
                                  string_builder_create()};
  Game *game = config_get_game(config);
  ValueNetHistory histories[2];
  for (long pair_idx = 0; pair_idx < pairs; pair_idx++) {
    if (pair_idx % workers != worker) {
      continue;
    }
    const uint64_t pair_seed = vnt_mix(seed ^ vnt_mix((uint64_t)pair_idx));
    for (int game_in_pair = 0; game_in_pair < 2; game_in_pair++) {
      const int a_seat = game_in_pair;
      game_reset(game);
      game_seed(game, pair_seed);
      game_set_starting_player_index(game, (int)(pair_idx % 2));
      draw_starting_racks(game);
      value_net_history_reset(&histories[0]);
      value_net_history_reset(&histories[1]);
      game_timer_reset_for_players(&game_timer, clock_seconds[a_seat],
                                   clock_seconds[1 - a_seat]);
      move_set_as_pass(&last_move);
      int turn = 0;
      while (!game_over(game) && turn < VNT_MAX_TURNS) {
        const int seat = game_get_player_on_turn_index(game);
        const int player_idx = seat == a_seat ? 0 : 1;
        if (pat_name != NULL) {
          player_set_pat_usage(game_get_player(game, seat),
                               pat_names[player_idx] == NULL, 0);
        }
        const int64_t start = ctimer_monotonic_ns();
        game_timer_start_turn(&game_timer, seat);
        uint64_t sim_iterations = 0;
        Move move;
        switch (kinds[player_idx]) {
        case VNT_PLAYER_NN:
          move_copy(&move, value_net_player_choose(players[player_idx], game,
                                                   &histories[seat]));
          break;
        case VNT_PLAYER_STATIC:
          move_copy(&move, vnt_static_move(game, static_list));
          break;
        case VNT_PLAYER_SIM:
        case VNT_PLAYER_SIM_NN:
          rollout_histories[player_idx] = histories[1 - seat];
          rollout_own_histories[player_idx] = histories[seat];
          // The process-wide counters give this decision's sim iterations
          // (decisions here are sequential).
          play_chooser_benchmark_reset();
          play_chooser_choose_move(choosers[player_idx], game, &move,
                                   error_stack);
          PlayChooserBenchmarkStats chooser_stats;
          play_chooser_benchmark_get(&chooser_stats);
          sim_iterations = chooser_stats.sim_iterations;
          if (!error_stack_is_empty(error_stack)) {
            error_stack_print_and_reset(error_stack);
            log_fatal("sim player failed");
          }
          break;
        }
        game_timer_end_turn(&game_timer);
        const double total_ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
        // A sim player's move against the plain static move (no PAT), as
        // the static player would play it, off the clock.
        string_builder_clear(move_names[0]);
        string_builder_clear(move_names[1]);
        int static_agree = -1;
        if (kinds[player_idx] == VNT_PLAYER_SIM ||
            kinds[player_idx] == VNT_PLAYER_SIM_NN) {
          Player *mover = game_get_player(game, seat);
          if (pat_name != NULL) {
            player_set_pat_usage(mover, true, 0);
          }
          Move static_move;
          move_copy(&static_move, vnt_static_move(game, static_list));
          if (pat_name != NULL) {
            player_set_pat_usage(mover, pat_names[player_idx] == NULL, 0);
          }
          static_agree =
              compare_moves_without_equity(&move, &static_move, true) == -1;
          string_builder_add_move(move_names[0], game_get_board(game), &move,
                                  game_get_ld(game), false);
          string_builder_add_move(move_names[1], game_get_board(game),
                                  &static_move, game_get_ld(game), false);
        }
        (void)fprintf(moves_out, "%ld,%d,%d,%c,%d,%.3f,%llu,\"%s\",\"%s\",%d\n",
                      pair_idx, game_in_pair, turn, player_idx == 0 ? 'a' : 'b',
                      bag_get_letters(game_get_bag(game)), total_ms,
                      (unsigned long long)sim_iterations,
                      string_builder_peek(move_names[0]),
                      string_builder_peek(move_names[1]), static_agree);
        value_net_history_record_opponent_move(&histories[1 - seat], &move);
        game_copy(before_last_move, game);
        move_copy(&last_move, &move);
        play_move(&move, game, NULL);
        turn++;
      }
      const int a_score =
          equity_to_int(player_get_score(game_get_player(game, a_seat)));
      const int b_score =
          equity_to_int(player_get_score(game_get_player(game, 1 - a_seat)));
      const int spread = a_score - b_score;
      const double result = spread > 0 ? 1.0 : (spread == 0 ? 0.5 : 0.0);
      (void)fprintf(games_out, "%ld,%d,%d,%d,%d,%d,%.1f,%d,%.2f,%.2f\n",
                    pair_idx, game_in_pair, a_seat, a_score, b_score, spread,
                    result, turn,
                    game_timer_get_seconds_used(&game_timer, a_seat),
                    game_timer_get_seconds_used(&game_timer, 1 - a_seat));
      (void)fflush(games_out);
    }
    (void)fflush(moves_out);
  }
  (void)fclose(games_out);
  (void)fclose(moves_out);
  string_builder_destroy(move_names[0]);
  string_builder_destroy(move_names[1]);
  game_destroy(before_last_move);
  for (int player_idx = 0; player_idx < 2; player_idx++) {
    if (choosers[player_idx] != NULL) {
      play_chooser_destroy(choosers[player_idx]);
    }
  }
  move_list_destroy(static_list);
  value_net_player_destroy(players[0]);
  value_net_player_destroy(players[1]);
  value_net_player_destroy(rescorers[0]);
  value_net_player_destroy(rescorers[1]);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// A move's static equity in points, VNT_DISTILL_PASS_EQUITY for a pass.
static double vnt_equity_points(const Move *move) {
  const Equity equity = move_get_equity(move);
  return equity == EQUITY_PASS_VALUE ? VNT_DISTILL_PASS_EQUITY
                                     : equity_to_double(equity);
}

// A uniform double in [0, 1) from state.
static double vnt_uniform(uint64_t *state) {
  *state = vnt_mix(*state);
  return (double)(*state >> 11) * 0x1.0p-53;
}

// A candidate index drawn with probability proportional to
// exp((equity - best) / temperature) over the first count moves of list.
static int vnt_softmax_pick(const MoveList *list, int count, double temperature,
                            uint64_t *state) {
  const double best = vnt_equity_points(move_list_get_move(list, 0));
  double weights[VNT_DISTILL_CANDIDATES];
  double total = 0.0;
  for (int move_idx = 0; move_idx < count; move_idx++) {
    const double equity = vnt_equity_points(move_list_get_move(list, move_idx));
    weights[move_idx] = exp((equity - best) / temperature);
    total += weights[move_idx];
  }
  double target = vnt_uniform(state) * total;
  for (int move_idx = 0; move_idx < count; move_idx++) {
    target -= weights[move_idx];
    if (target < 0.0) {
      return move_idx;
    }
  }
  return count - 1;
}

// Packs a board row of 0/1 floats into bits (VntDistillRecord.board_bits).
static void vnt_pack_board(const float *board_row, uint8_t *bits) {
  memset(bits, 0, VNT_DISTILL_BOARD_BYTES);
  for (int idx = 0; idx < VALUE_NET_BOARD_FLOATS; idx++) {
    if (board_row[idx] == 1.0F) {
      bits[idx / 8] |= (uint8_t)(0x80U >> (idx % 8));
    } else if (board_row[idx] != 0.0F) {
      log_fatal("board input %d is %f, not 0 or 1", idx, board_row[idx]);
    }
  }
}

// One game-playing thread of vnt_distill.
typedef struct VntDistillThread {
  ValueNetPlayer *teacher;
  const Game *start_game;
  long first_game;
  long game_stride;
  long games;
  uint64_t seed;
  double w_winpct;
  double w_spread;
  double spread_scale;
  double temperature;
  double explore;
  int explore_bag;
  // With keep > 0, only the played row and the top keep - 1 others by static
  // equity are written per decision (candidates counts those).
  int keep;
  FILE *records_out;
  // Each decision's opponent record (VntOpponentRecord), or NULL.
  FILE *opponent_out;
  const char *out;
  // Shared progress, for the log.
  atomic_long *rows_written;
  atomic_long *games_played;
  int64_t start_ns;
} VntDistillThread;

static void *vnt_distill_thread(void *arg) {
  const VntDistillThread *args = arg;
  Game *game = game_duplicate(args->start_game);
  Game *scratch = game_duplicate(args->start_game);
  MoveList *list = move_list_create(VNT_DISTILL_CANDIDATES);
  float *board_rows = malloc_or_die(sizeof(float) * VNT_DISTILL_CANDIDATES *
                                    VALUE_NET_BOARD_FLOATS);
  float *scalar_rows =
      malloc_or_die(sizeof(float) * VNT_DISTILL_CANDIDATES * VALUE_NET_SCALARS);
  float values[VNT_DISTILL_CANDIDATES];
  float spreads[VNT_DISTILL_CANDIDATES];
  VntDistillRecord *records =
      malloc_or_die(sizeof(VntDistillRecord) * VNT_DISTILL_CANDIDATES);
  ValueNetHistory histories[2];
  // Each player's last move: the leave it kept, flags, score and tiles.
  VntOpponentRecord last[2];
  const int ld_size = ld_get_size(game_get_ld(args->start_game));
  if (ld_size > VNT_OPPONENT_LETTERS) {
    log_fatal("opponent records hold %d letters, not %d", VNT_OPPONENT_LETTERS,
              ld_size);
  }
  uint64_t rng =
      vnt_mix(args->seed ^ UINT64_C(0xd1571d) ^ (uint64_t)args->first_game);
  for (long game_idx = args->first_game; game_idx < args->games;
       game_idx += args->game_stride) {
    game_reset(game);
    game_seed(game, vnt_mix(args->seed ^ vnt_mix((uint64_t)game_idx)));
    game_set_starting_player_index(game, (int)(game_idx % 2));
    draw_starting_racks(game);
    value_net_history_reset(&histories[0]);
    value_net_history_reset(&histories[1]);
    memset(last, 0, sizeof(last));
    for (int turn = 0; !game_over(game) && turn < VNT_MAX_TURNS; turn++) {
      const int seat = game_get_player_on_turn_index(game);
      move_list_reset(list);
      const MoveGenArgs gen_args = {
          .game = game,
          .move_list = list,
          .move_record_type = MOVE_RECORD_ALL,
          .move_sort_type = MOVE_SORT_EQUITY,
          .override_kwg = NULL,
          .eq_margin_movegen = 0,
          .target_equity = EQUITY_MAX_VALUE,
          .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      };
      generate_moves(&gen_args);
      move_list_sort_moves(list);
      const int count = move_list_get_count(list);
      const int bag = bag_get_letters(game_get_bag(game));
      int chosen = 0;
      if (count > 1 && bag > 0) {
        for (int move_idx = 0; move_idx < count; move_idx++) {
          value_net_features_for_move(
              game, move_list_get_move(list, move_idx), &histories[seat],
              scratch, board_rows + ((size_t)move_idx * VALUE_NET_BOARD_FLOATS),
              scalar_rows + ((size_t)move_idx * VALUE_NET_SCALARS));
        }
        value_net_player_evaluate_rows(args->teacher, count, board_rows,
                                       scalar_rows, values, spreads);
        double best_utility = 0.0;
        for (int move_idx = 0; move_idx < count; move_idx++) {
          const Move *move = move_list_get_move(list, move_idx);
          const double spread_after = value_net_spread_after_move(game, move);
          const double utility = value_net_utility(
              values[move_idx], spreads[move_idx], spread_after, args->w_winpct,
              args->w_spread, args->spread_scale);
          if (move_idx == 0 || utility > best_utility ||
              (utility == best_utility &&
               move_get_tiles_played(move) >
                   move_get_tiles_played(move_list_get_move(list, chosen)))) {
            chosen = move_idx;
            best_utility = utility;
          }
          VntDistillRecord *record = &records[move_idx];
          vnt_pack_board(board_rows +
                             ((size_t)move_idx * VALUE_NET_BOARD_FLOATS),
                         record->board_bits);
          memcpy(record->scalars,
                 scalar_rows + ((size_t)move_idx * VALUE_NET_SCALARS),
                 sizeof(record->scalars));
          record->value = values[move_idx];
          record->spread = spreads[move_idx];
          record->spread_after = (float)spread_after;
          record->equity = (float)vnt_equity_points(move);
          record->game_id = (uint32_t)game_idx;
          record->turn = (uint16_t)turn;
          record->candidate = (uint16_t)move_idx;
          record->candidates = (uint16_t)count;
          record->bag = (uint16_t)bag;
          record->tiles_played = (uint16_t)move_get_tiles_played(move);
          record->chosen = 0;
        }
        if (bag > args->explore_bag || vnt_uniform(&rng) < args->explore) {
          chosen = vnt_softmax_pick(list, count, args->temperature, &rng);
        }
        records[chosen].chosen = 1;
        int written = count;
        if (args->keep > 0 && args->keep < count) {
          written = 0;
          int others = 0;
          for (int move_idx = 0; move_idx < count; move_idx++) {
            if (move_idx == chosen || others < args->keep - 1) {
              others += move_idx == chosen ? 0 : 1;
              records[written++] = records[move_idx];
            }
          }
          for (int move_idx = 0; move_idx < written; move_idx++) {
            records[move_idx].candidates = (uint16_t)written;
          }
        }
        if (fwrite(records, sizeof(VntDistillRecord), (size_t)written,
                   args->records_out) != (size_t)written) {
          log_fatal("could not write %s", args->out);
        }
        if (args->opponent_out != NULL) {
          VntOpponentRecord opponent = last[1 - seat];
          opponent.game_id = (uint32_t)game_idx;
          opponent.turn = (uint16_t)turn;
          const Rack *opponent_rack =
              player_get_rack(game_get_player(game, 1 - seat));
          for (int letter = 0; letter < ld_size; letter++) {
            opponent.rack[letter] =
                (uint8_t)rack_get_letter(opponent_rack, letter);
          }
          if (fwrite(&opponent, sizeof(opponent), 1, args->opponent_out) != 1) {
            log_fatal("could not write the opponent records of %s", args->out);
          }
        }
        atomic_fetch_add(args->rows_written, written);
      }
      Move move;
      move_copy(&move, move_list_get_move(list, chosen));
      if (args->opponent_out != NULL) {
        // The mover's leave: its rack less the tiles this move uses.
        Rack leave;
        rack_copy(&leave, player_get_rack(game_get_player(game, seat)));
        uint8_t played_counts[VNT_OPPONENT_LETTERS] = {0};
        const game_event_t move_type = move_get_type(&move);
        if (move_type == GAME_EVENT_TILE_PLACEMENT_MOVE ||
            move_type == GAME_EVENT_EXCHANGE) {
          for (int tile_idx = 0; tile_idx < move_get_tiles_length(&move);
               tile_idx++) {
            MachineLetter ml = move_get_tile(&move, tile_idx);
            if (ml == PLAYED_THROUGH_MARKER) {
              continue;
            }
            const MachineLetter unblanked =
                get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml;
            rack_take_letter(&leave, unblanked);
            played_counts[unblanked]++;
          }
        }
        VntOpponentRecord *kept = &last[seat];
        memset(kept, 0, sizeof(*kept));
        for (int letter = 0; letter < ld_size; letter++) {
          kept->leave[letter] = (uint8_t)rack_get_letter(&leave, letter);
        }
        kept->flags = VNT_OPPONENT_MOVED;
        if (move_type == GAME_EVENT_EXCHANGE) {
          kept->flags |= VNT_OPPONENT_EXCHANGE;
        } else if (move_type != GAME_EVENT_TILE_PLACEMENT_MOVE) {
          kept->flags |= VNT_OPPONENT_PASS;
        } else if (move_get_tiles_played(&move) == RACK_SIZE) {
          kept->flags |= VNT_OPPONENT_BINGO;
        }
        memcpy(kept->played, played_counts, sizeof(played_counts));
        kept->last_score = (int16_t)equity_to_int(move_get_score(&move));
        kept->last_tiles = (uint16_t)move_get_tiles_played(&move);
      }
      value_net_history_record_opponent_move(&histories[1 - seat], &move);
      play_move(&move, game, NULL);
    }
    const long games_done = atomic_fetch_add(args->games_played, 1) + 1;
    if (games_done % VNT_DISTILL_PROGRESS_GAMES == 0) {
      const double seconds =
          (double)(ctimer_monotonic_ns() - args->start_ns) / 1e9;
      const long rows = atomic_load(args->rows_written);
      printf("distill games=%ld rows=%ld rows_per_s=%.0f\n", games_done, rows,
             (double)rows / seconds);
      (void)fflush(stdout);
    }
  }
  free(records);
  free(board_rows);
  free(scalar_rows);
  move_list_destroy(list);
  game_destroy(scratch);
  game_destroy(game);
  return NULL;
}

// "distill:<model_dir>:<backend>:<lexicon>:<games>:<seed>:<worker>:<workers>:
// <out>[:key=value...]": teacher self-play for distillation. Of games games,
// this worker plays those with index = worker mod workers, on threads=
// threads (default 1) that share the teacher. Each player takes the top 50
// static moves; the net (the teacher) scores them all, and every
// candidate's row, the teacher's value and spread, and the move's static
// equity and spread after it go to <out> (VntDistillRecord; with several
// threads, thread k writes <out>.t<k>). The move played is the best by the
// utility (uwin=, uspread=, uscale=; MAGPIE's defaults otherwise), except
// that while the bag holds more than explore_bag= tiles (default 60), and
// otherwise with probability explore= (default 0.05), it is drawn by softmax
// over static equity with temperature temp= points (default 1). With an
// empty bag (not recorded), the top static move. opp=1 also writes each
// recorded decision's opponent rack, kept leave and played tiles
// (VntOpponentRecord, .opp2), keep=<n> writes only the played row and the
// top n - 1 others by static equity per decision, and tables=1 uses the
// lexicon's .rit and .wit tables in move generation.
static void vnt_distill(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) < 9) {
    log_fatal("distill needs at least 8 fields");
  }
  const char *model_dir = string_splitter_get_item(fields, 1);
  const value_net_backend_t backend =
      vnt_parse_backend(string_splitter_get_item(fields, 2));
  const char *lexicon = string_splitter_get_item(fields, 3);
  const long games = strtol(string_splitter_get_item(fields, 4), NULL, 10);
  const uint64_t seed = strtoull(string_splitter_get_item(fields, 5), NULL, 10);
  const long worker = strtol(string_splitter_get_item(fields, 6), NULL, 10);
  const long workers = strtol(string_splitter_get_item(fields, 7), NULL, 10);
  const char *out = string_splitter_get_item(fields, 8);
  const int threads = (int)vnt_option_double(fields, 9, "threads", 0, 1.0);
  if (threads < 1 || threads > VNT_MAX_THROUGHPUT_THREADS) {
    log_fatal("distill threads must be 1..%d", VNT_MAX_THROUGHPUT_THREADS);
  }
  // tables=1 also uses the lexicon's rack info and word info tables
  // (.rit, .wit), which only speed up move generation.
  char *settings = get_formatted_string(
      "set -lex %s -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -threads 1%s",
      lexicon,
      vnt_option_double(fields, 9, "tables", 0, 0.0) > 0
          ? " -rit true -wit true -ritmmap true"
          : "");
  Config *config = config_create_or_die(settings);
  free(settings);
  ErrorStack *error_stack = error_stack_create();
  VntDistillThread args = {
      .w_winpct = vnt_option_double(fields, 9, "uwin", 0,
                                    config_get_utility_w_winpct(config)),
      .w_spread = vnt_option_double(fields, 9, "uspread", 0,
                                    config_get_utility_w_spread(config)),
      .spread_scale = vnt_option_double(
          fields, 9, "uscale", 0, config_get_utility_spread_scale(config)),
      .temperature = vnt_option_double(fields, 9, "temp", 0, 1.0),
      .explore = vnt_option_double(fields, 9, "explore", 0, 0.05),
      .explore_bag = (int)vnt_option_double(fields, 9, "explore_bag", 0, 60.0),
      .keep = (int)vnt_option_double(fields, 9, "keep", 0, 0.0),
      .games = games,
      .seed = seed,
      .game_stride = workers * threads,
      .out = out,
  };
  args.teacher = value_net_player_create(
      model_dir, backend, VNT_DISTILL_CANDIDATES, args.w_winpct, args.w_spread,
      args.spread_scale, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("could not load the teacher");
  }
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  args.start_game = config_get_game(config);
  atomic_long rows_written = 0;
  atomic_long games_played = 0;
  args.rows_written = &rows_written;
  args.games_played = &games_played;
  args.start_ns = ctimer_monotonic_ns();
  VntDistillThread thread_args[VNT_MAX_THROUGHPUT_THREADS];
  cpthread_t thread_ids[VNT_MAX_THROUGHPUT_THREADS];
  for (int thread_idx = 0; thread_idx < threads; thread_idx++) {
    thread_args[thread_idx] = args;
    thread_args[thread_idx].first_game = (worker * threads) + thread_idx;
    char *path = threads > 1 ? get_formatted_string("%s.t%d", out, thread_idx)
                             : string_duplicate(out);
    thread_args[thread_idx].records_out = fopen_or_die(path, "wb");
    free(path);
    thread_args[thread_idx].opponent_out = NULL;
    if (vnt_option_double(fields, 9, "opp", 0, 0.0) > 0) {
      path = threads > 1 ? get_formatted_string("%s.t%d.opp2", out, thread_idx)
                         : get_formatted_string("%s.opp2", out);
      thread_args[thread_idx].opponent_out = fopen_or_die(path, "wb");
      free(path);
    }
    cpthread_create(&thread_ids[thread_idx], vnt_distill_thread,
                    &thread_args[thread_idx]);
  }
  for (int thread_idx = 0; thread_idx < threads; thread_idx++) {
    cpthread_join(thread_ids[thread_idx]);
    (void)fclose(thread_args[thread_idx].records_out);
    if (thread_args[thread_idx].opponent_out != NULL) {
      (void)fclose(thread_args[thread_idx].opponent_out);
    }
  }
  const double seconds = (double)(ctimer_monotonic_ns() - args.start_ns) / 1e9;
  printf("distill worker=%ld done games=%ld rows=%ld rows_per_s=%.0f\n", worker,
         atomic_load(&games_played), atomic_load(&rows_written),
         (double)atomic_load(&rows_written) / seconds);
  value_net_player_destroy(args.teacher);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// "dump:<lexicon>:<positions>:<seed>:<out>": plays static games and, every
// third turn while tiles remain, writes the position's CGP (player on turn
// first, then a tab, the opponent's last move and its score) to <out>.cgps
// and, for its top 50 static moves with that one move as the history, the move
// (<out>.tsv: position, move, equity, score) and its input row (<out>.bin:
// board floats then scalars), for comparison with Macondo's encoder on the same
// positions.
static void vnt_dump(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 5) {
    log_fatal("dump needs 4 fields");
  }
  const char *lexicon = string_splitter_get_item(fields, 1);
  const long positions = strtol(string_splitter_get_item(fields, 2), NULL, 10);
  const uint64_t seed = strtoull(string_splitter_get_item(fields, 3), NULL, 10);
  const char *out = string_splitter_get_item(fields, 4);
  char *settings = get_formatted_string(
      "set -lex %s -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -threads 1",
      lexicon);
  Config *config = config_create_or_die(settings);
  free(settings);
  char *path = get_formatted_string("%s.cgps", out);
  FILE *cgps_out = fopen_or_die(path, "w");
  free(path);
  path = get_formatted_string("%s.tsv", out);
  FILE *tsv_out = fopen_or_die(path, "w");
  free(path);
  path = get_formatted_string("%s.bin", out);
  FILE *bin_out = fopen_or_die(path, "wb");
  free(path);
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *game = config_get_game(config);
  Game *scratch = game_duplicate(game);
  MoveList *list = move_list_create(50);
  MoveList *static_list = move_list_create(1);
  float *board_row = malloc_or_die(sizeof(float) * VALUE_NET_BOARD_FLOATS);
  float scalars_row[VALUE_NET_SCALARS];
  // A one-move history: the opponent's last move, and 0 or 1 moves since
  // their last bingo (Macondo's count over that one move).
  ValueNetHistory history;
  Move last;
  bool has_last = false;
  long written = 0;
  for (uint64_t game_idx = 0; written < positions; game_idx++) {
    game_reset(game);
    game_seed(game, vnt_mix(seed ^ game_idx));
    draw_starting_racks(game);
    has_last = false;
    for (int turn = 0; !game_over(game) && written < positions; turn++) {
      if (turn % 3 == 1 && bag_get_letters(game_get_bag(game)) > 0) {
        char *cgp = game_get_cgp(game, true);
        value_net_history_reset(&history);
        if (has_last) {
          value_net_history_record_opponent_move(&history, &last);
          StringBuilder *last_sb = string_builder_create();
          string_builder_add_move_description(last_sb, &last,
                                              game_get_ld(game));
          (void)fprintf(cgps_out, "%s\t%s\t%d\n", cgp,
                        string_builder_peek(last_sb),
                        equity_to_int(move_get_score(&last)));
          string_builder_destroy(last_sb);
        } else {
          (void)fprintf(cgps_out, "%s\n", cgp);
        }
        free(cgp);
        move_list_reset(list);
        const MoveGenArgs args = {
            .game = game,
            .move_list = list,
            .move_record_type = MOVE_RECORD_ALL,
            .move_sort_type = MOVE_SORT_EQUITY,
            .override_kwg = NULL,
            .eq_margin_movegen = 0,
            .target_equity = EQUITY_MAX_VALUE,
            .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
        };
        generate_moves(&args);
        move_list_sort_moves(list);
        for (int move_idx = 0; move_idx < move_list_get_count(list);
             move_idx++) {
          const Move *move = move_list_get_move(list, move_idx);
          StringBuilder *sb = string_builder_create();
          string_builder_add_move_description(sb, move, game_get_ld(game));
          (void)fprintf(tsv_out, "%ld\t%s\t%.4f\t%d\n", written,
                        string_builder_peek(sb),
                        equity_to_double(move_get_equity(move)),
                        equity_to_int(move_get_score(move)));
          string_builder_destroy(sb);
          value_net_features_for_move(game, move, &history, scratch, board_row,
                                      scalars_row);
          (void)fwrite(board_row, sizeof(float), VALUE_NET_BOARD_FLOATS,
                       bin_out);
          (void)fwrite(scalars_row, sizeof(float), VALUE_NET_SCALARS, bin_out);
        }
        written++;
      }
      move_copy(&last, vnt_static_move(game, static_list));
      has_last = true;
      play_move(&last, game, NULL);
    }
  }
  free(board_row);
  move_list_destroy(list);
  move_list_destroy(static_list);
  game_destroy(scratch);
  (void)fclose(cgps_out);
  (void)fclose(tsv_out);
  (void)fclose(bin_out);
  config_destroy(config);
}

typedef struct VntThroughputWorker {
  ValueNetMetal *metal;
  int rows;
  int calls;
  const float *board;
  const float *scalars;
  float *values;
} VntThroughputWorker;

static void *vnt_throughput_worker(void *arg) {
  const VntThroughputWorker *worker = arg;
  for (int call_idx = 0; call_idx < worker->calls; call_idx++) {
    value_net_metal_evaluate(worker->metal, worker->rows, worker->board,
                             worker->scalars, worker->values, NULL);
  }
  return NULL;
}

// Runs calls calls of rows rows on each of threads threads at once; returns
// the wall time in seconds.
static double vnt_throughput_run(ValueNetMetal *metal, int threads, int rows,
                                 int calls, const float *board,
                                 const float *scalars) {
  cpthread_t thread_ids[VNT_MAX_THROUGHPUT_THREADS];
  VntThroughputWorker workers[VNT_MAX_THROUGHPUT_THREADS];
  float *values = malloc_or_die(sizeof(float) * (size_t)threads *
                                (size_t)VALUE_NET_MAX_GPU_ROWS);
  const int64_t start = ctimer_monotonic_ns();
  for (int thread_idx = 0; thread_idx < threads; thread_idx++) {
    workers[thread_idx] = (VntThroughputWorker){
        .metal = metal,
        .rows = rows,
        .calls = calls,
        .board = board,
        .scalars = scalars,
        .values = values + ((size_t)thread_idx * VALUE_NET_MAX_GPU_ROWS),
    };
    cpthread_create(&thread_ids[thread_idx], vnt_throughput_worker,
                    &workers[thread_idx]);
  }
  for (int thread_idx = 0; thread_idx < threads; thread_idx++) {
    cpthread_join(thread_ids[thread_idx]);
  }
  const double seconds = (double)(ctimer_monotonic_ns() - start) / 1e9;
  free(values);
  return seconds;
}

// "throughput:<dir>:<parity_dir>:<fp32|fp16>[:<threads>[:<concurrency>]]":
// Metal rows per second by batch size, the parity rows tiled to fill each
// batch, with threads callers (default 1) sharing one ValueNetMetal of
// concurrency slots (default threads).
static void vnt_throughput(const char *dir, const char *parity_dir,
                           bool half_precision, int threads, int concurrency) {
  if (threads < 1 || threads > VNT_MAX_THROUGHPUT_THREADS) {
    log_fatal("throughput threads must be 1..%d", VNT_MAX_THROUGHPUT_THREADS);
  }
  ErrorStack *error_stack = error_stack_create();
  ValueNet *net = value_net_create(dir, error_stack);
  ValueNetMetal *metal = net != NULL
                             ? value_net_metal_create(net, half_precision,
                                                      concurrency, error_stack)
                             : NULL;
  if (metal == NULL) {
    error_stack_print_and_reset(error_stack);
    log_fatal("no Metal value net");
  }
  char *path = get_formatted_string("%s/board.f32", parity_dir);
  float *board = vnt_read_floats(path, (size_t)VALUE_NET_PARITY_ROWS *
                                           VALUE_NET_BOARD_FLOATS);
  free(path);
  path = get_formatted_string("%s/scalars.f32", parity_dir);
  float *scalars =
      vnt_read_floats(path, (size_t)VALUE_NET_PARITY_ROWS * VALUE_NET_SCALARS);
  free(path);
  // No more than one capped GPU call (VALUE_NET_MAX_GPU_ROWS).
  const int max_rows = VALUE_NET_MAX_GPU_ROWS;
  float *big_board =
      malloc_or_die(sizeof(float) * (size_t)max_rows * VALUE_NET_BOARD_FLOATS);
  float *big_scalars =
      malloc_or_die(sizeof(float) * (size_t)max_rows * VALUE_NET_SCALARS);
  for (int row = 0; row < max_rows; row++) {
    memcpy(big_board + ((size_t)row * VALUE_NET_BOARD_FLOATS),
           board +
               ((size_t)(row % VALUE_NET_PARITY_ROWS) * VALUE_NET_BOARD_FLOATS),
           sizeof(float) * VALUE_NET_BOARD_FLOATS);
    memcpy(big_scalars + ((size_t)row * VALUE_NET_SCALARS),
           scalars +
               ((size_t)(row % VALUE_NET_PARITY_ROWS) * VALUE_NET_SCALARS),
           sizeof(float) * VALUE_NET_SCALARS);
  }
  for (int rows = 1; rows <= max_rows; rows *= 2) {
    // Warm up: the first calls compile each slot's graph for this size.
    (void)vnt_throughput_run(metal, threads, rows, 2, big_board, big_scalars);
    const int calls = rows <= 64 ? 20 : 5;
    const double seconds =
        vnt_throughput_run(metal, threads, rows, calls, big_board, big_scalars);
    printf("value_net_throughput %s threads=%d concurrency=%d rows=%d "
           "ms_per_call=%.2f rows_per_s=%.0f\n",
           half_precision ? "fp16" : "fp32", threads,
           concurrency > 0 ? concurrency : threads, rows, seconds * 1e3 / calls,
           (double)threads * calls * rows / seconds);
  }
  free(big_board);
  free(big_scalars);
  free(board);
  free(scalars);
  value_net_metal_destroy(metal);
  value_net_destroy(net);
  error_stack_destroy(error_stack);
}

// "simbench:<model_dir>:<none|cpu|fp32|fp16|ane|anegpu>:<plies>:
// <root_cands>:<threads>:<iterations>:<reply_cands>:<batch>:<positions>
// [:<nn_plies>]": sims of the top root_cands static plays, round robin, for
// a fixed number of iterations on NWL23 positions from static games, with
// value net plays on the first nn_plies rollout plies (default 1; or static
// rollouts for "none"). Prints iterations per second per position and
// overall.
static void vnt_simbench(const StringSplitter *fields) {
  const int num_fields = string_splitter_get_number_of_items(fields);
  if (num_fields != 10 && num_fields != 11) {
    log_fatal("simbench needs 9 or 10 fields");
  }
  const char *model_dir = string_splitter_get_item(fields, 1);
  const char *backend_name = string_splitter_get_item(fields, 2);
  const int plies = (int)strtol(string_splitter_get_item(fields, 3), NULL, 10);
  const int root_cands =
      (int)strtol(string_splitter_get_item(fields, 4), NULL, 10);
  const int threads =
      (int)strtol(string_splitter_get_item(fields, 5), NULL, 10);
  const uint64_t iterations =
      strtoull(string_splitter_get_item(fields, 6), NULL, 10);
  const int reply_cands =
      (int)strtol(string_splitter_get_item(fields, 7), NULL, 10);
  const int batch = (int)strtol(string_splitter_get_item(fields, 8), NULL, 10);
  const int positions =
      (int)strtol(string_splitter_get_item(fields, 9), NULL, 10);
  const int nn_plies =
      num_fields == 11
          ? (int)strtol(string_splitter_get_item(fields, 10), NULL, 10)
          : 1;
  Config *config = config_create_or_die(
      "set -lex NWL23 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -threads 1");
  ErrorStack *error_stack = error_stack_create();
  config_load_win_pcts(config, error_stack);
  ValueNetPlayer *player = NULL;
  if (!strings_equal(backend_name, "none")) {
    // Pure win%, as the sims below.
    player = value_net_player_create(model_dir, vnt_parse_backend(backend_name),
                                     0, 1.0, 0.0, 100.0, error_stack);
  }
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("simbench setup failed");
  }
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *game = config_get_game(config);
  MoveList *static_list = move_list_create(1);
  MoveList *root = move_list_create(root_cands);
  double total_seconds = 0.0;
  uint64_t total_iterations = 0;
  int done = 0;
  for (uint64_t game_idx = 0; done < positions; game_idx++) {
    game_reset(game);
    game_seed(game, vnt_mix(UINT64_C(77) ^ game_idx));
    draw_starting_racks(game);
    for (int turn = 0; !game_over(game) && done < positions; turn++) {
      if (turn == 6 && bag_get_letters(game_get_bag(game)) > 0) {
        move_list_reset(root);
        const MoveGenArgs args = {
            .game = game,
            .move_list = root,
            .move_record_type = MOVE_RECORD_ALL,
            .move_sort_type = MOVE_SORT_EQUITY,
            .override_kwg = NULL,
            .eq_margin_movegen = 0,
            .target_equity = EQUITY_MAX_VALUE,
            .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
        };
        generate_moves(&args);
        move_list_sort_moves(root);
        ThreadControl *control = thread_control_create();
        thread_control_set_status(control, THREAD_CONTROL_STATUS_STARTED);
        SimArgs sim_args;
        sim_args_fill(plies, root, move_list_get_count(root), NULL,
                      config_get_win_pcts(config), NULL, control, game, false,
                      false, threads, 0, move_list_get_count(root), plies, 7,
                      iterations, 1, 0.0, BAI_THRESHOLD_NONE, 0.0,
                      BAI_SAMPLING_RULE_ROUND_ROBIN, 0.0, 1.0, 0.0, 100.0,
                      false, NULL, &sim_args);
        if (player != NULL) {
          sim_args.rollout_value_net_evaluate = value_net_player_evaluate_rows;
          sim_args.rollout_value_net_context = player;
          sim_args.rollout_value_net_candidates = reply_cands;
          sim_args.rollout_value_net_batch = batch;
          sim_args.rollout_value_net_plies = nn_plies;
        }
        SimResults *results = sim_results_create(0.0);
        const int64_t start = ctimer_monotonic_ns();
        simulate_without_ctx(&sim_args, results, error_stack);
        const double seconds = (double)(ctimer_monotonic_ns() - start) / 1e9;
        if (!error_stack_is_empty(error_stack)) {
          error_stack_print_and_reset(error_stack);
          log_fatal("sim failed");
        }
        const uint64_t done_iterations =
            sim_results_get_iteration_count(results);
        printf("simbench position=%d iterations=%llu seconds=%.3f "
               "it_per_s=%.1f\n",
               done, (unsigned long long)done_iterations, seconds,
               (double)done_iterations / seconds);
        if (done > 0) {
          // The first position pays for compiling the Metal graph.
          total_seconds += seconds;
          total_iterations += done_iterations;
        }
        sim_results_destroy(results);
        thread_control_destroy(control);
        done++;
      }
      Move move;
      move_copy(&move, vnt_static_move(game, static_list));
      play_move(&move, game, NULL);
    }
  }
  printf("simbench backend=%s plies=%d nn_plies=%d threads=%d reply_cands=%d "
         "batch=%d it_per_s=%.1f (positions after the first)\n",
         backend_name, plies, nn_plies, threads, reply_cands, batch,
         total_seconds > 0 ? (double)total_iterations / total_seconds : 0.0);
  move_list_destroy(root);
  move_list_destroy(static_list);
  value_net_player_destroy(player);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// "anebench:<dir>:<parity_dir>": the CoreML (Neural Engine) build at
// <dir>/ane.mlpackage on the parity rows, then rows per second by rows per
// call (the parity rows tiled), up to VALUE_NET_MAX_GPU_ROWS.
static void vnt_anebench(const char *dir, const char *parity_dir) {
  ErrorStack *error_stack = error_stack_create();
  char *path = get_formatted_string("%s/ane.mlpackage", dir);
  ValueNetCoreML *coreml = value_net_coreml_create(path, 0, error_stack);
  free(path);
  if (coreml == NULL) {
    error_stack_print_and_reset(error_stack);
    log_fatal("no CoreML value net");
  }
  path = get_formatted_string("%s/board.f32", parity_dir);
  float *board = vnt_read_floats(path, (size_t)VALUE_NET_PARITY_ROWS *
                                           VALUE_NET_BOARD_FLOATS);
  free(path);
  path = get_formatted_string("%s/scalars.f32", parity_dir);
  float *scalars =
      vnt_read_floats(path, (size_t)VALUE_NET_PARITY_ROWS * VALUE_NET_SCALARS);
  free(path);
  path = get_formatted_string("%s/value.f32", parity_dir);
  float *ref_value = vnt_read_floats(path, VALUE_NET_PARITY_ROWS);
  free(path);
  path = get_formatted_string("%s/spread.f32", parity_dir);
  float *ref_spread = vnt_read_floats(path, VALUE_NET_PARITY_ROWS);
  free(path);
  float value[VALUE_NET_PARITY_ROWS];
  float spread[VALUE_NET_PARITY_ROWS];
  const int64_t start = ctimer_monotonic_ns();
  value_net_coreml_evaluate(coreml, VALUE_NET_PARITY_ROWS, board, scalars,
                            value, spread);
  vnt_report("ane", VALUE_NET_PARITY_ROWS, value, spread, ref_value, ref_spread,
             (double)(ctimer_monotonic_ns() - start) / 1e9);
  const int max_rows = VALUE_NET_MAX_GPU_ROWS;
  float *big_board =
      malloc_or_die(sizeof(float) * (size_t)max_rows * VALUE_NET_BOARD_FLOATS);
  float *big_scalars =
      malloc_or_die(sizeof(float) * (size_t)max_rows * VALUE_NET_SCALARS);
  for (int row = 0; row < max_rows; row++) {
    memcpy(big_board + ((size_t)row * VALUE_NET_BOARD_FLOATS),
           board +
               ((size_t)(row % VALUE_NET_PARITY_ROWS) * VALUE_NET_BOARD_FLOATS),
           sizeof(float) * VALUE_NET_BOARD_FLOATS);
    memcpy(big_scalars + ((size_t)row * VALUE_NET_SCALARS),
           scalars +
               ((size_t)(row % VALUE_NET_PARITY_ROWS) * VALUE_NET_SCALARS),
           sizeof(float) * VALUE_NET_SCALARS);
  }
  float *values = malloc_or_die(sizeof(float) * (size_t)max_rows);
  for (int rows = 8; rows <= max_rows; rows *= 2) {
    value_net_coreml_evaluate(coreml, rows, big_board, big_scalars, values,
                              NULL);
    const int repeats = 20;
    const int64_t loop_start = ctimer_monotonic_ns();
    for (int repeat = 0; repeat < repeats; repeat++) {
      value_net_coreml_evaluate(coreml, rows, big_board, big_scalars, values,
                                NULL);
    }
    const double seconds =
        (double)(ctimer_monotonic_ns() - loop_start) / 1e9 / repeats;
    printf("value_net_throughput ane rows=%d ms_per_call=%.2f "
           "rows_per_s=%.0f\n",
           rows, seconds * 1e3, rows / seconds);
  }
  free(values);
  free(big_board);
  free(big_scalars);
  free(board);
  free(scalars);
  free(ref_value);
  free(ref_spread);
  value_net_coreml_destroy(coreml);
  error_stack_destroy(error_stack);
}

void value_net_test_run_spec(const char *spec) {
  StringSplitter *fields = split_string(spec, ':', true);
  const int num_fields = string_splitter_get_number_of_items(fields);
  const char *mode = string_splitter_get_item(fields, 0);
  if (strings_equal(mode, "anebench") && num_fields == 3) {
    vnt_anebench(string_splitter_get_item(fields, 1),
                 string_splitter_get_item(fields, 2));
  } else if (strings_equal(mode, "simbench")) {
    vnt_simbench(fields);
  } else if (strings_equal(mode, "throughput") && num_fields >= 4 &&
             num_fields <= 6) {
    const int threads =
        num_fields >= 5
            ? (int)strtol(string_splitter_get_item(fields, 4), NULL, 10)
            : 1;
    const int concurrency =
        num_fields >= 6
            ? (int)strtol(string_splitter_get_item(fields, 5), NULL, 10)
            : threads;
    vnt_throughput(string_splitter_get_item(fields, 1),
                   string_splitter_get_item(fields, 2),
                   strings_equal(string_splitter_get_item(fields, 3), "fp16"),
                   threads, concurrency);
  } else if (strings_equal(mode, "distill")) {
    vnt_distill(fields);
  } else if (strings_equal(mode, "dump")) {
    vnt_dump(fields);
  } else if (strings_equal(mode, "simcompare")) {
    vnt_simcompare(fields);
  } else if (strings_equal(mode, "leafcheck")) {
    vnt_leafcheck(fields);
  } else if (strings_equal(mode, "priorfit")) {
    vnt_priorfit(fields);
  } else if (strings_equal(mode, "games")) {
    vnt_games(fields);
  } else if (strings_equal(mode, "xsprobe")) {
    vnt_cross_set_probe();
  } else if (strings_equal(mode, "parity") && num_fields >= 3) {
    int rows = VALUE_NET_PARITY_ROWS;
    if (num_fields >= 4) {
      rows = (int)strtol(string_splitter_get_item(fields, 3), NULL, 10);
    }
    if (rows < 1 || rows > VALUE_NET_PARITY_ROWS) {
      log_fatal("parity rows must be 1..%d", VALUE_NET_PARITY_ROWS);
    }
    vnt_parity(string_splitter_get_item(fields, 1),
               string_splitter_get_item(fields, 2), rows);
  } else {
    log_fatal("unknown valuenet spec: %s", spec);
  }
  string_splitter_destroy(fields);
}
