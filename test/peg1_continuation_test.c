// How much does the endgame continuation used to score a 1PEG decision change
// what a 1PEG chooser appears to gain over static?
//
// Each board is a 1PEG (one tile in the bag, both racks full) reached by static
// self-play. Every chooser picks from the mover's rack and public information
// only: it sees a copy of the position whose hidden tiles were replaced by a
// canonical assignment from the unseen pool (the bag holds the lowest unseen
// letter, the opponent the rest), so nothing it reads can depend on the actual
// last tile or the opponent's rack. Each distinct pick is then played on the
// real position, drawing the real last tile, and the rest of the game is
// scored three ways:
//   static: both players play static top-1 to the end of the game;
//   twoply: both players play a 2-ply endgame_solve each turn;
//   exact:  one full-depth endgame_solve from the post-move position, whose
//           value is the final spread under optimal play by both sides.
//
// Choosers (spreads are the mover's, in points):
//   static     static top-1 (the baseline);
//   score      highest score;
//   oneply     static top-K ranked by the spread after the move minus the
//              expected highest-scoring opponent reply (plus twice the mover's
//              rack when the reply goes out), over every possible last tile;
//   d2m, d2w   static top-K ranked by a 2-ply endgame_solve per possible last
//              tile: expected margin first (d2m) or expected wins first (d2w);
//   exm, exw   the same with a full-depth solve (PEG1C_ORACLE_K > 0);
//   greedy_all, greedy64  MAGPIE's PEG greedy seed over every move or over
//              the static top 64 (one thread, full enumeration, rational
//              opponent, no nesting);
//   pegfull    MAGPIE's default PEG schedule (boards below PEG1C_PEGFULL).
//
// Env:
//   PEG1C_OUT          JSONL output path (required)
//   PEG1C_SEED         base game seed (default 202610141700)
//   PEG1C_BOARDS       boards generated in total (default 64)
//   PEG1C_SHARD, PEG1C_NSHARDS  this process scores the boards whose index %
//                      NSHARDS == SHARD (default 0 of 1)
//   PEG1C_LEX          lexicon (default NWL23)
//   PEG1C_TOPK         static top-K for oneply and the 2-ply teachers (64)
//   PEG1C_ORACLE_K     static top-K for the exact teachers (0 = off)
//   PEG1C_EXACT_PLIES  plies of the exact solve (default 25)
//   PEG1C_TWO_PLY_NOPRUNE  1 = skip word pruning in 2-ply solves (speed only)
//   PEG1C_PEGFULL      run the default PEG schedule on boards with index
//                      below this (default 0 = none)
//   PEG1C_PEG_THREADS  threads for the default PEG (default 1)
//   PEG1C_TT           transposition table fraction of memory, each of two
//                      tables (default 0.03)
//   PEG1C_SKIP_FROM    comma-separated JSONL files from earlier runs; boards
//                      with a complete line there are skipped
//   PEG1C_REVERSE      1 = score this shard's boards from the last one back

#include "peg1_continuation_test.h"

#include "../src/compat/ctime.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/def/thread_control_defs.h"
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
#include "../src/ent/transposition_table.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/peg.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  P1C_MAX_TOPK = 64,
  P1C_MAX_PICKS = 16,
  P1C_MOVE_STR_SIZE = 64,
  P1C_MAX_PLAYOUT_PLIES = 60,
  P1C_ALL_MOVES_CAPACITY = 4096,
  P1C_MAX_ATTEMPTS = 10000000,
  P1C_TWO_PLY = 2,
  P1C_LINE_SIZE = 1 << 16,
};

typedef struct P1CHarness {
  Config *config;
  Game *game; // the config's game; boards are generated in it
  const LetterDistribution *ld;
  MoveList *best_list; // MOVE_RECORD_BEST scratch
  MoveList *all_list;  // MOVE_RECORD_ALL scratch
  EndgameCtx *endgame_ctx;
  EndgameResults *endgame_results;
  TranspositionTable *tt_two;
  TranspositionTable *tt_exact;
  Game *world;   // scratch for hypothetical worlds
  Game *playout; // scratch for continuations
  int topk;
  int oracle_k;
  int exact_plies;
  bool two_ply_skip_pruning;
  int pegfull_boards;
  int peg_threads;
} P1CHarness;

// A chooser's pick and how long it took.
typedef struct P1CPick {
  const char *chooser;
  Move move;
  char move_str[P1C_MOVE_STR_SIZE];
  double ms;
} P1CPick;

static int env_int(const char *name, int fallback) {
  const char *value = getenv(name);
  return (value && *value) ? (int)strtol(value, NULL, 10) : fallback;
}

