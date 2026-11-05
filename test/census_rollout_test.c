#include "census_rollout_test.h"

#include "../src/compat/ctime.h"
#include "../src/def/board_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/move_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_results.h"
#include "../src/impl/census_rollout.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/simmer.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  // Each position's candidates, the rollouts of each, and the plies of a
  // rollout, which go one past the plies the census serves.
  CENSUS_ROLLOUT_TEST_CANDIDATES = 8,
  CENSUS_ROLLOUT_TEST_ROLLOUTS = 12,
  CENSUS_ROLLOUT_TEST_PLIES = CENSUS_ROLLOUT_MAX_PLY + 2,
  CENSUS_ROLLOUT_TEST_BUILD_THREADS = 4,
  // A sim's candidates, past any fixed count the census might keep, and the
  // last of them whose rollouts are checked.
  CENSUS_ROLLOUT_TEST_MANY_CANDIDATES = 100,
  CENSUS_ROLLOUT_TEST_CHECKED_CANDIDATES = 8,
  // The sims compared with and without the census.
  CENSUS_ROLLOUT_TEST_SIM_ITERATIONS = 300,
  CENSUS_ROLLOUT_TEST_MAX_PLAYS = 16,
  // The benchmark: its sims' plies and iterations, on one thread.
  CENSUS_ROLLOUT_BENCH_PLIES = 2,
  CENSUS_ROLLOUT_BENCH_ITERATIONS = 20000,
};

// The bags of the positions: a full one, where the census is built from the
// most anchors, and emptier ones, down to bags whose plays get pre-endgame
// adjustments.
static const int census_rollout_test_bag_sizes[] = {70, 45, 25, 10};

// Plays a static game from seed until the bag holds at most bag_tiles tiles.
static void play_to_bag_size(Game *game, MoveList *move_list, uint64_t seed,
                             int bag_tiles) {
  game_reset(game);
  game_seed(game, seed);
  draw_starting_racks(game);
  while (!game_over(game) && bag_get_letters(game_get_bag(game)) > bag_tiles) {
    play_move(get_top_equity_move(game, move_list), game, NULL);
  }
}

// The top number_of_candidates moves of the position by static equity.
static void generate_candidates(Game *game, MoveList *candidates) {
  const MoveGenArgs args = {.game = game,
                            .move_list = candidates,
                            .move_record_type = MOVE_RECORD_ALL,
                            .move_sort_type = MOVE_SORT_EQUITY,
                            .override_kwg = NULL,
                            .eq_margin_movegen = 0,
                            .target_equity = EQUITY_MAX_VALUE};
  generate_moves(&args);
  move_list_sort_moves(candidates);
}

