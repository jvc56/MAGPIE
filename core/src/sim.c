#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "gameplay.h"
#include "log.h"
#include "rack.h"
#include "sim.h"
#include "stats.h"
#include "ucgi_formats.h"
#include "ucgi_print.h"
#include "util.h"
#include "xoshiro.h"

#define MAX_STOPPING_ITERATION_CT 4000
#define PER_PLY_STOPPING_SCALING 1250
#define SIMILAR_PLAYS_ITER_CUTOFF 1000

Simmer *create_simmer(const Config *config) {
  Simmer *simmer = malloc_or_die(sizeof(Simmer));
  simmer->threads = config->number_of_threads;
  simmer->win_pcts = config->win_pcts;
  simmer->max_iterations = 0;
  simmer->stopping_condition = SIM_STOPPING_CONDITION_NONE;
  simmer->simmed_plays = NULL;
  simmer->known_opp_rack = NULL;
  simmer->play_similarity_cache = NULL;
  simmer->num_simmed_plays = 0;
  simmer->similar_plays_rack = NULL;
  pthread_mutex_init(&simmer->iteration_count_mutex, NULL);
  return simmer;
}

void create_simmed_plays(Simmer *simmer, Game *game) {
  simmer->simmed_plays =
      malloc_or_die((sizeof(SimmedPlay)) * simmer->num_simmed_plays);
  for (int i = 0; i < simmer->num_simmed_plays; i++) {
    SimmedPlay *sp = malloc_or_die(sizeof(SimmedPlay));
    sp->move = create_move();
    copy_move(game->gen->move_list->moves[i], sp->move);

    sp->score_stat = malloc_or_die(sizeof(Stat *) * simmer->max_plies);
    sp->bingo_stat = malloc_or_die(sizeof(Stat *) * simmer->max_plies);
    sp->equity_stat = create_stat();
    sp->leftover_stat = create_stat();
    sp->win_pct_stat = create_stat();
    for (int j = 0; j < simmer->max_plies; j++) {
      sp->score_stat[j] = create_stat();
      sp->bingo_stat[j] = create_stat();
    }
    sp->ignore = 0;
    sp->play_id = i;
    pthread_mutex_init(&sp->mutex, NULL);
    simmer->simmed_plays[i] = sp;
  }
  pthread_mutex_init(&simmer->simmed_plays_mutex, NULL);
}

// destructors

void destroy_simmed_plays(Simmer *simmer) {
  for (int i = 0; i < simmer->num_simmed_plays; i++) {
    for (int j = 0; j < simmer->max_plies; j++) {
      destroy_stat(simmer->simmed_plays[i]->bingo_stat[j]);
      destroy_stat(simmer->simmed_plays[i]->score_stat[j]);
    }
    free(simmer->simmed_plays[i]->bingo_stat);
    free(simmer->simmed_plays[i]->score_stat);
    destroy_stat(simmer->simmed_plays[i]->equity_stat);
    destroy_stat(simmer->simmed_plays[i]->leftover_stat);
    destroy_stat(simmer->simmed_plays[i]->win_pct_stat);
    destroy_move(simmer->simmed_plays[i]->move);
    pthread_mutex_destroy(&simmer->simmed_plays[i]->mutex);
    free(simmer->simmed_plays[i]);
  }
  free(simmer->simmed_plays);
  // Use defensive style to catch bugs earlier.
  simmer->simmed_plays = NULL;
}

void destroy_simmer(Simmer *simmer) {
  if (simmer->simmed_plays) {
    destroy_simmed_plays(simmer);
  }
  if (simmer->similar_plays_rack) {
    destroy_rack(simmer->similar_plays_rack);
  }

  if (simmer->known_opp_rack) {
    destroy_rack(simmer->known_opp_rack);
  }

  if (simmer->play_similarity_cache) {
    free(simmer->play_similarity_cache);
  }

  free(simmer);
}

