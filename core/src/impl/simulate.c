#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

sim_status_t simulate(const Config *config, const Game *game, Simmer *simmer) {
  unhalt(config->thread_control);

  reset_move_list(game->gen->move_list);
  generate_moves(game->players[1 - game->player_on_turn_index]->rack, game->gen,
                 game->players[game->player_on_turn_index],
                 get_tiles_remaining(game->gen->bag) >= RACK_SIZE,
                 MOVE_RECORD_ALL, MOVE_SORT_EQUITY, true);
  int number_of_moves_generated = game->gen->move_list->count;
  sort_moves(game->gen->move_list);

  if (config->static_search_only) {
    print_ucgi_static_moves(game, config->num_plays, config->thread_control);
    return SIM_STATUS_SUCCESS;
  }

  // It is important that we first destroy the simmed plays
  // then set the new values for the simmer. The destructor
  // relies on the previous values of the simmer to properly
  // free everything.
  if (simmer->simmed_plays) {
    destroy_simmed_plays(simmer);
  }

  // Prepare the shared simmer attributes
  simmer->thread_control = config->thread_control;
  simmer->max_plies = config->plies;
  simmer->threads = config->thread_control->number_of_threads;
  simmer->seed = config->seed;
  simmer->max_iterations = config->max_iterations;
  simmer->stopping_condition = config->stopping_condition;

  simmer->num_simmed_plays = config->num_plays;
  if (number_of_moves_generated < simmer->num_simmed_plays) {
    simmer->num_simmed_plays = number_of_moves_generated;
  }
  simmer->iteration_count = 0;
  simmer->initial_player = game->player_on_turn_index;
  simmer->initial_spread = game->players[game->player_on_turn_index]->score -
                           game->players[1 - game->player_on_turn_index]->score;
  atomic_init(&simmer->node_count, 0);
  create_simmed_plays(game, simmer);

  // The letter distribution may have changed,
  // so we might need to recreate the rack.
  update_or_create_rack(&simmer->similar_plays_rack,
                        config->letter_distribution->size);

  if (simmer->num_simmed_plays > 0) {
    if (config->rack) {
      if (simmer->known_opp_rack) {
        destroy_rack(simmer->known_opp_rack);
      }
      simmer->known_opp_rack = rack_duplicate(config->rack);
    } else {
      simmer->known_opp_rack = NULL;
    }

    free(simmer->play_similarity_cache);
    simmer->play_similarity_cache =
        malloc_or_die(sizeof(int) * config->num_plays * config->num_plays);
    for (int i = 0; i < config->num_plays; i++) {
      for (int j = 0; j < config->num_plays; j++) {
        if (i == j) {
          simmer->play_similarity_cache[i * config->num_plays + j] =
              PLAYS_IDENTICAL;
        } else {
          simmer->play_similarity_cache[i * config->num_plays + j] =
              UNINITIALIZED_SIMILARITY;
        }
      }
    }

    SimmerWorker **simmer_workers = malloc_or_die(
        (sizeof(SimmerWorker *)) * (config->thread_control->number_of_threads));
    pthread_t *worker_ids = malloc_or_die(
        (sizeof(pthread_t)) * (config->thread_control->number_of_threads));

    clock_gettime(CLOCK_MONOTONIC, &config->thread_control->start_time);
    for (int thread_index = 0;
         thread_index < config->thread_control->number_of_threads;
         thread_index++) {
      simmer_workers[thread_index] =
          create_simmer_worker(game, simmer, thread_index);
      pthread_create(&worker_ids[thread_index], NULL, simmer_worker,
                     simmer_workers[thread_index]);
    }

    for (int thread_index = 0;
         thread_index < config->thread_control->number_of_threads;
         thread_index++) {
      pthread_join(worker_ids[thread_index], NULL);
      destroy_simmer_worker(simmer_workers[thread_index]);
    }

    // Destroy intrasim structs
    free(simmer_workers);
    free(worker_ids);
  }

  // Print out the stats
  print_ucgi_sim_stats(game, simmer, 1);
  return SIM_STATUS_SUCCESS;
}