// Rollouts as a sim plays them: the opponent draws a random rack, the
// candidate is played, and each ply plays the best move. Every census-steered
// move must be the move a plain generation finds, from a census built on one
// thread and from one built on several.
static void test_census_rollout_moves_match_plain(void) {
  if (BOARD_DIM != 15) {
    return;
  }
  Config *config = config_create_or_die("set -lex CSW21 -wmp true");
  Game *game = config_game_create(config);
  Game *rollout = game_duplicate(game);
  MoveList *move_list = move_list_create(1);
  MoveList *census_moves = move_list_create(1);
  MoveList *candidates = move_list_create(CENSUS_ROLLOUT_TEST_CANDIDATES);
  CensusRollout *serial_census = census_rollout_create(1);
  CensusRollout *threaded_census =
      census_rollout_create(CENSUS_ROLLOUT_TEST_BUILD_THREADS);
  CensusRolloutWorker *serial_worker = census_rollout_worker_create();
  CensusRolloutWorker *threaded_worker = census_rollout_worker_create();
  Move census_move;
  uint64_t served = 0;
  uint64_t steered = 0;
  const int number_of_positions =
      (int)(sizeof(census_rollout_test_bag_sizes) /
            sizeof(census_rollout_test_bag_sizes[0]));
  for (int position_idx = 0; position_idx < number_of_positions;
       position_idx++) {
    play_to_bag_size(game, move_list, 100 + (uint64_t)position_idx,
                     census_rollout_test_bag_sizes[position_idx]);
    if (game_over(game)) {
      continue;
    }
    generate_candidates(game, candidates);
    census_rollout_prepare(serial_census, game, candidates);
    census_rollout_prepare(threaded_census, game, candidates);
    const int opponent_index = 1 - game_get_player_on_turn_index(game);
    for (int candidate_idx = 0; candidate_idx < move_list_get_count(candidates);
         candidate_idx++) {
      const Move *candidate = move_list_get_move(candidates, candidate_idx);
      for (int rollout_idx = 0; rollout_idx < CENSUS_ROLLOUT_TEST_ROLLOUTS;
           rollout_idx++) {
        game_copy(rollout, game);
        return_rack_to_bag(rollout, opponent_index);
        game_seed(rollout,
                  1000 + (uint64_t)(candidate_idx * 100 + rollout_idx));
        set_random_rack(rollout, opponent_index, NULL);
        play_move(candidate, rollout, NULL);
        census_rollout_worker_set_candidate(serial_worker, serial_census,
                                            candidate);
        census_rollout_worker_set_candidate(threaded_worker, threaded_census,
                                            candidate);
        for (int ply = 0;
             ply < CENSUS_ROLLOUT_TEST_PLIES && !game_over(rollout); ply++) {
          move_copy(&census_move,
                    census_rollout_get_best_move(serial_worker, serial_census,
                                                 rollout, census_moves, ply));
          served += census_rollout_worker_last_served(serial_worker);
          steered += census_rollout_worker_last_steered(serial_worker);
          assert_moves_are_equal(
              &census_move,
              census_rollout_get_best_move(threaded_worker, threaded_census,
                                           rollout, census_moves, ply));
          const Move *plain = get_top_equity_move(rollout, move_list);
          assert_moves_are_equal(&census_move, plain);
          play_move(plain, rollout, NULL);
        }
      }
    }
  }
  // The census did the steering, not just the plain generation.
  assert(served > 0);
  assert(steered > 0);
  census_rollout_worker_destroy(threaded_worker);
  census_rollout_worker_destroy(serial_worker);
  census_rollout_destroy(threaded_census);
  census_rollout_destroy(serial_census);
  move_list_destroy(candidates);
  move_list_destroy(census_moves);
  move_list_destroy(move_list);
  game_destroy(rollout);
  game_destroy(game);
  config_destroy(config);
}

// A sim's results, play by play, for comparing two sims of the same work.
typedef struct SimSnapshot {
  int number_of_plays;
  uint64_t node_count;
  Move moves[CENSUS_ROLLOUT_TEST_MAX_PLAYS];
  uint64_t rollouts[CENSUS_ROLLOUT_TEST_MAX_PLAYS];
  double equity_means[CENSUS_ROLLOUT_TEST_MAX_PLAYS];
  double win_pct_means[CENSUS_ROLLOUT_TEST_MAX_PLAYS];
} SimSnapshot;

static void take_sim_snapshot(const SimResults *sim_results,
                              SimSnapshot *snapshot) {
  snapshot->number_of_plays = sim_results_get_number_of_plays(sim_results);
  assert(snapshot->number_of_plays <= CENSUS_ROLLOUT_TEST_MAX_PLAYS);
  snapshot->node_count = sim_results_get_node_count(sim_results);
  for (int play_idx = 0; play_idx < snapshot->number_of_plays; play_idx++) {
    const SimmedPlay *simmed_play =
        sim_results_get_simmed_play(sim_results, play_idx);
    move_copy(&snapshot->moves[play_idx], simmed_play_get_move(simmed_play));
    snapshot->rollouts[play_idx] = simmed_play_get_num_rollouts(simmed_play);
    snapshot->equity_means[play_idx] =
        simmed_play_get_exact_equity_mean(simmed_play);
    snapshot->win_pct_means[play_idx] =
        simmed_play_get_exact_win_pct_mean(simmed_play);
  }
}