SimmerWorker *create_simmer_worker(Simmer *simmer, Game *game,
                                   int worker_index) {
  SimmerWorker *simmer_worker = malloc_or_die(sizeof(SimmerWorker));

  simmer_worker->simmer = simmer;
  simmer_worker->thread_index = worker_index;
  uint64_t seed = time(NULL);
  Game *new_game = copy_game(game, 1);
  simmer_worker->game = new_game;
  set_backup_mode(new_game, BACKUP_MODE_SIMULATION);
  for (int j = 0; j < 2; j++) {
    // Simmer only needs to record top equity plays:
    new_game->players[j]->move_record_type = MOVE_RECORD_BEST;
  }

  simmer_worker->rack_placeholder =
      create_rack(game->gen->letter_distribution->size);
  // Give each game bag the same seed, but then change these:
  seed_prng(new_game->gen->bag->prng, seed);
  // "jump" each bag's prng thread number of times.
  for (int j = 0; j < worker_index; j++) {
    xoshiro_jump(new_game->gen->bag->prng);
  }

  return simmer_worker;
}

void destroy_simmer_worker(SimmerWorker *simmer_worker) {
  destroy_game(simmer_worker->game);
  destroy_rack(simmer_worker->rack_placeholder);
  free(simmer_worker);
}

int is_multithreaded(Simmer *simmer) { return simmer->threads > 1; }

void add_score_stat(SimmedPlay *sp, int score, int is_bingo, int ply,
                    int lock) {
  if (lock) {
    pthread_mutex_lock(&sp->mutex);
  }
  push(sp->score_stat[ply], (double)score, 1);
  push(sp->bingo_stat[ply], (double)is_bingo, 1);
  if (lock) {
    pthread_mutex_unlock(&sp->mutex);
  }
}

void add_equity_stat(SimmedPlay *sp, int initial_spread, int spread,
                     float leftover, int lock) {
  if (lock) {
    pthread_mutex_lock(&sp->mutex);
  }
  push(sp->equity_stat, (double)(spread - initial_spread) + (double)(leftover),
       1);
  push(sp->leftover_stat, (double)leftover, 1);
  if (lock) {
    pthread_mutex_unlock(&sp->mutex);
  }
}

void add_winpct_stat(SimmedPlay *sp, WinPct *wp, int spread, float leftover,
                     int game_end_reason, int tiles_unseen, int plies_are_odd,
                     int lock) {

  double wpct = 0.0;
  if (game_end_reason != GAME_END_REASON_NONE) {
    // the game ended; use the actual result.
    if (spread == 0) {
      wpct = 0.5;
    } else if (spread > 0) {
      wpct = 1.0;
    }
  } else {
    int spread_plus_leftover = spread + round_to_nearest_int((double)leftover);
    // for an even-ply sim, it is our opponent's turn at the end of the sim.
    // the table is calculated from our perspective, so flip the spread.
    // i.e. if we are winning by 20 pts at the end of the sim, and our opponent
    // is on turn, we want to look up -20 as the spread, and then flip the win %
    // as well.
    if (!plies_are_odd) {
      spread_plus_leftover = -spread_plus_leftover;
    }
    wpct = (double)win_pct(wp, spread_plus_leftover, tiles_unseen);
    if (!plies_are_odd) {
      // see above comment regarding flipping win%
      wpct = 1.0 - wpct;
    }
  }
  if (lock) {
    pthread_mutex_lock(&sp->mutex);
  }
  push(sp->win_pct_stat, wpct, 1);
  if (lock) {
    pthread_mutex_unlock(&sp->mutex);
  }
}

void ignore_play(SimmedPlay *sp, int lock) {
  if (lock) {
    pthread_mutex_lock(&sp->mutex);
  }
  sp->ignore = 1;
  if (lock) {
    pthread_mutex_unlock(&sp->mutex);
  }
}

