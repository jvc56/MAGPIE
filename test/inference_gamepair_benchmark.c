// Inference game-pair benchmark: measures the value of using inferences
// during simulations by playing game pairs between two simming players.
//
// Usage: ./bin/magpie_test infgp
//
// Both players use time-limited BAI with 15 candidate moves and 2-ply sims.
// The INFER player runs inference on the opponent's previous move before
// simming, which provides a distribution over likely opponent racks.
// The PLAIN player sims with uniform rack sampling (no inference).
//
// Time budgets:
//   After scoring plays: 10s per turn (both players)
//   After exchanges:     45s per turn (both players)
//   First turn:          10s (no previous move)
//
// Inference time eats into the INFER player's simming time. If inference
// exceeds 7.5s (scoring) or 42.5s (exchange), it is interrupted and the
// INFER player sims without inference for the remaining ~2.5s.

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

#define NUM_GAME_PAIRS 18
#define NUM_PLAYS 15
#define NUM_PLIES 2
#define NUM_THREADS 10

// Time budgets (seconds)
#define BUDGET_AFTER_SCORING_S 10.0
#define BUDGET_AFTER_EXCHANGE_S 45.0

// Inference cutoffs (seconds). If inference exceeds this, it is interrupted
// and the player sims without inference for the remaining time.
#define INFER_CUTOFF_SCORING_S 7.5
#define INFER_CUTOFF_EXCHANGE_S 42.5

// Late-game switches to many-ply sims when tiles unseen drops below threshold.
#define LATE_GAME_TILE_THRESHOLD 21
#define LATE_GAME_PLIES 99

// Endgame solver (used when bag is empty — perfect information)
#define ENDGAME_PLIES 25
#define ENDGAME_TIME_LIMIT_S 10.0

// ── Player identity ────────────────────────────────────────────────────────

typedef enum {
  PLAYER_INFER, // Runs inference before simming
  PLAYER_PLAIN, // Sims with uniform rack sampling
} player_type_t;

static const char *player_label(player_type_t type) {
  switch (type) {
  case PLAYER_INFER:
    return "INFER";
  case PLAYER_PLAIN:
    return "PLAIN";
  default:
    return "?????";
  }
}

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

// ── Game results tracking ──────────────────────────────────────────────────

typedef struct {
  int p0_wins;
  int p1_wins;
  int ties;
  int total_turns;
  double total_elapsed_s;
} PairResults;

// ── Inference stats ────────────────────────────────────────────────────────

typedef struct {
  int total_inferences_attempted;
  int total_inferences_succeeded;
  int total_inferences_interrupted;
  int scoring_inferences;
  int exchange_inferences;
  double total_inference_time_s;
  double max_inference_time_s;
} InferenceStats;

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

static double get_infer_cutoff(game_event_t prev_move_type) {
  if (prev_move_type == GAME_EVENT_EXCHANGE) {
    return INFER_CUTOFF_EXCHANGE_S;
  }
  return INFER_CUTOFF_SCORING_S;
}

// ── Run inference on the opponent's previous move ──────────────────────────
// Returns true if inference completed successfully (not interrupted).
// Populates inference_results with the inferred rack distribution.
// elapsed_out receives the wall-clock time spent on inference.

