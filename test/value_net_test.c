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
#include "../src/ent/leave_odds.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/sim_args.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/stats.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/value_net.h"
#include "../src/ent/win_pct.h"
#include "../src/ent/xoshiro.h"
#include "../src/impl/blocking_setup.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/inference.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/play_chooser.h"
#include "../src/impl/simmer.h"
#include "../src/impl/value_net_coreml.h"
#include "../src/impl/value_net_features.h"
#include "../src/impl/value_net_inference.h"
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

enum { VNT_INFER_MARGINS = 4 };

// What exact inference (MAGPIE's infer) made of the opponent's last move at
// one decision, written beside the opponent records when inferq=1
// (<out>.inf, <out>.t<k>.inf): for each of VNT_INFER_MARGINS equity margins,
// the natural log of the probability it gave the leave the opponent
// actually kept (-1e30 if that leave was not among those consistent with
// the move) and how many leaves it found consistent. Written for every
// decision (flags 0 when there was nothing to infer: no tiles played or
// exchanged, or inference failed). Little-endian, no padding.
typedef struct VntInferRecord {
  uint32_t game_id;
  uint16_t turn;
  uint16_t flags;
  float log_p[VNT_INFER_MARGINS];
  uint32_t leaves[VNT_INFER_MARGINS];
} VntInferRecord;

static_assert(sizeof(VntInferRecord) == 4 + 2 + 2 + (8 * VNT_INFER_MARGINS),
              "VntInferRecord must have no padding");

// How long each of a decision's inference runs took, in microseconds, in
// <out>.inft beside the .inf records (same order).
typedef struct VntInferTimeRecord {
  uint32_t game_id;
  uint16_t turn;
  uint16_t reserved;
  uint32_t micros[VNT_INFER_MARGINS];
} VntInferTimeRecord;

static_assert(sizeof(VntInferTimeRecord) == 4 + 2 + 2 + (4 * VNT_INFER_MARGINS),
              "VntInferTimeRecord must have no padding");

enum { VNT_NET_INFER_TEMPERATURES = 8 };

// Slots of VntNetInferRecord.log_p: the net (0 the teacher, 1 the student
// given by student=, -1 unused), whether the candidates take the PAT term,
// and the temperature.
static const int VNT_NET_INFER_MODEL[VNT_NET_INFER_TEMPERATURES] = {
    0, 0, 0, 0, 1, 1, 1, -1};
static const bool VNT_NET_INFER_PAT[VNT_NET_INFER_TEMPERATURES] = {
    true, true, true, false, true, true, true, false};
static const double VNT_NET_INFER_TEMPERATURE[VNT_NET_INFER_TEMPERATURES] = {
    0.001, 0.003, 0.01, 0.003, 0.001, 0.003, 0.01, 0.003};

// Net-based inference (value_net_inference.h, a net as the opponent's
// policy, 16 candidates per rack) of the leave the opponent kept, for each
// decision where they kept 1 or 2 tiles from a tile placement: the natural
// log of the probability it gave the actual leave at each temperature
// (-1e30 if none), the leaves it weighed, the rows the net evaluated and
// the time. In <out>.ninf with inferq=1, one per decision (flags 0 when not
// run). Little-endian, no padding.
typedef struct VntNetInferRecord {
  uint32_t game_id;
  uint16_t turn;
  uint16_t flags;
  float log_p[VNT_NET_INFER_TEMPERATURES];
  uint32_t leaves;
  uint32_t rows;
  uint32_t micros;
} VntNetInferRecord;

static_assert(sizeof(VntNetInferRecord) ==
                  4 + 2 + 2 + (4 * VNT_NET_INFER_TEMPERATURES) + 12,
              "VntNetInferRecord must have no padding");

// Slots of VntInferRecord: equity margin, and whether the inference takes
// the PAT term (when the target has PAT).
static const double VNT_INFER_MARGIN_POINTS[VNT_INFER_MARGINS] = {0.0, 10.0,
                                                                  0.0, 10.0};
static const bool VNT_INFER_PAT[VNT_INFER_MARGINS] = {true, true, false, false};

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
  int nonfinite = 0;
  for (int row = 0; row < rows; row++) {
    // fmax skips NaN, so count non-finite outputs apart.
    if (!isfinite(value[row]) || !isfinite(spread[row])) {
      nonfinite++;
    }
    max_value = fmax(max_value, fabs((double)value[row] - ref_value[row]));
    max_spread = fmax(max_spread, fabs((double)spread[row] - ref_spread[row]));
  }
  if (nonfinite > 0) {
    printf("value_net_parity backend=%s nonfinite_rows=%d\n", backend,
           nonfinite);
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
  // The SIMD kernels (best of the repeats) and the scalar reference.
  double best = INFINITY;
  int64_t start = 0;
  for (int repeat = 0; repeat < VALUE_NET_PARITY_REPEATS; repeat++) {
    start = ctimer_monotonic_ns();
    value_net_evaluate_cpu(net, rows, board, scalars, value, spread);
    best = fmin(best, (double)(ctimer_monotonic_ns() - start) / 1e9);
  }
  char *label = get_formatted_string("cpu-%s", value_net_cpu_kernels());
  vnt_report(label, rows, value, spread, ref_value, ref_spread, best);
  free(label);
  float simd_value[VALUE_NET_PARITY_ROWS];
  float simd_spread[VALUE_NET_PARITY_ROWS];
  memcpy(simd_value, value, sizeof(float) * (size_t)rows);
  memcpy(simd_spread, spread, sizeof(float) * (size_t)rows);
  start = ctimer_monotonic_ns();
  value_net_evaluate_cpu_reference(net, rows, board, scalars, value, spread);
  vnt_report("cpu-reference", rows, value, spread, ref_value, ref_spread,
             (double)(ctimer_monotonic_ns() - start) / 1e9);
  vnt_report("cpu-simd-vs-reference", rows, simd_value, simd_spread, value,
             spread, 0.0);
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

enum {
  // Cheap features for the diversity extras (kinship): score, leave value,
  // tiles played, row and column of the tiles placed, exchange.
  VNT_KIN_CHEAP = 6,
  VNT_KIN_MAX_POOL = 256,
  VNT_KIN_ALL_MOVES = 20000,
  VNT_KIN_BS_RACKS = 64,
};

// The cheap feature vector of move (see VNT_KIN_CHEAP): an exchange sits at
// the center of the board.
static void vnt_kin_cheap(const Game *game, const Move *move, double *f) {
  const Player *mover =
      game_get_player(game, game_get_player_on_turn_index(game));
  Rack rack;
  rack_copy(&rack, player_get_rack(mover));
  f[0] = equity_to_double(move_get_score(move));
  f[1] = equity_to_double(
      get_leave_value_for_move(player_get_klv(mover), move, &rack));
  f[2] = move_get_tiles_played(move);
  f[3] = 7.0;
  f[4] = 7.0;
  f[5] = 0.0;
  if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    f[5] = 1.0;
    return;
  }
  double rows = 0.0;
  double cols = 0.0;
  int placed = 0;
  const bool horizontal = move_get_dir(move) == BOARD_HORIZONTAL_DIRECTION;
  for (int idx = 0; idx < move_get_tiles_length(move); idx++) {
    if (move_get_tile(move, idx) == PLAYED_THROUGH_MARKER) {
      continue;
    }
    rows += move_get_row_start(move) + (horizontal ? 0 : idx);
    cols += move_get_col_start(move) + (horizontal ? idx : 0);
    placed++;
  }
  if (placed > 0) {
    f[3] = rows / placed;
    f[4] = cols / placed;
  }
}