// The same plays, rollouts and means, exactly: the rollouts played the same
// moves.
static void assert_sim_snapshots_equal(const SimSnapshot *first,
                                       const SimSnapshot *second) {
  assert(first->number_of_plays == second->number_of_plays);
  assert(first->node_count == second->node_count);
  for (int play_idx = 0; play_idx < first->number_of_plays; play_idx++) {
    assert_moves_are_equal(&first->moves[play_idx], &second->moves[play_idx]);
    assert(first->rollouts[play_idx] == second->rollouts[play_idx]);
    assert(first->equity_means[play_idx] == second->equity_means[play_idx]);
    assert(first->win_pct_means[play_idx] == second->win_pct_means[play_idx]);
  }
}

// Runs the sim command, whose seed option starts the sim from the same state
// each time, and snapshots its results.
static void sim_and_snapshot(Config *config, const char *sim_command,
                             SimSnapshot *snapshot) {
  load_and_exec_config_or_die(config, sim_command);
  take_sim_snapshot(config_get_sim_results(config), snapshot);
}

// A census grows to a sim's candidates however many there are: after a sim
// of a few candidates, one of many steers the rollouts of its last candidates,
// each move still the plain one.
static void test_census_rollout_steers_many_candidates(void) {
  if (BOARD_DIM != 15) {
    return;
  }
  Config *config = config_create_or_die("set -lex CSW21 -wmp true");
  Game *game = config_game_create(config);
  Game *rollout = game_duplicate(game);
  MoveList *move_list = move_list_create(1);
  MoveList *census_moves = move_list_create(1);
  MoveList *few_candidates = move_list_create(CENSUS_ROLLOUT_TEST_CANDIDATES);
  MoveList *candidates = move_list_create(CENSUS_ROLLOUT_TEST_MANY_CANDIDATES);
  CensusRollout *census = census_rollout_create(1);
  CensusRolloutWorker *worker = census_rollout_worker_create();
  play_to_bag_size(game, move_list, 100, census_rollout_test_bag_sizes[0]);
  generate_candidates(game, few_candidates);
  census_rollout_prepare(census, game, few_candidates);
  census_rollout_worker_set_candidate(worker, census,
                                      move_list_get_move(few_candidates, 0));
  generate_candidates(game, candidates);
  const int number_of_candidates = move_list_get_count(candidates);
  assert(number_of_candidates == CENSUS_ROLLOUT_TEST_MANY_CANDIDATES);
  census_rollout_prepare(census, game, candidates);
  const int opponent_index = 1 - game_get_player_on_turn_index(game);
  Move census_move;
  uint64_t steered = 0;
  for (int candidate_idx =
           number_of_candidates - CENSUS_ROLLOUT_TEST_CHECKED_CANDIDATES;
       candidate_idx < number_of_candidates; candidate_idx++) {
    const Move *candidate = move_list_get_move(candidates, candidate_idx);
    for (int rollout_idx = 0; rollout_idx < CENSUS_ROLLOUT_TEST_ROLLOUTS;
         rollout_idx++) {
      game_copy(rollout, game);
      return_rack_to_bag(rollout, opponent_index);
      game_seed(rollout, 1000 + (uint64_t)(candidate_idx * 100 + rollout_idx));
      set_random_rack(rollout, opponent_index, NULL);
      play_move(candidate, rollout, NULL);
      census_rollout_worker_set_candidate(worker, census, candidate);
      for (int ply = 0; ply < CENSUS_ROLLOUT_TEST_PLIES && !game_over(rollout);
           ply++) {
        move_copy(&census_move,
                  census_rollout_get_best_move(worker, census, rollout,
                                               census_moves, ply));
        steered += census_rollout_worker_last_steered(worker);
        const Move *plain = get_top_equity_move(rollout, move_list);
        assert_moves_are_equal(&census_move, plain);
        play_move(plain, rollout, NULL);
      }
    }
  }
  assert(steered > 0);
  census_rollout_worker_destroy(worker);
  census_rollout_destroy(census);
  move_list_destroy(candidates);
  move_list_destroy(few_candidates);
  move_list_destroy(census_moves);
  move_list_destroy(move_list);
  game_destroy(rollout);
  game_destroy(game);
  config_destroy(config);
}

