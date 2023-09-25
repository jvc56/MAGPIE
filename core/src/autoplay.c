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
#include "ucgi_print.h"

#include "../test/game_print.h"
#include "../test/move_print.h"

typedef struct AutoplayWorker {
  Config *config;
  ThreadControl *thread_control;
  AutoplayResults *autoplay_results;
  int max_games_for_worker;
  int worker_index;
} AutoplayWorker;

AutoplayResults *create_autoplay_results() {
  AutoplayResults *autoplay_results = malloc(sizeof(AutoplayResults));
  autoplay_results->total_games = 0;
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

AutoplayWorker *create_autoplay_worker(Config *config,
                                       ThreadControl *thread_control,
                                       int max_games_for_worker,
                                       int worker_index) {
  AutoplayWorker *autoplay_worker = malloc(sizeof(AutoplayWorker));
  autoplay_worker->config = config;
  autoplay_worker->thread_control = thread_control;
  autoplay_worker->max_games_for_worker = max_games_for_worker;
  autoplay_worker->worker_index = worker_index;
  autoplay_worker->autoplay_results = create_autoplay_results();
  return autoplay_worker;
}

void destroy_autoplay_worker(AutoplayWorker *autoplay_worker) {
  destroy_autoplay_results(autoplay_worker->autoplay_results);
  free(autoplay_worker);
}

void record_results(Game *game, int starting_player_index,
                    AutoplayResults *autoplay_results) {
  autoplay_results->total_games++;
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
}

void add_autoplay_results(AutoplayResults *autoplay_results_1,
                          AutoplayResults *autoplay_results_2) {
  // Stats are combined elsewhere
  autoplay_results_1->p1_firsts += autoplay_results_2->p1_firsts;
  autoplay_results_1->p1_wins += autoplay_results_2->p1_wins;
  autoplay_results_1->p1_losses += autoplay_results_2->p1_losses;
  autoplay_results_1->p1_ties += autoplay_results_2->p1_ties;
  autoplay_results_1->total_games += autoplay_results_2->total_games;
}

void play_game(Game *game, time_t seed, AutoplayResults *autoplay_results,
               int starting_player_index) {
  reseed_prng(game->gen->bag, seed);
  reset_game(game);
  set_player_on_turn(game, starting_player_index);
  draw_at_most_to_rack(game->gen->bag,
                       game->players[starting_player_index]->rack, RACK_SIZE);
  draw_at_most_to_rack(game->gen->bag,
                       game->players[1 - starting_player_index]->rack,
                       RACK_SIZE);
  while (!game->game_end_reason) {
    generate_moves(game->gen, game->players[game->player_on_turn_index],
                   game->players[1 - game->player_on_turn_index]->rack,
                   game->gen->bag->last_tile_index + 1 >= RACK_SIZE);
    play_move(game, game->gen->move_list->moves[0]);
    reset_move_list(game->gen->move_list);
  }
  record_results(game, starting_player_index, autoplay_results);
}

void *autoplay_worker(void *uncasted_autoplay_worker) {
  AutoplayWorker *autoplay_worker = (AutoplayWorker *)uncasted_autoplay_worker;

  Game *game_1 = create_game(autoplay_worker->config);
  Game *game_2 = create_game(autoplay_worker->config);
  uint64_t seed;

  int starting_player_for_thread = autoplay_worker->worker_index % 2;

  for (int i = 0; i < autoplay_worker->max_games_for_worker; i++) {
    if (is_halted(autoplay_worker->thread_control)) {
      break;
    }
    seed = rand_uint64();
    int starting_player_index = (i + starting_player_for_thread) % 2;
    play_game(game_1, seed, autoplay_worker->autoplay_results,
              starting_player_index);
    if (autoplay_worker->config->use_game_pairs) {
      play_game(game_2, seed, autoplay_worker->autoplay_results,
                1 - starting_player_index);
    }
  }

  destroy_game(game_1);
  destroy_game(game_2);
  return NULL;
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

void autoplay(ThreadControl *thread_control, AutoplayResults *autoplay_results,
              Config *config, uint64_t seed) {
  seed_random(seed);

  int saved_player_1_recorder_type =
      config->player_1_strategy_params->play_recorder_type;
  config->player_1_strategy_params->play_recorder_type =
      PLAY_RECORDER_TYPE_TOP_EQUITY;
  int saved_player_2_recorder_type =
      config->player_2_strategy_params->play_recorder_type;
  config->player_2_strategy_params->play_recorder_type =
      PLAY_RECORDER_TYPE_TOP_EQUITY;
  int save_move_list_capacity = config->move_list_capacity;
  config->move_list_capacity = 1;

  AutoplayWorker **autoplay_workers =
      malloc((sizeof(AutoplayWorker *)) * (config->number_of_threads));
  pthread_t *worker_ids =
      malloc((sizeof(pthread_t)) * (config->number_of_threads));
  for (int thread_index = 0; thread_index < config->number_of_threads;
       thread_index++) {

    int number_of_games_for_worker =
        get_number_of_games_for_worker(config, thread_index);

    autoplay_workers[thread_index] = create_autoplay_worker(
        config, thread_control, number_of_games_for_worker, thread_index);

    pthread_create(&worker_ids[thread_index], NULL, autoplay_worker,
                   autoplay_workers[thread_index]);
  }

  Stat **p1_score_stats =
      malloc((sizeof(Stat *)) * (config->number_of_threads));
  Stat **p2_score_stats =
      malloc((sizeof(Stat *)) * (config->number_of_threads));

  for (int thread_index = 0; thread_index < config->number_of_threads;
       thread_index++) {
    pthread_join(worker_ids[thread_index], NULL);
    add_autoplay_results(autoplay_results,
                         autoplay_workers[thread_index]->autoplay_results);
    p1_score_stats[thread_index] =
        autoplay_workers[thread_index]->autoplay_results->p1_score;
    p2_score_stats[thread_index] =
        autoplay_workers[thread_index]->autoplay_results->p2_score;
  }

  combine_stats(p1_score_stats, config->number_of_threads,
                autoplay_results->p1_score);
  free(p1_score_stats);

  combine_stats(p2_score_stats, config->number_of_threads,
                autoplay_results->p2_score);
  free(p2_score_stats);

  for (int thread_index = 0; thread_index < config->number_of_threads;
       thread_index++) {
    destroy_autoplay_worker(autoplay_workers[thread_index]);
  }

  // Destroy intrasim structs
  free(autoplay_workers);
  free(worker_ids);

  config->player_1_strategy_params->play_recorder_type =
      saved_player_1_recorder_type;
  config->player_2_strategy_params->play_recorder_type =
      saved_player_2_recorder_type;
  config->move_list_capacity = save_move_list_capacity;

  print_ucgi_autoplay_results(autoplay_results, thread_control);
}