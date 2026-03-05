// Round-robin benchmark: STATIC vs SIM2 vs INFER1 vs INFER20
//
// Usage: ./bin/magpie_test roundrobin
//
// Four player types compete in a full round-robin:
//   STATIC  — picks the equity-best move instantly (no simulation),
//             but uses the endgame solver when the bag is empty.
//   SIM2    — 2-ply simulation with uniform rack sampling.
//   INFER1  — 2-ply simulation with inference (eq_margin=1).
//   INFER20 — 2-ply simulation with inference (eq_margin=20).
//
// Inference players infer after scoring moves only (not exchanges),
// with a 1s time limit.  All sim players use a 2s time budget and
// Top-Two IDS sampling.
//
// Results are logged to CSV after every game so that
// partial results can be computed at any time.

#include "../src/def/bai_defs.h"
#include "../src/def/config_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/bai_result.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/inference_results.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_args.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/win_pct.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/inference.h"
#include "../src/impl/simmer.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

// ── Configuration ──────────────────────────────────────────────────────────

#define NUM_SEEDS 350
#define NUM_PLAYS 15
#define NUM_PLIES 2
#define NUM_THREADS 10

// Time budgets (seconds)
#define BUDGET_AFTER_SCORING_S 2.0
#define BUDGET_AFTER_EXCHANGE_S 2.0

// Inference time limit (seconds)
#define INFER_TIME_LIMIT_S 1.0

// Late-game switches to many-ply sims
#define LATE_GAME_TILE_THRESHOLD 14
#define LATE_GAME_PLIES 99

// Endgame solver (bag empty — all players, including STATIC)
#define ENDGAME_PLIES 25
#define ENDGAME_TIME_LIMIT_S 2.0

// Log file
#define LOG_FILENAME "infer_tournament_4_log.csv"

// ── Player identity ────────────────────────────────────────────────────────

typedef enum {
  PLAYER_STATIC, // Equity-best move instantly; endgame solver when bag empty
  PLAYER_SIM2,   // 2-ply simulation, uniform rack sampling
  PLAYER_INFER1,  // 2-ply simulation + inference, eq_margin=1
  PLAYER_INFER20, // 2-ply simulation + inference, eq_margin=20
  NUM_PLAYER_TYPES,
} player_type_t;

static const char *player_label(player_type_t type) {
  switch (type) {
  case PLAYER_STATIC:
    return "STATIC";
  case PLAYER_SIM2:
    return "SIM2";
  case PLAYER_INFER1:
    return "INFER1";
  case PLAYER_INFER20:
    return "INFR20";
  default:
    return "?????";
  }
}

static bool player_uses_inference(player_type_t type) {
  return type == PLAYER_INFER1 || type == PLAYER_INFER20;
}

static Equity player_infer_eq_margin(player_type_t type) {
  switch (type) {
  case PLAYER_INFER1:
    return int_to_equity(1);
  case PLAYER_INFER20:
    return int_to_equity(20);
  default:
    return 0;
  }
}

// ── Matchup definitions ────────────────────────────────────────────────────

typedef struct {
  player_type_t a;
  player_type_t b;
} Matchup;

// Full round-robin: 4 players → 6 matchups, each as a game pair
static const Matchup matchups[] = {
    {PLAYER_STATIC, PLAYER_SIM2},
    {PLAYER_STATIC, PLAYER_INFER1},
    {PLAYER_STATIC, PLAYER_INFER20},
    {PLAYER_SIM2, PLAYER_INFER1},
    {PLAYER_SIM2, PLAYER_INFER20},
    {PLAYER_INFER1, PLAYER_INFER20},
};
#define NUM_MATCHUPS 6

// ── Per-matchup results ────────────────────────────────────────────────────

typedef struct {
  int a_wins; // wins for matchups[i].a
  int b_wins; // wins for matchups[i].b
  int ties;
  int a_total_score;
  int b_total_score;
} MatchupResults;

// ── Timer thread (fires USER_INTERRUPT after a delay) ──────────────────────

typedef struct {
  ThreadControl *tc;
  double seconds;
  volatile bool done;
} TimerArgs;