static uint64_t env_u64(const char *name, uint64_t fallback) {
  const char *value = getenv(name);
  return (value && *value) ? strtoull(value, NULL, 10) : fallback;
}

static double env_double(const char *name, double fallback) {
  const char *value = getenv(name);
  return (value && *value) ? strtod(value, NULL) : fallback;
}

static void p1c_move_string(const Game *game, const Move *move, char *out) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_move(sb, game_get_board(game), move, game_get_ld(game),
                          false);
  (void)snprintf(out, P1C_MOVE_STR_SIZE, "%s", string_builder_peek(sb));
  string_builder_destroy(sb);
}

static double p1c_elapsed_ms(Timer *timer) {
  return ctimer_elapsed_seconds(timer) * 1000.0;
}

// The mover's spread in points, with each rack's tile values charged against
// its holder when the game has not ended (as peg_greedy_playout does).
static int p1c_spread(const Game *game, int mover) {
  const Player *me = game_get_player(game, mover);
  const Player *op = game_get_player(game, 1 - mover);
  int spread = equity_to_int(player_get_score(me) - player_get_score(op));
  if (game_get_game_end_reason(game) == GAME_END_REASON_NONE) {
    const LetterDistribution *ld = game_get_ld(game);
    spread -= equity_to_int(rack_get_score(ld, player_get_rack(me)));
    spread += equity_to_int(rack_get_score(ld, player_get_rack(op)));
  }
  return spread;
}

// Tiles the mover cannot see: distribution minus the mover's rack minus the
// board. Returns the total.
static int p1c_unseen(const Game *game, int mover,
                      uint8_t unseen[MAX_ALPHABET_SIZE]) {
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
  memset(unseen, 0, sizeof(uint8_t) * MAX_ALPHABET_SIZE);
  for (int ml = 0; ml < ld_size; ml++) {
    unseen[ml] = (uint8_t)ld_get_dist(ld, ml);
  }
  const Rack *mover_rack = player_get_rack(game_get_player(game, mover));
  for (int ml = 0; ml < ld_size; ml++) {
    unseen[ml] -= (uint8_t)rack_get_letter(mover_rack, ml);
  }
  const Board *board = game_get_board(game);
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      if (board_is_empty(board, row, col)) {
        continue;
      }
      const MachineLetter ml = board_get_letter(board, row, col);
      const MachineLetter unseen_ml =
          get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml;
      assert(unseen[unseen_ml] > 0);
      unseen[unseen_ml]--;
    }
  }
  int total = 0;
  for (int ml = 0; ml < ld_size; ml++) {
    total += unseen[ml];
  }
  return total;
}

// Gives the opponent (unseen minus last_tile) and puts last_tile alone in the
// bag.
static void p1c_set_hidden(Game *game, int mover,
                           const uint8_t unseen[MAX_ALPHABET_SIZE],
                           MachineLetter last_tile) {
  const int ld_size = ld_get_size(game_get_ld(game));
  Rack *opp_rack = player_get_rack(game_get_player(game, 1 - mover));
  rack_reset(opp_rack);
  for (int ml = 0; ml < ld_size; ml++) {
    const int count = unseen[ml] - (ml == last_tile ? 1 : 0);
    for (int tile_idx = 0; tile_idx < count; tile_idx++) {
      rack_add_letter(opp_rack, (MachineLetter)ml);
    }
  }
  bag_set_to_tiles(game_get_bag(game), &last_tile, 1);
}

static void p1c_copy_into(Game **slot, const Game *src) {
  if (*slot == NULL) {
    *slot = game_duplicate(src);
  } else {
    game_copy(*slot, src);
  }
}

// ----- solving -------------------------------------------------------------