// A single-threaded sim gives the same results with the census as without
// it, since every census-steered move is the plain one.
static void test_census_rollout_sim_matches_plain(void) {
  if (BOARD_DIM != 15) {
    return;
  }
  char *command = get_formatted_string(
      "set -lex CSW21 -wmp true -threads 1 -plies 2 -numplays 8 -it %d "
      "-scond none -patrollout false -sinfer false",
      CENSUS_ROLLOUT_TEST_SIM_ITERATIONS);
  Config *config = config_create_or_die(command);
  free(command);
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  MoveList *move_list = move_list_create(1);
  play_to_bag_size(config_get_game(config), move_list, 7, 40);
  load_and_exec_config_or_die(config, "gen");
  SimSnapshot plain_snapshot;
  SimSnapshot census_snapshot;
  sim_and_snapshot(config, "sim -seed 7 -scensus false", &plain_snapshot);
  sim_and_snapshot(config, "sim -seed 7 -scensus true", &census_snapshot);
  assert_sim_snapshots_equal(&plain_snapshot, &census_snapshot);
  move_list_destroy(move_list);
  config_destroy(config);
}

// The census leaves out PAT's term, so a sim refuses it while its rollouts
// use PAT, and takes it with rollout PAT off.
static void test_census_rollout_refuses_pat(void) {
  if (BOARD_DIM != 15) {
    return;
  }
  Config *config = config_create_or_die(
      "set -lex CSW21 -pat CSW21 -wmp true -threads 1 -plies 2 -numplays 4 "
      "-it 20 -scond none -seed 7 -scensus true -patrollout true");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  MoveList *move_list = move_list_create(1);
  play_to_bag_size(config_get_game(config), move_list, 7, 40);
  load_and_exec_config_or_die(config, "gen");
  SimResults *sim_results = sim_results_create(0);
  assert(config_simulate_and_return_status(config, NULL, NULL, sim_results) ==
         ERROR_STATUS_SIM_CENSUS_WITH_PAT);
  load_and_exec_config_or_die(config, "set -patrollout false");
  assert(config_simulate_and_return_status(config, NULL, NULL, sim_results) ==
         ERROR_STATUS_SUCCESS);
  sim_results_destroy(sim_results);
  move_list_destroy(move_list);
  config_destroy(config);
}