static bool run_inference_on_prev_move(
    const Game *game, ThreadControl *tc,
    const Move *prev_move, int prev_player_index,
    game_event_t prev_move_type, double infer_cutoff_s,
    InferenceResults *inference_results, ErrorStack *error_stack,
    double *elapsed_out) {

  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);

  int num_exch = 0;
  Rack target_played_tiles;
  rack_set_dist_size_and_reset(&target_played_tiles, ld_size);
  Rack target_known_rack;
  rack_set_dist_size_and_reset(&target_known_rack, ld_size);
  Rack nontarget_known_rack;
  rack_set_dist_size_and_reset(&nontarget_known_rack, ld_size);
  // The nontarget player is the player currently on turn (the one doing
  // the inferring). We must tell inference which tiles this player holds
  // so it excludes them from the bag of tiles available for possible leaves.
  const int nontarget_index = 1 - prev_player_index;
  rack_copy(&nontarget_known_rack,
            player_get_rack(game_get_player(game, nontarget_index)));

  Equity score = move_get_score(prev_move);

  if (prev_move_type == GAME_EVENT_EXCHANGE) {
    num_exch = move_get_tiles_played(prev_move);
  } else {
    // Scoring play: extract placed tiles
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
  }

  // Build inference args using the actual game state (not config->game)
  InferenceArgs args;
  infer_args_fill(&args, NUM_PLAYS, 0,
                  /* game_history */ NULL, game, NUM_THREADS,
                  /* print_interval */ 0, tc,
                  /* use_game_history */ false,
                  /* use_inference_cutoff_optimization */ true,
                  prev_player_index, score, num_exch,
                  &target_played_tiles, &target_known_rack,
                  &nontarget_known_rack);

  // Run inference with cutoff timer
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  thread_control_set_status(tc, THREAD_CONTROL_STATUS_STARTED);

  TimerArgs ta = {.tc = tc, .seconds = infer_cutoff_s, .done = false};
  pthread_t timer_tid;
  pthread_create(&timer_tid, NULL, timer_thread_func, &ta);

  infer_without_ctx(&args, inference_results, error_stack);

  ta.done = true;
  pthread_join(timer_tid, NULL);

  clock_gettime(CLOCK_MONOTONIC, &t1);
  *elapsed_out = timespec_diff_s(&t0, &t1);

  // Check if inference was interrupted
  bool interrupted =
      thread_control_get_status(tc) == THREAD_CONTROL_STATUS_USER_INTERRUPT;

  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    return false;
  }

  return !interrupted;
}

// ── Play one simmed turn ───────────────────────────────────────────────────
// Generates moves, runs BAI sim with time budget, plays the best move.
// If use_inference is true, simulate() will run inference internally before
// BAI, using the provided inference_args.

static const Move *play_sim_turn(Game *game, MoveList *move_list,
                                 SimResults *sim_results, SimCtx **sim_ctx,
                                 WinPct *win_pcts, ThreadControl *tc,
                                 int num_plies, double budget_s,
                                 bool use_inference,
                                 InferenceArgs *inference_args,
                                 InferenceResults *inference_results,
                                 ErrorStack *error_stack) {
  // Generate candidate moves
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

  // Build SimArgs
  SimArgs sim_args;
  sim_args_fill(num_plies, move_list,
                /* known_opp_rack */ NULL, win_pcts, inference_results, tc,
                game, use_inference,
                /* use_heat_map */ false, NUM_THREADS,
                /* print_interval */ 0,
                /* max_num_display_plays */ NUM_PLAYS,
                /* max_num_display_plies */ num_plies,
                /* seed */ 0,
                /* max_iterations */ UINT64_MAX,
                /* min_play_iterations */ 1,
                /* scond */ 101.0, // > 100 → BAI_THRESHOLD_NONE
                BAI_THRESHOLD_NONE,
                /* time_limit_seconds */ 999, // rely on external timer
                BAI_SAMPLING_RULE_ROUND_ROBIN,
                /* cutoff */ 0.0, inference_args, &sim_args);

  thread_control_set_status(tc, THREAD_CONTROL_STATUS_STARTED);

  TimerArgs ta = {.tc = tc, .seconds = budget_s, .done = false};
  pthread_t timer_tid;
  pthread_create(&timer_tid, NULL, timer_thread_func, &ta);

  simulate(&sim_args, sim_ctx, sim_results, error_stack);

  ta.done = true;
  pthread_join(timer_tid, NULL);

  // Get best arm from BAI
  BAIResult *bai_result = sim_results_get_bai_result(sim_results);
  int best_arm = bai_result_get_best_arm(bai_result);
  if (best_arm < 0) {
    best_arm = 0; // fallback to static equity best
  }

  const Move *best_move =
      simmed_play_get_move(sim_results_get_simmed_play(sim_results, best_arm));
  play_move(best_move, game, NULL);
  return best_move;
}

// ── Play one endgame turn (bag is empty → perfect information) ──────────────

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

