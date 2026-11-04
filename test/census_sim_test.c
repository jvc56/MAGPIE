#include "census_sim_test.h"

#include "../src/compat/cpthread.h"
#include "../src/compat/cpthread_defs.h"
#include "../src/compat/ctime.h"
#include "../src/def/equity_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/xoshiro.h"
#include "../src/impl/census.h"
#include "../src/impl/census_play_scores.h"
#include "../src/impl/census_sim.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/rack_draw.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <assert.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum {
  CENSUS_SIM_MAX_CANDIDATES = 64,
  // The samples each candidate's sampled one-ply value takes.
  CENSUS_SIM_TEST_SAMPLES = 4000,
  // The timing: candidates per position, and the threaded sim's threads.
  CENSUS_SIM_TEST_TIME_CANDIDATES = 100,
  CENSUS_SIM_TEST_TIME_THREADS = 18,
  // The match against static: game pairs, threads, and candidates per turn.
  CENSUS_SIM_TEST_MATCH_PAIRS = 20,
  CENSUS_SIM_TEST_MATCH_THREADS = 8,
  CENSUS_SIM_TEST_MATCH_CANDIDATES = 10,
};

// The top number_of_candidates moves by static equity, best first.
static int top_static_moves(Game *game, MoveList *move_list, Move *candidates,
                            int number_of_candidates) {
  const MoveGenArgs args = {.game = game,
                            .move_list = move_list,
                            .move_record_type = MOVE_RECORD_ALL,
                            .move_sort_type = MOVE_SORT_EQUITY,
                            .override_kwg = NULL,
                            .eq_margin_movegen = 0,
                            .target_equity = EQUITY_MAX_VALUE};
  generate_moves(&args);
  move_list_sort_moves(move_list);
  int count = move_list_get_count(move_list);
  if (count > number_of_candidates) {
    count = number_of_candidates;
  }
  for (int move_idx = 0; move_idx < count; move_idx++) {
    move_copy(&candidates[move_idx], move_list_get_move(move_list, move_idx));
  }
  return count;
}

// A one-ply sim of move by sampling: the opponent's rack is drawn from the
// tiles the mover cannot see before the move, the move is played, and the
// opponent's best reply by static equity is subtracted, as sim_rollout does.
static void sampled_one_ply(const Game *game, const Move *move, int samples,
                            XoshiroPRNG *prng, double *mean,
                            double *standard_error) {
  const int mover_index = game_get_player_on_turn_index(game);
  const int opponent_index = 1 - mover_index;
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
  Game *rollout = game_duplicate(game);
  MoveList *move_list = move_list_create(1);
  uint8_t pool[CENSUS_MAX_LETTERS];
  census_get_unseen_tally(game, pool);
  const Rack *mover_rack = player_get_rack(game_get_player(game, mover_index));
  MachineLetter tiles[200];
  int number_of_tiles = 0;
  for (int ml = 0; ml < ld_size; ml++) {
    const int count = pool[ml] - rack_get_letter(mover_rack, ml);
    for (int copy_idx = 0; copy_idx < count; copy_idx++) {
      tiles[number_of_tiles++] = (MachineLetter)ml;
    }
  }
  Rack leave_rack;
  rack_copy(&leave_rack, mover_rack);
  const Equity move_value =
      move_get_score(move) +
      get_leave_value_for_move(
          player_get_klv(game_get_player(game, mover_index)), move,
          &leave_rack);
  double sum = 0;
  double sum_of_squares = 0;
  for (int sample_idx = 0; sample_idx < samples; sample_idx++) {
    game_copy(rollout, game);
    for (int draw_idx = 0; draw_idx < RACK_SIZE; draw_idx++) {
      const int pick =
          draw_idx + (int)prng_get_random_number(
                         prng, (uint64_t)(number_of_tiles - draw_idx));
      const MachineLetter swap = tiles[draw_idx];
      tiles[draw_idx] = tiles[pick];
      tiles[pick] = swap;
    }
    Rack opponent_rack;
    rack_set_dist_size_and_reset(&opponent_rack, ld_size);
    for (int draw_idx = 0; draw_idx < RACK_SIZE; draw_idx++) {
      rack_add_letter(&opponent_rack, tiles[draw_idx]);
    }
    return_rack_to_bag(rollout, opponent_index);
    const bool drawn =
        draw_rack_from_bag(rollout, opponent_index, &opponent_rack);
    assert(drawn);
    play_move(move, rollout, NULL);
    const Move *reply = get_top_equity_move(rollout, move_list);
    Rack reply_rack;
    rack_copy(&reply_rack,
              player_get_rack(game_get_player(rollout, opponent_index)));
    const Equity reply_value =
        move_get_score(reply) +
        get_leave_value_for_move(
            player_get_klv(game_get_player(rollout, opponent_index)), reply,
            &reply_rack);
    const double value = equity_to_double(move_value - reply_value);
    sum += value;
    sum_of_squares += value * value;
  }
  *mean = sum / samples;
  const double variance = sum_of_squares / samples - *mean * *mean;
  *standard_error = sqrt(variance / samples);
  move_list_destroy(move_list);
  game_destroy(rollout);
}