int handle_potential_stopping_condition(Simmer *simmer) {
  pthread_mutex_lock(&simmer->simmed_plays_mutex);
  sort_plays_by_win_rate(simmer->simmed_plays, simmer->num_simmed_plays);

  double zval = 0;
  switch (simmer->stopping_condition) {
  case SIM_STOPPING_CONDITION_95PCT:
    zval = STATS_Z95;
    break;
  case SIM_STOPPING_CONDITION_98PCT:
    zval = STATS_Z98;
    break;
  case SIM_STOPPING_CONDITION_99PCT:
    zval = STATS_Z99;
    break;
  }

  SimmedPlay *tentative_winner = simmer->simmed_plays[0];
  double mu = tentative_winner->win_pct_stat->mean;
  double stderr = get_standard_error(tentative_winner->win_pct_stat, zval);
  int total_ignored = 0;
  for (int i = 1; i < simmer->num_simmed_plays; i++) {
    if (simmer->simmed_plays[i]->ignore) {
      total_ignored++;
      continue;
    }
    double mu_i = simmer->simmed_plays[i]->win_pct_stat->mean;
    double stderr_i =
        get_standard_error(simmer->simmed_plays[i]->win_pct_stat, zval);

    if ((mu - stderr) > (mu_i + stderr_i)) {
      ignore_play(simmer->simmed_plays[i], is_multithreaded(simmer));
      total_ignored++;
    } else if (simmer->iteration_count > SIMILAR_PLAYS_ITER_CUTOFF) {
      if (plays_are_similar(simmer, tentative_winner,
                            simmer->simmed_plays[i])) {
        ignore_play(simmer->simmed_plays[i], is_multithreaded(simmer));
        total_ignored++;
      }
    }
  }

  pthread_mutex_unlock(&simmer->simmed_plays_mutex);
  log_debug("total ignored: %d\n", total_ignored);
  if (total_ignored >= simmer->num_simmed_plays - 1) {
    // if there is only 1 unignored play, exit.
    // printf("Only one unignored play, we should stop simming.\n");
    return 1;
  }
  return 0;
}

void sim_single_iteration(SimmerWorker *simmer_worker) {
  Game *game = simmer_worker->game;
  Rack *rack_placeholder = simmer_worker->rack_placeholder;
  Simmer *simmer = simmer_worker->simmer;
  int plies = simmer->max_plies;

  // set random rack for opponent (throw in rack, shuffle, draw new tiles).
  set_random_rack(game, 1 - game->player_on_turn_index, simmer->known_opp_rack);
  // need a new shuffle for every iteration:
  shuffle(game->gen->bag);

  for (int i = 0; i < simmer->num_simmed_plays; i++) {
    if (simmer->simmed_plays[i]->ignore) {
      continue;
    }

    double leftover = 0.0;
    set_backup_mode(game, BACKUP_MODE_SIMULATION);
    // play move
    play_move(game, simmer->simmed_plays[i]->move);
    atomic_fetch_add(&simmer->node_count, 1);
    set_backup_mode(game, BACKUP_MODE_OFF);
    // further plies will NOT be backed up.
    for (int ply = 0; ply < plies; ply++) {
      int onturn = game->player_on_turn_index;
      if (game->game_end_reason != GAME_END_REASON_NONE) {
        // game is over.
        break;
      }

      Move *best_play = get_top_equity_move(game);
      copy_rack_into(rack_placeholder, game->players[onturn]->rack);
      play_move(game, best_play);
      atomic_fetch_add(&simmer->node_count, 1);
      if (ply == plies - 2 || ply == plies - 1) {
        double this_leftover = get_leave_value_for_move(
            game->players[0]->klv, best_play, rack_placeholder);
        if (onturn == simmer->initial_player) {
          leftover += this_leftover;
        } else {
          leftover -= this_leftover;
        }
      }
      add_score_stat(simmer->simmed_plays[i], best_play->score,
                     best_play->tiles_played == 7, ply,
                     is_multithreaded(simmer));
    }

    int spread = game->players[simmer->initial_player]->score -
                 game->players[1 - simmer->initial_player]->score;
    add_equity_stat(simmer->simmed_plays[i], simmer->initial_spread, spread,
                    leftover, is_multithreaded(simmer));
    add_winpct_stat(
        simmer->simmed_plays[i], simmer->win_pcts, spread, leftover,
        game->game_end_reason,
        // number of tiles unseen to us: bag tiles + tiles on opp rack.
        game->gen->bag->last_tile_index + 1 +
            game->players[1 - simmer->initial_player]->rack->number_of_letters,
        plies % 2, is_multithreaded(simmer));
    // reset to first state. we only need to restore one backup.
    unplay_last_move(game);
  }
}