// Solves the position for the player on turn. Returns the solve's value: the
// spread the rest of the game nets the player on turn. Sets *best to the
// first move of the principal variation when best is non-NULL.
static int p1c_solve(P1CHarness *harness, Game *game, int plies,
                     TranspositionTable *tt, Move *best) {
  // Word pruning only removes words neither rack can play, so skipping it
  // changes the cost of a solve, never its value.
  const bool skip_word_pruning =
      plies == P1C_TWO_PLY && harness->two_ply_skip_pruning;
  ThreadControl *thread_control = thread_control_create();
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  EndgameArgs args;
  endgame_args_fill(
      thread_control, game, /*tt_fraction_of_mem=*/0.0, plies,
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE, /*num_threads=*/1,
      /*use_heuristics=*/true, /*num_top_moves=*/1,
      /*per_ply_callback=*/NULL, /*per_ply_callback_data=*/NULL,
      /*before_search_callback=*/NULL, /*before_search_callback_data=*/NULL,
      /*per_root_move_callback=*/NULL, /*per_root_move_callback_data=*/NULL,
      DUAL_LEXICON_MODE_IGNORANT, /*forced_pass_bypass=*/false,
      /*incremental_movegen=*/true, /*enable_pv_display=*/false,
      /*soft_time_limit=*/0.0, /*hard_time_limit=*/0.0, /*seed=*/1,
      skip_word_pruning, tt, /*max_workers=*/0,
      /*first_win=*/false, /*first_win_fallback_moves=*/0,
      /*use_initial_window=*/false, /*initial_alpha=*/0, /*initial_beta=*/0,
      /*external_deadline_ns=*/0, /*actual_move=*/NULL, &args);
  ErrorStack *error_stack = error_stack_create();
  endgame_solve(&harness->endgame_ctx, &args, harness->endgame_results,
                error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("peg1cont: endgame_solve failed");
  }
  error_stack_destroy(error_stack);
  thread_control_destroy(thread_control);
  const int value =
      endgame_results_get_value(harness->endgame_results, ENDGAME_RESULT_BEST);
  if (best != NULL) {
    const PVLine *pv_line = endgame_results_get_pvline(harness->endgame_results,
                                                       ENDGAME_RESULT_BEST);
    assert(pv_line->num_moves > 0);
    small_move_to_move(best, &pv_line->moves[0], game_get_board(game));
  }
  return value;
}

static void p1c_peg(P1CHarness *harness, const Game *chooser_game,
                    bool greedy_seed_only, const Move *const *only_moves,
                    int n_only_moves, Move *out) {
  static const int stage_top_k[] = {32, 16, 8, 4, 2};
  static const int nested_cand_caps[] = {8, 4, 2};
  static const int nested_strides[] = {0, 1, 1, 5, 7};
  ThreadControl *thread_control = thread_control_create();
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  const bool nested = !greedy_seed_only;
  PegArgs args;
  peg_args_fill(
      chooser_game, thread_control, greedy_seed_only ? 1 : harness->peg_threads,
      /*time_budget_seconds=*/0.0, /*max_stage=*/0, greedy_seed_only,
      stage_top_k, (int)(sizeof(stage_top_k) / sizeof(stage_top_k[0])),
      /*inner_top_k=*/0, PEG_OPP_RATIONAL, /*scenario_stride=*/1, nested,
      /*nested_cand_cap=*/0, nested ? nested_cand_caps : NULL,
      nested ? (int)(sizeof(nested_cand_caps) / sizeof(nested_cand_caps[0]))
             : 0,
      /*nested_stride=*/0, nested ? nested_strides : NULL,
      nested ? (int)(sizeof(nested_strides) / sizeof(nested_strides[0])) : 0,
      /*nested_emptier_ply_cap=*/0, nested ? PEG_NESTED_DEFAULT_DEPTH : 0,
      /*eval_bag_order=*/NULL, /*eval_bag_order_len=*/0, only_moves,
      n_only_moves, /*protect_moves=*/NULL, /*n_protect_moves=*/0,
      /*include_per_scenario=*/false, /*on_stage_start=*/NULL,
      /*on_cand_done=*/NULL, /*on_scenario_done=*/NULL, /*user_data=*/NULL,
      /*poll=*/NULL, harness->tt_exact, /*checkpoint=*/NULL,
      /*resume=*/false, &args);
  PegResult result = {0};
  ErrorStack *error_stack = error_stack_create();
  peg_solve(&args, &result, error_stack);
  if (!error_stack_is_empty(error_stack) || result.n_top_cands == 0) {
    error_stack_print_and_reset(error_stack);
    log_fatal("peg1cont: peg_solve failed");
  }
  error_stack_destroy(error_stack);
  thread_control_destroy(thread_control);
  move_copy(out, &result.best_move);
  peg_result_destroy(&result);
}

// ----- continuations ---------------------------------------------------------

typedef enum {
  P1C_CONT_STATIC,
  P1C_CONT_TWO_PLY,
} p1c_playout_t;

// A turn taken while the bag still holds the last tile, which happens only
// after a pass at the 1PEG: static top-1, or MAGPIE's default PEG pick when
// `use_peg` (the exact continuation's best stand-in for perfect play there).
static void p1c_play_peg_turn(P1CHarness *harness, Game *game, bool use_peg) {
  Move next;
  if (use_peg) {
    p1c_peg(harness, game, /*greedy_seed_only=*/false, NULL, 0, &next);
  } else {
    move_copy(&next, get_top_equity_move(game, harness->best_list));
  }
  play_move(&next, game, NULL);
}

