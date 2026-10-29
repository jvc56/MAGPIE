#include "peg_net_util.h"

#include "../src/compat/cfile.h"
#include "../src/compat/ctime.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/peg_defs.h"
#include "../src/def/players_data_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/def/value_net_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/thread_control.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/peg.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

FILE *fopen_exclusive_or_die(const char *path, const char *mode) {
  FILE *stream = cfile_open(path, mode, true);
  if (stream == NULL) {
    log_fatal("failed to create file '%s': %s", path, strerror(errno));
  }
  return stream;
}

uint64_t splitmix64(uint64_t value) {
  value += UINT64_C(0x9e3779b97f4a7c15);
  value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
  return value ^ (value >> 31);
}

const Move *top_equity_move(const Game *game, MoveList *list) {
  move_list_reset(list);
  const MoveGenArgs args = {
      .game = game,
      .move_list = list,
      .move_record_type = MOVE_RECORD_BEST,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
  };
  generate_moves(&args);
  return move_list_get_move(list, 0);
}

int fixed_depth_endgame_move(const Game *game, Move *move, int plies) {
  EndgameCtx *ctx = NULL;
  ThreadControl *control = thread_control_create();
  EndgameResults *results = endgame_results_create();
  EndgameArgs args = {
      .thread_control = control,
      .game = game,
      .plies = plies,
      .tt_fraction_of_mem = 0,
      .initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
      .num_threads = 1,
      .use_heuristics = true,
      .num_top_moves = 1,
      .incremental_movegen = true,
      .enable_pv_display = true,
      .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
  };
  endgame_solve_inline(&ctx, &args, results);
  const PVLine *pv = endgame_results_get_pvline(results, ENDGAME_RESULT_BEST);
  const int completed_depth =
      endgame_results_get_depth(results, ENDGAME_RESULT_BEST);
  if (endgame_results_get_status(results) != ENDGAME_RESULT_STATUS_FINISHED ||
      completed_depth != plies || pv->num_moves < 1) {
    log_fatal("fixed-depth match endgame did not finish");
  }
  small_move_to_move(move, &pv->moves[0], game_get_board(game));
  endgame_ctx_destroy(ctx);
  endgame_results_destroy(results);
  thread_control_destroy(control);
  return completed_depth;
}

void endgame_stuck_fractions(Game *game, MoveList *moves, int seat,
                             float *count_fraction, float *point_fraction) {
  const int saved = game_get_player_on_turn_index(game);
  game_set_player_on_turn_index(game, seat);
  const Rack *rack = player_get_rack(game_get_player(game, seat));
  const LetterDistribution *ld = game_get_ld(game);
  uint64_t rack_bits = 0;
  for (int ml = 0; ml < ld_get_size(ld); ml++) {
    if (rack_get_letter(rack, ml) > 0) {
      rack_bits |= UINT64_C(1) << ml;
    }
  }
  uint64_t played = 0;
  const Board *board = game_get_board(game);
  if (board_get_cross_sets_valid(board)) {
    const bool shared = game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG);
    const int cross_idx = board_get_cross_set_index(shared, seat);
    const uint64_t playable =
        board_get_playable_tiles_bv(board, cross_idx, rack_bits & ~UINT64_C(1));
    played = playable & rack_bits;
    if ((rack_bits & 1) && (playable >> 1)) {
      played |= 1;
    }
  }
  if (played != rack_bits && (!board_get_cross_sets_valid(board) ||
                              rack_get_total_letters(rack) > 1)) {
    move_list_reset(moves);
    const MoveGenArgs args = {.game = game,
                              .move_list = moves,
                              .move_record_type = MOVE_RECORD_TILES_PLAYED,
                              .move_sort_type = MOVE_SORT_SCORE,
                              .override_kwg = NULL,
                              .eq_margin_movegen = 0,
                              .target_equity = EQUITY_MAX_VALUE,
                              .tiles_played_bv = &played,
                              .initial_tiles_bv = played};
    generate_moves(&args);
  }
  int total = 0, stuck = 0, points = 0, stuck_points = 0;
  for (int ml = 0; ml < ld_get_size(ld); ml++) {
    const int count = rack_get_letter(rack, ml);
    const int score = count * ld_get_score(ld, ml);
    total += count;
    points += score;
    if (!(played & (UINT64_C(1) << ml))) {
      stuck += count;
      stuck_points += score;
    }
  }
  *count_fraction = total ? (float)stuck / total : 0;
  *point_fraction = points ? (float)stuck_points / points : 0;
  game_set_player_on_turn_index(game, saved);
}

double spread_for_player(const Game *game, int mover) {
  return equity_to_double(player_get_score(game_get_player(game, mover)) -
                          player_get_score(game_get_player(game, 1 - mover)));
}