void *simmer_worker(void *uncasted_simmer_worker) {
  SimmerWorker *simmer_worker = (SimmerWorker *)uncasted_simmer_worker;
  Simmer *simmer = simmer_worker->simmer;
  ThreadControl *thread_control = simmer->thread_control;
  while (!is_halted(thread_control)) {
    int current_iteration_count;
    int reached_max_iteration = 0;
    if (is_multithreaded(simmer)) {
      pthread_mutex_lock(&simmer->iteration_count_mutex);
    }
    if (simmer->iteration_count == simmer->max_iterations) {
      reached_max_iteration = 1;
    } else {
      simmer->iteration_count++;
      current_iteration_count = simmer->iteration_count;
    }
    if (is_multithreaded(simmer)) {
      pthread_mutex_unlock(&simmer->iteration_count_mutex);
    }

    if (reached_max_iteration) {
      halt(thread_control, HALT_STATUS_MAX_ITERATIONS);
      break;
    }
    sim_single_iteration(simmer_worker);

    if (thread_control->print_info_interval > 0 &&
        current_iteration_count > 0 &&
        current_iteration_count % thread_control->print_info_interval == 0) {
      print_ucgi_sim_stats(simmer, simmer_worker->game, 0);
    }

    if (thread_control->check_stopping_condition_interval > 0 &&
        current_iteration_count %
                thread_control->check_stopping_condition_interval ==
            0 &&
        set_check_stop_active(thread_control)) {
      if (!is_halted(thread_control) &&
          handle_potential_stopping_condition(simmer)) {
        halt(thread_control, HALT_STATUS_PROBABILISTIC);
      }
      set_check_stop_inactive(thread_control);
    }
  }
  log_trace("thread %d exiting", simmer_worker->thread_index);
  return NULL;
}

int plays_are_similar(Simmer *simmer, SimmedPlay *m1, SimmedPlay *m2) {
  // look in the cache first
  int cache_value =
      simmer->play_similarity_cache[m1->play_id +
                                    simmer->num_simmed_plays * m2->play_id];
  assert(cache_value != PLAYS_IDENTICAL);
  if (cache_value == PLAYS_SIMILAR) {
    return 1;
  } else if (cache_value == PLAYS_NOT_SIMILAR) {
    return 0;
  }
  int cache_index1 = m1->play_id + simmer->num_simmed_plays * m2->play_id;
  int cache_index2 = m2->play_id + simmer->num_simmed_plays * m1->play_id;

  if (m1->move->move_type != m2->move->move_type) {
    simmer->play_similarity_cache[cache_index1] = PLAYS_NOT_SIMILAR;
    simmer->play_similarity_cache[cache_index2] = PLAYS_NOT_SIMILAR;
    return 0;
  }

  // Otherwise, we must compute play similarity and fill in the cache.
  // two plays are "similar" if they use the same tiles, and they start at
  // the same square.
  if (!(m1->move->vertical == m2->move->vertical &&
        m1->move->col_start == m2->move->col_start &&
        m1->move->row_start == m2->move->row_start)) {

    simmer->play_similarity_cache[cache_index1] = PLAYS_NOT_SIMILAR;
    simmer->play_similarity_cache[cache_index2] = PLAYS_NOT_SIMILAR;
    return 0;
  }
  if (!(m1->move->tiles_played == m2->move->tiles_played &&
        m1->move->tiles_length == m2->move->tiles_length)) {
    simmer->play_similarity_cache[cache_index1] = PLAYS_NOT_SIMILAR;
    simmer->play_similarity_cache[cache_index2] = PLAYS_NOT_SIMILAR;
    return 0;
  }

  // Create a rack from m1, then subtract the rack from m2. The final
  // rack should have all zeroes.
  reset_rack(simmer->similar_plays_rack);
  for (int i = 0; i < m1->move->tiles_length; i++) {
    if (m1->move->tiles[i] == PLAYED_THROUGH_MARKER) {
      continue;
    }
    int ml = m1->move->tiles[i];
    if (is_blanked(ml)) {
      ml = 0;
    }
    simmer->similar_plays_rack->array[ml]++;
  }

  for (int i = 0; i < m2->move->tiles_length; i++) {
    if (m2->move->tiles[i] == PLAYED_THROUGH_MARKER) {
      continue;
    }
    int ml = m2->move->tiles[i];
    if (is_blanked(ml)) {
      ml = 0;
    }
    simmer->similar_plays_rack->array[ml]--;
  }

  for (int i = 0; i < simmer->similar_plays_rack->array_size; i++) {
    if (simmer->similar_plays_rack->array[i] != 0) {
      simmer->play_similarity_cache[cache_index1] = PLAYS_NOT_SIMILAR;
      simmer->play_similarity_cache[cache_index2] = PLAYS_NOT_SIMILAR;
      return 0;
    }
  }
  simmer->play_similarity_cache[cache_index1] = PLAYS_SIMILAR;
  simmer->play_similarity_cache[cache_index2] = PLAYS_SIMILAR;

  return 1;
}