// Plays `move` on a copy of the real position (drawing the real last tile if
// it places tiles), then plays the game out with `kind` for both players.
// Returns the mover's final spread.
static int p1c_playout(P1CHarness *harness, const Game *real, const Move *move,
                       p1c_playout_t kind) {
  const int mover = game_get_player_on_turn_index(real);
  p1c_copy_into(&harness->playout, real);
  Game *game = harness->playout;
  play_move(move, game, NULL);
  for (int ply = 0; ply < P1C_MAX_PLAYOUT_PLIES; ply++) {
    if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
      break;
    }
    if (kind == P1C_CONT_STATIC || bag_get_letters(game_get_bag(game)) > 0) {
      p1c_play_peg_turn(harness, game, /*use_peg=*/false);
    } else {
      Move next;
      (void)p1c_solve(harness, game, P1C_TWO_PLY, harness->tt_two, &next);
      play_move(&next, game, NULL);
    }
  }
  assert(game_get_game_end_reason(game) != GAME_END_REASON_NONE);
  return p1c_spread(game, mover);
}

// The mover's final spread when both sides play the rest of the game
// perfectly after `move`. Sets *proven to whether the endgame solve proved
// its value (it stopped on a pure root before the ply cap).
static int p1c_exact(P1CHarness *harness, const Game *real, const Move *move,
                     double *ms, bool *proven) {
  Timer timer;
  ctimer_start(&timer);
  const int mover = game_get_player_on_turn_index(real);
  p1c_copy_into(&harness->playout, real);
  Game *game = harness->playout;
  play_move(move, game, NULL);
  for (int ply = 0; ply < P1C_MAX_PLAYOUT_PLIES &&
                    game_get_game_end_reason(game) == GAME_END_REASON_NONE &&
                    bag_get_letters(game_get_bag(game)) > 0;
       ply++) {
    p1c_play_peg_turn(harness, game, /*use_peg=*/true);
  }
  *proven = true;
  int spread = p1c_spread(game, mover);
  if (game_get_game_end_reason(game) == GAME_END_REASON_NONE) {
    // The solve's value is what the rest of the game nets the player on turn.
    const int on_turn = game_get_player_on_turn_index(game);
    const int value =
        p1c_solve(harness, game, harness->exact_plies, harness->tt_exact, NULL);
    int depth = 0;
    int root_done = 0;
    int root_total = 0;
    int ply2_done = 0;
    int ply2_total = 0;
    endgame_ctx_get_progress(harness->endgame_ctx, &depth, &root_done,
                             &root_total, &ply2_done, &ply2_total);
    *proven = depth < harness->exact_plies;
    const Player *me = game_get_player(game, mover);
    const Player *op = game_get_player(game, 1 - mover);
    const int spread_now =
        equity_to_int(player_get_score(me) - player_get_score(op));
    spread = on_turn == mover ? spread_now + value : spread_now - value;
  }
  *ms = p1c_elapsed_ms(&timer);
  return spread;
}

// ----- choosers --------------------------------------------------------------

static void p1c_top_by(P1CHarness *harness, const Game *game,
                       move_sort_t sort_type, Move *out) {
  const MoveGenArgs args = {
      .game = game,
      .move_list = harness->best_list,
      .move_record_type = MOVE_RECORD_BEST,
      .move_sort_type = sort_type,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&args);
  move_copy(out, move_list_get_move(harness->best_list, 0));
}

// The static top-K tile placements, best first. Returns how many.
static int p1c_static_top_k(P1CHarness *harness, const Game *game, int k,
                            Move *out) {
  const MoveGenArgs args = {
      .game = game,
      .move_list = harness->all_list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&args);
  move_list_sort_moves(harness->all_list);
  int count = 0;
  const int total = move_list_get_count(harness->all_list);
  for (int move_idx = 0; move_idx < total && count < k; move_idx++) {
    const Move *move = move_list_get_move(harness->all_list, move_idx);
    if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      continue;
    }
    move_copy(&out[count++], move);
  }
  return count;
}

// The world where the mover plays `cand` and draws `last_tile`, with the
// opponent holding the rest of the unseen pool. The opponent is on turn.
static Game *p1c_world(P1CHarness *harness, const Game *chooser_game,
                       const uint8_t unseen[MAX_ALPHABET_SIZE],
                       MachineLetter last_tile, const Move *cand) {
  const int mover = game_get_player_on_turn_index(chooser_game);
  p1c_copy_into(&harness->world, chooser_game);
  p1c_set_hidden(harness->world, mover, unseen, last_tile);
  play_move(cand, harness->world, NULL);
  return harness->world;
}

typedef enum {
  P1C_LEAF_ONE_PLY,
  P1C_LEAF_TWO_PLY,
  P1C_LEAF_EXACT,
} p1c_leaf_t;