static void *timer_thread_func(void *arg) {
  TimerArgs *ta = (TimerArgs *)arg;
  double remaining = ta->seconds;
  while (remaining > 0 && !ta->done) {
    double sleep_time = remaining > 0.05 ? 0.05 : remaining;
    struct timespec ts;
    ts.tv_sec = (time_t)sleep_time;
    ts.tv_nsec = (long)((sleep_time - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
    remaining -= sleep_time;
  }
  if (!ta->done) {
    thread_control_set_status(ta->tc, THREAD_CONTROL_STATUS_USER_INTERRUPT);
  }
  return NULL;
}

// ── Elapsed time helper ────────────────────────────────────────────────────

static double timespec_diff_s(const struct timespec *start,
                              const struct timespec *end) {
  return (double)(end->tv_sec - start->tv_sec) +
         (double)(end->tv_nsec - start->tv_nsec) / 1e9;
}

// ── Count tiles unseen by the player on turn ───────────────────────────────

static int tiles_unseen(const Game *game) {
  return bag_get_letters(game_get_bag(game)) +
         rack_get_total_letters(player_get_rack(game_get_player(
             game, 1 - game_get_player_on_turn_index(game))));
}

// ── Determine turn budget based on previous move type ──────────────────────

static double get_turn_budget(game_event_t prev_move_type) {
  if (prev_move_type == GAME_EVENT_EXCHANGE) {
    return BUDGET_AFTER_EXCHANGE_S;
  }
  return BUDGET_AFTER_SCORING_S;
}

// ── Run inference on the opponent's previous move ──────────────────────────

static bool run_inference_on_prev_move(
    const Game *game_before_prev, ThreadControl *tc, const Move *prev_move,
    int prev_player_index, Equity eq_margin,
    InferenceResults *inference_results, ErrorStack *error_stack,
    double *elapsed_out) {

  const LetterDistribution *ld = game_get_ld(game_before_prev);
  const int ld_size = ld_get_size(ld);

  Rack target_played_tiles;
  rack_set_dist_size_and_reset(&target_played_tiles, ld_size);
  Rack target_known_rack;
  rack_set_dist_size_and_reset(&target_known_rack, ld_size);
  Rack nontarget_known_rack;
  rack_set_dist_size_and_reset(&nontarget_known_rack, ld_size);
  const int nontarget_index = 1 - prev_player_index;
  rack_copy(&nontarget_known_rack,
            player_get_rack(
                game_get_player(game_before_prev, nontarget_index)));

  Equity score = move_get_score(prev_move);

  const int tiles_length = move_get_tiles_length(prev_move);
  for (int i = 0; i < tiles_length; i++) {
    const MachineLetter ml = move_get_tile(prev_move, i);
    if (ml != PLAYED_THROUGH_MARKER) {
      if (get_is_blanked(ml)) {
        rack_add_letter(&target_played_tiles, BLANK_MACHINE_LETTER);
      } else {
        rack_add_letter(&target_played_tiles, ml);
      }
    }
  }

  InferenceArgs args;
  infer_args_fill(&args, NUM_PLAYS, eq_margin, NULL, game_before_prev,
                  NUM_THREADS, 0, tc, false, true, prev_player_index, score, 0,
                  &target_played_tiles, &target_known_rack,
                  &nontarget_known_rack);

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  thread_control_set_status(tc, THREAD_CONTROL_STATUS_STARTED);

  TimerArgs ta = {.tc = tc, .seconds = INFER_TIME_LIMIT_S, .done = false};
  pthread_t timer_tid;
  pthread_create(&timer_tid, NULL, timer_thread_func, &ta);

  infer_without_ctx(&args, inference_results, error_stack);

  ta.done = true;
  pthread_join(timer_tid, NULL);

  clock_gettime(CLOCK_MONOTONIC, &t1);
  *elapsed_out = timespec_diff_s(&t0, &t1);

  bool interrupted =
      thread_control_get_status(tc) == THREAD_CONTROL_STATUS_USER_INTERRUPT;

  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    return false;
  }

  return !interrupted;
}

// ── Play one static turn (equity-best, no simulation) ──────────────────────

static const Move *play_static_turn(Game *game, MoveList *move_list) {
  const MoveGenArgs gen_args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_BEST,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&gen_args);
  const Move *move = move_list_get_move(move_list, 0);
  play_move(move, game, NULL);
  return move;
}