static void play_game(Game *game, MoveList *move_list,
                      SimResults *sim_results, SimCtx **sim_ctx,
                      WinPct *win_pcts, ThreadControl *tc,
                      InferenceResults *inference_results,
                      EndgameSolver *endgame_solver,
                      EndgameResults *endgame_results,
                      player_type_t p0_type, player_type_t p1_type,
                      int game_num, uint64_t seed, PairResults *results,
                      InferenceStats *infer_stats) {
  game_reset(game);
  game_seed(game, seed);
  draw_starting_racks(game);

  ErrorStack *error_stack = error_stack_create();
  StringBuilder *sb = string_builder_create();

  struct timespec game_start, game_end;
  clock_gettime(CLOCK_MONOTONIC, &game_start);

  // Track the previous move for inference and budget decisions.
  // On the very first turn there is no previous move.
  game_event_t prev_move_type = GAME_EVENT_TILE_PLACEMENT_MOVE; // default: 10s
  const Move *prev_move = NULL;
  int prev_player_index = -1;

  // We need a persistent copy of the previous move since move_list changes.
  Move saved_prev_move;

  // The inference expects the game state BEFORE the target player's move
  // (so the played tiles are still on the target's rack, not on the board).
  // We save a snapshot before each move for this purpose.
  Game *game_before_prev_move = game_duplicate(game);

  int turn = 0;
  while (!game_over(game)) {
    int player_idx = game_get_player_on_turn_index(game);
    player_type_t player_type =
        (player_idx == 0) ? p0_type : p1_type;
    const char *label = player_label(player_type);
    int unseen = tiles_unseen(game);
    const Bag *bag = game_get_bag(game);
    int bag_tiles = bag_get_letters(bag);

    // Determine time budget based on opponent's previous move type
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
      // Endgame: perfect information, use endgame solver for both players
      used_endgame = true;
      move = play_endgame_turn(game, move_list, endgame_solver,
                               endgame_results, tc, error_stack);
    } else {
      // Mid-game: sim with optional inference

      if (player_type == PLAYER_INFER && prev_move != NULL) {
        // Determine if inference is valid for the previous move
        bool can_infer = false;
        if (prev_move_type == GAME_EVENT_TILE_PLACEMENT_MOVE &&
            bag_tiles >= RACK_SIZE) {
          can_infer = true;
        } else if (prev_move_type == GAME_EVENT_EXCHANGE &&
                   bag_tiles >= RACK_SIZE * 2) {
          can_infer = true;
        }

        if (can_infer) {
          did_infer = true;
          double infer_cutoff = get_infer_cutoff(prev_move_type);

          infer_ok = run_inference_on_prev_move(
              game_before_prev_move, tc, &saved_prev_move, prev_player_index,
              prev_move_type, infer_cutoff, inference_results, error_stack,
              &infer_elapsed);

          infer_stats->total_inferences_attempted++;
          infer_stats->total_inference_time_s += infer_elapsed;
          if (infer_elapsed > infer_stats->max_inference_time_s) {
            infer_stats->max_inference_time_s = infer_elapsed;
          }

          if (infer_ok) {
            infer_stats->total_inferences_succeeded++;
            if (prev_move_type == GAME_EVENT_EXCHANGE) {
              infer_stats->exchange_inferences++;
            } else {
              infer_stats->scoring_inferences++;
            }
          } else {
            infer_stats->total_inferences_interrupted++;
            printf("    *** INFERENCE INTERRUPTED after %.2fs "
                   "(cutoff=%.1fs, prev=%s) ***\n",
                   infer_elapsed,
                   get_infer_cutoff(prev_move_type),
                   prev_move_type == GAME_EVENT_EXCHANGE ? "exchange"
                                                         : "scoring");
          }
        }
      }

      // Compute remaining sim budget
      double sim_budget = turn_budget - infer_elapsed;
      if (sim_budget < 0.5) {
        sim_budget = 0.5; // minimum sim time
      }

      // For the INFER player with successful inference, we call simulate()
      // with use_inference=true. This re-runs inference internally (wasteful
      // but necessary given the simulate() API), then uses inference results
      // for informed rack sampling during sims.
      //
      // For the PLAIN player, or when inference failed/was skipped,
      // use_inference=false gives uniform rack sampling.
      bool use_inference = (did_infer && infer_ok);

      // Build inference_args for simulate() (needed when use_inference=true).
      // Racks must be declared at this scope so they survive into
      // play_sim_turn, because InferenceArgs stores pointers to them.
      InferenceArgs inference_args;
      const LetterDistribution *ld = game_get_ld(game);
      const int ld_size = ld_get_size(ld);
      Rack ia_target_played_tiles;
      rack_set_dist_size_and_reset(&ia_target_played_tiles, ld_size);
      Rack ia_target_known_rack;
      rack_set_dist_size_and_reset(&ia_target_known_rack, ld_size);
      Rack ia_nontarget_known_rack;
      rack_set_dist_size_and_reset(&ia_nontarget_known_rack, ld_size);
      // Set nontarget rack to the current player's rack from the pre-move
      // game state so inference excludes these tiles from possible leaves.
      // (The nontarget player's rack is the same in pre-move and post-move
      // states since only the target player moved.)
      rack_copy(&ia_nontarget_known_rack,
                player_get_rack(
                    game_get_player(game_before_prev_move, player_idx)));

      if (use_inference) {
        int num_exch = 0;
        Equity score = move_get_score(&saved_prev_move);

        if (prev_move_type == GAME_EVENT_EXCHANGE) {
          num_exch = move_get_tiles_played(&saved_prev_move);
        } else {
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
        }

        infer_args_fill(&inference_args, NUM_PLAYS, 0,
                        /* game_history */ NULL, game_before_prev_move,
                        NUM_THREADS, /* print_interval */ 0, tc,
                        /* use_game_history */ false,
                        /* use_inference_cutoff_optimization */ true,
                        prev_player_index, score, num_exch,
                        &ia_target_played_tiles, &ia_target_known_rack,
                        &ia_nontarget_known_rack);
      }

      // Save game state BEFORE play_sim_turn changes it.
      // We save to a temp and copy after play_sim_turn so that
      // inference_args.game (which points to game_before_prev_move)
      // isn't overwritten before simulate() uses it.
      Game *game_snapshot = game_duplicate(game);

      move = play_sim_turn(game, move_list, sim_results, sim_ctx, win_pcts, tc,
                           effective_plies, sim_budget, use_inference,
                           use_inference ? &inference_args : NULL,
                           inference_results, error_stack);

      // Now that play_sim_turn is done and inference_args is no longer needed,
      // update game_before_prev_move with the pre-move snapshot for the next
      // turn's inference.
      game_copy(game_before_prev_move, game_snapshot);
      game_destroy(game_snapshot);
    } // end mid-game branch

    clock_gettime(CLOCK_MONOTONIC, &turn_end);
    double turn_elapsed = timespec_diff_s(&turn_start, &turn_end);
    turn++;

    // Log the move
    string_builder_clear(sb);
    string_builder_add_move(sb, game_get_board(game), move,
                            game_get_ld(game), true);
    uint64_t iters = used_endgame ? 0
                                  : sim_results_get_iteration_count(sim_results);
    if (used_endgame) {
      printf("    Turn %2d (%-5s p%d): %s  [%.1fs, endgame]\n",
             turn, label, player_idx,
             string_builder_peek(sb), turn_elapsed);
    } else if (did_infer) {
      printf("    Turn %2d (%-5s p%d): %s  [%.1fs, %" PRIu64
             " iters, infer=%.2fs%s]\n",
             turn, label, player_idx,
             string_builder_peek(sb), turn_elapsed, iters, infer_elapsed,
             infer_ok ? "" : " INTERRUPTED");
    } else {
      printf("    Turn %2d (%-5s p%d): %s  [%.1fs, %" PRIu64 " iters]\n",
             turn, label, player_idx,
             string_builder_peek(sb), turn_elapsed, iters);
    }

    if (!error_stack_is_empty(error_stack)) {
      printf("  ERROR on turn %d: ", turn);
      error_stack_print_and_reset(error_stack);
    }

    // Save previous move info for next turn's inference/budget
    prev_move_type = move_get_type(move);
    prev_player_index = player_idx;
    // Copy the move struct so it persists across move_list regeneration
    saved_prev_move = *move;
    prev_move = &saved_prev_move;
  }

  clock_gettime(CLOCK_MONOTONIC, &game_end);
  double elapsed = timespec_diff_s(&game_start, &game_end);

  int p0_score =
      equity_to_int(player_get_score(game_get_player(game, 0)));
  int p1_score =
      equity_to_int(player_get_score(game_get_player(game, 1)));

  const char *p0_label = player_label(p0_type);
  const char *p1_label = player_label(p1_type);
  const char *winner_label;
  if (p0_score > p1_score) {
    results->p0_wins++;
    winner_label = p0_label;
  } else if (p1_score > p0_score) {
    results->p1_wins++;
    winner_label = p1_label;
  } else {
    results->ties++;
    winner_label = "TIE";
  }
  results->total_turns += turn;
  results->total_elapsed_s += elapsed;

  printf("  Game %2d: %s(%d) vs %s(%d) → %s  [%d turns, %.1fs]\n\n",
         game_num, p0_label, p0_score, p1_label, p1_score, winner_label, turn,
         elapsed);

  game_destroy(game_before_prev_move);
  string_builder_destroy(sb);
  error_stack_destroy(error_stack);
}