// The mover's spread in one world, by the chooser's own leaf evaluation.
static double p1c_leaf(P1CHarness *harness, Game *world, int mover,
                       p1c_leaf_t leaf) {
  const Player *me = game_get_player(world, mover);
  const Player *op = game_get_player(world, 1 - mover);
  const int spread_now =
      equity_to_int(player_get_score(me) - player_get_score(op));
  if (game_get_game_end_reason(world) != GAME_END_REASON_NONE) {
    return p1c_spread(world, mover);
  }
  if (leaf == P1C_LEAF_ONE_PLY) {
    // The opponent's highest-scoring reply, plus twice the mover's rack when
    // it goes out.
    Move reply;
    p1c_top_by(harness, world, MOVE_SORT_SCORE, &reply);
    double value = 0.0;
    if (move_get_type(&reply) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      value = equity_to_double(move_get_score(&reply));
      const Rack *opp_rack = player_get_rack(op);
      if (move_get_tiles_played(&reply) ==
          (int)rack_get_total_letters(opp_rack)) {
        value += 2.0 * equity_to_double(rack_get_score(game_get_ld(world),
                                                       player_get_rack(me)));
      }
    }
    return spread_now - value;
  }
  const int plies =
      leaf == P1C_LEAF_TWO_PLY ? P1C_TWO_PLY : harness->exact_plies;
  TranspositionTable *tt =
      leaf == P1C_LEAF_TWO_PLY ? harness->tt_two : harness->tt_exact;
  return spread_now - p1c_solve(harness, world, plies, tt, NULL);
}

static double p1c_win(double spread) {
  if (spread > 0) {
    return 1.0;
  }
  return spread < 0 ? 0.0 : 0.5;
}

// Ranks `cands` by the expected leaf spread over every possible last tile
// (weighted by its unseen count). Writes the margin-first pick to *margin_out
// and, if non-NULL, the win-first pick (ties by margin) to *win_out.
static void p1c_rank_by_leaf(P1CHarness *harness, const Game *chooser_game,
                             const Move *cands, int num_cands, p1c_leaf_t leaf,
                             Move *margin_out, Move *win_out) {
  const int mover = game_get_player_on_turn_index(chooser_game);
  uint8_t unseen[MAX_ALPHABET_SIZE];
  const int total = p1c_unseen(chooser_game, mover, unseen);
  const int ld_size = ld_get_size(game_get_ld(chooser_game));
  int best_margin_idx = -1;
  int best_win_idx = -1;
  double best_margin = 0.0;
  double best_win = 0.0;
  double best_win_margin = 0.0;
  for (int cand_idx = 0; cand_idx < num_cands; cand_idx++) {
    double margin = 0.0;
    double wins = 0.0;
    for (int ml = 0; ml < ld_size; ml++) {
      if (unseen[ml] == 0) {
        continue;
      }
      Game *world = p1c_world(harness, chooser_game, unseen, (MachineLetter)ml,
                              &cands[cand_idx]);
      const double spread = p1c_leaf(harness, world, mover, leaf);
      const double weight = (double)unseen[ml] / (double)total;
      margin += weight * spread;
      wins += weight * p1c_win(spread);
    }
    if (best_margin_idx < 0 || margin > best_margin) {
      best_margin_idx = cand_idx;
      best_margin = margin;
    }
    if (best_win_idx < 0 || wins > best_win ||
        (wins == best_win && margin > best_win_margin)) {
      best_win_idx = cand_idx;
      best_win = wins;
      best_win_margin = margin;
    }
  }
  assert(best_margin_idx >= 0);
  move_copy(margin_out, &cands[best_margin_idx]);
  if (win_out != NULL) {
    move_copy(win_out, &cands[best_win_idx]);
  }
}

// ----- boards ----------------------------------------------------------------

// Static self-play from `seed` until a turn starts with exactly one tile in
// the bag and both racks full. Returns false if the game skips that state.
static bool p1c_play_to_1peg(P1CHarness *harness, uint64_t seed) {
  Game *game = harness->game;
  game_reset(game);
  game_seed(game, seed);
  draw_starting_racks(game);
  while (bag_get_letters(game_get_bag(game)) > 1) {
    const Move *move = get_top_equity_move(game, harness->best_list);
    play_move(move, game, NULL);
    if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
      return false;
    }
  }
  if (bag_get_letters(game_get_bag(game)) != 1) {
    return false;
  }
  return rack_get_total_letters(player_get_rack(game_get_player(game, 0))) ==
             RACK_SIZE &&
         rack_get_total_letters(player_get_rack(game_get_player(game, 1))) ==
             RACK_SIZE;
}

