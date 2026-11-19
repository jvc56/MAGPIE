#include "infer_match_test.h"

#include "../src/compat/cpthread.h"
#include "../src/compat/cpthread_defs.h"
#include "../src/compat/ctime.h"
#include "../src/def/bai_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/inference_args.h"
#include "../src/ent/inference_results.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_args.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/thread_control.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/move_gen_base.h"
#include "../src/impl/simmer.h"
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

enum { MATCH_CANDIDATES = 15, MATCH_MAX_TURNS = 200 };

typedef struct MatchDeadline {
  cpthread_mutex_t mutex;
  cpthread_cond_t condition;
  bool finished;
  double seconds;
  MovegenBudget budget;
} MatchDeadline;

// Reuse the engine's cooperative search-budget stop without changing its
// production timing semantics. This timer sleeps; each match uses one compute
// thread. Inference may borrow from the remaining GAME clock, not free time.
static void *match_deadline_run(void *context) {
  MatchDeadline *deadline = context;
  TimeSpec until;
  ctimer_clock_gettime_realtime(&until);
  const int64_t nanoseconds = (int64_t)(deadline->seconds * 1e9);
  until.tv_sec += nanoseconds / 1000000000;
  until.tv_nsec += nanoseconds % 1000000000;
  if (until.tv_nsec >= 1000000000) {
    until.tv_sec++;
    until.tv_nsec -= 1000000000;
  }
  cpthread_mutex_lock(&deadline->mutex);
  while (!deadline->finished) {
    if (!cpthread_cond_timedwait(&deadline->condition, &deadline->mutex,
                                 &until) &&
        !deadline->finished) {
      atomic_store(&deadline->budget.spent, deadline->budget.limit);
      break;
    }
  }
  cpthread_mutex_unlock(&deadline->mutex);
  return NULL;
}

typedef struct MatchSeatStats {
  double elapsed;
  uint64_t iterations;
  int turns;
  int static_fallbacks;
  int inference_attempts;
  int inference_skips;
  int emergency_passes;
  int inferred_sims;
  int clock_stops;
} MatchSeatStats;