// "kinship:<model_dir>:<backend>:<positions>:<first_game>:<iterations>:
// <base>:<extras>:<margin>:<racks>:<threads>:<out>": data for how similar
// candidate moves are and whether similar moves have similar values. On
// positions from static NWL23 self-play (every 4th turn from 2 with tiles in
// the bag; games first_game on), the pool is the mover's top base moves by
// static equity with PAT plus up to extras more, chosen by farthest-point
// selection in standardized cheap features (score, leave value, tiles
// played, placement row and column, exchange) among the moves within margin
// points of the best. Every pool move is simmed for iterations iterations
// (round robin, shared seeds, 4 plies of static rollouts scored by the
// net's leaf, uniform opponent racks, utility 1 win + 0.5 spread / 100) and
// its features recorded: the cheap ones, static equity and the PAT term,
// the net's win% and final spread, and board openness (the opponent's best
// reply score and whether it bingoes, over racks racks drawn from the tiles
// unseen to the mover, the same draws for every move), and the
// blocking/setup teacher's pass-relative blocking and setup deltas with its
// reply and follow-up means over 64 racks (blocking_setup.h). Writes
// <out>.cands.csv (one row per candidate, with its sim mean, sd and count)
// and <out>.samples (per position: int32 position, candidates, iterations,
// then candidates x iterations float32 utilities, each candidate's samples
// placed by their seed's index in the shared seed sequence, NaN where
// missing).
static void vnt_kinship(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 12) {
    log_fatal("kinship needs 11 fields");
  }
  const char *model_dir = string_splitter_get_item(fields, 1);
  const char *backend_name = string_splitter_get_item(fields, 2);
  const int positions =
      (int)strtol(string_splitter_get_item(fields, 3), NULL, 10);
  const uint64_t first_game =
      strtoull(string_splitter_get_item(fields, 4), NULL, 10);
  const int iterations =
      (int)strtol(string_splitter_get_item(fields, 5), NULL, 10);
  const int base = (int)strtol(string_splitter_get_item(fields, 6), NULL, 10);
  const int extras = (int)strtol(string_splitter_get_item(fields, 7), NULL, 10);
  const double margin = strtod(string_splitter_get_item(fields, 8), NULL);
  const int racks = (int)strtol(string_splitter_get_item(fields, 9), NULL, 10);
  const int threads =
      (int)strtol(string_splitter_get_item(fields, 10), NULL, 10);
  const char *out = string_splitter_get_item(fields, 11);
  if (base + extras > VNT_KIN_MAX_POOL || base < 1 || iterations < 2) {
    log_fatal("kinship: pool at most %d, iterations at least 2",
              VNT_KIN_MAX_POOL);
  }
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
    log_fatal("kinship setup failed");
  }
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *game = config_get_game(config);
  Game *scratch = game_duplicate(game);
  Game *open_game = game_duplicate(game);
  const LetterDistribution *ld = game_get_ld(game);
  MoveList *all = move_list_create(VNT_KIN_ALL_MOVES);
  MoveList *pool = move_list_create(VNT_KIN_MAX_POOL);
  MoveList *static_list = move_list_create(1);
  MoveList *reply_list = move_list_create(1);
  float *board =
      malloc_or_die(sizeof(float) * VNT_KIN_MAX_POOL * VALUE_NET_BOARD_FLOATS);
  float *scalars =
      malloc_or_die(sizeof(float) * VNT_KIN_MAX_POOL * VALUE_NET_SCALARS);
  float values[VNT_KIN_MAX_POOL];
  float spreads[VNT_KIN_MAX_POOL];
  double pat_terms[VNT_KIN_MAX_POOL];
  // The pass-relative blocking/setup teacher (blocking_setup.h) at
  // VNT_KIN_BS_RACKS racks, as the blocking/setup studies deal them.
  BlockingSetupSamples *bs_samples = blocking_setup_samples_create(
      VNT_KIN_BS_RACKS, ld_get_total_tiles(game_get_ld(game)));
  BlockingSetupChecker *bs_checker = blocking_setup_checker_create();
  BlockingSetupResult bs_results[VNT_KIN_MAX_POOL];
  // The extras' selection: each candidate move's cheap features and its
  // distance to the nearest move chosen so far.
  double (*cheap)[VNT_KIN_CHEAP] =
      malloc_or_die(sizeof(double[VNT_KIN_CHEAP]) * VNT_KIN_ALL_MOVES);
  double *nearest = malloc_or_die(sizeof(double) * VNT_KIN_ALL_MOVES);
  // The shared seed sequence, to place each sample by its seed.
  const uint64_t sim_seed = 7;
  // Refills compute iterations ahead in batches (64 per thread), all of
  // which are sampled, so a play can take this many seeds beyond its share.
  const int sequence = (iterations * 2) + (threads * 64 * 4);
  uint64_t *seed_order = malloc_or_die(sizeof(uint64_t) * sequence);
  int *seed_rank = malloc_or_die(sizeof(int) * sequence);
  {
    XoshiroPRNG *prng = prng_create(sim_seed);
    for (int idx = 0; idx < sequence; idx++) {
      seed_order[idx] = prng_next(prng);
      seed_rank[idx] = idx;
    }
    prng_destroy(prng);
    // Sort seeds (with their ranks) for binary search.
    for (int i = 1; i < sequence; i++) {
      const uint64_t key = seed_order[i];
      const int rank = seed_rank[i];
      int j = i - 1;
      while (j >= 0 && seed_order[j] > key) {
        seed_order[j + 1] = seed_order[j];
        seed_rank[j + 1] = seed_rank[j];
        j--;
      }
      seed_order[j + 1] = key;
      seed_rank[j + 1] = rank;
    }
  }
  const int capacity = sequence;
  SimSampleRecord record = {
      .plays = VNT_KIN_MAX_POOL,
      .capacity = capacity,
      .seeds = malloc_or_die(sizeof(uint64_t) * VNT_KIN_MAX_POOL * capacity),
      .utilities = malloc_or_die(sizeof(float) * VNT_KIN_MAX_POOL * capacity),
      .counts = calloc_or_die(VNT_KIN_MAX_POOL, sizeof(_Atomic int)),
  };
  float *matrix = malloc_or_die(sizeof(float) * VNT_KIN_MAX_POOL * iterations);
  char *path = get_formatted_string("%s.cands.csv", out);
  FILE *csv = fopen_or_die(path, "w");
  free(path);
  path = get_formatted_string("%s.samples", out);
  FILE *samples = fopen_or_die(path, "wb");
  free(path);
  fprintf(csv, "position,game,turn,bag,mover_spread,cand,source,move,score,"
               "leave,tiles,row,col,exchange,static_equity,pat_term,net_win,"
               "net_spread,open_mean,open_max,open_bingo,blocking,setup,"
               "reply64,followup64,sim_mean,sim_sd,sim_n\n");
  StringBuilder *name = string_builder_create();
  ValueNetHistory histories[2];
  int done = 0;
  for (uint64_t game_idx = first_game; done < positions; game_idx++) {
    game_reset(game);
    game_seed(game, vnt_mix(UINT64_C(9191) ^ game_idx));
    draw_starting_racks(game);
    value_net_history_reset(&histories[0]);
    value_net_history_reset(&histories[1]);
    for (int turn = 0; !game_over(game) && done < positions; turn++) {
      const int mover = game_get_player_on_turn_index(game);
      const int bag = bag_get_letters(game_get_bag(game));
      // Both players without PAT, except the mover's pool generation.
      player_set_pat_usage(game_get_player(game, 0), true, 0);
      player_set_pat_usage(game_get_player(game, 1), true, 0);
      if (turn >= 2 && turn % 4 == 2 && bag > 0) {
        const int64_t start = ctimer_monotonic_ns();
        // Every legal move by static equity with PAT, and each one's PAT
        // term (read right after this generation).
        player_set_pat_usage(game_get_player(game, mover), false, 0);
        move_list_reset(all);
        const MoveGenArgs args = {
            .game = game,
            .move_list = all,
            .move_record_type = MOVE_RECORD_ALL,
            .move_sort_type = MOVE_SORT_EQUITY,
            .override_kwg = NULL,
            .eq_margin_movegen = 0,
            .target_equity = EQUITY_MAX_VALUE,
            .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
        };
        generate_moves(&args);
        move_list_sort_moves(all);
        // Passes stay out of the pool (moves sort by equity, passes last).
        int count = move_list_get_count(all);
        while (count > 0 && move_get_type(move_list_get_move(all, count - 1)) ==
                                GAME_EVENT_PASS) {
          count--;
        }
        if (count == 0) {
          Move move;
          move_copy(&move, vnt_static_move(game, static_list));
          value_net_history_record_opponent_move(&histories[1 - mover], &move);
          play_move(&move, game, NULL);
          continue;
        }
        // The pool: the top base, then extras by farthest point.
        move_list_reset(pool);
        const int base_count = count < base ? count : base;
        for (int idx = 0; idx < base_count; idx++) {
          move_list_add_move(pool, move_list_get_move(all, idx));
        }
        const double best =
            equity_to_double(move_get_equity(move_list_get_move(all, 0)));
        int eligible = 0;
        while (eligible < count &&
               equity_to_double(move_get_equity(
                   move_list_get_move(all, eligible))) >= best - margin) {
          eligible++;
        }
        double mean[VNT_KIN_CHEAP] = {0};
        double sd[VNT_KIN_CHEAP] = {0};
        for (int idx = 0; idx < eligible; idx++) {
          vnt_kin_cheap(game, move_list_get_move(all, idx), cheap[idx]);
          for (int dim = 0; dim < VNT_KIN_CHEAP; dim++) {
            mean[dim] += cheap[idx][dim] / eligible;
          }
        }
        for (int idx = 0; idx < eligible; idx++) {
          for (int dim = 0; dim < VNT_KIN_CHEAP; dim++) {
            const double d = cheap[idx][dim] - mean[dim];
            sd[dim] += d * d / eligible;
          }
        }
        for (int dim = 0; dim < VNT_KIN_CHEAP; dim++) {
          sd[dim] = sd[dim] > 1e-9 ? sqrt(sd[dim]) : 1.0;
        }
        for (int idx = base_count; idx < eligible; idx++) {
          nearest[idx] = INFINITY;
          for (int chosen = 0; chosen < base_count; chosen++) {
            double dist = 0.0;
            for (int dim = 0; dim < VNT_KIN_CHEAP; dim++) {
              const double d = (cheap[idx][dim] - cheap[chosen][dim]) / sd[dim];
              dist += d * d;
            }
            nearest[idx] = fmin(nearest[idx], dist);
          }
        }
        for (int pick = 0; pick < extras && base_count + pick < eligible;
             pick++) {
          int far = -1;
          for (int idx = base_count; idx < eligible; idx++) {
            if (nearest[idx] >= 0.0 &&
                (far < 0 || nearest[idx] > nearest[far])) {
              far = idx;
            }
          }
          if (far < 0 || nearest[far] <= 0.0) {
            break;
          }
          move_list_add_move(pool, move_list_get_move(all, far));
          for (int idx = base_count; idx < eligible; idx++) {
            if (nearest[idx] < 0.0) {
              continue;
            }
            double dist = 0.0;
            for (int dim = 0; dim < VNT_KIN_CHEAP; dim++) {
              const double d = (cheap[idx][dim] - cheap[far][dim]) / sd[dim];
              dist += d * d;
            }
            nearest[idx] = fmin(nearest[idx], dist);
          }
          nearest[far] = -1.0;
        }
        move_list_sort_moves(pool);
        const int cands = move_list_get_count(pool);
        // PAT terms need the PAT generation's state: regenerate it for the
        // pool's moves' leaves now (the extras changed nothing in game).
        move_list_reset(all);
        generate_moves(&args);
        for (int idx = 0; idx < cands; idx++) {
          Rack leave;
          get_leave_for_move(move_list_get_move(pool, idx), game, &leave);
          pat_terms[idx] = equity_to_double(
              gen_last_pat_term(move_list_get_move(pool, idx), &leave));
        }
        player_set_pat_usage(game_get_player(game, mover), true, 0);
        // Blocking and setup against a pass, every candidate on the same
        // racks.
        blocking_setup_samples_deal(
            bs_samples, game, VNT_KIN_BS_RACKS, true, true,
            vnt_mix(UINT64_C(7171) ^ (game_idx << 8) ^ (uint64_t)turn));
        blocking_setup_checker_load(bs_checker, game, bs_samples, 1);
        for (int idx = 0; idx < cands; idx++) {
          blocking_setup_checker_measure(
              bs_checker, move_list_get_move(pool, idx), &bs_results[idx]);
        }
        // The net's view of each candidate.
        for (int idx = 0; idx < cands; idx++) {
          value_net_features_for_move(
              game, move_list_get_move(pool, idx), &histories[mover], scratch,
              board + ((size_t)idx * VALUE_NET_BOARD_FLOATS),
              scalars + ((size_t)idx * VALUE_NET_SCALARS));
        }
        value_net_player_evaluate_rows(player, cands, board, scalars, values,
                                       spreads);
        // The sim, every sample recorded.
        for (int idx = 0; idx < cands; idx++) {
          atomic_store(&record.counts[idx], 0);
        }
        ThreadControl *control = thread_control_create();
        thread_control_set_status(control, THREAD_CONTROL_STATUS_STARTED);
        SimArgs sim_args;
        sim_args_fill(4, pool, cands, NULL, config_get_win_pcts(config), NULL,
                      control, game, false, false, threads, 0, cands, 4,
                      sim_seed, (uint64_t)iterations * (uint64_t)cands, 1, 0.0,
                      BAI_THRESHOLD_NONE, 0.0, BAI_SAMPLING_RULE_ROUND_ROBIN,
                      0.0, 1.0, 0.5, 100.0, false, NULL, &sim_args);
        sim_args.rollout_value_net_evaluate = value_net_player_evaluate_rows;
        sim_args.rollout_value_net_context = player;
        sim_args.rollout_value_net_batch = 64;
        sim_args.rollout_value_net_plies = 0;
        sim_args.rollout_value_net_leaf = true;
        sim_args.rollout_value_net_history = histories[1 - mover];
        sim_args.rollout_value_net_own_history = histories[mover];
        sim_args.sample_record = &record;
        // Static rollouts without PAT.
        sim_args.pat_rollout_disabled = true;
        SimResults *results = sim_results_create(0.0);
        simulate_without_ctx(&sim_args, results, error_stack);
        if (!error_stack_is_empty(error_stack)) {
          error_stack_print_and_reset(error_stack);
          log_fatal("kinship sim failed");
        }
        thread_control_destroy(control);
        // Each candidate's samples by seed index.
        for (int idx = 0; idx < cands * iterations; idx++) {
          matrix[idx] = NAN;
        }
        double sim_sum[VNT_KIN_MAX_POOL] = {0};
        double sim_sq[VNT_KIN_MAX_POOL] = {0};
        int sim_n[VNT_KIN_MAX_POOL] = {0};
        for (int play = 0; play < cands; play++) {
          // The play's index in the pool: the sim keeps the move list's
          // order.
          int stored = atomic_load(&record.counts[play]);
          if (stored > capacity) {
            stored = capacity;
          }
          for (int sample = 0; sample < stored; sample++) {
            const size_t at = ((size_t)play * capacity) + (size_t)sample;
            const uint64_t seed = record.seeds[at];
            int low = 0;
            int high = sequence - 1;
            while (low < high) {
              const int middle = (low + high) / 2;
              if (seed_order[middle] < seed) {
                low = middle + 1;
              } else {
                high = middle;
              }
            }
            if (seed_order[low] != seed) {
              log_fatal("kinship: a sample's seed is not in the sequence");
            }
            const int rank = seed_rank[low];
            const double utility = record.utilities[at];
            sim_sum[play] += utility;
            sim_sq[play] += utility * utility;
            sim_n[play]++;
            if (rank < iterations) {
              matrix[((size_t)play * iterations) + (size_t)rank] =
                  (float)utility;
            }
          }
        }
        // Openness: the opponent's best reply by score over racks drawn
        // from the tiles unseen to the mover, the same draws for every
        // candidate.
        double open_mean[VNT_KIN_MAX_POOL];
        double open_max[VNT_KIN_MAX_POOL];
        double open_bingo[VNT_KIN_MAX_POOL];
        const int opponent = 1 - mover;
        for (int idx = 0; idx < cands; idx++) {
          open_mean[idx] = 0.0;
          open_max[idx] = 0.0;
          open_bingo[idx] = 0.0;
          for (int rack_idx = 0; rack_idx < racks; rack_idx++) {
            game_copy(open_game, game);
            player_set_pat_usage(game_get_player(open_game, 0), true, 0);
            player_set_pat_usage(game_get_player(open_game, 1), true, 0);
            return_rack_to_bag(open_game, opponent);
            play_move_without_drawing_tiles(move_list_get_move(pool, idx),
                                            open_game);
            game_seed(open_game, vnt_mix(UINT64_C(5151) ^ (uint64_t)rack_idx ^
                                         (game_idx << 20)));
            set_random_rack(open_game, opponent, NULL);
            move_list_reset(reply_list);
            const MoveGenArgs reply_args = {
                .game = open_game,
                .move_list = reply_list,
                .move_record_type = MOVE_RECORD_BEST,
                .move_sort_type = MOVE_SORT_SCORE,
                .override_kwg = NULL,
                .eq_margin_movegen = 0,
                .target_equity = EQUITY_MAX_VALUE,
                .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
            };
            generate_moves(&reply_args);
            if (move_list_get_count(reply_list) > 0) {
              const Move *reply = move_list_get_move(reply_list, 0);
              const double reply_score =
                  equity_to_double(move_get_score(reply));
              open_mean[idx] += reply_score / racks;
              open_max[idx] = fmax(open_max[idx], reply_score);
              open_bingo[idx] +=
                  (move_get_tiles_played(reply) == RACK_SIZE ? 1.0 : 0.0) /
                  racks;
            }
          }
        }
        const double mover_spread =
            equity_to_double(player_get_score(game_get_player(game, mover)) -
                             player_get_score(game_get_player(game, opponent)));
        for (int idx = 0; idx < cands; idx++) {
          const Move *move = move_list_get_move(pool, idx);
          double f[VNT_KIN_CHEAP];
          vnt_kin_cheap(game, move, f);
          string_builder_clear(name);
          string_builder_add_move(name, game_get_board(game), move, ld, false);
          const double n = sim_n[idx];
          const double sim_mean = n > 0 ? sim_sum[idx] / n : NAN;
          const double sim_sd =
              n > 1 ? sqrt(fmax(0.0, (sim_sq[idx] - (n * sim_mean * sim_mean)) /
                                         (n - 1)))
                    : NAN;
          const double net_spread = value_net_final_spread(
              spreads[idx], value_net_spread_after_move(game, move));
          fprintf(csv,
                  "%d,%llu,%d,%d,%.0f,%d,%d,\"%s\",%.1f,%.3f,%.0f,%.2f,%.2f,"
                  "%.0f,%.3f,%.3f,%.5f,%.2f,%.2f,%.0f,%.3f,%.3f,%.3f,%.3f,%.3f,"
                  "%.6f,%.6f,%d\n",
                  done, (unsigned long long)game_idx, turn, bag, mover_spread,
                  idx, idx < base_count ? 0 : 1, string_builder_peek(name),
                  f[0], f[1], f[2], f[3], f[4], f[5],
                  equity_to_double(move_get_equity(move)) - pat_terms[idx],
                  pat_terms[idx], (1.0 + (double)values[idx]) / 2.0, net_spread,
                  open_mean[idx], open_max[idx], open_bingo[idx],
                  bs_results[idx].blocking_delta, bs_results[idx].setup_delta,
                  bs_results[idx].candidate_reply_mean,
                  bs_results[idx].candidate_followup_mean, sim_mean, sim_sd,
                  sim_n[idx]);
        }
        const int32_t header[3] = {done, cands, iterations};
        fwrite(header, sizeof(int32_t), 3, samples);
        fwrite(matrix, sizeof(float), (size_t)cands * iterations, samples);
        (void)fflush(csv);
        (void)fflush(samples);
        sim_results_destroy(results);
        done++;
        printf("kinship position=%d cands=%d seconds=%.1f\n", done, cands,
               (double)(ctimer_monotonic_ns() - start) / 1e9);
        (void)fflush(stdout);
      }
      Move move;
      move_copy(&move, vnt_static_move(game, static_list));
      value_net_history_record_opponent_move(&histories[1 - mover], &move);
      play_move(&move, game, NULL);
    }
  }
  string_builder_destroy(name);
  blocking_setup_samples_destroy(bs_samples);
  blocking_setup_checker_destroy(bs_checker);
  (void)fclose(csv);
  (void)fclose(samples);
  free(record.seeds);
  free(record.utilities);
  free((void *)record.counts);
  free(matrix);
  free(seed_order);
  free(seed_rank);
  free(cheap);
  free(nearest);
  free(board);
  free(scalars);
  move_list_destroy(all);
  move_list_destroy(pool);
  move_list_destroy(static_list);
  move_list_destroy(reply_list);
  game_destroy(scratch);
  game_destroy(open_game);
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