// ── Play one simmed turn ───────────────────────────────────────────────────

static const Move *play_sim_turn(Game *game, MoveList *move_list,
                                 SimResults *sim_results, SimCtx **sim_ctx,
                                 WinPct *win_pcts, ThreadControl *tc,
                                 int num_plies, double budget_s,
                                 bool use_inference,
                                 InferenceArgs *inference_args,
                                 InferenceResults *inference_results,
                                 ErrorStack *error_stack) {
  const MoveGenArgs gen_args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&gen_args);

  const int num_moves = move_list_get_count(move_list);
  if (num_moves <= 1) {
    const Move *move = move_list_get_move(move_list, 0);
    play_move(move, game, NULL);
    return move;
  }

  SimArgs sim_args;
  sim_args_fill(num_plies, move_list, NULL, win_pcts, inference_results, tc,
                game, use_inference, false, NUM_THREADS, 0, NUM_PLAYS,
                num_plies, 0, UINT64_MAX, 50, 101.0, BAI_THRESHOLD_NONE, 999,
                BAI_SAMPLING_RULE_TOP_TWO_IDS, -1.0, inference_args, &sim_args);

  thread_control_set_status(tc, THREAD_CONTROL_STATUS_STARTED);

  TimerArgs ta = {.tc = tc, .seconds = budget_s, .done = false};
  pthread_t timer_tid;
  pthread_create(&timer_tid, NULL, timer_thread_func, &ta);

  simulate(&sim_args, sim_ctx, sim_results, error_stack);

  ta.done = true;
  pthread_join(timer_tid, NULL);

  // If simulate failed (e.g. inference error), fall back to equity-best move.
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    printf("    *** SIM FAILED, falling back to equity-best ***\n");
    const Move *move = move_list_get_move(move_list, 0);
    play_move(move, game, NULL);
    return move;
  }

  BAIResult *bai_result = sim_results_get_bai_result(sim_results);
  int best_arm = bai_result_get_best_arm(bai_result);
  if (best_arm < 0) {
    best_arm = 0;
  }

  const Move *best_move =
      simmed_play_get_move(sim_results_get_simmed_play(sim_results, best_arm));
  play_move(best_move, game, NULL);
  return best_move;
}

// ── Play one endgame turn (bag empty → perfect information) ────────────────

static const Move *play_endgame_turn(Game *game, MoveList *move_list,
                                     EndgameSolver *solver,
                                     EndgameResults *endgame_results,
                                     ThreadControl *tc,
                                     ErrorStack *error_stack) {
  EndgameArgs args = {
      .game = game,
      .thread_control = tc,
      .plies = ENDGAME_PLIES,
      .tt_fraction_of_mem = 0.05,
      .initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
      .num_threads = NUM_THREADS,
      .num_top_moves = 1,
      .use_heuristics = true,
      .forced_pass_bypass = true,
  };

  thread_control_set_status(tc, THREAD_CONTROL_STATUS_STARTED);
  endgame_results_reset(endgame_results);

  TimerArgs ta = {.tc = tc, .seconds = ENDGAME_TIME_LIMIT_S, .done = false};
  pthread_t timer_tid;
  pthread_create(&timer_tid, NULL, timer_thread_func, &ta);

  endgame_solve(solver, &args, endgame_results, error_stack);

  ta.done = true;
  pthread_join(timer_tid, NULL);

  const PVLine *pv =
      endgame_results_get_pvline(endgame_results, ENDGAME_RESULT_BEST);
  Move *spare = move_list_get_spare_move(move_list);
  if (pv->num_moves > 0) {
    small_move_to_move(spare, &pv->moves[0], game_get_board(game));
  } else {
    move_set_as_pass(spare);
  }
  play_move(spare, game, NULL);
  return spare;
}

// ── Play a full game ───────────────────────────────────────────────────────
// Returns: +1 if p0 wins, -1 if p1 wins, 0 if tie.
// Writes p0_score_out and p1_score_out.

