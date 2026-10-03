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
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/sim_args.h"
#include "../src/ent/sim_results.h"
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
// static, sim with static rollouts, simnn with value net replies on the
// first rollout ply; default nn vs static). Sim players take plies=,
// cands= (root candidates), ms= (per move) and iters= (cap), rcands= and
// batch= (value net replies). uwin=, uspread= and uscale= set the utility
// (as -uwin, -uspread, -uspreadscale; MAGPIE's defaults otherwise) that an
// nn player and each sim, value net replies included, rank by. model= sets
// the net's directory (default <model_dir>). pat=<name> ranks a player's
// moves by static equity with that PAT term (one file for both players).
// An nn player scores the top cands= static moves (default 50); with
// rescore=<dir> it cascades: the net at <dir> rescores its top
// rescore_top= (default 5) candidates and decides. Any key may be given
// per player as <key>_a or <key>_b. Both games of a pair use one seed and
// the players swap seats, so each seat draws the same tiles. Writes
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
      "-numplays 1 -threads 1%s%s",
      lexicon, pat_name != NULL ? " -pat " : "",
      pat_name != NULL ? pat_name : "");
  Config *config = config_create_or_die(settings);
  free(settings);
  ErrorStack *error_stack = error_stack_create();
  config_load_win_pcts(config, error_stack);
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
  // The replier's history for each sim player's rollouts, refreshed before
  // each of its decisions.
  ValueNetHistory rollout_histories[2];
  PlayChooser *choosers[2] = {NULL, NULL};
  for (int player_idx = 0; player_idx < 2; player_idx++) {
    value_net_history_reset(&rollout_histories[player_idx]);
    if (kinds[player_idx] != VNT_PLAYER_SIM &&
        kinds[player_idx] != VNT_PLAYER_SIM_NN) {
      continue;
    }
    const double ms =
        strtod(vnt_option(fields, 9, "ms", player_idx, "0"), NULL);
    const PlayChooserStrategy strategy = {
        .pre_endgame_eval = PLAY_CHOOSER_EVAL_SIM,
        .endgame_eval = PLAY_CHOOSER_EVAL_STATIC,
        .sim_plies = (int)strtol(
            vnt_option(fields, 9, "plies", player_idx, "2"), NULL, 10),
        .sim_max_candidates = (int)strtol(
            vnt_option(fields, 9, "cands", player_idx, "15"), NULL, 10),
        .sim_max_iterations =
            strtoull(vnt_option(fields, 9, "iters", player_idx, "0"), NULL, 10),
        // An iteration cap alone gets an ample time budget.
        .fixed_seconds_per_move = ms > 0 ? ms / 1000.0 : 600.0,
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
        .rollout_value_net_history = &rollout_histories[player_idx],
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
                "pair,game,a_seat,a_score,b_score,a_spread,a_win,turns\n");
  (void)fprintf(moves_out, "pair,game,turn,player,bag,total_ms\n");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
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
      int turn = 0;
      while (!game_over(game) && turn < VNT_MAX_TURNS) {
        const int seat = game_get_player_on_turn_index(game);
        const int player_idx = seat == a_seat ? 0 : 1;
        if (pat_name != NULL) {
          player_set_pat_usage(game_get_player(game, seat),
                               pat_names[player_idx] == NULL, 0);
        }
        const int64_t start = ctimer_monotonic_ns();
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
          play_chooser_choose_move(choosers[player_idx], game, &move,
                                   error_stack);
          if (!error_stack_is_empty(error_stack)) {
            error_stack_print_and_reset(error_stack);
            log_fatal("sim player failed");
          }
          break;
        }
        (void)fprintf(moves_out, "%ld,%d,%d,%c,%d,%.3f\n", pair_idx,
                      game_in_pair, turn, player_idx == 0 ? 'a' : 'b',
                      bag_get_letters(game_get_bag(game)),
                      (double)(ctimer_monotonic_ns() - start) / 1e6);
        value_net_history_record_opponent_move(&histories[1 - seat], &move);
        play_move(&move, game, NULL);
        turn++;
      }
      const int a_score =
          equity_to_int(player_get_score(game_get_player(game, a_seat)));
      const int b_score =
          equity_to_int(player_get_score(game_get_player(game, 1 - a_seat)));
      const int spread = a_score - b_score;
      const double result = spread > 0 ? 1.0 : (spread == 0 ? 0.5 : 0.0);
      (void)fprintf(games_out, "%ld,%d,%d,%d,%d,%d,%.1f,%d\n", pair_idx,
                    game_in_pair, a_seat, a_score, b_score, spread, result,
                    turn);
      (void)fflush(games_out);
    }
    (void)fflush(moves_out);
  }
  (void)fclose(games_out);
  (void)fclose(moves_out);
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
  FILE *records_out;
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
        if (fwrite(records, sizeof(VntDistillRecord), (size_t)count,
                   args->records_out) != (size_t)count) {
          log_fatal("could not write %s", args->out);
        }
        atomic_fetch_add(args->rows_written, count);
      }
      Move move;
      move_copy(&move, move_list_get_move(list, chosen));
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
// empty bag (not recorded), the top static move.
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
  char *settings = get_formatted_string(
      "set -lex %s -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -threads 1",
      lexicon);
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
    cpthread_create(&thread_ids[thread_idx], vnt_distill_thread,
                    &thread_args[thread_idx]);
  }
  for (int thread_idx = 0; thread_idx < threads; thread_idx++) {
    cpthread_join(thread_ids[thread_idx]);
    (void)fclose(thread_args[thread_idx].records_out);
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

// "simbench:<model_dir>:<none|cpu|fp32|fp16>:<plies>:<root_cands>:
// <threads>:<iterations>:<reply_cands>:<batch>:<positions>": sims of the
// top root_cands static plays, round robin, for a fixed number of
// iterations on NWL23 positions from static games, with value net replies
// on the first rollout ply (or static rollouts for "none"). Prints
// iterations per second per position and overall.
static void vnt_simbench(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 10) {
    log_fatal("simbench needs 9 fields");
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
  printf("simbench backend=%s plies=%d threads=%d reply_cands=%d batch=%d "
         "it_per_s=%.1f (positions after the first)\n",
         backend_name, plies, threads, reply_cands, batch,
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
    const int64_t start = ctimer_monotonic_ns();
    for (int repeat = 0; repeat < repeats; repeat++) {
      value_net_coreml_evaluate(coreml, rows, big_board, big_scalars, values,
                                NULL);
    }
    const double seconds =
        (double)(ctimer_monotonic_ns() - start) / 1e9 / repeats;
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