// "leaveklvcheck:<lexicon>": checks leave_odds_reweight's resampling pool
// against an independent estimate. With the full bag unseen and fixed
// pseudo-random head odds, for kept leaves of 5 and 6 tiles: the mean KLV of
// leaves drawn from the pool tilted by beta, against the mean KLV of exact
// draws weighted by exp(beta * KLV) (the same expectation), and the untilted
// mean.
static void vnt_leaveklvcheck(const char *lexicon) {
  char *settings = get_formatted_string(
      "set -lex %s -wmp true -s1 equity -s2 equity", lexicon);
  Config *config = config_create_or_die(settings);
  free(settings);
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  const Game *game = config_get_game(config);
  const KLV *klv = player_get_klv(game_get_player(game, 0));
  const int letters = ld_get_size(game_get_ld(game));
  int unseen[MAX_ALPHABET_SIZE] = {0};
  bag_increment_unseen_count(game_get_bag(game), unseen);
  for (int player = 0; player < 2; player++) {
    const Rack *rack = player_get_rack(game_get_player(game, player));
    for (int letter = 0; letter < letters; letter++) {
      unseen[letter] += rack_get_letter(rack, letter);
    }
  }
  XoshiroPRNG *prng = prng_create(11);
  float theta[MAX_ALPHABET_SIZE];
  for (int letter = 0; letter < letters; letter++) {
    theta[letter] = (float)(((double)prng_get_random_number(prng, XOSHIRO_MAX) /
                             (double)XOSHIRO_MAX) -
                            0.5);
  }
  const int pool_size = 2048;
  Rack *pool = malloc_or_die(sizeof(Rack) * pool_size);
  double *cumulative = malloc_or_die(sizeof(double) * pool_size);
  for (int size = 5; size <= 6; size++) {
    const double beta = size == 5 ? 0.03 : 0.06;
    LeaveOdds odds;
    if (!leave_odds_prepare(&odds, unseen, theta, letters, size)) {
      log_fatal("leaveklvcheck: no leaves of %d", size);
    }
    // Exact draws: untilted mean and the exp(beta KLV)-weighted mean.
    const int exact_draws = 400000;
    double plain = 0.0;
    double weighted = 0.0;
    double weights = 0.0;
    Rack leave;
    for (int draw = 0; draw < exact_draws; draw++) {
      leave_odds_sample(&odds, prng, &leave);
      const double value = equity_to_double(klv_get_leave_value(klv, &leave));
      plain += value;
      weighted += exp(beta * value) * value;
      weights += exp(beta * value);
    }
    // Pool draws, averaged over several pools.
    const int pools = 20;
    const int pool_draws = 20000;
    double tilted = 0.0;
    for (int pool_idx = 0; pool_idx < pools; pool_idx++) {
      LeaveOdds tilted_odds = odds;
      leave_odds_reweight(&tilted_odds, prng, klv, beta, pool, cumulative,
                          pool_size);
      for (int draw = 0; draw < pool_draws; draw++) {
        leave_odds_sample(&tilted_odds, prng, &leave);
        tilted += equity_to_double(klv_get_leave_value(klv, &leave));
      }
    }
    printf("leaveklvcheck size=%d beta=%.2f untilted_mean_klv=%.3f "
           "weighted_exact_mean_klv=%.3f pool_mean_klv=%.3f\n",
           size, beta, plain / exact_draws, weighted / weights,
           tilted / (pools * pool_draws));
  }
  free(pool);
  free(cumulative);
  prng_destroy(prng);
  config_destroy(config);
}