static int play_game(Game *game, MoveList *move_list, SimResults *sim_results,
                     SimCtx **sim_ctx, WinPct *win_pcts, ThreadControl *tc,
                     InferenceResults *inference_results,
                     EndgameSolver *endgame_solver,
                     EndgameResults *endgame_results, player_type_t p0_type,
                     player_type_t p1_type, uint64_t seed, int *p0_score_out,
                     int *p1_score_out, int *turns_out, double *elapsed_out) {
  game_reset(game);
  game_seed(game, seed);
  draw_starting_racks(game);

  ErrorStack *error_stack = error_stack_create();
  StringBuilder *sb = string_builder_create();

  struct timespec game_start, game_end;
  clock_gettime(CLOCK_MONOTONIC, &game_start);

  game_event_t prev_move_type = GAME_EVENT_TILE_PLACEMENT_MOVE;
  int prev_player_index = -1;
  Move saved_prev_move;

  Game *game_before_prev_move = game_duplicate(game);

  int turn = 0;
  while (!game_over(game)) {
    int player_idx = game_get_player_on_turn_index(game);
    player_type_t player_type = (player_idx == 0) ? p0_type : p1_type;
    const char *label = player_label(player_type);
    int unseen = tiles_unseen(game);
    const Bag *bag = game_get_bag(game);
    int bag_tiles = bag_get_letters(bag);

    double turn_budget = get_turn_budget(prev_move_type);

    int effective_plies = NUM_PLIES;
    if (!bag_is_empty(bag) && unseen < LATE_GAME_TILE_THRESHOLD) {
      effective_plies = LATE_GAME_PLIES;
    }

    struct timespec turn_start, turn_end;
    clock_gettime(CLOCK_MONOTONIC, &turn_start);

    const Move *move = NULL;
    double infer_elapsed = 0.0;
    bool did_infer = false;
    bool infer_ok = false;
    bool used_endgame = false;

    if (bag_is_empty(bag)) {
      // Endgame: all players (including STATIC) use endgame solver.
      used_endgame = true;
      move = play_endgame_turn(game, move_list, endgame_solver,
                               endgame_results, tc, error_stack);
    } else if (player_type == PLAYER_STATIC) {
      // STATIC: pick equity-best move instantly.
      Game *game_snapshot = game_duplicate(game);
      move = play_static_turn(game, move_list);
      game_copy(game_before_prev_move, game_snapshot);
      game_destroy(game_snapshot);
    } else {
      // SIM or INFER player

      // Try inference for INFER players after scoring moves (not exchanges)
      if (player_uses_inference(player_type) &&
          prev_player_index >= 0 &&
          prev_move_type == GAME_EVENT_TILE_PLACEMENT_MOVE &&
          bag_tiles >= RACK_SIZE) {
        did_infer = true;
        infer_ok = run_inference_on_prev_move(
            game_before_prev_move, tc, &saved_prev_move, prev_player_index,
            player_infer_eq_margin(player_type), inference_results, error_stack,
            &infer_elapsed);

        if (!infer_ok) {
          printf("    *** INFERENCE INTERRUPTED after %.2fs ***\n",
                 infer_elapsed);
        }
      }

      double sim_budget = turn_budget - infer_elapsed;
      if (sim_budget < 0.5) {
        sim_budget = 0.5;
      }

      // If inference ate most of the budget, fall back to equity-best.
      // With minp=50 and 15 arms, initial phase alone needs meaningful time.
      if (sim_budget < 1.0) {
        printf("    *** Budget too low (%.2fs), falling back to equity-best ***\n",
               sim_budget);
        const MoveGenArgs gen_args_fb = {
            .game = game,
            .move_list = move_list,
            .move_record_type = MOVE_RECORD_BEST,
            .move_sort_type = MOVE_SORT_EQUITY,
            .override_kwg = NULL,
            .thread_index = 0,
            .eq_margin_movegen = 0,
            .target_equity = EQUITY_MAX_VALUE,
            .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
        };
        generate_moves(&gen_args_fb);
        Game *game_snapshot = game_duplicate(game);
        move = move_list_get_move(move_list, 0);
        play_move(move, game, NULL);
        game_copy(game_before_prev_move, game_snapshot);
        game_destroy(game_snapshot);
        goto turn_done;
      }

      bool use_inference = (did_infer && infer_ok);

      InferenceArgs inference_args;
      const LetterDistribution *ld = game_get_ld(game);
      const int ld_size = ld_get_size(ld);
      Rack ia_target_played_tiles;
      rack_set_dist_size_and_reset(&ia_target_played_tiles, ld_size);
      Rack ia_target_known_rack;
      rack_set_dist_size_and_reset(&ia_target_known_rack, ld_size);
      Rack ia_nontarget_known_rack;
      rack_set_dist_size_and_reset(&ia_nontarget_known_rack, ld_size);
      rack_copy(&ia_nontarget_known_rack,
                player_get_rack(
                    game_get_player(game_before_prev_move, player_idx)));

      if (use_inference) {
        Equity score = move_get_score(&saved_prev_move);

        const int tiles_length = move_get_tiles_length(&saved_prev_move);
        for (int i = 0; i < tiles_length; i++) {
          const MachineLetter ml = move_get_tile(&saved_prev_move, i);
          if (ml != PLAYED_THROUGH_MARKER) {
            if (get_is_blanked(ml)) {
              rack_add_letter(&ia_target_played_tiles, BLANK_MACHINE_LETTER);
            } else {
              rack_add_letter(&ia_target_played_tiles, ml);
            }
          }
        }

        infer_args_fill(&inference_args, NUM_PLAYS,
                        player_infer_eq_margin(player_type), NULL,
                        game_before_prev_move, NUM_THREADS, 0, tc, false, true,
                        prev_player_index, score, 0,
                        &ia_target_played_tiles, &ia_target_known_rack,
                        &ia_nontarget_known_rack);
      }

      // Save game state BEFORE play_sim_turn changes it.
      // inference_args.game points to game_before_prev_move, so we must
      // not overwrite it until after simulate() is done.
      Game *game_snapshot = game_duplicate(game);

      move = play_sim_turn(game, move_list, sim_results, sim_ctx, win_pcts, tc,
                           effective_plies, sim_budget, use_inference,
                           use_inference ? &inference_args : NULL,
                           inference_results, error_stack);

      // Now safe to update game_before_prev_move for the next turn.
      game_copy(game_before_prev_move, game_snapshot);
      game_destroy(game_snapshot);
    }

turn_done:
    clock_gettime(CLOCK_MONOTONIC, &turn_end);
    double turn_elapsed = timespec_diff_s(&turn_start, &turn_end);
    turn++;

    // Log the move
    string_builder_clear(sb);
    string_builder_add_move(sb, game_get_board(game), move, game_get_ld(game),
                            true);
    if (used_endgame) {
      printf("    Turn %2d (%-6s p%d): %s  [%.1fs, endgame]\n", turn, label,
             player_idx, string_builder_peek(sb), turn_elapsed);
    } else if (player_type == PLAYER_STATIC) {
      printf("    Turn %2d (%-6s p%d): %s  [instant]\n", turn, label,
             player_idx, string_builder_peek(sb));
    } else if (did_infer) {
      uint64_t iters = sim_results_get_iteration_count(sim_results);
      printf("    Turn %2d (%-6s p%d): %s  [%.1fs, %" PRIu64
             " iters, infer=%.2fs%s]\n",
             turn, label, player_idx, string_builder_peek(sb), turn_elapsed,
             iters, infer_elapsed, infer_ok ? "" : " INTERRUPTED");
    } else {
      uint64_t iters = sim_results_get_iteration_count(sim_results);
      printf("    Turn %2d (%-6s p%d): %s  [%.1fs, %" PRIu64 " iters]\n",
             turn, label, player_idx, string_builder_peek(sb), turn_elapsed,
             iters);
    }

    if (!error_stack_is_empty(error_stack)) {
      printf("  ERROR on turn %d: ", turn);
      error_stack_print_and_reset(error_stack);
    }

    // Save previous move info for next turn
    prev_move_type = move_get_type(move);
    prev_player_index = player_idx;
    saved_prev_move = *move;
  }

  clock_gettime(CLOCK_MONOTONIC, &game_end);
  *elapsed_out = timespec_diff_s(&game_start, &game_end);
  *turns_out = turn;

  *p0_score_out = equity_to_int(player_get_score(game_get_player(game, 0)));
  *p1_score_out = equity_to_int(player_get_score(game_get_player(game, 1)));

  game_destroy(game_before_prev_move);
  string_builder_destroy(sb);
  error_stack_destroy(error_stack);

  if (*p0_score_out > *p1_score_out)
    return 1;
  if (*p1_score_out > *p0_score_out)
    return -1;
  return 0;
}

