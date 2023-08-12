#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "gameplay.h"
#include "go_params.h"
#include "log.h"
#include "rack.h"
#include "sim.h"
#include "stats.h"
#include "ucgi_formats.h"
#include "util.h"
#include "xoshiro.h"

#define MAX_STOPPING_ITERATION_CT 4000
#define PER_PLY_STOPPING_SCALING 1250
#define SIMILAR_PLAYS_ITER_CUTOFF 1000

Simmer *create_simmer(Config *config, FILE *outfile) {
  Simmer *simmer = malloc(sizeof(Simmer));
  simmer->threads = config->number_of_threads;
  simmer->win_pcts = config->win_pcts;
  simmer->max_iterations = 0;
  simmer->stopping_condition = SIM_STOPPING_CONDITION_NONE;
  simmer->simmed_plays = NULL;
  simmer->known_opp_rack = NULL;
  simmer->play_similarity_cache = NULL;
  simmer->num_simmed_plays = 0;
  pthread_mutex_init(&simmer->iteration_count_mutex, NULL);
  simmer->similar_plays_rack = create_rack(config->letter_distribution->size);
  if (outfile == NULL) {
    simmer->outfile = stdout;
  } else {
    simmer->outfile = outfile;
  }
  return simmer;
}

SimmerWorker *create_simmer_worker(Simmer *simmer, Game *game,
                                   int worker_index) {
  SimmerWorker *simmer_worker = malloc(sizeof(SimmerWorker));

  simmer_worker->simmer = simmer;
  simmer_worker->thread_index = worker_index;
  uint64_t seed = time(NULL);
  Game *new_game = copy_game(game, 1);
  simmer_worker->game = new_game;
  set_backup_mode(new_game, BACKUP_MODE_SIMULATION);
  for (int j = 0; j < 2; j++) {
    // Simmer only needs to record top equity plays:
    new_game->players[j]->strategy_params->play_recorder_type =
        PLAY_RECORDER_TYPE_TOP_EQUITY;
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
  if (simmer->num_simmed_plays < 2) {
    return 1; // should stop
  }
  if (simmer->iteration_count >
      (MAX_STOPPING_ITERATION_CT +
       (simmer->max_plies * PER_PLY_STOPPING_SCALING))) {
    return 1;
  }
  int ignored_plays = 0;
  for (int i = 0; i < simmer->num_simmed_plays; i++) {
    if (simmer->simmed_plays[i]->ignore) {
      ignored_plays++;
    }
  }
  if (ignored_plays >= simmer->num_simmed_plays - 1) {
    // There is only one unignored play; exit.
    return 1;
  }
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
  int new_ignored = 0;
  for (int i = 1; i < simmer->num_simmed_plays; i++) {
    if (simmer->simmed_plays[i]->ignore) {
      continue;
    }
    double mu_i = simmer->simmed_plays[i]->win_pct_stat->mean;
    double stderr_i =
        get_standard_error(simmer->simmed_plays[i]->win_pct_stat, zval);

    if ((mu - stderr) > (mu_i + stderr_i)) {
      ignore_play(simmer->simmed_plays[i], simmer->threads);
      new_ignored++;
    } else if (simmer->iteration_count > SIMILAR_PLAYS_ITER_CUTOFF) {
      if (plays_are_similar(simmer, tentative_winner,
                            simmer->simmed_plays[i])) {
        ignore_play(simmer->simmed_plays[i], simmer->threads);
        new_ignored++;
      }
    }
  }
  pthread_mutex_unlock(&simmer->simmed_plays_mutex);

  if (ignored_plays + new_ignored >= simmer->num_simmed_plays - 1) {
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
      char placeholder[80];
      store_move_description(best_play, placeholder,
                             game->gen->letter_distribution);

      if (ply == plies - 2 || ply == plies - 1) {
        double this_leftover =
            get_leave_value_for_move(game->players[0]->strategy_params->klv,
                                     best_play, rack_placeholder);
        if (onturn == simmer->initial_player) {
          leftover += this_leftover;
        } else {
          leftover -= this_leftover;
        }
      }
      add_score_stat(simmer->simmed_plays[i], best_play->score,
                     best_play->tiles_played == 7, ply, simmer->threads);
    }

    int spread = game->players[simmer->initial_player]->score -
                 game->players[1 - simmer->initial_player]->score;
    add_equity_stat(simmer->simmed_plays[i], simmer->initial_spread, spread,
                    leftover, simmer->threads);
    add_winpct_stat(
        simmer->simmed_plays[i], simmer->win_pcts, spread, leftover,
        game->game_end_reason,
        // number of tiles unseen to us: bag tiles + tiles on opp rack.
        game->gen->bag->last_tile_index + 1 +
            game->players[1 - simmer->initial_player]->rack->number_of_letters,
        plies % 2, simmer->threads);
    // reset to first state. we only need to restore one backup.
    unplay_last_move(game);
  }
  atomic_fetch_add(&simmer->iteration_count, 1);
}