// Plays static moves from the start of a seeded game until the bag holds at
// most bag_tiles tiles.
static void play_to_bag_size(Game *game, uint64_t seed, int bag_tiles,
                             MoveList *move_list) {
  game_reset(game);
  game_seed(game, seed);
  draw_starting_racks(game);
  while (!game_over(game) && bag_get_letters(game_get_bag(game)) > bag_tiles) {
    play_move(get_top_equity_move(game, move_list), game, NULL);
  }
}

static void test_census_sim_matches_sampling(const Config *config) {
  Game *game = config_get_game(config);
  Game *scratch = game_duplicate(game);
  MoveList *move_list = move_list_create(CENSUS_SIM_MAX_CANDIDATES);
  MoveList *static_list = move_list_create(1);
  CensusSim *sim = census_sim_create(game, config_get_num_threads(config));
  XoshiroPRNG *prng = prng_create(17);
  const int samples = CENSUS_SIM_TEST_SAMPLES;
  Move candidates[CENSUS_SIM_MAX_CANDIDATES];
  const int bag_sizes[] = {70, 50, 30};
  for (int position_idx = 0; position_idx < 3; position_idx++) {
    play_to_bag_size(game, 100 + (uint64_t)position_idx,
                     bag_sizes[position_idx], static_list);
    const int count = top_static_moves(game, move_list, candidates, 5);
    char *cgp = game_get_cgp(game, true);
    printf("position %d, bag %d, cgp %s\n", position_idx,
           bag_get_letters(game_get_bag(game)), cgp);
    free(cgp);
    // the play scores built whole for each move, then from the position's
    // groups of lanes: the same values
    Equity full_values[CENSUS_SIM_MAX_CANDIDATES];
    census_sim_clear_position(sim);
    for (int move_idx = 0; move_idx < count; move_idx++) {
      full_values[move_idx] = census_sim_value_move(
          sim, game, &candidates[move_idx], scratch, NULL);
    }
    const CensusSimTimes full_times = *census_sim_get_times(sim);
    Timer position_timer;
    ctimer_start(&position_timer);
    census_sim_set_position(sim, game);
    ctimer_stop(&position_timer);
    for (int move_idx = 0; move_idx < count; move_idx++) {
      assert(census_sim_value_move(sim, game, &candidates[move_idx], scratch,
                                   NULL) == full_values[move_idx]);
    }
    const CensusSimTimes *incremental_times = census_sim_get_times(sim);
    printf(
        "  play scores per move: whole %.2f ms, from the position %.2f ms "
        "(position %.2f ms once)\n",
        1000 * full_times.play_scores_seconds / full_times.number_of_moves,
        1000 *
            (incremental_times->play_scores_seconds -
             full_times.play_scores_seconds) /
            (incremental_times->number_of_moves - full_times.number_of_moves),
        1000 * ctimer_elapsed_seconds(&position_timer));
    for (int move_idx = 0; move_idx < count; move_idx++) {
      const Equity census_value = full_values[move_idx];
      double mean;
      double standard_error;
      sampled_one_ply(game, &candidates[move_idx], samples, prng, &mean,
                      &standard_error);
      const double census_double = equity_to_double(census_value);
      printf("  static %8.3f  census %8.3f  sampled %8.3f +- %.3f  (%+.1f "
             "se)\n",
             equity_to_double(move_get_equity(&candidates[move_idx])),
             census_double, mean, standard_error,
             (census_double - mean) / standard_error);
    }
  }
  const CensusSimTimes *times = census_sim_get_times(sim);
  printf("per move: play scores %.2f ms, expected best %.2f ms\n",
         1000 * times->play_scores_seconds / times->number_of_moves,
         1000 * times->expected_best_seconds / times->number_of_moves);
  prng_destroy(prng);
  census_sim_destroy(sim);
  move_list_destroy(static_list);
  move_list_destroy(move_list);
  game_destroy(scratch);
}