// "rackparity:<model_dir>:<backend>:<records file>:<decisions>:<out>": the
// opponent-leave head's log odds for the played row of each of the first
// decisions of a distill records file written with opp=1 (opponent records
// in <file>.opp2), through backend and through the CPU, written to
// <out>.csv (decision, then 27 odds from each). For the first ten, prints
// how far 40,000 leaves sampled from the odds (leave_odds.h) land from the
// exact expected counts.
static void vnt_rackparity(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 6) {
    log_fatal("rackparity needs 5 fields");
  }
  const char *model_dir = string_splitter_get_item(fields, 1);
  const char *backend_name = string_splitter_get_item(fields, 2);
  const char *records_path = string_splitter_get_item(fields, 3);
  const int decisions =
      (int)strtol(string_splitter_get_item(fields, 4), NULL, 10);
  const char *out = string_splitter_get_item(fields, 5);
  ErrorStack *error_stack = error_stack_create();
  ValueNetPlayer *player =
      value_net_player_create(model_dir, vnt_parse_backend(backend_name), 0,
                              1.0, 0.5, 100.0, error_stack);
  ValueNetPlayer *cpu = value_net_player_create(
      model_dir, vnt_parse_backend("cpu"), 0, 1.0, 0.5, 100.0, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("rackparity setup failed");
  }
  if (!value_net_player_has_rack_head(player)) {
    log_fatal("%s has no opponent-leave head", model_dir);
  }
  FILE *records = fopen_or_die(records_path, "rb");
  char *opp_path = get_formatted_string("%s.opp2", records_path);
  FILE *opponents = fopen_or_die(opp_path, "rb");
  free(opp_path);
  char *path = get_formatted_string("%s.csv", out);
  FILE *csv = fopen_or_die(path, "w");
  free(path);
  VntDistillRecord *rows =
      malloc_or_die(sizeof(VntDistillRecord) * VNT_DISTILL_CANDIDATES);
  float *board = malloc_or_die(sizeof(float) * VALUE_NET_BOARD_FLOATS);
  float scalars[VALUE_NET_SCALARS];
  XoshiroPRNG *prng = prng_create(17);
  double worst_sampling_error = 0.0;
  // Seconds in each step of turning the head's output into leaves, and how
  // often each ran.
  double backend_seconds = 0.0;
  double cpu_seconds = 0.0;
  double prepare_seconds = 0.0;
  double sample_seconds = 0.0;
  int timed = 0;
  int prepared = 0;
  int64_t samples_drawn = 0;
  for (int decision = 0; decision < decisions; decision++) {
    VntOpponentRecord opponent;
    if (fread(&rows[0], sizeof(VntDistillRecord), 1, records) != 1 ||
        fread(&opponent, sizeof(opponent), 1, opponents) != 1) {
      break;
    }
    const int count = rows[0].candidates;
    if (count > 1 && fread(&rows[1], sizeof(VntDistillRecord),
                           (size_t)count - 1, records) != (size_t)count - 1) {
      break;
    }
    int played = 0;
    for (int row = 0; row < count; row++) {
      if (rows[row].chosen) {
        played = row;
      }
    }
    for (int idx = 0; idx < VALUE_NET_BOARD_FLOATS; idx++) {
      board[idx] =
          (float)((rows[played].board_bits[idx / 8] >> (7 - (idx % 8))) & 1);
    }
    memcpy(scalars, rows[played].scalars, sizeof(scalars));
    float side[VALUE_NET_RACK_SIDE] = {0};
    for (int letter = 0; letter < VALUE_NET_TILE_TYPES; letter++) {
      side[letter] = (float)opponent.played[letter] / 7.0F;
    }
    side[VALUE_NET_TILE_TYPES] = (float)opponent.last_score / 100.0F;
    side[VALUE_NET_TILE_TYPES + 1] =
        (opponent.flags & VNT_OPPONENT_EXCHANGE) ? 1.0F : 0.0F;
    side[VALUE_NET_TILE_TYPES + 2] =
        (opponent.flags & VNT_OPPONENT_BINGO) ? 1.0F : 0.0F;
    side[VALUE_NET_TILE_TYPES + 3] =
        (opponent.flags & VNT_OPPONENT_MOVED) ? 0.0F : 1.0F;
    float theta[VALUE_NET_RACK_LETTERS];
    float theta_cpu[VALUE_NET_RACK_LETTERS];
    int64_t start = ctimer_monotonic_ns();
    value_net_player_rack_odds(player, 1, board, scalars, side, theta);
    const int64_t middle = ctimer_monotonic_ns();
    value_net_player_rack_odds(cpu, 1, board, scalars, side, theta_cpu);
    // The first call compiles the backend's graph.
    if (decision > 0) {
      backend_seconds += (double)(middle - start) / 1e9;
      cpu_seconds += (double)(ctimer_monotonic_ns() - middle) / 1e9;
      timed++;
    }
    fprintf(csv, "%d", decision);
    for (int letter = 0; letter < VALUE_NET_RACK_LETTERS; letter++) {
      fprintf(csv, ",%.6f", theta[letter]);
    }
    for (int letter = 0; letter < VALUE_NET_RACK_LETTERS; letter++) {
      fprintf(csv, ",%.6f", theta_cpu[letter]);
    }
    fprintf(csv, "\n");
    int size = 0;
    for (int letter = 0; letter < VALUE_NET_TILE_TYPES; letter++) {
      size += opponent.leave[letter];
    }
    if (decision < 10 && size > 0) {
      int unseen[MAX_ALPHABET_SIZE] = {0};
      for (int letter = 0; letter < VALUE_NET_TILE_TYPES; letter++) {
        unseen[letter] = (int)lround(scalars[VALUE_NET_TILE_TYPES + letter] *
                                     (float)(rows[played].bag + RACK_SIZE));
      }
      LeaveOdds odds;
      start = ctimer_monotonic_ns();
      const bool ready = leave_odds_prepare(&odds, unseen, theta_cpu,
                                            VALUE_NET_TILE_TYPES, size);
      prepare_seconds += (double)(ctimer_monotonic_ns() - start) / 1e9;
      prepared++;
      if (ready) {
        double expected[MAX_ALPHABET_SIZE];
        leave_odds_expected_counts(&odds, expected);
        double sampled[MAX_ALPHABET_SIZE] = {0};
        const int samples = 40000;
        Rack leave;
        rack_set_dist_size(&leave, VALUE_NET_TILE_TYPES);
        start = ctimer_monotonic_ns();
        for (int sample = 0; sample < samples; sample++) {
          leave_odds_sample(&odds, prng, &leave);
        }
        sample_seconds += (double)(ctimer_monotonic_ns() - start) / 1e9;
        samples_drawn += samples;
        for (int sample = 0; sample < samples; sample++) {
          leave_odds_sample(&odds, prng, &leave);
          for (int letter = 0; letter < VALUE_NET_TILE_TYPES; letter++) {
            sampled[letter] += rack_get_letter(&leave, letter);
          }
        }
        double error = 0.0;
        double total = 0.0;
        for (int letter = 0; letter < VALUE_NET_TILE_TYPES; letter++) {
          error =
              fmax(error, fabs(sampled[letter] / samples - expected[letter]));
          total += expected[letter];
        }
        printf("rackparity decision=%d leave=%d expected_total=%.4f "
               "max_sampling_error=%.4f\n",
               decision, size, total, error);
        worst_sampling_error = fmax(worst_sampling_error, error);
      }
    }
  }
  printf("rackparity worst_sampling_error=%.4f\n", worst_sampling_error);
  printf("rackparity per-call ms: head on %s %.3f, head on cpu %.3f, "
         "leave_odds_prepare %.4f (%d calls); per sample us %.4f\n",
         backend_name, timed > 0 ? backend_seconds * 1e3 / timed : 0.0,
         timed > 0 ? cpu_seconds * 1e3 / timed : 0.0,
         prepared > 0 ? prepare_seconds * 1e3 / prepared : 0.0, prepared,
         samples_drawn > 0 ? sample_seconds * 1e6 / (double)samples_drawn
                           : 0.0);
  prng_destroy(prng);
  free(rows);
  free(board);
  (void)fclose(records);
  (void)fclose(opponents);
  (void)fclose(csv);
  value_net_player_destroy(player);
  value_net_player_destroy(cpu);
  error_stack_destroy(error_stack);
}