void *simmer_worker(void *uncasted_simmer_worker) {
  SimmerWorker *simmer_worker = (SimmerWorker *)uncasted_simmer_worker;
  Simmer *simmer = simmer_worker->simmer;
  ThreadControl *thread_control = simmer->thread_control;
  while (!is_halted(thread_control)) {
    int current_iteration_count;
    if (simmer->threads) {
      pthread_mutex_lock(&simmer->iteration_count_mutex);
    }
    if (simmer->iteration_count < simmer->max_iterations) {
      simmer->iteration_count++;
    }
    current_iteration_count = simmer->iteration_count;
    if (simmer->threads) {
      pthread_mutex_unlock(&simmer->iteration_count_mutex);
    }

    if (simmer->iteration_count >= simmer->max_iterations) {
      break;
    }

    sim_single_iteration(simmer_worker);

    if (thread_control->print_info_interval > 0 &&
        current_iteration_count % thread_control->print_info_interval == 0) {
      print_ucgi_stats(simmer, simmer_worker->game, 0);
    }

    if (thread_control->check_stopping_condition_interval > 0 &&
        current_iteration_count %
                thread_control->check_stopping_condition_interval ==
            0 &&
        handle_potential_stopping_condition(simmer)) {
      halt(thread_control);
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

void ucgi_print_moves(Game *game, int nmoves, FILE *outfile) {
  for (int i = 0; i < nmoves; i++) {
    char move[30];
    store_move_ucgi(game->gen->move_list->moves[i], game->gen->board, move,
                    game->gen->letter_distribution);
    fprintf(outfile, "info currmove %s sc %d eq %.3f it 0\n", move,
            game->gen->move_list->moves[i]->score,
            game->gen->move_list->moves[i]->equity);
    fflush(outfile);
  }
}

void simulate(ThreadControl *thread_control, Simmer *simmer, Game *game,
              Rack *known_opp_rack, int plies, int threads, int num_plays,
              int max_iterations, int stopping_condition) {

  int sorting_type = game->players[0]->strategy_params->move_sorting;
  game->players[0]->strategy_params->move_sorting = SORT_BY_EQUITY;
  generate_moves(game->gen, game->players[game->player_on_turn_index],
                 game->players[1 - game->player_on_turn_index]->rack,
                 game->gen->bag->last_tile_index + 1 >= RACK_SIZE);
  sort_moves(game->gen->move_list);
  game->players[0]->strategy_params->move_sorting = sorting_type;

  if (max_iterations <= 0) {
    ucgi_print_moves(game, num_plays, simmer->outfile);
  }
  // Prepare the shared simmer attributes
  simmer->thread_control = thread_control;
  simmer->max_plies = plies;
  simmer->threads = threads;
  simmer->max_iterations = max_iterations;
  simmer->stopping_condition = stopping_condition;
  if (known_opp_rack != NULL) {
    simmer->known_opp_rack = copy_rack(known_opp_rack);
  } else {
    simmer->known_opp_rack = NULL;
  }
  simmer->simmed_plays = malloc((sizeof(SimmedPlay)) * num_plays);
  simmer->num_simmed_plays = num_plays;
  simmer->iteration_count = 0;
  simmer->initial_player = game->player_on_turn_index;
  simmer->initial_spread = game->players[game->player_on_turn_index]->score -
                           game->players[1 - game->player_on_turn_index]->score;

  game->players[0]->strategy_params->move_sorting = SORT_BY_EQUITY;

  for (int i = 0; i < num_plays; i++) {
    SimmedPlay *sp = malloc(sizeof(SimmedPlay));
    sp->move = create_move();
    copy_move(game->gen->move_list->moves[i], sp->move);

    sp->score_stat = malloc(sizeof(Stat *) * plies);
    sp->bingo_stat = malloc(sizeof(Stat *) * plies);
    sp->equity_stat = create_stat();
    sp->leftover_stat = create_stat();
    sp->win_pct_stat = create_stat();
    for (int j = 0; j < plies; j++) {
      sp->score_stat[j] = create_stat();
      sp->bingo_stat[j] = create_stat();
    }
    sp->ignore = 0;
    sp->play_id = i;
    pthread_mutex_init(&sp->mutex, NULL);
    simmer->simmed_plays[i] = sp;
  }
  pthread_mutex_init(&simmer->simmed_plays_mutex, NULL);

  simmer->play_similarity_cache = malloc(sizeof(int) * num_plays * num_plays);
  for (int i = 0; i < num_plays; i++) {
    for (int j = 0; j < num_plays; j++) {
      if (i == j) {
        simmer->play_similarity_cache[i * num_plays + j] = PLAYS_IDENTICAL;
      } else {
        simmer->play_similarity_cache[i * num_plays + j] =
            UNINITIALIZED_SIMILARITY;
      }
    }
  }

  SimmerWorker **simmer_workers = malloc((sizeof(SimmerWorker *)) * (threads));
  pthread_t *worker_ids = malloc((sizeof(pthread_t)) * (threads));
  for (int thread_index = 0; thread_index < threads; thread_index++) {
    simmer_workers[thread_index] =
        create_simmer_worker(simmer, game, thread_index);
    pthread_create(&worker_ids[thread_index], NULL, simmer_worker,
                   simmer_workers[thread_index]);
  }

  for (int thread_index = 0; thread_index < threads; thread_index++) {
    pthread_join(worker_ids[thread_index], NULL);
    destroy_simmer_worker(simmer_workers[thread_index]);
  }

  game->players[0]->strategy_params->move_sorting = sorting_type;

  free(simmer_workers);
  free(worker_ids);
}

void print_ucgi_stats(Simmer *simmer, Game *game, int print_best_play) {
  pthread_mutex_lock(&simmer->simmed_plays_mutex);
  sort_plays_by_win_rate(simmer->simmed_plays, simmer->num_simmed_plays);
  pthread_mutex_unlock(&simmer->simmed_plays_mutex);
  // No need to keep the mutex locked too long here. This is because this
  // function (print_ucgi_stats) will only execute on a single thread.

  // info currmove h4.HADJI sc 40 wp 3.5 wpe 0.731 eq 7.2 eqe 0.812 it 12345 ig
  // 0 plies ply 1 scm 30 scd 3.7 bp 23 ply 2 ...

  // sc - score, wp(e) - win perc
  // (error), eq(e) - equity (error) scm - mean of score, scd - stdev of score,
  // bp - bingo perc ig - this play has been cut-off
  // plies ply 1 ... ply 2 ... ply 3 ...

  for (int i = 0; i < simmer->num_simmed_plays; i++) {
    SimmedPlay *play = simmer->simmed_plays[i];
    char move[30];
    double wp_mean = play->win_pct_stat->mean * 100.0;
    double wp_se = get_standard_error(play->win_pct_stat, STATS_Z99) * 100.0;

    double eq_mean = play->equity_stat->mean;
    double eq_se = get_standard_error(play->equity_stat, STATS_Z99);
    uint64_t niters = play->equity_stat->cardinality;
    store_move_ucgi(play->move, game->gen->board, move,
                    game->gen->letter_distribution);

    fprintf(simmer->outfile,
            "info currmove %s sc %d wp %.3f wpe %.3f eq %.3f eqe %.3f it %lu "
            "ig %d ",
            move, play->move->score, wp_mean, wp_se, eq_mean, eq_se, niters,
            play->ignore);
    fprintf(simmer->outfile, "plies ");
    for (int i = 0; i < simmer->max_plies; i++) {
      fprintf(simmer->outfile, "ply %d ", i + 1);
      fprintf(simmer->outfile, "scm %.3f scd %.3f bp %.3f ",
              play->score_stat[i]->mean, get_stdev(play->score_stat[i]),
              play->bingo_stat[i]->mean * 100.0);
    }
    fprintf(simmer->outfile, "\n");
    fflush(simmer->outfile);
  }
  if (print_best_play) {
    char move[30];
    SimmedPlay *play = simmer->simmed_plays[0];
    store_move_ucgi(play->move, game->gen->board, move,
                    game->gen->letter_distribution);
    fprintf(simmer->outfile, "bestmove %s\n", move);
    fflush(simmer->outfile);
  }
}

// destructors

void free_simmed_plays(Simmer *simmer) {
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
}

void destroy_simmer(Simmer *simmer) {
  destroy_rack(simmer->similar_plays_rack);
  if (simmer->num_simmed_plays > 0) {
    free_simmed_plays(simmer);
  }
  if (simmer->known_opp_rack != NULL) {
    destroy_rack(simmer->known_opp_rack);
  }
  if (simmer->play_similarity_cache != NULL) {
    free(simmer->play_similarity_cache);
  }
  free(simmer);
}