// A sim of the config's position on sim_ctx and the sim_results it keeps, as
// the sim command runs one: whether the census steered it, its results equal
// to a plain sim's.
static bool census_rollout_sim_steers(Config *config, SimCtx **sim_ctx,
                                      SimResults *sim_results,
                                      const char *census_setting) {
  SimSnapshot plain_snapshot;
  sim_and_snapshot(config, "sim -seed 7 -scensus false", &plain_snapshot);
  char *command =
      get_formatted_string("set -seed 7 -scensus %s", census_setting);
  load_and_exec_config_or_die(config, command);
  free(command);
  // The sim command's default: the opponent's rack as it is known.
  const Game *game = config_get_game(config);
  Rack known_opp_rack;
  rack_set_dist_size_and_reset(&known_opp_rack, ld_get_size(game_get_ld(game)));
  rack_copy(&known_opp_rack,
            player_get_rack(game_get_player(
                game, 1 - game_get_player_on_turn_index(game))));
  ErrorStack *error_stack = error_stack_create();
  config_simulate(config, sim_ctx, &known_opp_rack, sim_results, NULL, 0,
                  error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  SimSnapshot snapshot;
  take_sim_snapshot(sim_results, &snapshot);
  assert_sim_snapshots_equal(&plain_snapshot, &snapshot);
  return sim_results_get_census_rollout(sim_results);
}

// -scensus auto: the memory it weighs, the builds it judges worth their
// cost, the games it serves, and sims that give the plain results, PAT or
// not, saved as auto.
static void test_census_rollout_auto(void) {
  // Within a quarter of the physical memory, and what is available less the
  // reserve; an unknown available leaves the quarter alone.
  const uint64_t gigabyte = 1000000000ULL;
  assert(census_rollout_fits_in_memory(gigabyte, 16 * gigabyte,
                                       (int64_t)(8 * gigabyte)));
  assert(!census_rollout_fits_in_memory(gigabyte, 3 * gigabyte,
                                        (int64_t)(8 * gigabyte)));
  assert(!census_rollout_fits_in_memory(gigabyte, 16 * gigabyte,
                                        (int64_t)(gigabyte + gigabyte / 2)));
  assert(!census_rollout_fits_in_memory(gigabyte, 16 * gigabyte,
                                        (int64_t)(gigabyte / 2)));
  assert(census_rollout_fits_in_memory(gigabyte, 16 * gigabyte,
                                       (int64_t)(2 * gigabyte)));
  assert(census_rollout_fits_in_memory(gigabyte, 4 * gigabyte, -1));
  assert(!census_rollout_fits_in_memory(gigabyte + 1, 4 * gigabyte, -1));

  // 1000 entries over 10 anchors predict 10000 for 100 candidate anchors,
  // which 60 entries a ply pay for in 84 rollouts of 2 plies or more (only
  // 2 count), and not in 83, or in 166 rollouts of 1 ply.
  const int per_ply = CENSUS_ROLLOUT_AUTO_ENTRIES_PER_PLY;
  const uint64_t paying_rollouts = 10000 / (2 * per_ply) + 1;
  assert(census_rollout_build_pays(1000, 10, 100, paying_rollouts, 2));
  assert(census_rollout_build_pays(1000, 10, 100, paying_rollouts, 5));
  assert(!census_rollout_build_pays(1000, 10, 100, paying_rollouts - 1, 5));
  assert(!census_rollout_build_pays(1000, 10, 100, 2 * paying_rollouts - 2, 1));
  // Candidates that change no anchor cost nothing to build.
  assert(census_rollout_build_pays(1000, 10, 0, 0, 2));
  assert(!census_rollout_build_pays(1000, 10, 100, paying_rollouts, 0));

  if (BOARD_DIM != 15) {
    return;
  }
  Config *config = config_create_or_die(
      "set -lex CSW21 -pat CSW21 -wmp true -threads 1 -plies 2 -numplays 8 "
      "-it 300 -scond none -sinfer false -patrollout false");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  MoveList *move_list = move_list_create(1);
  play_to_bag_size(config_get_game(config), move_list, 7, 40);
  const Game *game = config_get_game(config);
  assert(census_rollout_supports(game));
  // English: a lattice of 5.4M ranks, its leave and scratch tables 21.5 MB
  // each. The estimate grows with the threads and the candidates, every
  // candidate taking a table.
  const uint64_t one_thread = census_rollout_memory_bytes(game, 1, 64);
  assert(one_thread > 200000000ULL && one_thread < 600000000ULL);
  assert(census_rollout_memory_bytes(game, 12, 64) > one_thread);
  assert(census_rollout_memory_bytes(game, 1, 8) < one_thread);
  assert(census_rollout_memory_bytes(game, 1, 100) > one_thread);

  // With 40 tiles in the bag, 300 rollouts do not pay for the candidates'
  // tables, so auto plays the sim without the census, keeping what it built.
  load_and_exec_config_or_die(config, "gen");
  SimCtx *sim_ctx = NULL;
  SimResults *sim_results = sim_results_create(0);
  assert(census_rollout_sim_steers(config, &sim_ctx, sim_results, "true"));
  const uint64_t held_bytes = sim_ctx_census_rollout_held_bytes(sim_ctx);
  // What it holds is within what it was estimated to take.
  assert(held_bytes > 0 &&
         held_bytes <= census_rollout_memory_bytes(game, 1, 8));
  assert(!census_rollout_sim_steers(config, &sim_ctx, sim_results, "auto"));
  assert(sim_ctx_census_rollout_held_bytes(sim_ctx) > 0);
  // A sim without the census releases it.
  assert(!census_rollout_sim_steers(config, &sim_ctx, sim_results, "false"));
  assert(sim_ctx_census_rollout_held_bytes(sim_ctx) == 0);

  // With 10 tiles in the bag, the tables are small, and 2000 rollouts pay
  // for them many times over (the position's moves differ by platform, so
  // the margin is wide), so auto steers the sim. It needs this machine to
  // have about 1.1 GB available: the census and the reserve auto leaves.
  play_to_bag_size(config_get_game(config), move_list, 7, 10);
  load_and_exec_config_or_die(config, "set -it 2000");
  load_and_exec_config_or_die(config, "gen");
  assert(census_rollout_sim_steers(config, &sim_ctx, sim_results, "auto"));
  // A second sim reuses the census, whose memory auto counts as free.
  assert(census_rollout_sim_steers(config, &sim_ctx, sim_results, "auto"));
  // With rollout PAT on, auto leaves the census out rather than refusing.
  load_and_exec_config_or_die(config, "set -patrollout true");
  assert(!census_rollout_sim_steers(config, &sim_ctx, sim_results, "auto"));
  sim_ctx_destroy(sim_ctx);
  sim_results_destroy(sim_results);

  // Saved as auto, not as what this machine decided.
  StringBuilder *sb = string_builder_create();
  config_add_settings_to_string_builder(config, sb);
  assert(strstr(string_builder_peek(sb), " -scensus auto") != NULL);
  string_builder_destroy(sb);
  load_and_exec_config_or_die(config, "set -scensus false");
  sb = string_builder_create();
  config_add_settings_to_string_builder(config, sb);
  assert(strstr(string_builder_peek(sb), " -scensus false") != NULL);
  string_builder_destroy(sb);
  move_list_destroy(move_list);
  config_destroy(config);

  // The census reads the first player's word graph and leaves, so it serves
  // no game whose players differ in them.
  config = config_create_or_die("set -l1 NWL20 -l2 CSW21 -wmp false");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  assert(!census_rollout_supports(config_get_game(config)));
  assert(census_rollout_memory_bytes(config_get_game(config), 1, 8) == 0);
  config_destroy(config);
}

void test_census_rollout(void) {
  test_census_rollout_moves_match_plain();
  test_census_rollout_steers_many_candidates();
  test_census_rollout_sim_matches_plain();
  test_census_rollout_refuses_pat();
  test_census_rollout_auto();
}

// Single-threaded sims of four positions of a CSW24 game, with and without
// the census: the same results, and the time each takes.
void test_census_rollout_bench(void) {
  char *command = get_formatted_string(
      "set -lex CSW24 -wmp true -threads 1 -plies %d -numplays 8 -it %d "
      "-scond none -patrollout false -sinfer false",
      CENSUS_ROLLOUT_BENCH_PLIES, CENSUS_ROLLOUT_BENCH_ITERATIONS);
  Config *config = config_create_or_die(command);
  free(command);
  load_and_exec_config_or_die(config,
                              "load testdata/gcgs/success_long_game.gcg");
  const int turns[] = {14, 18, 22, 26};
  const int number_of_turns = (int)(sizeof(turns) / sizeof(turns[0]));
  double plain_total = 0;
  double census_total = 0;
  SimSnapshot snapshots[2];
  for (int turn_idx = 0; turn_idx < number_of_turns; turn_idx++) {
    char *goto_command = get_formatted_string("goto %d", turns[turn_idx]);
    load_and_exec_config_or_die(config, goto_command);
    free(goto_command);
    load_and_exec_config_or_die(config, "gen");
    double seconds[2];
    for (int census_on = 0; census_on < 2; census_on++) {
      Timer timer;
      ctimer_start(&timer);
      sim_and_snapshot(config,
                       census_on ? "sim -seed 7 -scensus true"
                                 : "sim -seed 7 -scensus false",
                       &snapshots[census_on]);
      ctimer_stop(&timer);
      seconds[census_on] = ctimer_elapsed_seconds(&timer);
    }
    assert_sim_snapshots_equal(&snapshots[0], &snapshots[1]);
    printf("census rollout bench: turn %d, bag %d: %.3f s without the census, "
           "%.3f s with it, %.2fx\n",
           turns[turn_idx],
           bag_get_letters(game_get_bag(config_get_game(config))), seconds[0],
           seconds[1], seconds[0] / seconds[1]);
    plain_total += seconds[0];
    census_total += seconds[1];
  }
  printf("census rollout bench: %.3f s without the census, %.3f s with it, "
         "%.2fx\n",
         plain_total, census_total, plain_total / census_total);
  config_destroy(config);
}