// Times a turn of the census one-ply player with many candidates: the top
// CENSUS_SIM_TEST_TIME_CANDIDATES static moves of each position, valued with
// the play scores built whole and with the expected best on one thread, then
// built from the position, then with the expected best on
// CENSUS_SIM_TEST_TIME_THREADS threads. All three give the same values.
static void time_census_sim_turns(const Config *config) {
  Game *game = config_get_game(config);
  Game *scratch = game_duplicate(game);
  const int number_of_candidates = CENSUS_SIM_TEST_TIME_CANDIDATES;
  const int number_of_threads = CENSUS_SIM_TEST_TIME_THREADS;
  MoveList *move_list = move_list_create(number_of_candidates);
  MoveList *static_list = move_list_create(1);
  Move *candidates = malloc_or_die(sizeof(Move) * (size_t)number_of_candidates);
  Equity *values = malloc_or_die(sizeof(Equity) * (size_t)number_of_candidates);
  CensusSim *serial_sim = census_sim_create(game, number_of_threads);
  CensusSim *threaded_sim = census_sim_create(game, number_of_threads);
  // census_sim_create's thread count also builds the add table; the serial
  // sim values with one thread
  census_sim_set_threads(serial_sim, 1);
  const int bag_sizes[] = {70, 50, 30};
  for (int position_idx = 0; position_idx < 3; position_idx++) {
    play_to_bag_size(game, 100 + (uint64_t)position_idx,
                     bag_sizes[position_idx], static_list);
    const int count =
        top_static_moves(game, move_list, candidates, number_of_candidates);
    printf("position %d, bag %d, %d candidates\n", position_idx,
           bag_get_letters(game_get_bag(game)), count);
    for (int mode = 0; mode < 3; mode++) {
      CensusSim *sim = mode == 2 ? threaded_sim : serial_sim;
      const CensusSimTimes before = *census_sim_get_times(sim);
      Timer timer;
      ctimer_start(&timer);
      if (mode == 0) {
        census_sim_clear_position(sim);
      } else {
        census_sim_set_position(sim, game);
      }
      for (int move_idx = 0; move_idx < count; move_idx++) {
        const Equity value = census_sim_value_move(
            sim, game, &candidates[move_idx], scratch, NULL);
        if (mode == 0) {
          values[move_idx] = value;
        } else {
          assert(value == values[move_idx]);
        }
      }
      ctimer_stop(&timer);
      const CensusSimTimes *after = census_sim_get_times(sim);
      const int moves = after->number_of_moves - before.number_of_moves;
      const char *names[] = {"whole build, expected best on 1 thread",
                             "build from position, expected best on 1 thread",
                             "build from position, expected best threaded"};
      printf("  %-50s turn %6.2f s: play scores %6.2f ms, expected best "
             "%6.2f ms a move\n",
             names[mode], ctimer_elapsed_seconds(&timer),
             1000 * (after->play_scores_seconds - before.play_scores_seconds) /
                 moves,
             1000 *
                 (after->expected_best_seconds - before.expected_best_seconds) /
                 moves);
    }
  }
  census_sim_destroy(threaded_sim);
  census_sim_destroy(serial_sim);
  free(values);
  free(candidates);
  move_list_destroy(static_list);
  move_list_destroy(move_list);
  game_destroy(scratch);
}

typedef struct CensusSimMatchWorker {
  const Game *game;
  int number_of_candidates;
  int min_bag;
  int number_of_pairs;
  int number_of_workers;
  _Atomic int *next_pair;
  // results, from the census player's side
  double spread_sum;
  double spread_sum_of_squares;
  // over pairs, the mean of the pair's two spreads
  double pair_sum;
  double pair_sum_of_squares;
  int pairs;
  double wins;
  int games;
  int census_moves;
  int changed_moves;
  // per census move, the chosen move's expected opponent reply, and its own
  // score and leave, less the static choice's
  double reply_change_sum;
  double own_change_sum;
  // points per game of the census player and its opponent, and of the same
  // seats in the static game from the same seed
  double census_points;
  double opponent_points;
  double baseline_census_seat_points;
  double baseline_opponent_seat_points;
  int opponent_moves;
  double seconds_valuing;
  cpthread_t cpthread_id;
} CensusSimMatchWorker;

