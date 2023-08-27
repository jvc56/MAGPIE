#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "autoplay.h"
#include "config.h"
#include "game.h"
#include "gameplay.h"
#include "infer.h"
#include "random.h"
#include "thread_control.h"

typedef struct AutoplayWorker {
  Config *config;
  ThreadControl *thread_control;
  AutoplayResults *autoplay_results;
  int max_games_for_worker;
} AutoplayWorker;

AutoplayWorker *create_autoplay_worker(Config *config,
                                       ThreadControl *thread_control,
                                       AutoplayResults *autoplay_results,
                                       int max_games_for_worker) {
  AutoplayWorker *autoplay_worker = malloc(sizeof(AutoplayWorker));
  autoplay_worker->config = config;
  autoplay_worker->thread_control = thread_control;
  autoplay_worker->autoplay_results = autoplay_results;
  autoplay_worker->max_games_for_worker = max_games_for_worker;
  return autoplay_worker;
}

void destroy_autoplay_worker(AutoplayWorker *autoplay_worker) {
  free(autoplay_worker);
}

AutoplayResults *create_autoplay_results() {
  AutoplayResults *autoplay_results = malloc(sizeof(AutoplayResults));
  pthread_mutex_init(&autoplay_results->update_results_mutex, NULL);
  autoplay_results->p1_wins = 0;
  autoplay_results->p1_losses = 0;
  autoplay_results->p1_ties = 0;
  autoplay_results->p1_firsts = 0;
  autoplay_results->p1_score = create_stat();
  autoplay_results->p2_score = create_stat();
  return autoplay_results;
}

void destroy_autoplay_results(AutoplayResults *autoplay_results) {
  destroy_stat(autoplay_results->p1_score);
  destroy_stat(autoplay_results->p2_score);
  free(autoplay_results);
}

void record_results(Game *game, int starting_player_index,
                    AutoplayResults *autoplay_results) {
  pthread_mutex_lock(&autoplay_results->update_results_mutex);
  if (game->players[0]->score > game->players[1]->score) {
    autoplay_results->p1_wins++;
  } else if (game->players[1]->score > game->players[0]->score) {
    autoplay_results->p1_losses++;
  } else {
    autoplay_results->p1_ties++;
  }
  if (starting_player_index == 0) {
    autoplay_results->p1_firsts++;
  }
  push(autoplay_results->p1_score, (double)game->players[0]->score, 1);
  push(autoplay_results->p2_score, (double)game->players[1]->score, 1);
  pthread_mutex_unlock(&autoplay_results->update_results_mutex);
}

void play_game(ThreadControl *thread_control, Game *game, time_t seed,
               AutoplayResults *autoplay_results, int starting_player_index) {
  reset_game(game);
  reseed_prng(game->gen->bag, seed);
  set_player_on_turn(game, starting_player_index);
  draw_at_most_to_rack(game->gen->bag, game->players[0]->rack, RACK_SIZE);
  draw_at_most_to_rack(game->gen->bag, game->players[1]->rack, RACK_SIZE);
  while (!game->game_end_reason) {
    generate_moves(game->gen, game->players[game->player_on_turn_index],
                   game->players[1 - game->player_on_turn_index]->rack,
                   game->gen->bag->last_tile_index + 1 >= RACK_SIZE);
    play_move(game, game->gen->move_list->moves[0]);
    reset_move_list(game->gen->move_list);
  }
  record_results(game, starting_player_index, autoplay_results);
}

void autoplay_worker(void *uncasted_autoplay_worker) {
  AutoplayWorker *autoplay_worker = (AutoplayWorker *)uncasted_autoplay_worker;

  Game *game = create_game(autoplay_worker->config);
  uint64_t seed;

  for (int i = 0; i < autoplay_worker->config->number_of_games_or_pairs; i++) {
    if (is_halted(autoplay_worker->thread_control)) {
      break;
    }
    seed = rand_uint64();
    int starting_player_index = i % 2;
    play_game(autoplay_worker->thread_control, game, seed,
              autoplay_worker->autoplay_results, starting_player_index);
    if (autoplay_worker->config->use_game_pairs) {
      set_player_on_turn(game, 1 - starting_player_index);
      play_game(autoplay_worker->thread_control, game, seed,
                autoplay_worker->autoplay_results, starting_player_index);
    }
  }

  destroy_game(game);
}

int get_number_of_games_for_worker(Config *config, int thread_index) {
  int number_of_games_for_worker =
      (config->number_of_games_or_pairs / config->number_of_threads);
  if (config->number_of_games_or_pairs % config->number_of_threads >
      thread_index) {
    number_of_games_for_worker++;
  }
  return number_of_games_for_worker;
}

void autoplay(Config *config, ThreadControl *thread_control) {
  AutoplayResults *autoplay_results = create_autoplay_results();

  seed_random(time(NULL));

  AutoplayWorker **autoplay_workers =
      malloc((sizeof(AutoplayWorker *)) * (config->number_of_threads));
  pthread_t *worker_ids =
      malloc((sizeof(pthread_t)) * (config->number_of_threads));
  for (int thread_index = 0; thread_index < config->number_of_threads;
       thread_index++) {

    autoplay_workers[thread_index] = create_autoplay_worker(
        config, thread_control, autoplay_results,
        get_number_of_games_for_worker(config, thread_index));

    pthread_create(&worker_ids[thread_index], NULL, autoplay_worker,
                   autoplay_workers[thread_index]);
  }

  for (int thread_index = 0; thread_index < config->number_of_threads;
       thread_index++) {
    pthread_join(worker_ids[thread_index], NULL);
    destroy_autoplay_worker(autoplay_workers[thread_index]);
  }

  // Destroy intrasim structs
  free(autoplay_workers);
  free(worker_ids);
}