// "priorfit:<model_dir>:<backend>:<threads>:<iterations>:<positions>:<out>
// [:hist[:infer[:margin[:mix]]]]":
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
  if (field_count < 7 || field_count > 11) {
    log_fatal("priorfit needs 6 to 10 fields");
  }
  // Fields 9 and 10: the inference's equity margin in points and the
  // probability a rollout draws the opponent's rack uniformly instead.
  const double infer_margin =
      field_count >= 10 ? strtod(string_splitter_get_item(fields, 9), NULL)
                        : 0.0;
  const double infer_mix =
      field_count >= 11 ? strtod(string_splitter_get_item(fields, 10), NULL)
                        : 0.0;
  // infer 1: opponent racks drawn from an inference of their last move.
  const bool use_inference =
      field_count >= 9 &&
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
            infer_args_fill(&inference_args, 20, double_to_equity(infer_margin),
                            NULL, before_last_move, threads, 0, 0, control,
                            false, true, 1 - mover, move_get_score(&last_move),
                            num_exchanged, &played_tiles, &target_known_rack,
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
          sim_args.inference_uniform_mix = infer ? infer_mix : 0.0;
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
// first nnplies= rollout plies, default 1, the rest static; default nn vs
// static). Sim
// players take plies=, cands= (root candidates), ms= (per move) and iters=
// (cap), rcands= and batch= (value net plays: candidates per position and
// iterations per evaluation). clock= gives each player a game clock in
// seconds, from which a sim player without ms= budgets each move
// (PlayChooser; after its flag falls it plays static). A sim player plays
// endgames static, or with egplies= > 0 by an endgame search that many
// plies deep; with pre=static it plays static until the bag is empty. leaf=1
// scores a simnn player's rollouts by the net at the horizon: its value for
// the final ply's move, the net's or (past nnplies=, which may be 0) the top
// static one; with leafevery=m > 1 only every m-th iteration takes the net
// leaf, the rest valued by a control variate (SimArgs). pool= > 0 has a simnn
// player sim
// the cands= plays its net rates best among the top pool= static plays, and
// prior= > 0 counts each one's net utility as that many sim iterations.
// infer=1 draws a sim player's opponent racks from an inference of the
// opponent's last move (ipat=1 with the PAT term when the opponent has it;
// imargin= its equity margin in points, default 0;
// imix= the probability a rollout draws the opponent's rack uniformly
// instead, default 0; imaxleave= > 0 infers only when the opponent kept at
// most that many tiles, and never after an exchange). lodds=1 also draws a
// simnn player's opponent racks from its model's opponent-leave head
// (lshare= its share of the non-uniform draws when leaves are also
// inferred, default 0.5; lklv=1 scales its odds and tilts long kept leaves
// by their KLV as fitted, PlayChooserStrategy.sim_leave_odds_klv). ninfer=1
// infers opponent leaves of at most nmaxleave= tiles (default 2) with a net
// (nimodel=, default the games' model) as the opponent's policy: ncands=
// candidates per rack (16), temperature ntemp= (0.003), and with lodds= the
// head taking nhead= of the non-uniform draws (0.1). uwin=, uspread= and
// uscale= set the utility (as -uwin, -uspread, -uspreadscale; MAGPIE's defaults
// otherwise) that an nn player and each sim, value net replies included, rank
// by. model= sets the net's directory (default <model_dir>). pat=<name> ranks a
// player's moves by static equity with that PAT term (one file for both
// players). An nn player scores the top cands= static moves (default 50); with
// rescore=<dir> it cascades: the net at <dir> rescores its top rescore_top=
// (default 5) candidates and decides. Any key may be given per player as
// <key>_a or <key>_b. Both games of a pair use one seed and the players swap
// seats, so each seat draws the same tiles. Writes <out>.games.csv and
// per-decision timing to <out>.moves.csv.
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
  // Each sim player's net for net-based leave inference (ninfer=1; the
  // model nimodel=, default the games' model), or NULL.
  ValueNetPlayer *net_inference_players[2] = {NULL, NULL};
  // Leave-odds sims with and without a KLV-tilted pool, over every move.
  uint64_t leave_klv_pools = 0;
  uint64_t leave_odds_untilted = 0;
  for (int player_idx = 0; player_idx < 2; player_idx++) {
    if ((kinds[player_idx] == VNT_PLAYER_SIM ||
         kinds[player_idx] == VNT_PLAYER_SIM_NN) &&
        vnt_option_double(fields, 9, "ninfer", player_idx, 0.0) > 0) {
      net_inference_players[player_idx] = value_net_player_create(
          vnt_option(fields, 9, "nimodel", player_idx, model_dir), backend, 0,
          utility_w_winpct[player_idx], utility_w_spread[player_idx],
          utility_spread_scale[player_idx], error_stack);
      if (!error_stack_is_empty(error_stack)) {
        error_stack_print_and_reset(error_stack);
        log_fatal("could not load the leave inference net");
      }
    }
  }
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
    const int rollout_net_plies = (int)strtol(
        vnt_option(fields, 9, "nnplies", player_idx, "1"), NULL, 10);
    const bool rollout_net_used =
        rollout_net_plies > 0 ||
        vnt_option_double(fields, 9, "leaf", player_idx, 0.0) > 0;
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
        // nnplies=0 without leaf=1: static rollouts and leaf, the net only
        // choosing and weighing the root candidates (pool=, prior=).
        .rollout_value_net_evaluate =
            kinds[player_idx] == VNT_PLAYER_SIM_NN && rollout_net_used
                ? value_net_player_evaluate_rows
                : NULL,
        .rollout_value_net_context =
            kinds[player_idx] == VNT_PLAYER_SIM_NN && rollout_net_used
                ? players[player_idx]
                : NULL,
        .rollout_value_net_candidates = (int)strtol(
            vnt_option(fields, 9, "rcands", player_idx, "15"), NULL, 10),
        .rollout_value_net_batch = (int)strtol(
            vnt_option(fields, 9, "batch", player_idx, "8"), NULL, 10),
        .rollout_value_net_plies = (int)strtol(
            vnt_option(fields, 9, "nnplies", player_idx, "1"), NULL, 10),
        .rollout_value_net_leaf =
            vnt_option_double(fields, 9, "leaf", player_idx, 0.0) > 0,
        .rollout_value_net_leaf_every =
            (int)vnt_option_double(fields, 9, "leafevery", player_idx, 1.0),
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
        .sim_inference_uniform_mix =
            vnt_option_double(fields, 9, "imix", player_idx, 0.0),
        .sim_inference_max_leave =
            (int)vnt_option_double(fields, 9, "imaxleave", player_idx, 0.0),
        .sim_inference_threads =
            (int)vnt_option_double(fields, 9, "ithreads", player_idx, 0.0),
        .sim_inference_use_pat =
            vnt_option_double(fields, 9, "ipat", player_idx, 0.0) > 0,
        .sim_net_inference_evaluate = net_inference_players[player_idx] != NULL
                                          ? value_net_player_evaluate_rows
                                          : NULL,
        .sim_net_inference_context = net_inference_players[player_idx],
        .sim_net_inference_max_leave =
            (int)vnt_option_double(fields, 9, "nmaxleave", player_idx, 2.0),
        .sim_net_inference_candidates =
            (int)vnt_option_double(fields, 9, "ncands", player_idx, 16.0),
        .sim_net_inference_temperature =
            vnt_option_double(fields, 9, "ntemp", player_idx, 0.003),
        .sim_net_inference_head_share =
            vnt_option_double(fields, 9, "nhead", player_idx, 0.1),
        .sim_leave_odds_evaluate =
            kinds[player_idx] == VNT_PLAYER_SIM_NN &&
                    vnt_option_double(fields, 9, "lodds", player_idx, 0.0) > 0
                ? value_net_player_rack_odds
                : NULL,
        .sim_leave_odds_context =
            kinds[player_idx] == VNT_PLAYER_SIM_NN ? players[player_idx] : NULL,
        .sim_leave_odds_klv =
            vnt_option_double(fields, 9, "lklv", player_idx, 0.0) > 0,
        .sim_leave_odds_share =
            vnt_option_double(fields, 9, "lshare", player_idx, 0.5),
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
  (void)fprintf(
      moves_out,
      "pair,game,turn,player,bag,total_ms,sim_iterations,"
      "move,static_move,static_agree,net_inference_ms,net_rows,net_calls\n");
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
        double net_inference_ms = -1.0;
        // Rows and calls through this player's net during the move.
        int64_t rows_before = 0;
        int64_t calls_before = 0;
        if (players[player_idx] != NULL) {
          value_net_player_get_evaluated(players[player_idx], &rows_before,
                                         &calls_before);
        }
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
          net_inference_ms =
              (double)chooser_stats.net_inference_micros / 1000.0;
          leave_klv_pools += chooser_stats.leave_klv_pools;
          leave_odds_untilted += chooser_stats.leave_odds_untilted;
          if (!error_stack_is_empty(error_stack)) {
            error_stack_print_and_reset(error_stack);
            log_fatal("sim player failed");
          }
          break;
        }
        game_timer_end_turn(&game_timer);
        int64_t net_rows = 0;
        int64_t net_calls = 0;
        if (players[player_idx] != NULL) {
          value_net_player_get_evaluated(players[player_idx], &net_rows,
                                         &net_calls);
          net_rows -= rows_before;
          net_calls -= calls_before;
        }
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
        (void)fprintf(
            moves_out,
            "%ld,%d,%d,%c,%d,%.3f,%llu,\"%s\",\"%s\",%d,%.1f,%lld,%lld\n",
            pair_idx, game_in_pair, turn, player_idx == 0 ? 'a' : 'b',
            bag_get_letters(game_get_bag(game)), total_ms,
            (unsigned long long)sim_iterations,
            string_builder_peek(move_names[0]),
            string_builder_peek(move_names[1]), static_agree, net_inference_ms,
            (long long)net_rows, (long long)net_calls);
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
  if (leave_klv_pools + leave_odds_untilted > 0) {
    printf("leave_odds sims: %llu with a KLV-tilted pool, %llu without\n",
           (unsigned long long)leave_klv_pools,
           (unsigned long long)leave_odds_untilted);
  }
  // The net and static horizon utilities' agreement on iterations that took
  // both (see SimLeafMoments), for choosing leafevery=.
  SimLeafMoments moments;
  sim_leaf_moments_get(&moments);
  if (moments.iterations > 1) {
    const double var_net = moments.net_ss / moments.iterations;
    const double var_static = moments.static_ss / moments.iterations;
    const double cov = moments.cross_ss / moments.iterations;
    printf("leaf_moments iterations=%.0f sd_net=%.4f sd_static=%.4f "
           "corr=%.3f sd_diff=%.4f\n",
           moments.iterations, sqrt(var_net), sqrt(var_static),
           cov / sqrt(var_net * var_static),
           sqrt(var_net + var_static - (2.0 * cov)));
  }
  value_net_player_destroy(net_inference_players[0]);
  value_net_player_destroy(net_inference_players[1]);
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
  // A second net for net-based inference (student=), or NULL.
  ValueNetPlayer *student;
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
  // Each decision's inference record (VntInferRecord), or NULL.
  FILE *infer_out;
  // With inferq, the largest leave inferred (inferqmax=; 0 for every size).
  int infer_max_leave;
  FILE *infer_time_out;
  FILE *net_infer_out;
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
  // For inferq: the position before the last move and that move, and this
  // thread's inference objects.
  Game *before_last_move = game_duplicate(args->start_game);
  Move last_move;
  move_set_as_pass(&last_move);
  InferenceCtx *infer_ctx = NULL;
  InferenceResults *infer_results = inference_results_create(NULL);
  ThreadControl *infer_control = thread_control_create();
  thread_control_set_status(infer_control, THREAD_CONTROL_STATUS_STARTED);
  ErrorStack *infer_errors = error_stack_create();
  ValueNetLeaveInference *net_inference = value_net_leave_inference_create();
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
    move_set_as_pass(&last_move);
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
        if (args->infer_out != NULL) {
          VntInferRecord inferred;
          memset(&inferred, 0, sizeof(inferred));
          VntInferTimeRecord timing;
          memset(&timing, 0, sizeof(timing));
          timing.game_id = (uint32_t)game_idx;
          timing.turn = (uint16_t)turn;
          inferred.game_id = (uint32_t)game_idx;
          inferred.turn = (uint16_t)turn;
          const VntOpponentRecord *kept = &last[1 - seat];
          int leave_size = 0;
          for (int letter = 0; letter < ld_size; letter++) {
            leave_size += kept->leave[letter];
          }
          const game_event_t last_type = move_get_type(&last_move);
          const bool inferable = (kept->flags & VNT_OPPONENT_MOVED) > 0 &&
                                 leave_size > 0 &&
                                 (args->infer_max_leave == 0 ||
                                  leave_size <= args->infer_max_leave) &&
                                 (last_type == GAME_EVENT_TILE_PLACEMENT_MOVE ||
                                  (last_type == GAME_EVENT_EXCHANGE &&
                                   bag + RACK_SIZE >= RACK_SIZE * 2));
          for (int margin_idx = 0; margin_idx < VNT_INFER_MARGINS;
               margin_idx++) {
            inferred.log_p[margin_idx] = -1e30F;
          }
          if (inferable) {
            Rack played_tiles;
            Rack known;
            Rack mover_rack;
            rack_set_dist_size_and_reset(&played_tiles, ld_size);
            rack_set_dist_size_and_reset(&known, ld_size);
            rack_copy(&mover_rack,
                      player_get_rack(game_get_player(game, seat)));
            int exchanged = 0;
            if (last_type == GAME_EVENT_TILE_PLACEMENT_MOVE) {
              for (int tile_idx = 0;
                   tile_idx < move_get_tiles_length(&last_move); tile_idx++) {
                const MachineLetter ml = move_get_tile(&last_move, tile_idx);
                if (ml != PLAYED_THROUGH_MARKER) {
                  rack_add_letter(&played_tiles, get_is_blanked(ml)
                                                     ? BLANK_MACHINE_LETTER
                                                     : ml);
                }
              }
            } else {
              exchanged = move_get_tiles_played(&last_move);
            }
            Rack actual;
            rack_set_dist_size_and_reset(&actual, ld_size);
            for (int letter = 0; letter < ld_size; letter++) {
              for (int copy = 0; copy < kept->leave[letter]; copy++) {
                rack_add_letter(&actual, (MachineLetter)letter);
              }
            }
            for (int margin_idx = 0; margin_idx < VNT_INFER_MARGINS;
                 margin_idx++) {
              Rack margin_known = known;
              Rack margin_mover = mover_rack;
              Rack margin_played = played_tiles;
              InferenceArgs infer_args;
              infer_args_fill(
                  &infer_args, 60000,
                  double_to_equity(VNT_INFER_MARGIN_POINTS[margin_idx]), NULL,
                  before_last_move, 1, 0, 0, infer_control, false, true,
                  1 - seat, move_get_score(&last_move), exchanged,
                  &margin_played, &margin_known, &margin_mover);
              infer_args.use_pat =
                  VNT_INFER_PAT[margin_idx] &&
                  player_get_pat(game_get_player(before_last_move, 1 - seat)) !=
                      NULL &&
                  !player_get_pat_disabled(
                      game_get_player(before_last_move, 1 - seat));
              infer_args.target_move = &last_move;
              const int64_t infer_start = ctimer_monotonic_ns();
              infer(&infer_args, &infer_ctx, infer_results, infer_errors);
              timing.micros[margin_idx] =
                  (uint32_t)((ctimer_monotonic_ns() - infer_start) / 1000);
              if (!error_stack_is_empty(infer_errors)) {
                error_stack_reset(infer_errors);
                continue;
              }
              const LeaveRackList *leaves =
                  inference_results_get_leave_rack_list(infer_results);
              if (leaves == NULL) {
                continue;
              }
              double total = 0.0;
              double actual_draws = 0.0;
              const int found = leave_rack_list_get_count(leaves);
              for (int idx = 0; idx < found; idx++) {
                const LeaveRack *entry = leave_rack_list_get_rack(leaves, idx);
                Rack leave;
                rack_set_dist_size_and_reset(&leave, ld_size);
                leave_rack_get_leave(entry, &leave);
                const double draws = (double)leave_rack_get_draws(entry);
                total += draws;
                if (racks_are_equal(&leave, &actual)) {
                  actual_draws += draws;
                }
              }
              inferred.leaves[margin_idx] = (uint32_t)found;
              if (actual_draws > 0.0 && total > 0.0) {
                inferred.log_p[margin_idx] = (float)log(actual_draws / total);
              }
              inferred.flags |= (uint16_t)(1U << margin_idx);
            }
          }
          if (fwrite(&inferred, sizeof(inferred), 1, args->infer_out) != 1) {
            log_fatal("could not write the inference records of %s", args->out);
          }
          if (args->infer_time_out != NULL &&
              fwrite(&timing, sizeof(timing), 1, args->infer_time_out) != 1) {
            log_fatal("could not write the inference times of %s", args->out);
          }
          if (args->net_infer_out != NULL) {
            VntNetInferRecord net_record;
            memset(&net_record, 0, sizeof(net_record));
            net_record.game_id = (uint32_t)game_idx;
            net_record.turn = (uint16_t)turn;
            for (int temp_idx = 0; temp_idx < VNT_NET_INFER_TEMPERATURES;
                 temp_idx++) {
              net_record.log_p[temp_idx] = -1e30F;
            }
            if (inferable && last_type == GAME_EVENT_TILE_PLACEMENT_MOVE &&
                leave_size <= 2) {
              Rack actual;
              rack_set_dist_size_and_reset(&actual, ld_size);
              for (int letter = 0; letter < ld_size; letter++) {
                for (int copy = 0; copy < kept->leave[letter]; copy++) {
                  rack_add_letter(&actual, (MachineLetter)letter);
                }
              }
              const Player *target =
                  game_get_player(before_last_move, 1 - seat);
              const ValueNetLeaveInferenceArgs net_args = {
                  .before_move = before_last_move,
                  .target_index = 1 - seat,
                  .move = &last_move,
                  .target_history = &histories[1 - seat],
                  .nontarget_rack =
                      player_get_rack(game_get_player(game, seat)),
                  .leave_size = leave_size,
                  .candidates = 16,
                  .temperature = VNT_NET_INFER_TEMPERATURE[0],
                  .utility_w_winpct = args->w_winpct,
                  .utility_w_spread = args->w_spread,
                  .utility_spread_scale = args->spread_scale,
                  .use_pat = player_get_pat(target) != NULL &&
                             !player_get_pat_disabled(target),
                  .evaluate = value_net_player_evaluate_rows,
                  .evaluate_context = args->teacher,
              };
              const int64_t net_start = ctimer_monotonic_ns();
              // Passes: the teacher with and without PAT, the student with.
              static const int pass_model[3] = {0, 0, 1};
              static const bool pass_pat[3] = {true, false, true};
              for (int pass = 0; pass < 3; pass++) {
                ValueNetPlayer *model =
                    pass_model[pass] == 0 ? args->teacher : args->student;
                if (model == NULL || (pass_pat[pass] && !net_args.use_pat)) {
                  continue;
                }
                ValueNetLeaveInferenceArgs pass_args = net_args;
                pass_args.use_pat = pass_pat[pass];
                pass_args.evaluate_context = model;
                if (!value_net_leave_inference_run(net_inference, &pass_args)) {
                  continue;
                }
                net_record.flags |= (uint16_t)(1U << pass);
                if (pass == 0) {
                  net_record.leaves =
                      (uint32_t)value_net_leave_inference_get_count(
                          net_inference);
                  net_record.rows =
                      (uint32_t)value_net_leave_inference_get_rows(
                          net_inference);
                }
                for (int slot = 0; slot < VNT_NET_INFER_TEMPERATURES; slot++) {
                  if (VNT_NET_INFER_MODEL[slot] != pass_model[pass] ||
                      VNT_NET_INFER_PAT[slot] != pass_pat[pass]) {
                    continue;
                  }
                  value_net_leave_inference_set_temperature(
                      net_inference, VNT_NET_INFER_TEMPERATURE[slot]);
                  const double probability =
                      value_net_leave_inference_probability_of(net_inference,
                                                               &actual);
                  if (probability > 0.0) {
                    net_record.log_p[slot] = (float)log(probability);
                  }
                }
                if (pass == 0) {
                  net_record.micros =
                      (uint32_t)((ctimer_monotonic_ns() - net_start) / 1000);
                }
              }
            }
            if (fwrite(&net_record, sizeof(net_record), 1,
                       args->net_infer_out) != 1) {
              log_fatal("could not write the net inference records of %s",
                        args->out);
            }
          }
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
      if (args->opponent_out != NULL || args->infer_out != NULL) {
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
      game_copy(before_last_move, game);
      move_copy(&last_move, &move);
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
  game_destroy(before_last_move);
  inference_ctx_destroy(infer_ctx);
  inference_results_destroy(infer_results);
  thread_control_destroy(infer_control);
  error_stack_destroy(infer_errors);
  value_net_leave_inference_destroy(net_inference);
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
// (VntOpponentRecord, .opp2), inferq=1 also writes, per decision, what exact
// inference gave the leave the opponent kept (VntInferRecord, .inf; needs
// opp=1), keep=<n> writes only the played row and the
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
  // (.rit, .wit), which only speed up move generation; pat=<name> loads PAT
  // weights, so both players take their candidates by the PAT term.
  char *settings = get_formatted_string(
      "set -lex %s -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -threads 1%s%s%s",
      lexicon,
      vnt_option_double(fields, 9, "tables", 0, 0.0) > 0
          ? " -rit true -wit true -ritmmap true"
          : "",
      vnt_option(fields, 9, "pat", 0, NULL) != NULL ? " -pat " : "",
      vnt_option(fields, 9, "pat", 0, NULL) != NULL
          ? vnt_option(fields, 9, "pat", 0, NULL)
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
      .infer_max_leave = (int)vnt_option_double(fields, 9, "inferqmax", 0, 0.0),
      .games = games,
      .seed = seed,
      .game_stride = workers * threads,
      .out = out,
  };
  args.teacher = value_net_player_create(
      model_dir, backend, VNT_DISTILL_CANDIDATES, args.w_winpct, args.w_spread,
      args.spread_scale, error_stack);
  args.student = NULL;
  const char *student_dir = vnt_option(fields, 9, "student", 0, NULL);
  if (student_dir != NULL && error_stack_is_empty(error_stack)) {
    args.student = value_net_player_create(
        student_dir, backend, VNT_DISTILL_CANDIDATES, args.w_winpct,
        args.w_spread, args.spread_scale, error_stack);
  }
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
    thread_args[thread_idx].infer_time_out = NULL;
    thread_args[thread_idx].net_infer_out = NULL;
    thread_args[thread_idx].infer_out = NULL;
    if (vnt_option_double(fields, 9, "inferq", 0, 0.0) > 0) {
      path = threads > 1 ? get_formatted_string("%s.t%d.inf", out, thread_idx)
                         : get_formatted_string("%s.inf", out);
      thread_args[thread_idx].infer_out = fopen_or_die(path, "wb");
      free(path);
      path = threads > 1 ? get_formatted_string("%s.t%d.inft", out, thread_idx)
                         : get_formatted_string("%s.inft", out);
      thread_args[thread_idx].infer_time_out = fopen_or_die(path, "wb");
      free(path);
      path = threads > 1 ? get_formatted_string("%s.t%d.ninf", out, thread_idx)
                         : get_formatted_string("%s.ninf", out);
      thread_args[thread_idx].net_infer_out = fopen_or_die(path, "wb");
      free(path);
    }
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
    if (thread_args[thread_idx].infer_out != NULL) {
      (void)fclose(thread_args[thread_idx].infer_out);
    }
    if (thread_args[thread_idx].infer_time_out != NULL) {
      (void)fclose(thread_args[thread_idx].infer_time_out);
    }
    if (thread_args[thread_idx].net_infer_out != NULL) {
      (void)fclose(thread_args[thread_idx].net_infer_out);
    }
  }
  const double seconds = (double)(ctimer_monotonic_ns() - args.start_ns) / 1e9;
  printf("distill worker=%ld done games=%ld rows=%ld rows_per_s=%.0f\n", worker,
         atomic_load(&games_played), atomic_load(&rows_written),
         (double)atomic_load(&rows_written) / seconds);
  value_net_player_destroy(args.teacher);
  value_net_player_destroy(args.student);
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

typedef struct VntCpuThroughputWorker {
  const ValueNet *net;
  int rows;
  int calls;
  const float *board;
  const float *scalars;
} VntCpuThroughputWorker;

static void *vnt_cpu_throughput_worker(void *arg) {
  const VntCpuThroughputWorker *worker = arg;
  float value = 0.0F;
  for (int call_idx = 0; call_idx < worker->calls; call_idx++) {
    const int row = call_idx % worker->rows;
    value_net_evaluate_cpu(
        worker->net, 1, worker->board + ((size_t)row * VALUE_NET_BOARD_FLOATS),
        worker->scalars + ((size_t)row * VALUE_NET_SCALARS), &value, NULL);
  }
  return NULL;
}

// "cputhroughput:<dir>:<parity_dir>:<calls>:<threads>[,<threads>...]": CPU
// rows per second with each thread count, every thread evaluating calls
// single rows (cycling through the parity rows) at once.
static void vnt_cpu_throughput(const char *dir, const char *parity_dir,
                               int calls, const char *thread_list) {
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
  double single = 0.0;
  const char *cursor = thread_list;
  while (*cursor != '\0') {
    char *end = NULL;
    const int threads = (int)strtol(cursor, &end, 10);
    if (threads < 1 || threads > VNT_MAX_THROUGHPUT_THREADS) {
      log_fatal("cputhroughput threads must be 1..%d",
                VNT_MAX_THROUGHPUT_THREADS);
    }
    cpthread_t thread_ids[VNT_MAX_THROUGHPUT_THREADS];
    VntCpuThroughputWorker workers[VNT_MAX_THROUGHPUT_THREADS];
    const int64_t start = ctimer_monotonic_ns();
    for (int thread_idx = 0; thread_idx < threads; thread_idx++) {
      workers[thread_idx] = (VntCpuThroughputWorker){
          .net = net,
          .rows = VALUE_NET_PARITY_ROWS,
          .calls = calls,
          .board = board,
          .scalars = scalars,
      };
      cpthread_create(&thread_ids[thread_idx], vnt_cpu_throughput_worker,
                      &workers[thread_idx]);
    }
    for (int thread_idx = 0; thread_idx < threads; thread_idx++) {
      cpthread_join(thread_ids[thread_idx]);
    }
    const double seconds = (double)(ctimer_monotonic_ns() - start) / 1e9;
    const double rate = (double)threads * calls / seconds;
    if (single == 0.0) {
      single = rate / threads;
    }
    printf("cputhroughput kernels=%s threads=%d rows_per_s=%.1f "
           "ms_per_row_per_thread=%.2f speedup=%.2f\n",
           value_net_cpu_kernels(), threads, rate, 1e3 * threads / rate,
           rate / single);
    (void)fflush(stdout);
    cursor = *end == ',' ? end + 1 : end;
  }
  free(board);
  free(scalars);
  value_net_destroy(net);
  error_stack_destroy(error_stack);
}

typedef enum {
  VNT_DEVICE_CPU,
  VNT_DEVICE_GPU,
  VNT_DEVICE_ANE,
  VNT_DEVICE_COUNT,
} vnt_device_t;

typedef struct VntDeviceWorker {
  vnt_device_t device;
  const ValueNet *net;
  ValueNetMetal *metal;
  ValueNetCoreML *coreml;
  int rows;
  const float *board;
  const float *scalars;
  float *values;
  float *spreads;
  atomic_bool *stop;
  int64_t evaluated;
} VntDeviceWorker;

static void *vnt_device_worker(void *arg) {
  VntDeviceWorker *worker = arg;
  while (!atomic_load(worker->stop)) {
    switch (worker->device) {
    case VNT_DEVICE_CPU:
      value_net_evaluate_cpu(worker->net, worker->rows, worker->board,
                             worker->scalars, worker->values, worker->spreads);
      break;
    case VNT_DEVICE_GPU:
      value_net_metal_evaluate(worker->metal, worker->rows, worker->board,
                               worker->scalars, worker->values,
                               worker->spreads);
      break;
    default:
      value_net_coreml_evaluate(worker->coreml, worker->rows, worker->board,
                                worker->scalars, worker->values,
                                worker->spreads);
      break;
    }
    worker->evaluated += worker->rows;
  }
  return NULL;
}

// "devices:<dir>:<parity_dir>:<seconds>:<cpu_threads>:<gpu_threads>:
// <ane_threads>[:<gpu_rows>[:<ane_rows>]]": rows per second on each device
// with the given threads running at once for seconds (after a warm-up):
// CPU threads one row per call, the GPU (Metal fp16) gpu_rows (default 256)
// and the Neural Engine (<dir>/ane.mlpackage) ane_rows (default 256), the
// parity rows tiled. Values and spreads both computed.
static void vnt_devices(const StringSplitter *fields) {
  const int num_fields = (int)string_splitter_get_number_of_items(fields);
  if (num_fields < 7) {
    log_fatal("devices needs at least 6 fields");
  }
  const char *dir = string_splitter_get_item(fields, 1);
  const char *parity_dir = string_splitter_get_item(fields, 2);
  const double seconds = strtod(string_splitter_get_item(fields, 3), NULL);
  int threads[VNT_DEVICE_COUNT];
  for (int device = 0; device < VNT_DEVICE_COUNT; device++) {
    threads[device] =
        (int)strtol(string_splitter_get_item(fields, 4 + device), NULL, 10);
  }
  const int rows[VNT_DEVICE_COUNT] = {
      1,
      num_fields > 7
          ? (int)strtol(string_splitter_get_item(fields, 7), NULL, 10)
          : VALUE_NET_MAX_GPU_ROWS,
      num_fields > 8
          ? (int)strtol(string_splitter_get_item(fields, 8), NULL, 10)
          : VALUE_NET_MAX_GPU_ROWS,
  };
  int total_threads = 0;
  for (int device = 0; device < VNT_DEVICE_COUNT; device++) {
    total_threads += threads[device];
  }
  if (total_threads < 1 || total_threads > VNT_MAX_THROUGHPUT_THREADS) {
    log_fatal("devices needs 1..%d threads", VNT_MAX_THROUGHPUT_THREADS);
  }
  ErrorStack *error_stack = error_stack_create();
  ValueNet *net = value_net_create(dir, error_stack);
  ValueNetMetal *metal = NULL;
  ValueNetCoreML *coreml = NULL;
  if (error_stack_is_empty(error_stack) && threads[VNT_DEVICE_GPU] > 0) {
    metal =
        value_net_metal_create(net, true, threads[VNT_DEVICE_GPU], error_stack);
  }
  if (error_stack_is_empty(error_stack) && threads[VNT_DEVICE_ANE] > 0) {
    char *path = get_formatted_string("%s/ane.mlpackage", dir);
    coreml = value_net_coreml_create(path, 0, error_stack);
    free(path);
  }
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("devices setup failed");
  }
  // The parity rows tiled to the largest call.
  int most_rows = 1;
  for (int device = 0; device < VNT_DEVICE_COUNT; device++) {
    if (rows[device] > most_rows) {
      most_rows = rows[device];
    }
  }
  char *path = get_formatted_string("%s/board.f32", parity_dir);
  float *parity_board = vnt_read_floats(path, (size_t)VALUE_NET_PARITY_ROWS *
                                                  VALUE_NET_BOARD_FLOATS);
  free(path);
  path = get_formatted_string("%s/scalars.f32", parity_dir);
  float *parity_scalars =
      vnt_read_floats(path, (size_t)VALUE_NET_PARITY_ROWS * VALUE_NET_SCALARS);
  free(path);
  float *board =
      malloc_or_die(sizeof(float) * (size_t)most_rows * VALUE_NET_BOARD_FLOATS);
  float *scalars =
      malloc_or_die(sizeof(float) * (size_t)most_rows * VALUE_NET_SCALARS);
  for (int row = 0; row < most_rows; row++) {
    const int source = row % VALUE_NET_PARITY_ROWS;
    memcpy(board + ((size_t)row * VALUE_NET_BOARD_FLOATS),
           parity_board + ((size_t)source * VALUE_NET_BOARD_FLOATS),
           sizeof(float) * VALUE_NET_BOARD_FLOATS);
    memcpy(scalars + ((size_t)row * VALUE_NET_SCALARS),
           parity_scalars + ((size_t)source * VALUE_NET_SCALARS),
           sizeof(float) * VALUE_NET_SCALARS);
  }
  // Warm-up: one call on each device in use (graph compilation).
  float *warm = malloc_or_die(sizeof(float) * 2 * (size_t)most_rows);
  if (metal != NULL) {
    value_net_metal_evaluate(metal, rows[VNT_DEVICE_GPU], board, scalars, warm,
                             warm + most_rows);
  }
  if (coreml != NULL) {
    value_net_coreml_evaluate(coreml, rows[VNT_DEVICE_ANE], board, scalars,
                              warm, warm + most_rows);
  }
  free(warm);
  atomic_bool stop;
  atomic_init(&stop, false);
  cpthread_t thread_ids[VNT_MAX_THROUGHPUT_THREADS];
  VntDeviceWorker workers[VNT_MAX_THROUGHPUT_THREADS];
  int count = 0;
  for (int device = 0; device < VNT_DEVICE_COUNT; device++) {
    for (int thread_idx = 0; thread_idx < threads[device]; thread_idx++) {
      workers[count] = (VntDeviceWorker){
          .device = (vnt_device_t)device,
          .net = net,
          .metal = metal,
          .coreml = coreml,
          .rows = rows[device],
          .board = board,
          .scalars = scalars,
          .values = malloc_or_die(sizeof(float) * (size_t)rows[device]),
          .spreads = malloc_or_die(sizeof(float) * (size_t)rows[device]),
          .stop = &stop,
      };
      count++;
    }
  }
  const int64_t start = ctimer_monotonic_ns();
  for (int worker_idx = 0; worker_idx < count; worker_idx++) {
    cpthread_create(&thread_ids[worker_idx], vnt_device_worker,
                    &workers[worker_idx]);
  }
  while ((double)(ctimer_monotonic_ns() - start) / 1e9 < seconds) {
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 50000000};
    nanosleep(&pause, NULL);
  }
  atomic_store(&stop, true);
  for (int worker_idx = 0; worker_idx < count; worker_idx++) {
    cpthread_join(thread_ids[worker_idx]);
  }
  // Rows finished by the stop over the time until the last call returned.
  const double elapsed = (double)(ctimer_monotonic_ns() - start) / 1e9;
  int64_t evaluated[VNT_DEVICE_COUNT] = {0};
  for (int worker_idx = 0; worker_idx < count; worker_idx++) {
    evaluated[workers[worker_idx].device] += workers[worker_idx].evaluated;
    free(workers[worker_idx].values);
    free(workers[worker_idx].spreads);
  }
  const double cpu = (double)evaluated[VNT_DEVICE_CPU] / elapsed;
  const double gpu = (double)evaluated[VNT_DEVICE_GPU] / elapsed;
  const double ane = (double)evaluated[VNT_DEVICE_ANE] / elapsed;
  printf("devices cpu_threads=%d gpu_threads=%d ane_threads=%d gpu_rows=%d "
         "ane_rows=%d cpu_rows_per_s=%.0f gpu_rows_per_s=%.0f "
         "ane_rows_per_s=%.0f total_rows_per_s=%.0f seconds=%.1f\n",
         threads[VNT_DEVICE_CPU], threads[VNT_DEVICE_GPU],
         threads[VNT_DEVICE_ANE], rows[VNT_DEVICE_GPU], rows[VNT_DEVICE_ANE],
         cpu, gpu, ane, cpu + gpu + ane, elapsed);
  (void)fflush(stdout);
  free(board);
  free(scalars);
  free(parity_board);
  free(parity_scalars);
  value_net_coreml_destroy(coreml);
  value_net_metal_destroy(metal);
  value_net_destroy(net);
  error_stack_destroy(error_stack);
}