// The single tile in the bag.
static MachineLetter p1c_bag_tile(const Game *game) {
  const Bag *bag = game_get_bag(game);
  const int ld_size = ld_get_size(game_get_ld(game));
  for (int ml = 0; ml < ld_size; ml++) {
    if (bag_get_letter(bag, (MachineLetter)ml) > 0) {
      return (MachineLetter)ml;
    }
  }
  log_fatal("peg1cont: empty bag");
  return 0;
}

static void p1c_add_pick(P1CPick *picks, int *num_picks, const Game *game,
                         const char *chooser, const Move *move, double ms) {
  assert(*num_picks < P1C_MAX_PICKS);
  P1CPick *pick = &picks[(*num_picks)++];
  pick->chooser = chooser;
  move_copy(&pick->move, move);
  p1c_move_string(game, move, pick->move_str);
  pick->ms = ms;
}

static void p1c_json_string(FILE *out, const char *str) {
  (void)fputc('"', out);
  for (const char *ch = str; *ch; ch++) {
    if (*ch == '"' || *ch == '\\') {
      (void)fputc('\\', out);
    }
    (void)fputc(*ch, out);
  }
  (void)fputc('"', out);
}

static void p1c_score_board(P1CHarness *harness, int board_idx, uint64_t seed,
                            FILE *out) {
  const Game *real = harness->game;
  const int mover = game_get_player_on_turn_index(real);
  const LetterDistribution *ld = harness->ld;
  uint8_t unseen[MAX_ALPHABET_SIZE];
  const int total_unseen = p1c_unseen(real, mover, unseen);
  assert(total_unseen == RACK_SIZE + 1);
  const MachineLetter last_tile = p1c_bag_tile(real);

  // The choosers' view: the lowest unseen letter in the bag, the rest on the
  // opponent's rack. A function of public information only.
  MachineLetter canonical = 0;
  while (unseen[canonical] == 0) {
    canonical++;
  }
  Game *chooser_game = game_duplicate(real);
  p1c_set_hidden(chooser_game, mover, unseen, canonical);

  P1CPick picks[P1C_MAX_PICKS];
  int num_picks = 0;
  Timer timer;
  Move move;
  Move win_move;

  ctimer_start(&timer);
  p1c_top_by(harness, chooser_game, MOVE_SORT_EQUITY, &move);
  p1c_add_pick(picks, &num_picks, real, "static", &move,
               p1c_elapsed_ms(&timer));

  ctimer_start(&timer);
  p1c_top_by(harness, chooser_game, MOVE_SORT_SCORE, &move);
  p1c_add_pick(picks, &num_picks, real, "score", &move, p1c_elapsed_ms(&timer));

  Move cands[P1C_MAX_TOPK];
  const int num_cands =
      p1c_static_top_k(harness, chooser_game, harness->topk, cands);
  if (num_cands > 0) {
    ctimer_start(&timer);
    p1c_rank_by_leaf(harness, chooser_game, cands, num_cands, P1C_LEAF_ONE_PLY,
                     &move, NULL);
    p1c_add_pick(picks, &num_picks, real, "oneply", &move,
                 p1c_elapsed_ms(&timer));

    // A fresh 2-ply table, so nothing from earlier boards or picks feeds the
    // ranking (2-ply results depend on what deeper entries the table holds).
    transposition_table_reset(harness->tt_two);
    ctimer_start(&timer);
    p1c_rank_by_leaf(harness, chooser_game, cands, num_cands, P1C_LEAF_TWO_PLY,
                     &move, &win_move);
    const double ms = p1c_elapsed_ms(&timer);
    p1c_add_pick(picks, &num_picks, real, "d2m", &move, ms);
    p1c_add_pick(picks, &num_picks, real, "d2w", &win_move, ms);

    if (harness->oracle_k > 0) {
      const int oracle_cands =
          num_cands < harness->oracle_k ? num_cands : harness->oracle_k;
      ctimer_start(&timer);
      p1c_rank_by_leaf(harness, chooser_game, cands, oracle_cands,
                       P1C_LEAF_EXACT, &move, &win_move);
      const double oracle_ms = p1c_elapsed_ms(&timer);
      p1c_add_pick(picks, &num_picks, real, "exm", &move, oracle_ms);
      p1c_add_pick(picks, &num_picks, real, "exw", &win_move, oracle_ms);
    }

    ctimer_start(&timer);
    p1c_peg(harness, chooser_game, /*greedy_seed_only=*/true, NULL, 0, &move);
    p1c_add_pick(picks, &num_picks, real, "greedy_all", &move,
                 p1c_elapsed_ms(&timer));

    const Move *only[P1C_MAX_TOPK];
    for (int cand_idx = 0; cand_idx < num_cands; cand_idx++) {
      only[cand_idx] = &cands[cand_idx];
    }
    ctimer_start(&timer);
    p1c_peg(harness, chooser_game, /*greedy_seed_only=*/true, only, num_cands,
            &move);
    p1c_add_pick(picks, &num_picks, real, "greedy64", &move,
                 p1c_elapsed_ms(&timer));

    if (board_idx < harness->pegfull_boards) {
      ctimer_start(&timer);
      p1c_peg(harness, chooser_game, /*greedy_seed_only=*/false, NULL, 0,
              &move);
      p1c_add_pick(picks, &num_picks, real, "pegfull", &move,
                   p1c_elapsed_ms(&timer));
    }
  }
  game_destroy(chooser_game);

  // Score each distinct pick on the real position.
  char *cgp = game_get_cgp(real, true);
  (void)fprintf(out, "{\"id\":%d,\"seed\":%llu,\"cgp\":", board_idx,
                (unsigned long long)seed);
  p1c_json_string(out, cgp);
  free(cgp);
  char *last_str = ld_ml_to_hl(ld, last_tile);
  (void)fprintf(out, ",\"last_tile\":");
  p1c_json_string(out, last_str);
  free(last_str);
  (void)fprintf(out, ",\"unseen\":\"");
  for (int ml = 0; ml < ld_get_size(ld); ml++) {
    for (int tile_idx = 0; tile_idx < unseen[ml]; tile_idx++) {
      char *letter = ld_ml_to_hl(ld, (MachineLetter)ml);
      (void)fputs(letter, out);
      free(letter);
    }
  }
  (void)fprintf(out, "\",\"start_spread\":%d,\"choices\":{",
                p1c_spread(real, mover));
  for (int pick_idx = 0; pick_idx < num_picks; pick_idx++) {
    (void)fprintf(out, "%s\"%s\":", pick_idx ? "," : "",
                  picks[pick_idx].chooser);
    p1c_json_string(out, picks[pick_idx].move_str);
  }
  (void)fprintf(out, "},\"ms\":{");
  for (int pick_idx = 0; pick_idx < num_picks; pick_idx++) {
    (void)fprintf(out, "%s\"%s\":%.3f", pick_idx ? "," : "",
                  picks[pick_idx].chooser, picks[pick_idx].ms);
  }
  (void)fprintf(out, "},\"eval\":{");
  int num_written = 0;
  for (int pick_idx = 0; pick_idx < num_picks; pick_idx++) {
    bool seen = false;
    for (int prev_idx = 0; prev_idx < pick_idx; prev_idx++) {
      if (strcmp(picks[prev_idx].move_str, picks[pick_idx].move_str) == 0) {
        seen = true;
        break;
      }
    }
    if (seen) {
      continue;
    }
    const Move *pick_move = &picks[pick_idx].move;
    const int static_spread =
        p1c_playout(harness, real, pick_move, P1C_CONT_STATIC);
    transposition_table_reset(harness->tt_two);
    const int two_ply_spread =
        p1c_playout(harness, real, pick_move, P1C_CONT_TWO_PLY);
    double exact_ms = 0.0;
    bool exact_proven = false;
    const int exact_spread =
        p1c_exact(harness, real, pick_move, &exact_ms, &exact_proven);
    (void)fprintf(out, "%s", num_written ? "," : "");
    p1c_json_string(out, picks[pick_idx].move_str);
    (void)fprintf(out,
                  ":{\"static\":%d,\"twoply\":%d,\"exact\":%d,"
                  "\"exact_ms\":%.1f,\"exact_complete\":%s}",
                  static_spread, two_ply_spread, exact_spread, exact_ms,
                  exact_proven ? "true" : "false");
    num_written++;
  }
  (void)fprintf(out, "}}\n");
  (void)fflush(out);
}