int greedy_endgame_move(Game *game, MoveList *small, Move *move, bool *out,
                        int *tiles, bool *pass) {
  const int stm = game_get_player_on_turn_index(game);
  const LetterDistribution *ld = game_get_ld(game);
  const Rack *own = player_get_rack(game_get_player(game, stm));
  const Rack *opp = player_get_rack(game_get_player(game, 1 - stm));
  const Equity bonus = calculate_end_rack_points(opp, ld);
  const MoveGenArgs args = {.game = game,
                            .move_list = small,
                            .move_record_type = MOVE_RECORD_BEST_SMALL,
                            .move_sort_type = MOVE_SORT_SCORE,
                            .override_kwg = NULL,
                            .eq_margin_movegen = 0,
                            .target_equity = EQUITY_MAX_VALUE,
                            .best_small_out_bonus = bonus};
  generate_moves(&args);
  const SmallMove *best = small->small_moves[0];
  if (small->count == 0 || small_move_is_pass(best)) {
    *out = false;
    *tiles = 0;
    *pass = true;
    move_set_as_pass(move);
    return 0;
  }
  *pass = false;
  *tiles = small_move_get_tiles_played(best);
  *out = *tiles == rack_get_total_letters(own);
  small_move_to_move(move, best, game_get_board(game));
  return small_move_get_score(best) + (*out ? equity_to_int(bonus) : 0);
}

bool best_reply_shortcuts_disabled(void) {
  const char *value = getenv("PEG_NET_BEST_REPLY");
  return value != NULL && strings_equal(value, "0");
}

// PEG_PICK_TRACE=1: each PEG stage's start (stage, candidates, endgame plies,
// seconds since the solve began) goes to stderr.
static void peg_trace_on_stage_start(int stage_idx, int k_cands, int inner_d,
                                     int emptier_plies, void *user_data) {
  (void)inner_d;
  const int64_t *start = user_data;
  fprintf(stderr, "peg stage %d cands %d plies %d at %.2f s\n", stage_idx,
          k_cands, emptier_plies,
          (double)(ctimer_monotonic_ns() - *start) / 1e9);
}

void default_peg_pick_with_nets(const Game *game, int threads,
                                PegPeg1Chooser chooser, void *chooser_ctx,
                                PegPeg2Chooser chooser2, void *chooser2_ctx,
                                int nest_depth, const Move *const *only,
                                int n_only, Move *best, double *ms) {
  static const int caps[] = {8, 4, 2};
  // PEG2_PICK_MAX_STAGE > 0 stops 2PEG picks (two or more tiles in the bag)
  // after that halving stage; 1PEG picks always run every stage.
  const char *stage_env = getenv("PEG2_PICK_MAX_STAGE");
  const int max_stage =
      stage_env != NULL && bag_get_letters(game_get_bag(game)) >= 2
          ? atoi(stage_env)
          : 0;
  // PEG2_PICK_BUDGET_S > 0 is a wall-clock backstop for those 2PEG picks: PEG
  // returns its last completed stage's ranking when the budget runs out.
  const char *budget_env = getenv("PEG2_PICK_BUDGET_S");
  const double budget =
      budget_env != NULL && bag_get_letters(game_get_bag(game)) >= 2
          ? strtod(budget_env, NULL)
          : 0;
  const char *trace_env = getenv("PEG_PICK_TRACE");
  const bool trace = trace_env != NULL && atoi(trace_env) != 0;
  int64_t start = 0;
  ThreadControl *control = thread_control_create();
  thread_control_set_status(control, THREAD_CONTROL_STATUS_STARTED);
  PegArgs args;
  peg_args_fill(
      game, control, threads, budget, max_stage, /*greedy_seed_only=*/false,
      /*stage_top_k=*/NULL, /*num_stages=*/0, /*inner_top_k=*/0,
      PEG_OPP_RATIONAL, /*scenario_stride=*/0,
      /*nested_enabled=*/true, /*nested_cand_cap=*/0, caps,
      (int)(sizeof(caps) / sizeof(caps[0])), /*nested_stride=*/0,
      /*nested_strides_by_bag=*/NULL,
      /*nested_n_strides_by_bag=*/0,
      /*nested_emptier_ply_cap=*/0, nest_depth, best_reply_shortcuts_disabled(),
      chooser, chooser_ctx, chooser2, chooser2_ctx,
      /*eval_bag_order=*/NULL, /*eval_bag_order_len=*/0, only, n_only,
      /*n_ranked_only_moves=*/0, /*rank_by_utility=*/false,
      /*root_ranker=*/NULL, /*root_ranker_ctx=*/NULL,
      /*protect_moves=*/NULL, /*n_protect_moves=*/0,
      /*include_per_scenario=*/false, trace ? peg_trace_on_stage_start : NULL,
      /*on_cand_done=*/NULL, /*on_scenario_done=*/NULL, trace ? &start : NULL,
      /*poll=*/NULL,
      /*shared_endgame_tt=*/NULL, /*checkpoint=*/NULL,
      /*resume=*/false, &args);
  PegResult result;
  ErrorStack *errors = error_stack_create();
  start = ctimer_monotonic_ns();
  peg_solve(&args, &result, errors);
  *ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
  if (!error_stack_is_empty(errors) || result.n_top_cands < 1) {
    error_stack_print_and_reset(errors);
    log_fatal("peg1exact default peg_solve failed");
  }
  move_copy(best, &result.best_move);
  peg_result_destroy(&result);
  error_stack_destroy(errors);
  thread_control_destroy(control);
}

