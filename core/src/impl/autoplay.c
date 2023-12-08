#include <pthread.h>
#include <stdlib.h>
#include <time.h>

#include "../str/autoplay_string.h"

#include "../def/autoplay_defs.h"
#include "../def/rack_defs.h"

#include "../ent/autoplay_results.h"
#include "../ent/config.h"
#include "../ent/game.h"
#include "../ent/movegen.h"
#include "../ent/player.h"
#include "../ent/stats.h"

#include "../impl/gameplay.h"

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

void record_results(Game *game, AutoplayResults *autoplay_results,
                    int starting_player_index) {

  int p0_score = player_get_score(game_get_player(game, 0));
  int p1_score = player_get_score(game_get_player(game, 1));

  increment_total_games(autoplay_results);
  if (p0_score > p1_score) {
    increment_p1_wins(autoplay_results);
  } else if (p1_score > p0_score) {
    increment_p1_losses(autoplay_results);
  } else {
    increment_p1_ties(autoplay_results);
  }
  if (starting_player_index == 0) {
    increment_p1_firsts(autoplay_results);
  }
  increment_p1_score(autoplay_results, p0_score);
  increment_p2_score(autoplay_results, p1_score);
}

void play_autoplay_game(Game *game, AutoplayResults *autoplay_results,
                        int starting_player_index) {
  game_end_reason_t game_end_reason = game_get_game_end_reason(game);
  int player_on_turn_index = game_get_player_on_turn_index(game);
  Player *player_on_turn = game_get_player(game, player_on_turn_index);
  Player *opponent = game_get_player(game, 1 - player_on_turn_index);
  Generator *gen = game_get_gen(game);
  Bag *bag = gen_get_bag(gen);
  MoveList *move_list = gen_get_move_list(gen);

  reset_game(game);
  set_starting_player_index(game, starting_player_index);
  draw_starting_racks(game);
  while (game_end_reason == GAME_END_REASON_NONE) {
    generate_moves(player_get_rack(opponent), gen, player_on_turn,
                   get_tiles_remaining(bag) >= RACK_SIZE, MOVE_RECORD_BEST,
                   player_get_move_sort_type(player_on_turn), true);
    play_move(move_list_get_move(move_list, 0), game);
    reset_move_list(move_list);
  }
  record_results(game, autoplay_results, starting_player_index);
}

void *autoplay_worker(void *uncasted_autoplay_worker) {
  AutoplayWorker *autoplay_worker = (AutoplayWorker *)uncasted_autoplay_worker;
  ThreadControl *thread_control =
      config_get_thread_control(autoplay_worker->config);
  Game *game = create_game(autoplay_worker->config);
  Generator *gen = game_get_gen(game);
  Bag *bag = gen_get_bag(gen);

  // Declare local vars for autoplay_worker fields for convenience
  bool use_game_pairs = config_get_use_game_pairs(autoplay_worker->config);
  int worker_index = autoplay_worker->worker_index;
  int starting_player_for_thread = worker_index % 2;

  Bag *game_pair_bag;
  if (use_game_pairs) {
    // Create a Bag to save the PRNG state of the game
    // to use for game pairs. The initial seed does
    // not matter since it will be overwritten before
    // the first game of the pair is played.
    game_pair_bag = create_bag(gen_get_ld(gen));
  }
  seed_bag_for_worker(bag, autoplay_worker->seed, worker_index);

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
      bag_copy(game_pair_bag, bag);
    }

    play_autoplay_game(game, autoplay_worker->autoplay_results,
                       starting_player_index);
    if (use_game_pairs) {
      bag_copy(bag, game_pair_bag);
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
  ThreadControl *thread_control = config_get_thread_control(config);
  unhalt(thread_control);
  reset_autoplay_results(autoplay_results);

  int number_of_threads = get_number_of_threads(thread_control);
  AutoplayWorker **autoplay_workers =
      malloc_or_die((sizeof(AutoplayWorker *)) * (number_of_threads));
  pthread_t *worker_ids =
      malloc_or_die((sizeof(pthread_t)) * (number_of_threads));
  for (int thread_index = 0; thread_index < number_of_threads; thread_index++) {

    int number_of_games_for_worker = get_number_of_games_for_worker(
        config_get_max_iterations(config), number_of_threads, thread_index);

    autoplay_workers[thread_index] =
        create_autoplay_worker(config, number_of_games_for_worker, thread_index,
                               config_get_seed(config));

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
  halt(thread_control, HALT_STATUS_MAX_ITERATIONS);

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

  print_ucgi_autoplay_results(autoplay_results, thread_control);
  return AUTOPLAY_STATUS_SUCCESS;
}