// Marks done[id] for every complete record (a whole line from `{"id":` to
// the closing `}}`) in each comma-separated file of `paths`. A record cut off
// by a crash is not complete, so its board is scored again.
static void p1c_mark_done(const char *paths, bool *done, int num_boards) {
  char *list = string_duplicate(paths);
  char *line = malloc_or_die(P1C_LINE_SIZE);
  for (char *path = strtok(list, ","); path != NULL; path = strtok(NULL, ",")) {
    FILE *in = fopen(path, "re");
    if (in == NULL) {
      continue;
    }
    while (fgets(line, P1C_LINE_SIZE, in) != NULL) {
      const size_t len = strlen(line);
      if (strncmp(line, "{\"id\":", 6) != 0 || len < 3 ||
          strncmp(line + len - 3, "}}\n", 3) != 0) {
        continue;
      }
      const int id = (int)strtol(line + 6, NULL, 10);
      if (id >= 0 && id < num_boards) {
        done[id] = true;
      }
    }
    (void)fclose(in);
  }
  free(line);
  free(list);
}

void test_peg1_continuation(void) {
  log_set_level(LOG_FATAL);
  const char *out_path = getenv("PEG1C_OUT");
  if (out_path == NULL || *out_path == '\0') {
    log_fatal("peg1cont: set PEG1C_OUT");
  }
  const uint64_t base_seed = env_u64("PEG1C_SEED", 202610141700ULL);
  const int num_boards = env_int("PEG1C_BOARDS", 64);
  const int shard = env_int("PEG1C_SHARD", 0);
  const int num_shards = env_int("PEG1C_NSHARDS", 1);
  const char *lex = getenv("PEG1C_LEX");
  if (lex == NULL || *lex == '\0') {
    lex = "NWL23";
  }
  const double tt_fraction = env_double("PEG1C_TT", 0.03);

  P1CHarness harness = {0};
  char *config_cmd = get_formatted_string(
      "set -lex %s -wmp true -threads 1 -s1 equity -s2 equity", lex);
  harness.config = config_create_or_die(config_cmd);
  free(config_cmd);
  load_and_exec_config_or_die(harness.config, "new");
  harness.game = config_get_game(harness.config);
  harness.ld = game_get_ld(harness.game);
  harness.best_list = move_list_create(1);
  harness.all_list = move_list_create(P1C_ALL_MOVES_CAPACITY);
  harness.endgame_ctx = endgame_ctx_create();
  harness.endgame_results = endgame_results_create();
  harness.tt_two = transposition_table_create(tt_fraction);
  harness.tt_exact = transposition_table_create(tt_fraction);
  harness.topk = env_int("PEG1C_TOPK", P1C_MAX_TOPK);
  if (harness.topk > P1C_MAX_TOPK) {
    harness.topk = P1C_MAX_TOPK;
  }
  harness.oracle_k = env_int("PEG1C_ORACLE_K", 0);
  harness.exact_plies = env_int("PEG1C_EXACT_PLIES", MAX_VARIANT_LENGTH);
  harness.two_ply_skip_pruning = env_int("PEG1C_TWO_PLY_NOPRUNE", 0) != 0;
  harness.pegfull_boards = env_int("PEG1C_PEGFULL", 0);
  harness.peg_threads = env_int("PEG1C_PEG_THREADS", 1);

  bool *done = calloc_or_die((size_t)num_boards, sizeof(bool));
  const char *skip_from = getenv("PEG1C_SKIP_FROM");
  if (skip_from != NULL && *skip_from != '\0') {
    p1c_mark_done(skip_from, done, num_boards);
  }
  FILE *out = fopen_or_die(out_path, "ae");
  // Find this shard's unfinished boards first, so they can be scored in
  // either order: a second runner taking a shard in reverse meets the first
  // in the middle.
  int *todo_idx = malloc_or_die((size_t)num_boards * sizeof(int));
  uint64_t *todo_seed = malloc_or_die((size_t)num_boards * sizeof(uint64_t));
  int num_todo = 0;
  int board_idx = 0;
  for (int attempt = 0; board_idx < num_boards && attempt < P1C_MAX_ATTEMPTS;
       attempt++) {
    const uint64_t seed = base_seed + (uint64_t)attempt;
    if (!p1c_play_to_1peg(&harness, seed)) {
      continue;
    }
    if (board_idx % num_shards == shard && !done[board_idx]) {
      todo_idx[num_todo] = board_idx;
      todo_seed[num_todo] = seed;
      num_todo++;
    }
    board_idx++;
  }
  const bool reverse = env_int("PEG1C_REVERSE", 0) != 0;
  for (int todo_pos = 0; todo_pos < num_todo; todo_pos++) {
    const int pos = reverse ? num_todo - 1 - todo_pos : todo_pos;
    const bool reached = p1c_play_to_1peg(&harness, todo_seed[pos]);
    assert(reached);
    (void)reached;
    Timer timer;
    ctimer_start(&timer);
    p1c_score_board(&harness, todo_idx[pos], todo_seed[pos], out);
    printf("peg1cont board %d seed %llu %.1fs\n", todo_idx[pos],
           (unsigned long long)todo_seed[pos], ctimer_elapsed_seconds(&timer));
    (void)fflush(stdout);
  }
  free(todo_idx);
  free(todo_seed);
  (void)fclose(out);
  free(done);

  game_destroy(harness.world);
  game_destroy(harness.playout);
  transposition_table_destroy(harness.tt_two);
  transposition_table_destroy(harness.tt_exact);
  endgame_results_destroy(harness.endgame_results);
  endgame_ctx_destroy(harness.endgame_ctx);
  move_list_destroy(harness.best_list);
  move_list_destroy(harness.all_list);
  config_destroy(harness.config);
}