void default_peg_pick_only(const Game *game, int threads,
                           PegPeg1Chooser chooser, void *chooser_ctx,
                           const Move *const *only, int n_only, Move *best,
                           double *ms) {
  default_peg_pick_with_nets(game, threads, chooser, chooser_ctx, NULL, NULL,
                             PEG_NESTED_DEFAULT_DEPTH, only, n_only, best, ms);
}

void default_peg_pick(const Game *game, int threads, PegPeg1Chooser chooser,
                      void *chooser_ctx, Move *best, double *ms) {
  default_peg_pick_only(game, threads, chooser, chooser_ctx, NULL, 0, best, ms);
}

uint64_t peg_net_cache_key(const Game *game, uint8_t *key) {
  const int on_turn = game_get_player_on_turn_index(game);
  const Board *board = game_get_board(game);
  int pos = 0;
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      key[pos++] = board_get_letter(board, row, col);
    }
  }
  const Rack *rack = player_get_rack(game_get_player(game, on_turn));
  for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
    key[pos++] = (uint8_t)rack_get_letter(rack, (MachineLetter)ml);
  }
  for (int player = 0; player < 2; player++) {
    const int32_t score = player_get_score(game_get_player(game, player));
    memcpy(&key[pos], &score, sizeof(score));
    pos += 4;
  }
  key[pos++] = (uint8_t)on_turn;
  key[pos++] = (uint8_t)game_get_consecutive_scoreless_turns(game);
  uint64_t hash = UINT64_C(1469598103934665603);
  for (int idx = 0; idx < PEG_NET_CACHE_KEY_BYTES; idx++) {
    hash = (hash ^ key[idx]) * UINT64_C(1099511628211);
  }
  return hash;
}

Config *peg_net_config_create(void) {
  Config *config = config_create_or_die(
      "set -path ./collection-runtime-data:./data -lex NWL23 -wmp true -wit "
      "true -rit false -threads 1 -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  return config;
}

int peg_positions_next_root(FILE *positions, char **line, size_t *capacity,
                            Game *root, ErrorStack *errors, int bag_size,
                            int *expected, uint64_t *source_seed) {
  while (getline_ignore_carriage_return(line, capacity, positions) >= 0) {
    int root_id, attempt, turn, seat, bag, first, pass_rank;
    unsigned long long seed;
    if (sscanf(*line, "%d,%llu,%d,%d,%d,%d,%d,%d,%d,", &root_id, &seed,
               &attempt, &turn, &seat, &bag, &first, expected,
               &pass_rank) != 9 ||
        bag != bag_size) {
      log_fatal("peg2 invalid position row");
    }
    char *cgp = strchr(*line, '"'), *end = strrchr(*line, '"');
    if (cgp == NULL || end <= cgp) {
      log_fatal("peg2 invalid CGP");
    }
    *end = '\0';
    game_load_cgp(root, cgp + 1, errors);
    game_set_player_on_turn_index(root, seat);
    if (!error_stack_is_empty(errors) ||
        bag_get_letters(game_get_bag(root)) != bag_size) {
      log_fatal("peg2 CGP load failed");
    }
    *source_seed = seed;
    return root_id;
  }
  return -1;
}

void weighted_stats(const double *values, int stride, const double *prob,
                    int rows, double *stats, int block) {
  double mean = 0, square = 0, low = INFINITY, high = -INFINITY;
  for (int row = 0; row < rows; row++) {
    const double value = values[(size_t)row * stride];
    mean += value * prob[row];
    square += value * value * prob[row];
    low = fmin(low, value);
    high = fmax(high, value);
  }
  stats[0] = mean;
  stats[block] = sqrt(fmax(0, square - mean * mean));
  stats[2 * block] = low;
  stats[3 * block] = high;
}