// Plays one game from seed, the census player as census_index (none if it is
// -1), and returns census_index's final spread.
static Equity play_census_game(CensusSimMatchWorker *worker, Game *game,
                               Game *scratch, CensusSim *sim,
                               MoveList *move_list, MoveList *static_list,
                               uint64_t seed, int census_index) {
  game_reset(game);
  game_seed(game, seed);
  draw_starting_racks(game);
  Move candidates[CENSUS_SIM_MAX_CANDIDATES];
  Move chosen;
  while (!game_over(game)) {
    const int on_turn = game_get_player_on_turn_index(game);
    if (census_index >= 0 && on_turn != census_index) {
      worker->opponent_moves++;
    }
    const bool use_census =
        on_turn == census_index &&
        bag_get_letters(game_get_bag(game)) >= worker->min_bag;
    if (!use_census) {
      move_copy(&chosen, get_top_equity_move(game, static_list));
    } else {
      const int count = top_static_moves(game, move_list, candidates,
                                         worker->number_of_candidates);
      Timer timer;
      ctimer_start(&timer);
      census_sim_set_position(sim, game);
      int best_idx = 0;
      Equity best_value = EQUITY_MIN_VALUE;
      Equity values[CENSUS_SIM_MAX_CANDIDATES];
      Equity replies[CENSUS_SIM_MAX_CANDIDATES];
      for (int move_idx = 0; move_idx < count; move_idx++) {
        values[move_idx] = census_sim_value_move(
            sim, game, &candidates[move_idx], scratch, &replies[move_idx]);
        if (values[move_idx] > best_value) {
          best_value = values[move_idx];
          best_idx = move_idx;
        }
      }
      worker->reply_change_sum +=
          equity_to_double(replies[best_idx] - replies[0]);
      worker->own_change_sum += equity_to_double(
          (values[best_idx] + replies[best_idx]) - (values[0] + replies[0]));
      ctimer_stop(&timer);
      worker->seconds_valuing += ctimer_elapsed_seconds(&timer);
      worker->census_moves++;
      if (best_idx != 0) {
        worker->changed_moves++;
      }
      move_copy(&chosen, &candidates[best_idx]);
    }
    play_move(&chosen, game, NULL);
  }
  if (census_index < 0) {
    return 0;
  }
  return player_get_score(game_get_player(game, census_index)) -
         player_get_score(game_get_player(game, 1 - census_index));
}

static void *census_sim_match_worker(void *uncasted_worker) {
  CensusSimMatchWorker *worker = (CensusSimMatchWorker *)uncasted_worker;
  Game *game = game_duplicate(worker->game);
  Game *scratch = game_duplicate(worker->game);
  MoveList *move_list = move_list_create(CENSUS_SIM_MAX_CANDIDATES);
  MoveList *static_list = move_list_create(1);
  CensusSim *sim = census_sim_create(game, 1);
  while (true) {
    const int pair_idx = atomic_fetch_add(worker->next_pair, 1);
    if (pair_idx >= worker->number_of_pairs) {
      break;
    }
    const uint64_t seed = 1000 + (uint64_t)pair_idx;
    // the static game from this seed, for the points of each seat
    play_census_game(worker, game, scratch, sim, move_list, static_list, seed,
                     -1);
    double static_points[2];
    for (int seat = 0; seat < 2; seat++) {
      static_points[seat] =
          equity_to_double(player_get_score(game_get_player(game, seat)));
    }
    double pair_spread = 0;
    for (int census_index = 0; census_index < 2; census_index++) {
      const double spread = equity_to_double(
          play_census_game(worker, game, scratch, sim, move_list, static_list,
                           seed, census_index));
      worker->census_points += equity_to_double(
          player_get_score(game_get_player(game, census_index)));
      worker->opponent_points += equity_to_double(
          player_get_score(game_get_player(game, 1 - census_index)));
      worker->baseline_census_seat_points += static_points[census_index];
      worker->baseline_opponent_seat_points += static_points[1 - census_index];
      pair_spread += spread / 2;
      worker->spread_sum += spread;
      worker->spread_sum_of_squares += spread * spread;
      if (spread > 0) {
        worker->wins += 1.0;
      } else if (spread == 0) {
        worker->wins += 0.5;
      }
      worker->games++;
    }
    worker->pair_sum += pair_spread;
    worker->pair_sum_of_squares += pair_spread * pair_spread;
    worker->pairs++;
  }
  census_sim_destroy(sim);
  move_list_destroy(static_list);
  move_list_destroy(move_list);
  game_destroy(scratch);
  game_destroy(game);
  return NULL;
}