// ── Print running summary ──────────────────────────────────────────────────

static void print_summary(int seeds_done, const MatchupResults *results) {
  printf("\n");
  printf("==== Running totals after %d seed(s) (%d games) "
         "====\n",
         seeds_done, seeds_done * NUM_MATCHUPS * 2);
  for (int m = 0; m < NUM_MATCHUPS; m++) {
    printf("  %-6s vs %-6s : %3d - %3d - %3d  (spread: %+d)\n",
           player_label(matchups[m].a), player_label(matchups[m].b),
           results[m].a_wins, results[m].b_wins, results[m].ties,
           results[m].a_total_score - results[m].b_total_score);
  }

  // Aggregate per-player wins across all matchups
  int wins[NUM_PLAYER_TYPES] = {0};
  int total_score[NUM_PLAYER_TYPES] = {0};
  int games[NUM_PLAYER_TYPES] = {0};
  for (int m = 0; m < NUM_MATCHUPS; m++) {
    player_type_t a = matchups[m].a;
    player_type_t b = matchups[m].b;
    wins[a] += results[m].a_wins;
    wins[b] += results[m].b_wins;
    total_score[a] += results[m].a_total_score;
    total_score[b] += results[m].b_total_score;
    int pair_games = results[m].a_wins + results[m].b_wins + results[m].ties;
    games[a] += pair_games;
    games[b] += pair_games;
  }
  printf("\n  Overall:\n");
  for (int t = 0; t < NUM_PLAYER_TYPES; t++) {
    if (games[t] > 0) {
      printf("    %-6s: %3d wins / %3d games (%.1f%%), avg score %.1f\n",
             player_label(t), wins[t], games[t],
             100.0 * wins[t] / games[t],
             (double)total_score[t] / games[t]);
    }
  }
  printf("\n");
}

