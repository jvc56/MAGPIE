#include <stdlib.h>
#include <time.h>

#include "def/autoplay_defs.h"

#include "autoplay_results.h"
#include "config.h"
#include "game.h"
#include "gameplay.h"
#include "stats.h"
#include "../util/util.h"

typedef struct AutoplayWorker {
  const Config *config;
  AutoplayResults *autoplay_results;
  int max_games_for_worker;
  int worker_index;
  uint64_t seed;
} AutoplayWorker;

AutoplayWorker *create_autoplay_worker(const Config *config,
                                       int max_games_for_worker,
                                       int worker_index, uint64_t seed) {
  AutoplayWorker *autoplay_worker = malloc_or_die(sizeof(AutoplayWorker));
  autoplay_worker->config = config;
  autoplay_worker->max_games_for_worker = max_games_for_worker;
  autoplay_worker->worker_index = worker_index;
  autoplay_worker->seed = seed;
  autoplay_worker->autoplay_results = create_autoplay_results();
  return autoplay_worker;
}

void destroy_autoplay_worker(AutoplayWorker *autoplay_worker) {
  destroy_autoplay_results(autoplay_worker->autoplay_results);
  free(autoplay_worker);
}

void record_results(const Game *game, AutoplayResults *autoplay_results,
                    int starting_player_index) {

  increment_total_games(autoplay_results);
  if (game->players[0]->score > game->players[1]->score) {
    increment_p1_wins(autoplay_results);
  } else if (game->players[1]->score > game->players[0]->score) {
    increment_p1_losses(autoplay_results);
  } else {
    increment_p1_ties(autoplay_results);
  }
  if (starting_player_index == 0) {
    increment_p1_firsts(autoplay_results);
  }
  increment_p1_score(autoplay_results, game->players[0]->score);
  increment_p2_score(autoplay_results, game->players[1]->score);
}

void play_autoplay_game(Game *game, AutoplayResults *autoplay_results,
                        int starting_player_index) {
  reset_game(game);
  set_starting_player_index(game, starting_player_index);
  draw_starting_racks(game);
  while (game->game_end_reason == GAME_END_REASON_NONE) {
    generate_moves(
        game->players[1 - game->player_on_turn_index]->rack, game->gen,
        game->players[game->player_on_turn_index],
        get_tiles_remaining(game->gen->bag) >= RACK_SIZE, MOVE_RECORD_BEST,
        game->players[game->player_on_turn_index]->move_sort_type, true);
    play_move(game->gen->move_list->moves[0], game);
    reset_move_list(game->gen->move_list);
  }
  record_results(game, autoplay_results, starting_player_index);
}

void *autoplay_worker(void *uncasted_autoplay_worker) {
  AutoplayWorker *autoplay_worker = (AutoplayWorker *)uncasted_autoplay_worker;
  ThreadControl *thread_control = autoplay_worker->config->thread_control;
  Game *game = create_game(autoplay_worker->config);

  // Declare local vars for autoplay_worker fields for convenience
  bool use_game_pairs = autoplay_worker->config->use_game_pairs;
  int worker_index = autoplay_worker->worker_index;
  int starting_player_for_thread = worker_index % 2;

  Bag *game_pair_bag;
  if (use_game_pairs) {
    // Create a Bag to save the PRNG state of the game
    // to use for game pairs. The initial seed does
    // not matter since it will be overwritten before
    // the first game of the pair is played.
    game_pair_bag = create_bag(game->gen->letter_distribution);
  }
  seed_bag_for_worker(game->gen->bag, autoplay_worker->seed, worker_index);

  for (int i = 0; i < autoplay_worker->max_games_for_worker; i++) {
    if (is_halted(thread_control)) {
      break;
    }
    int starting_player_index = (i + starting_player_for_thread) % 2;

    // If we are using game pairs, we have to save the state of the
    // Bag PRNG before playing the first game so the state can be
    // reloaded before playing the second game of the pair, ensuring
    // both games are played with an identical Bag PRNG.
    if (use_game_pairs) {
      bag_copy(game_pair_bag, game->gen->bag);
    }

    play_autoplay_game(game, autoplay_worker->autoplay_results,
                       starting_player_index);
    if (use_game_pairs) {
      bag_copy(game->gen->bag, game_pair_bag);
      play_autoplay_game(game, autoplay_worker->autoplay_results,
                         1 - starting_player_index);
    }
  }

  if (use_game_pairs) {
    destroy_bag(game_pair_bag);
  }
  destroy_game(game);
  return NULL;
}

int get_number_of_games_for_worker(int max_iterations, int number_of_threads,
                                   int thread_index) {
  int number_of_games_for_worker = (max_iterations / number_of_threads);
  if (max_iterations % number_of_threads > thread_index) {
    number_of_games_for_worker++;
  }
  return number_of_games_for_worker;
}

autoplay_status_t autoplay(const Config *config,
                           AutoplayResults *autoplay_results) {
  unhalt(config->thread_control);
  reset_autoplay_results(autoplay_results);

  int number_of_threads = config->thread_control->number_of_threads;
  AutoplayWorker **autoplay_workers =
      malloc_or_die((sizeof(AutoplayWorker *)) * (number_of_threads));
  pthread_t *worker_ids =
      malloc_or_die((sizeof(pthread_t)) * (number_of_threads));
  for (int thread_index = 0; thread_index < number_of_threads; thread_index++) {

    int number_of_games_for_worker = get_number_of_games_for_worker(
        config->max_iterations, number_of_threads, thread_index);

    autoplay_workers[thread_index] = create_autoplay_worker(
        config, number_of_games_for_worker, thread_index, config->seed);

    pthread_create(&worker_ids[thread_index], NULL, autoplay_worker,
                   autoplay_workers[thread_index]);
  }

  Stat **p1_score_stats = malloc_or_die((sizeof(Stat *)) * (number_of_threads));
  Stat **p2_score_stats = malloc_or_die((sizeof(Stat *)) * (number_of_threads));

  for (int thread_index = 0; thread_index < number_of_threads; thread_index++) {
    pthread_join(worker_ids[thread_index], NULL);
    add_autoplay_results(autoplay_workers[thread_index]->autoplay_results,
                         autoplay_results);
    p1_score_stats[thread_index] =
        get_p1_score(autoplay_workers[thread_index]->autoplay_results);
    p2_score_stats[thread_index] =
        get_p2_score(autoplay_workers[thread_index]->autoplay_results);
  }

  // If autoplay was interrupted by the user,
  // this will not change the status.
  halt(config->thread_control, HALT_STATUS_MAX_ITERATIONS);

  combine_stats(p1_score_stats, number_of_threads,
                get_p1_score(autoplay_results));
  free(p1_score_stats);

  combine_stats(p2_score_stats, number_of_threads,
                get_p2_score(autoplay_results));
  free(p2_score_stats);

  for (int thread_index = 0; thread_index < number_of_threads; thread_index++) {
    destroy_autoplay_worker(autoplay_workers[thread_index]);
  }

  // Destroy intrasim structs
  free(autoplay_workers);
  free(worker_ids);

  print_ucgi_autoplay_results(autoplay_results, config->thread_control);
  return AUTOPLAY_STATUS_SUCCESS;
}