// Game pairs of the census one-ply player, which picks among the top
// static moves by census_sim_value_move while the bag holds at least min_bag
// tiles, against the static player, each game played from both seats.
static void test_census_sim_match(const Config *config) {
  const int number_of_pairs = CENSUS_SIM_TEST_MATCH_PAIRS;
  const int number_of_workers = CENSUS_SIM_TEST_MATCH_THREADS;
  _Atomic int next_pair;
  atomic_init(&next_pair, 0);
  CensusSimMatchWorker workers[64];
  assert(number_of_workers <= 64);
  Timer timer;
  ctimer_start(&timer);
  for (int worker_idx = 0; worker_idx < number_of_workers; worker_idx++) {
    workers[worker_idx] = (CensusSimMatchWorker){
        .game = config_get_game(config),
        .number_of_candidates = CENSUS_SIM_TEST_MATCH_CANDIDATES,
        .min_bag = RACK_SIZE * 2,
        .number_of_pairs = number_of_pairs,
        .number_of_workers = number_of_workers,
        .next_pair = &next_pair,
    };
    cpthread_create(&workers[worker_idx].cpthread_id, census_sim_match_worker,
                    &workers[worker_idx]);
  }
  double spread_sum = 0;
  double spread_sum_of_squares = 0;
  double wins = 0;
  double pair_sum = 0;
  double pair_sum_of_squares = 0;
  double reply_change_sum = 0;
  double own_change_sum = 0;
  double census_points = 0;
  double opponent_points = 0;
  double baseline_census_seat_points = 0;
  double baseline_opponent_seat_points = 0;
  int opponent_moves = 0;
  int pairs = 0;
  int games = 0;
  int census_moves = 0;
  int changed_moves = 0;
  double seconds_valuing = 0;
  for (int worker_idx = 0; worker_idx < number_of_workers; worker_idx++) {
    cpthread_join(workers[worker_idx].cpthread_id);
    spread_sum += workers[worker_idx].spread_sum;
    spread_sum_of_squares += workers[worker_idx].spread_sum_of_squares;
    wins += workers[worker_idx].wins;
    pair_sum += workers[worker_idx].pair_sum;
    pair_sum_of_squares += workers[worker_idx].pair_sum_of_squares;
    pairs += workers[worker_idx].pairs;
    games += workers[worker_idx].games;
    census_moves += workers[worker_idx].census_moves;
    changed_moves += workers[worker_idx].changed_moves;
    reply_change_sum += workers[worker_idx].reply_change_sum;
    own_change_sum += workers[worker_idx].own_change_sum;
    census_points += workers[worker_idx].census_points;
    opponent_points += workers[worker_idx].opponent_points;
    baseline_census_seat_points +=
        workers[worker_idx].baseline_census_seat_points;
    baseline_opponent_seat_points +=
        workers[worker_idx].baseline_opponent_seat_points;
    opponent_moves += workers[worker_idx].opponent_moves;
    seconds_valuing += workers[worker_idx].seconds_valuing;
  }
  ctimer_stop(&timer);
  const double mean = spread_sum / games;
  const double standard_error =
      sqrt((spread_sum_of_squares / games - mean * mean) / games);
  const double pair_mean = pair_sum / pairs;
  const double pair_standard_error =
      sqrt((pair_sum_of_squares / pairs - pair_mean * pair_mean) / pairs);
  printf("paired: %d pairs, spread %+.2f +- %.2f\n", pairs, pair_mean,
         pair_standard_error);
  printf("per census move, chosen less static choice: expected opponent "
         "reply %+.3f, own score and leave %+.3f\n",
         reply_change_sum / census_moves, own_change_sum / census_moves);
  printf("points per game: census player %.2f (its seat in the static game "
         "%.2f), opponent %.2f (%.2f); opponent %.2f points a move\n",
         census_points / games, baseline_census_seat_points / games,
         opponent_points / games, baseline_opponent_seat_points / games,
         opponent_points / opponent_moves);
  printf("census one-ply vs static: %d games, spread %+.2f +- %.2f, wins "
         "%.1f%%, census moves %d (%.1f%% not the static choice), %.2f s "
         "valuing per census move, %.1f s wall\n",
         games, mean, standard_error, 100.0 * wins / games, census_moves,
         100.0 * changed_moves / census_moves, seconds_valuing / census_moves,
         ctimer_elapsed_seconds(&timer));
}

static Config *census_sim_test_config(void) {
  Config *config = config_create_or_die(
      "set -lex CSW24 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -threads 8");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0 -lex "
              "CSW24;");
  return config;
}

void test_census_sim(void) {
  Config *config = census_sim_test_config();
  test_census_sim_matches_sampling(config);
  test_census_sim_match(config);
  config_destroy(config);
}

void test_census_sim_time(void) {
  Config *config = census_sim_test_config();
  time_census_sim_turns(config);
  config_destroy(config);
}