// ── Entry point ────────────────────────────────────────────────────────────

void test_roundrobin_benchmark(void) {
  setbuf(stdout, NULL);
  printf("\n");
  printf("========================================================\n");
  printf("  Inference Tournament: STATIC vs SIM2 vs INFER1 vs INFER20\n");
  printf("  %d seeds × %d matchup(s) × 2 (game pairs) = %d games\n", NUM_SEEDS,
         NUM_MATCHUPS, NUM_SEEDS * NUM_MATCHUPS * 2);
  printf("  %d threads, %d candidates, %d-ply sims\n", NUM_THREADS, NUM_PLAYS,
         NUM_PLIES);
  printf("  Budget: %.0fs per turn, Top-Two IDS sampling, cutoff disabled, minp=50\n",
         BUDGET_AFTER_SCORING_S);
  printf("  Inference: %.0fs limit, scoring moves only, eq_margin 1 or 20\n",
         INFER_TIME_LIMIT_S);
  printf("  STATIC: instant (no sim), endgame solver when bag empty\n");
  printf("  Late game: %d-ply when unseen < %d tiles\n", LATE_GAME_PLIES,
         LATE_GAME_TILE_THRESHOLD);
  printf("  Endgame time limit: %.0fs (all players)\n", ENDGAME_TIME_LIMIT_S);
  printf("  Log file: %s\n", LOG_FILENAME);
  printf("========================================================\n\n");

  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 15 -plies 4 -threads 10");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);

  ErrorStack *load_es = error_stack_create();
  WinPct *win_pcts = win_pct_create(config_get_data_paths(config),
                                     DEFAULT_WIN_PCT, load_es);
  if (!error_stack_is_empty(load_es)) {
    error_stack_print_and_reset(load_es);
    error_stack_destroy(load_es);
    config_destroy(config);
    return;
  }
  error_stack_destroy(load_es);

  ThreadControl *tc = config_get_thread_control(config);

  Game *game = game_duplicate(config_get_game(config));
  MoveList *move_list = move_list_create(NUM_PLAYS);
  SimResults *sim_results = sim_results_create(0.0);
  SimCtx *sim_ctx = NULL;
  InferenceResults *inference_results = inference_results_create(NULL);
  EndgameSolver *endgame_solver = endgame_solver_create();
  EndgameResults *endgame_results = endgame_results_create();

  // Open log file
  FILE *log_fp = fopen(LOG_FILENAME, "w");
  if (!log_fp) {
    printf("ERROR: cannot open %s for writing\n", LOG_FILENAME);
    goto cleanup;
  }
  fprintf(log_fp, "seed,p0_type,p1_type,p0_score,p1_score,turns,elapsed_s\n");
  fflush(log_fp);

  MatchupResults results[NUM_MATCHUPS] = {{0}};

  for (int seed_idx = 0; seed_idx < NUM_SEEDS; seed_idx++) {
    uint64_t seed = 9000 + (uint64_t)seed_idx;

    printf("─── Seed %d (%" PRIu64 ") ───\n\n", seed_idx + 1, seed);

    for (int m = 0; m < NUM_MATCHUPS; m++) {
      player_type_t type_a = matchups[m].a;
      player_type_t type_b = matchups[m].b;

      // Game A: type_a as p0, type_b as p1
      printf("  %s(p0) vs %s(p1), seed=%" PRIu64 "\n",
             player_label(type_a), player_label(type_b), seed);
      int p0_score, p1_score, turns;
      double elapsed;
      int outcome_a = play_game(game, move_list, sim_results, &sim_ctx,
                                win_pcts, tc, inference_results,
                                endgame_solver, endgame_results, type_a,
                                type_b, seed, &p0_score, &p1_score, &turns,
                                &elapsed);
      printf("  → %s %d - %d %s (%s wins)  [%d turns, %.1fs]\n\n",
             player_label(type_a), p0_score, p1_score, player_label(type_b),
             outcome_a > 0   ? player_label(type_a)
             : outcome_a < 0 ? player_label(type_b)
                              : "TIE",
             turns, elapsed);

      fprintf(log_fp, "%" PRIu64 ",%s,%s,%d,%d,%d,%.1f\n", seed,
              player_label(type_a), player_label(type_b), p0_score, p1_score,
              turns, elapsed);
      fflush(log_fp);

      if (outcome_a > 0)
        results[m].a_wins++;
      else if (outcome_a < 0)
        results[m].b_wins++;
      else
        results[m].ties++;
      results[m].a_total_score += p0_score;
      results[m].b_total_score += p1_score;

      // Game B: type_b as p0, type_a as p1 (same seed, swapped)
      printf("  %s(p0) vs %s(p1), seed=%" PRIu64 "\n",
             player_label(type_b), player_label(type_a), seed);
      int bp0_score, bp1_score, bturns;
      double belapsed;
      int outcome_b = play_game(game, move_list, sim_results, &sim_ctx,
                                win_pcts, tc, inference_results,
                                endgame_solver, endgame_results, type_b,
                                type_a, seed, &bp0_score, &bp1_score, &bturns,
                                &belapsed);
      printf("  → %s %d - %d %s (%s wins)  [%d turns, %.1fs]\n\n",
             player_label(type_b), bp0_score, bp1_score, player_label(type_a),
             outcome_b > 0   ? player_label(type_b)
             : outcome_b < 0 ? player_label(type_a)
                              : "TIE",
             bturns, belapsed);

      fprintf(log_fp, "%" PRIu64 ",%s,%s,%d,%d,%d,%.1f\n", seed,
              player_label(type_b), player_label(type_a), bp0_score, bp1_score,
              bturns, belapsed);
      fflush(log_fp);

      // In game B, type_b is p0 and type_a is p1
      if (outcome_b > 0)
        results[m].b_wins++; // type_b won as p0
      else if (outcome_b < 0)
        results[m].a_wins++; // type_a won as p1
      else
        results[m].ties++;
      results[m].b_total_score += bp0_score;
      results[m].a_total_score += bp1_score;
    }

    print_summary(seed_idx + 1, results);
  }

  fclose(log_fp);
  printf("Results written to %s\n", LOG_FILENAME);

cleanup:
  sim_ctx_destroy(sim_ctx);
  endgame_results_destroy(endgame_results);
  endgame_solver_destroy(endgame_solver);
  inference_results_destroy(inference_results);
  sim_results_destroy(sim_results);
  move_list_destroy(move_list);
  game_destroy(game);
  win_pct_destroy(win_pcts);
  config_destroy(config);
}