int compare_simmed_plays(const void *a, const void *b) {
  const SimmedPlay *play_a = *(const SimmedPlay **)a;
  const SimmedPlay *play_b = *(const SimmedPlay **)b;

  if (play_a->ignore && !play_b->ignore) {
    return 1;
  } else if (play_b->ignore && !play_a->ignore) {
    return -1;
  }

  // Compare the mean values of win_pct_stat
  double mean_a = play_a->win_pct_stat->mean;
  double mean_b = play_b->win_pct_stat->mean;

  if (mean_a > mean_b) {
    return -1;
  } else if (mean_a < mean_b) {
    return 1;
  } else {
    // If win_pct_stat->mean values are equal, compare equity_stat->mean
    double equity_mean_a = play_a->equity_stat->mean;
    double equity_mean_b = play_b->equity_stat->mean;

    if (equity_mean_a > equity_mean_b) {
      return -1;
    } else if (equity_mean_a < equity_mean_b) {
      return 1;
    } else {
      return 0;
    }
  }
}

void sort_plays_by_win_rate(SimmedPlay **simmed_plays, int num_simmed_plays) {
  qsort(simmed_plays, num_simmed_plays, sizeof(SimmedPlay *),
        compare_simmed_plays);
}

sim_status_t simulate(const Config *config, ThreadControl *thread_control,
                      Simmer *simmer, Game *game) {
  generate_moves(game->gen, game->players[game->player_on_turn_index],
                 game->players[1 - game->player_on_turn_index]->rack,
                 game->gen->bag->last_tile_index + 1 >= RACK_SIZE,
                 MOVE_RECORD_ALL, MOVE_SORT_EQUITY, true);
  int number_of_moves_generated = game->gen->move_list->count;
  sort_moves(game->gen->move_list);

  if (config->static_search_only) {
    print_ucgi_static_moves(game, config->num_plays, thread_control);
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
  simmer->thread_control = thread_control;
  simmer->max_plies = config->plies;
  simmer->threads = config->number_of_threads;
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
  create_simmed_plays(simmer, game);

  // The letter distribution may have changed,
  // so we might need to recreate the rack.
  update_or_create_rack(&simmer->similar_plays_rack,
                        config->letter_distribution->size);

  if (simmer->num_simmed_plays) {
    if (config->rack) {
      if (simmer->known_opp_rack) {
        destroy_rack(simmer->known_opp_rack);
      }
      simmer->known_opp_rack = copy_rack(config->rack);
    } else {
      simmer->known_opp_rack = NULL;
    }

    if (simmer->play_similarity_cache) {
      free(simmer->play_similarity_cache);
    }
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

    SimmerWorker **simmer_workers =
        malloc_or_die((sizeof(SimmerWorker *)) * (config->number_of_threads));
    pthread_t *worker_ids =
        malloc_or_die((sizeof(pthread_t)) * (config->number_of_threads));

    clock_gettime(CLOCK_MONOTONIC, &thread_control->start_time);
    for (int thread_index = 0; thread_index < config->number_of_threads;
         thread_index++) {
      simmer_workers[thread_index] =
          create_simmer_worker(simmer, game, thread_index);
      pthread_create(&worker_ids[thread_index], NULL, simmer_worker,
                     simmer_workers[thread_index]);
    }

    for (int thread_index = 0; thread_index < config->number_of_threads;
         thread_index++) {
      pthread_join(worker_ids[thread_index], NULL);
      destroy_simmer_worker(simmer_workers[thread_index]);
    }

    // Destroy intrasim structs
    free(simmer_workers);
    free(worker_ids);
  }

  // Print out the stats
  print_ucgi_sim_stats(simmer, game, 1);
  return SIM_STATUS_SUCCESS;
}