static double match_game(const Config *config, unsigned int seed, int game_idx,
                         double game_seconds, bool null_match) {
  Game *game = config_get_game(config);
  game_reset(game);
  game_seed(game, seed);
  game_set_starting_player_index(game, game_idx);
  draw_starting_racks(game);
  Game *previous = game_duplicate(game);
  Move previous_move = {0};
  const LetterDistribution *ld = game_get_ld(game);
  Rack played;
  Rack known;
  Rack observer;
  Rack empty;
  rack_set_dist_size_and_reset(&played, ld_get_size(ld));
  rack_set_dist_size_and_reset(&known, ld_get_size(ld));
  rack_set_dist_size_and_reset(&observer, ld_get_size(ld));
  rack_set_dist_size_and_reset(&empty, ld_get_size(ld));
  MoveList *moves = move_list_create(MATCH_CANDIDATES);
  SimCtx *sim_ctx[2] = {NULL, NULL};
  SimResults *results = sim_results_create(0);
  InferenceResults *inferences = inference_results_create(NULL);
  ErrorStack *errors = error_stack_create();
  MatchSeatStats stats[2] = {0};
  ThreadControl *control = config_get_thread_control(config);
  int turn = 0;
  for (; !game_over(game) && turn < MATCH_MAX_TURNS; turn++) {
    const int seat = game_get_player_on_turn_index(game);
    MatchSeatStats *stat = &stats[seat];
    stat->turns++;
    const double remaining = fmax(0, game_seconds - stat->elapsed);
    const double reserve = fmin(1.0, game_seconds * 0.1);
    const double search_remaining = fmax(0, remaining - reserve);
    const bool inference_eligible =
        seat == 0 && !null_match && turn > 0 &&
        move_get_type(&previous_move) != GAME_EVENT_PASS &&
        (move_get_type(&previous_move) != GAME_EVENT_EXCHANGE ||
         bag_get_letters(game_get_bag(previous)) <= 30);
    const bool use_inference = inference_eligible && search_remaining >= 0.3;
    if (inference_eligible && !use_inference) {
      stat->inference_skips++;
    }
    Timer timer;
    ctimer_start(&timer);
    Move chosen;
    if (remaining < 0.05) {
      // No new search when the emergency reserve is exhausted.
      move_set_as_pass(&chosen);
      stat->emergency_passes++;
    } else if (search_remaining < 0.05) {
      move_copy(&chosen, get_top_equity_move(game, moves));
      stat->static_fallbacks++;
    } else {
      MatchDeadline deadline = {.finished = false, .seconds = search_remaining};
      cpthread_mutex_init(&deadline.mutex);
      cpthread_cond_init(&deadline.condition);
      movegen_budget_init(&deadline.budget, UINT64_C(1) << 60);
      cpthread_t timer_thread;
      cpthread_create(&timer_thread, match_deadline_run, &deadline);
      MovegenBudget *old_budget = gen_set_movegen_budget(&deadline.budget);
      rack_reset(&played);
      if (use_inference &&
          move_get_type(&previous_move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
        for (int tile_idx = 0; tile_idx < move_get_tiles_length(&previous_move);
             tile_idx++) {
          const MachineLetter ml = move_get_tile(&previous_move, tile_idx);
          if (ml != PLAYED_THROUGH_MARKER) {
            rack_add_letter(&played,
                            get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml);
          }
        }
      }
      rack_copy(&observer, player_get_rack(game_get_player(game, seat)));
      InferenceArgs infer_args;
      infer_args_fill(&infer_args, 0, int_to_equity(10000), NULL, previous, 1,
                      0, 0, control, false, true, 1 - seat,
                      move_get_score(&previous_move),
                      move_get_type(&previous_move) == GAME_EVENT_EXCHANGE
                          ? move_get_tiles_played(&previous_move)
                          : 0,
                      &played, &known, &observer);
      infer_args.temperature = int_to_equity(8);
      infer_args.target_move = &previous_move;
      // Reallocate the remaining clock after every turn. Inference time is
      // debited along with simulation, reducing subsequent search budgets.
      const double turns_left = fmax(
          2, (bag_get_letters(game_get_bag(game)) + (2.0 * RACK_SIZE)) / 8.0);
      const double sim_seconds =
          fmax(0.001, (search_remaining - 0.003) / turns_left);
      SimArgs args;
      sim_args_fill(2, moves, MATCH_CANDIDATES, &empty,
                    config_get_win_pcts(config), inferences, control, game,
                    use_inference, false, 1, 0, MATCH_CANDIDATES, 2,
                    (uint64_t)seed + (uint64_t)turn, UINT64_C(1000000000000000),
                    30, 0, BAI_THRESHOLD_NONE, sim_seconds,
                    BAI_SAMPLING_RULE_TOP_TWO_IDS, 0, 1, 0.5, 100, false,
                    &infer_args, &args);
      args.bag_cycle = true;
      args.pat_rollout_disabled = true;
      thread_control_set_status(control, THREAD_CONTROL_STATUS_STARTED);
      bool simulated = false;
      const Move *move = get_top_simming_move(
          game, moves, &args, &sim_ctx[seat], results, &simulated, errors);
      if (!error_stack_is_empty(errors) || move == NULL) {
        error_stack_print_and_reset(errors);
        log_fatal("infmatch failed: seed=%u game=%d turn=%d", seed, game_idx,
                  turn);
      }
      assert(move != NULL);
      move_copy(&chosen, move);
      if (use_inference && simulated) {
        stat->inference_attempts++;
        if (sim_results_get_iteration_count(results) > 0 &&
            sim_results_get_num_infer_leaves(results) > 0) {
          stat->inferred_sims++;
        }
      }
      if (simulated) {
        stat->iterations += sim_results_get_iteration_count(results);
      }
      stat->clock_stops += gen_movegen_budget_spent();
      (void)gen_set_movegen_budget(old_budget);
      cpthread_mutex_lock(&deadline.mutex);
      deadline.finished = true;
      cpthread_cond_signal(&deadline.condition);
      cpthread_mutex_unlock(&deadline.mutex);
      cpthread_join(timer_thread);
    }
    stat->elapsed += ctimer_elapsed_seconds(&timer);
    if (use_inference && move_list_get_count(moves) > 1) {
      printf("INFMATCH_INFERENCE seed=%u game=%d turn=%d exchange=%d "
             "seconds=%.9f\n",
             seed, game_idx, turn,
             move_get_type(&previous_move) == GAME_EVENT_EXCHANGE,
             inference_results_get_elapsed_seconds(inferences));
    }
    // Fail the experiment instead of accepting a move computed over budget.
    if (stat->elapsed > game_seconds) {
      log_fatal("infmatch clock overrun: seed=%u game=%d seat=%d elapsed=%.6f "
                "cap=%.6f",
                seed, game_idx, seat, stat->elapsed, game_seconds);
    }
    game_copy(previous, game);
    move_copy(&previous_move, &chosen);
    play_move(&chosen, game, NULL);
  }
  assert(game_over(game));
  const double spread =
      equity_to_double(player_get_score(game_get_player(game, 0)) -
                       player_get_score(game_get_player(game, 1)));
  printf(
      "INFMATCH_GAME seed=%u game=%d spread=%.3f turns=%d soft_seconds=%.6f "
      "control_seconds=%.6f soft_iterations=%llu control_iterations=%llu "
      "soft_fallbacks=%d control_fallbacks=%d infer_attempts=%d "
      "inferred_sims=%d soft_clock_stops=%d control_clock_stops=%d "
      "infer_skips=%d soft_emergency_passes=%d control_emergency_passes=%d\n",
      seed, game_idx, spread, turn, stats[0].elapsed, stats[1].elapsed,
      (unsigned long long)stats[0].iterations,
      (unsigned long long)stats[1].iterations, stats[0].static_fallbacks,
      stats[1].static_fallbacks, stats[0].inference_attempts,
      stats[0].inferred_sims, stats[0].clock_stops, stats[1].clock_stops,
      stats[0].inference_skips, stats[0].emergency_passes,
      stats[1].emergency_passes);
  if (fflush(stdout) != 0) {
    log_fatal("infmatch output flush failed");
  }
  sim_ctx_destroy(sim_ctx[0]);
  sim_ctx_destroy(sim_ctx[1]);
  inference_results_destroy(inferences);
  sim_results_destroy(results);
  error_stack_destroy(errors);
  move_list_destroy(moves);
  game_destroy(previous);
  return spread;
}

static double match_outcome(double spread) {
  if (spread > 0) {
    return 1;
  }
  if (spread < 0) {
    return 0;
  }
  return 0.5;
}

void infer_match_run_spec(const char *spec) {
  StringSplitter *fields = split_string(spec, ':', true);
  assert(string_splitter_get_number_of_items(fields) == 5);
  ErrorStack *errors = error_stack_create();
  const int pairs = string_to_int(string_splitter_get_item(fields, 0), errors);
  const int milliseconds =
      string_to_int(string_splitter_get_item(fields, 1), errors);
  const uint64_t seed =
      string_to_uint64(string_splitter_get_item(fields, 2), errors);
  const int stride = string_to_int(string_splitter_get_item(fields, 3), errors);
  const int null_match =
      string_to_int(string_splitter_get_item(fields, 4), errors);
  assert(error_stack_is_empty(errors) && pairs > 0 && milliseconds > 0 &&
         stride > 0);
  string_splitter_destroy(fields);
  error_stack_destroy(errors);
  Config *config = config_create_or_die(
      "set -lex CSW24 -leaves CSW24 -wmp true -rit true -ritmmap true -wit "
      "true -s1 equity "
      "-s2 equity "
      "-r1 all -r2 all -patcand1 false -patcand2 false -patrollout false "
      "-threads 1 -numplays 15 -savesettings false -autosavegcg false");
  load_and_exec_config_or_die(
      config,
      "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0 -lex CSW24;");
  errors = error_stack_create();
  config_load_win_pcts(config, errors);
  assert(error_stack_is_empty(errors) && config_get_win_pcts(config) != NULL);
  error_stack_destroy(errors);
  for (int seat = 0; seat < 2; seat++) {
    const Player *player = game_get_player(config_get_game(config), seat);
    assert(player_get_rack_info_table(player) != NULL);
    assert(player_get_word_info_table(player) != NULL);
  }
  printf(
      "INFMATCH_SETTINGS milliseconds=%d seed=%llu stride=%d null=%d "
      "lex=CSW24 threads=1 bag_cycle=1 rit=1 wit=1 margin=10000 temperature=8 "
      "candidates=15 plies=2 reserve_seconds=%.6f "
      "inference_min_search_seconds=0.3 exchange_max_bag=30\n",
      milliseconds, (unsigned long long)seed, stride, null_match,
      fmin(1.0, milliseconds / 10000.0));
  if (fflush(stdout) != 0) {
    log_fatal("infmatch output flush failed");
  }
  for (int pair_idx = 0; pair_idx < pairs; pair_idx++) {
    const unsigned int pair_seed =
        (unsigned int)(seed + ((uint64_t)pair_idx * (uint64_t)stride));
    const double first = match_game(config, pair_seed, 0, milliseconds / 1000.0,
                                    null_match != 0);
    const double second = match_game(config, pair_seed, 1,
                                     milliseconds / 1000.0, null_match != 0);
    const double outcome = (match_outcome(first) + match_outcome(second)) / 2;
    printf("INFMATCH_PAIR seed=%u win_fraction=%.6f spread=%.3f\n", pair_seed,
           outcome, (first + second) / 2);
    if (fflush(stdout) != 0) {
      log_fatal("infmatch output flush failed");
    }
  }
  config_destroy(config);
}