typedef struct VntPlayerWorker {
  ValueNetPlayer *player;
  int rows;
  const float *board;
  const float *scalars;
  float *values;
  float *spreads;
  atomic_bool *stop;
  int64_t evaluated;
} VntPlayerWorker;

static void *vnt_player_worker(void *arg) {
  VntPlayerWorker *worker = arg;
  while (!atomic_load(worker->stop)) {
    value_net_player_evaluate_rows(worker->player, worker->rows, worker->board,
                                   worker->scalars, worker->values,
                                   worker->spreads);
    worker->evaluated += worker->rows;
  }
  return NULL;
}

// "playerthroughput:<dir>:<backend>:<parity_dir>:<seconds>:<threads>:<rows>":
// rows per second through value_net_player_evaluate_rows (the games' path,
// with its routing between engines) from threads threads, each calling with
// rows rows (the parity rows tiled), for seconds after a warm-up call.
static void vnt_player_throughput(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 7) {
    log_fatal("playerthroughput needs 6 fields, got %d",
              (int)string_splitter_get_number_of_items(fields) - 1);
  }
  const char *dir = string_splitter_get_item(fields, 1);
  const char *backend_name = string_splitter_get_item(fields, 2);
  const char *parity_dir = string_splitter_get_item(fields, 3);
  const double seconds = strtod(string_splitter_get_item(fields, 4), NULL);
  const int threads =
      (int)strtol(string_splitter_get_item(fields, 5), NULL, 10);
  const int rows = (int)strtol(string_splitter_get_item(fields, 6), NULL, 10);
  if (threads < 1 || threads > VNT_MAX_THROUGHPUT_THREADS || rows < 1 ||
      rows > VALUE_NET_MAX_GPU_ROWS) {
    log_fatal("playerthroughput needs 1..%d threads and 1..%d rows",
              VNT_MAX_THROUGHPUT_THREADS, VALUE_NET_MAX_GPU_ROWS);
  }
  ErrorStack *error_stack = error_stack_create();
  ValueNetPlayer *player = value_net_player_create(
      dir, vnt_parse_backend(backend_name), 0, 1.0, 0.5, 100.0, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("playerthroughput setup failed");
  }
  char *path = get_formatted_string("%s/board.f32", parity_dir);
  float *parity_board = vnt_read_floats(path, (size_t)VALUE_NET_PARITY_ROWS *
                                                  VALUE_NET_BOARD_FLOATS);
  free(path);
  path = get_formatted_string("%s/scalars.f32", parity_dir);
  float *parity_scalars =
      vnt_read_floats(path, (size_t)VALUE_NET_PARITY_ROWS * VALUE_NET_SCALARS);
  free(path);
  float *board =
      malloc_or_die(sizeof(float) * (size_t)rows * VALUE_NET_BOARD_FLOATS);
  float *scalars =
      malloc_or_die(sizeof(float) * (size_t)rows * VALUE_NET_SCALARS);
  for (int row = 0; row < rows; row++) {
    const int source = row % VALUE_NET_PARITY_ROWS;
    memcpy(board + ((size_t)row * VALUE_NET_BOARD_FLOATS),
           parity_board + ((size_t)source * VALUE_NET_BOARD_FLOATS),
           sizeof(float) * VALUE_NET_BOARD_FLOATS);
    memcpy(scalars + ((size_t)row * VALUE_NET_SCALARS),
           parity_scalars + ((size_t)source * VALUE_NET_SCALARS),
           sizeof(float) * VALUE_NET_SCALARS);
  }
  float *warm = malloc_or_die(sizeof(float) * 2 * (size_t)rows);
  value_net_player_evaluate_rows(player, rows, board, scalars, warm,
                                 warm + rows);
  free(warm);
  atomic_bool stop;
  atomic_init(&stop, false);
  cpthread_t thread_ids[VNT_MAX_THROUGHPUT_THREADS];
  VntPlayerWorker workers[VNT_MAX_THROUGHPUT_THREADS];
  const int64_t start = ctimer_monotonic_ns();
  for (int thread_idx = 0; thread_idx < threads; thread_idx++) {
    workers[thread_idx] = (VntPlayerWorker){
        .player = player,
        .rows = rows,
        .board = board,
        .scalars = scalars,
        .values = malloc_or_die(sizeof(float) * (size_t)rows),
        .spreads = malloc_or_die(sizeof(float) * (size_t)rows),
        .stop = &stop,
    };
    cpthread_create(&thread_ids[thread_idx], vnt_player_worker,
                    &workers[thread_idx]);
  }
  while ((double)(ctimer_monotonic_ns() - start) / 1e9 < seconds) {
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 50000000};
    nanosleep(&pause, NULL);
  }
  atomic_store(&stop, true);
  int64_t evaluated = 0;
  for (int thread_idx = 0; thread_idx < threads; thread_idx++) {
    cpthread_join(thread_ids[thread_idx]);
    evaluated += workers[thread_idx].evaluated;
    free(workers[thread_idx].values);
    free(workers[thread_idx].spreads);
  }
  const double elapsed = (double)(ctimer_monotonic_ns() - start) / 1e9;
  printf("playerthroughput backend=%s threads=%d rows=%d rows_per_s=%.0f "
         "seconds=%.1f\n",
         backend_name, threads, rows, (double)evaluated / elapsed, elapsed);
  (void)fflush(stdout);
  free(board);
  free(scalars);
  free(parity_board);
  free(parity_scalars);
  value_net_player_destroy(player);
  error_stack_destroy(error_stack);
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
  } else if (strings_equal(mode, "kinship")) {
    vnt_kinship(fields);
  } else if (strings_equal(mode, "leaveklvcheck") && num_fields == 2) {
    vnt_leaveklvcheck(string_splitter_get_item(fields, 1));
  } else if (strings_equal(mode, "playerthroughput")) {
    vnt_player_throughput(fields);
  } else if (strings_equal(mode, "devices")) {
    vnt_devices(fields);
  } else if (strings_equal(mode, "cputhroughput") && num_fields == 5) {
    vnt_cpu_throughput(
        string_splitter_get_item(fields, 1),
        string_splitter_get_item(fields, 2),
        (int)strtol(string_splitter_get_item(fields, 3), NULL, 10),
        string_splitter_get_item(fields, 4));
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
  } else if (strings_equal(mode, "rackparity")) {
    vnt_rackparity(fields);
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