// ── Entry point ────────────────────────────────────────────────────────────

void test_inference_gamepair_benchmark(void) {
  setbuf(stdout, NULL);
  printf("\n");
  printf("========================================================\n");
  printf("  Inference Game-Pair Benchmark\n");
  printf("  %d game pair(s), %d threads, %d candidates, %d-ply sims\n",
         NUM_GAME_PAIRS, NUM_THREADS, NUM_PLAYS, NUM_PLIES);
  printf("  Budget: %.0fs after scoring, %.0fs after exchange\n",
         BUDGET_AFTER_SCORING_S, BUDGET_AFTER_EXCHANGE_S);
  printf("  Inference cutoffs: %.1fs (scoring), %.1fs (exchange)\n",
         INFER_CUTOFF_SCORING_S, INFER_CUTOFF_EXCHANGE_S);
  printf("  Late-game: <%d unseen → %d plies\n",
         LATE_GAME_TILE_THRESHOLD, LATE_GAME_PLIES);
  printf("  INFER = sims with inferred opponent rack distribution\n");
  printf("  PLAIN = sims with uniform rack sampling\n");
  printf("========================================================\n\n");

  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 15 -plies 2 -threads 10");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);

  // Load win_pcts directly
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

  PairResults infer_as_p0 = {0};
  PairResults plain_as_p0 = {0};
  InferenceStats infer_stats = {0};

  printf("--- Games ---\n\n");
  for (int pair = 0; pair < NUM_GAME_PAIRS; pair++) {
    uint64_t seed = 42 + (uint64_t)pair;

    // Game A: INFER=P0, PLAIN=P1
    printf("  Pair %d, Game A (INFER=P0, PLAIN=P1, seed=%" PRIu64 ")\n",
           pair + 1, seed);
    play_game(game, move_list, sim_results, &sim_ctx, win_pcts, tc,
              inference_results, endgame_solver, endgame_results,
              PLAYER_INFER, PLAYER_PLAIN,
              pair * 2 + 1, seed, &infer_as_p0, &infer_stats);

    // Game B: PLAIN=P0, INFER=P1 (same seed, swapped)
    printf("  Pair %d, Game B (PLAIN=P0, INFER=P1, seed=%" PRIu64 ")\n",
           pair + 1, seed);
    play_game(game, move_list, sim_results, &sim_ctx, win_pcts, tc,
              inference_results, endgame_solver, endgame_results,
              PLAYER_PLAIN, PLAYER_INFER,
              pair * 2 + 2, seed, &plain_as_p0, &infer_stats);
  }

  // Aggregate results by strategy
  int infer_wins = infer_as_p0.p0_wins + plain_as_p0.p1_wins;
  int plain_wins = infer_as_p0.p1_wins + plain_as_p0.p0_wins;
  int ties = infer_as_p0.ties + plain_as_p0.ties;
  int total_games = NUM_GAME_PAIRS * 2;
  int total_turns = infer_as_p0.total_turns + plain_as_p0.total_turns;
  double total_elapsed =
      infer_as_p0.total_elapsed_s + plain_as_p0.total_elapsed_s;

  printf("========================================================\n");
  printf("  RESULTS (%d games = %d pairs)\n", total_games, NUM_GAME_PAIRS);
  printf("========================================================\n\n");
  printf("  %-5s wins: %d (%.1f%%)\n", "INFER", infer_wins,
         100.0 * infer_wins / total_games);
  printf("  %-5s wins: %d (%.1f%%)\n", "PLAIN", plain_wins,
         100.0 * plain_wins / total_games);
  printf("  Ties:        %d\n", ties);
  printf("  Avg turns/game: %.1f\n", (double)total_turns / total_games);
  printf("  Total wall time: %.1fs (%.1fs/game avg)\n\n",
         total_elapsed, total_elapsed / total_games);

  printf("  Inference stats:\n");
  printf("    Attempted:   %d\n", infer_stats.total_inferences_attempted);
  printf("    Succeeded:   %d\n", infer_stats.total_inferences_succeeded);
  printf("    Interrupted: %d\n", infer_stats.total_inferences_interrupted);
  printf("    Scoring:     %d\n", infer_stats.scoring_inferences);
  printf("    Exchange:    %d\n", infer_stats.exchange_inferences);
  printf("    Total time:  %.1fs\n", infer_stats.total_inference_time_s);
  printf("    Max single:  %.2fs\n", infer_stats.max_inference_time_s);
  printf("\n========================================================\n");

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
