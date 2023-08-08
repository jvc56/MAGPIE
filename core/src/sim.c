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
#include "util.h"
#include "xoshiro.h"

#define MAX_STOPPING_ITERATION_CT 4000
#define PER_PLY_STOPPING_SCALING 1250
#define SIMILAR_PLAYS_ITER_CUTOFF 1000

Simmer *create_simmer(Config *config, Game *game) {
  Simmer *simmer = malloc(sizeof(Simmer));
  simmer->game = game;
  simmer->threads = 1;
  simmer->win_pcts = config->win_pcts;
  simmer->stopping_condition = SIM_STOPPING_CONDITION_NONE;
  simmer->ucgi_mode = UCGI_MODE_OFF;
  simmer->endsim_callback = NULL;
  simmer->game_copies = NULL;
  simmer->rack_placeholders = NULL;
  simmer->simmed_plays = NULL;
  simmer->known_opp_rack = NULL;
  simmer->play_similarity_cache = NULL;
  simmer->num_simmed_plays = 0;
  simmer->thread_control = NULL;
  return simmer;
}

void add_score_stat(SimmedPlay *sp, int score, int is_bingo, int ply) {
  if (sp->multithreaded) {
    pthread_mutex_lock(&sp->mutex);
  }
  push(sp->score_stat[ply], (double)score, 1);
  push(sp->bingo_stat[ply], (double)is_bingo, 1);
  if (sp->multithreaded) {
    pthread_mutex_unlock(&sp->mutex);
  }
}

void add_equity_stat(SimmedPlay *sp, int initial_spread, int spread,
                     float leftover) {
  if (sp->multithreaded) {
    pthread_mutex_lock(&sp->mutex);
  }
  push(sp->equity_stat, (double)(spread - initial_spread) + (double)(leftover),
       1);
  push(sp->leftover_stat, (double)leftover, 1);
  if (sp->multithreaded) {
    pthread_mutex_unlock(&sp->mutex);
  }
}

void add_winpct_stat(SimmedPlay *sp, WinPct *wp, int spread, float leftover,
                     int game_end_reason, int tiles_unseen, int plies_are_odd) {

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
  if (sp->multithreaded) {
    pthread_mutex_lock(&sp->mutex);
  }
  push(sp->win_pct_stat, wpct, 1);
  if (sp->multithreaded) {
    pthread_mutex_unlock(&sp->mutex);
  }
}

void ignore_play(SimmedPlay *sp) {
  if (sp->multithreaded) {
    pthread_mutex_lock(&sp->mutex);
  }
  sp->ignore = 1;
  if (sp->multithreaded) {
    pthread_mutex_unlock(&sp->mutex);
  }
}

void make_game_copies(Simmer *simmer) {
  simmer->game_copies = malloc((sizeof(Game)) * simmer->threads);
  simmer->rack_placeholders = malloc((sizeof(Rack)) * simmer->threads);
  simmer->thread_control = malloc((sizeof(ThreadControl)) * simmer->threads);

  uint64_t seed = time(NULL);
  for (int i = 0; i < simmer->threads; i++) {
    Game *cp = copy_game(simmer->game, 1);
    set_backup_mode(cp, BACKUP_MODE_SIMULATION);
    for (int j = 0; j < 2; j++) {
      // Simmer only needs to record top equity plays:
      cp->players[j]->strategy_params->play_recorder_type =
          PLAY_RECORDER_TYPE_TOP_EQUITY;
    }

    simmer->rack_placeholders[i] =
        create_rack(simmer->game->gen->letter_distribution->size);
    simmer->thread_control[i] = malloc(sizeof(ThreadControl));
    // Give each game bag the same seed, but then change these:
    seed_prng(cp->gen->bag->prng, seed);
    // "jump" each bag's prng thread number of times.
    for (int j = 0; j < i; j++) {
      xoshiro_jump(cp->gen->bag->prng);
    }
    simmer->game_copies[i] = cp;
  }
}

// this does all the reset work.
// note: this does not check that num_plays and the actual number of plays
// are the same. Caller should make sure we are not going to have a buffer
// overrun here.
void prepare_simmer(Simmer *simmer, int plies, int threads, Move **plays,
                    int num_plays, Rack *known_opp_rack) {

  simmer->max_plies = plies;
  simmer->threads = threads;
  make_game_copies(simmer);
  if (known_opp_rack != NULL) {
    simmer->known_opp_rack = copy_rack(known_opp_rack);
  } else {
    simmer->known_opp_rack = NULL;
  }
  simmer->simmed_plays = malloc((sizeof(SimmedPlay)) * num_plays);
  simmer->num_simmed_plays = num_plays;
  for (int i = 0; i < simmer->threads; i++) {
    simmer->thread_control[i]->status = THREAD_CONTROL_IDLE;
    simmer->thread_control[i]->thread_number = i;
    simmer->thread_control[i]->simmer = simmer;
    simmer->thread_control[i]->last_iteration_ct = 0;
  }

  for (int i = 0; i < num_plays; i++) {
    SimmedPlay *sp = malloc(sizeof(SimmedPlay));
    sp->move = create_move();
    copy_move(plays[i], sp->move);

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
    sp->multithreaded = threads > 1 ? 1 : 0;
    sp->play_id = i;
    pthread_mutex_init(&sp->mutex, NULL);
    simmer->simmed_plays[i] = sp;
  }
  simmer->iteration_count = 0;
  simmer->node_count = 0;
  Game *gc = simmer->game_copies[0];
  pthread_mutex_init(&simmer->simmed_plays_mutex, NULL);
  simmer->initial_player = gc->player_on_turn_index;
  simmer->initial_spread = gc->players[gc->player_on_turn_index]->score -
                           gc->players[1 - gc->player_on_turn_index]->score;

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
}

Move *best_equity_play(Game *game) {
  StrategyParams *sp =
      game->players[game->player_on_turn_index]->strategy_params;
  int recorder_type = sp->play_recorder_type;
  sp->play_recorder_type = PLAY_RECORDER_TYPE_TOP_EQUITY;
  reset_move_list(game->gen->move_list);
  generate_moves(game->gen, game->players[game->player_on_turn_index],
                 game->players[1 - game->player_on_turn_index]->rack,
                 game->gen->bag->last_tile_index + 1 >= RACK_SIZE);
  // restore old recorder type
  sp->play_recorder_type = recorder_type;
  return game->gen->move_list->moves[0];
}

void set_stop_flags(Simmer *simmer) {
  log_debug("setting stop flags");
  for (int t = 0; t < simmer->threads; t++) {
    simmer->thread_control[t]->status = THREAD_CONTROL_SHOULD_STOP;
  }
}

void stop_simming(Simmer *simmer) { set_stop_flags(simmer); }

void set_endsim_callback(Simmer *simmer, callback_fn f) {
  simmer->endsim_callback = f;
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
      ignore_play(simmer->simmed_plays[i]);
      new_ignored++;
    } else if (simmer->iteration_count > SIMILAR_PLAYS_ITER_CUTOFF) {
      if (plays_are_similar(simmer, tentative_winner,
                            simmer->simmed_plays[i])) {
        ignore_play(simmer->simmed_plays[i]);
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

void *single_thread_simmer(void *ptr) {
  ThreadControl *tc = (ThreadControl *)ptr;
  while (tc->status != THREAD_CONTROL_SHOULD_STOP) {
    sim_single_iteration(tc->simmer, tc->simmer->max_plies, tc->thread_number);
    if (tc->thread_number == 0) {
      int total_iterations = atomic_load(&tc->simmer->iteration_count);
      if (tc->simmer->stopping_condition != SIM_STOPPING_CONDITION_NONE) {
        // Let's let this thread also be the "main" thread; only this one can
        // decide to ignore plays and/or stop the simulation if there is a
        // stopping condition set.
        // We should check every 512 iterations or so, across all different
        // threads.
        int should_stop = 0;
        if (total_iterations - tc->last_iteration_ct > 500) {
          should_stop = handle_potential_stopping_condition(tc->simmer);
          tc->last_iteration_ct = total_iterations;
        }
        if (should_stop) {
          set_stop_flags(tc->simmer);
        }
      }
      if (tc->simmer->ucgi_mode == UCGI_MODE_ON &&
          (total_iterations % 100) < tc->simmer->threads) {
        // Print out an estimate of how it's going in UCGI format.
        print_ucgi_stats(tc->simmer, 0);
      }
    }
  }
  tc->status = THREAD_CONTROL_EXITING;
  log_trace("thread %d exiting", tc->thread_number);
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

  // Re-use thread 0's rack placeholder (this should only run in thread 0).
  Rack *ph = simmer->rack_placeholders[0];
  // Create a rack from m1, then subtract the rack from m2. The final
  // rack should have all zeroes.
  reset_rack(ph);
  for (int i = 0; i < m1->move->tiles_length; i++) {
    if (m1->move->tiles[i] == PLAYED_THROUGH_MARKER) {
      continue;
    }
    int ml = m1->move->tiles[i];
    if (is_blanked(ml)) {
      ml = 0;
    }
    ph->array[ml]++;
  }

  for (int i = 0; i < m2->move->tiles_length; i++) {
    if (m2->move->tiles[i] == PLAYED_THROUGH_MARKER) {
      continue;
    }
    int ml = m2->move->tiles[i];
    if (is_blanked(ml)) {
      ml = 0;
    }
    ph->array[ml]--;
  }

  for (int i = 0; i < ph->array_size; i++) {
    if (ph->array[i] != 0) {
      simmer->play_similarity_cache[cache_index1] = PLAYS_NOT_SIMILAR;
      simmer->play_similarity_cache[cache_index2] = PLAYS_NOT_SIMILAR;
      return 0;
    }
  }
  simmer->play_similarity_cache[cache_index1] = PLAYS_SIMILAR;
  simmer->play_similarity_cache[cache_index2] = PLAYS_SIMILAR;

  return 1;
}

void simulate(Simmer *simmer) {
  // assume that prepare_simmer has already been called.
  if (simmer->num_simmed_plays == 0) {
    printf("Please prepare simmer first.\n");
    return;
  }
  clock_gettime(CLOCK_MONOTONIC, &simmer->start_time);

  for (int t = 0; t < simmer->threads; t++) {
    pthread_create(&simmer->thread_control[t]->thread, NULL,
                   single_thread_simmer, simmer->thread_control[t]);
    simmer->thread_control[t]->status = THREAD_CONTROL_RUNNING;
  }
}

void join_threads(Simmer *simmer) {
  log_debug("join_threads waiting...");

  for (int t = 0; t < simmer->threads; t++) {
    pthread_join(simmer->thread_control[t]->thread, NULL);
    simmer->thread_control[t]->status = THREAD_CONTROL_IDLE;
  }
  log_debug("all sim threads joined");

  // We're done simming now. Print out all stats, etc.

  struct timespec finish;
  double elapsed;

  clock_gettime(CLOCK_MONOTONIC, &finish);

  elapsed = (finish.tv_sec - simmer->start_time.tv_sec);
  elapsed += (finish.tv_nsec - simmer->start_time.tv_nsec) / 1000000000.0;
  double nps = (double)simmer->node_count / elapsed;
  // Print out the bestplay, UCGI. (and the final rankings/data)
  if (simmer->ucgi_mode == UCGI_MODE_ON) {
    print_ucgi_stats(simmer, 1);
    fprintf(stdout, "info nps %f\n", nps);
  }
  if (simmer->endsim_callback != NULL) {
    simmer->endsim_callback();
  }

  log_debug("elapsed time %f s\n", elapsed);
  log_debug("nps %f\n", nps);
}

void blocking_simulate(Simmer *simmer) {
  // Like simulate, but it blocks. This function must only be called if we
  // have a stopping condition set up.
  if (simmer->num_simmed_plays == 0) {
    printf("Please prepare simmer first.\n");
    return;
  }
  if (simmer->stopping_condition == SIM_STOPPING_CONDITION_NONE) {
    printf("You must have a stopping condition set to use this function.\n");
    return;
  }
  clock_gettime(CLOCK_MONOTONIC, &simmer->start_time);

  for (int t = 0; t < simmer->threads; t++) {
    pthread_create(&simmer->thread_control[t]->thread, NULL,
                   single_thread_simmer, simmer->thread_control[t]);
    simmer->thread_control[t]->status = THREAD_CONTROL_RUNNING;
  }
  join_threads(simmer);
}

void sim_single_iteration(Simmer *simmer, int plies, int thread) {
  Game *gc = simmer->game_copies[thread];
  Rack *rack_placeholder = simmer->rack_placeholders[thread];

  // set random rack for opponent (throw in rack, shuffle, draw new tiles).
  set_random_rack(gc, 1 - gc->player_on_turn_index, simmer->known_opp_rack);
  // need a new shuffle for every iteration:
  shuffle(gc->gen->bag);

  for (int i = 0; i < simmer->num_simmed_plays; i++) {
    if (simmer->simmed_plays[i]->ignore) {
      continue;
    }

    double leftover = 0.0;
    set_backup_mode(gc, BACKUP_MODE_SIMULATION);
    // play move
    play_move(gc, simmer->simmed_plays[i]->move);
    atomic_fetch_add(&simmer->node_count, 1);
    set_backup_mode(gc, BACKUP_MODE_OFF);
    // further plies will NOT be backed up.
    for (int ply = 0; ply < plies; ply++) {
      int onturn = gc->player_on_turn_index;
      if (gc->game_end_reason != GAME_END_REASON_NONE) {
        // game is over.
        break;
      }

      Move *best_play = best_equity_play(gc);
      copy_rack_into(rack_placeholder, gc->players[onturn]->rack);
      play_move(gc, best_play);
      atomic_fetch_add(&simmer->node_count, 1);
      char placeholder[80];
      store_move_description(best_play, placeholder,
                             simmer->game->gen->letter_distribution);

      if (ply == plies - 2 || ply == plies - 1) {
        double this_leftover = get_leave_value_for_move(
            gc->players[0]->strategy_params->klv, best_play, rack_placeholder);
        if (onturn == simmer->initial_player) {
          leftover += this_leftover;
        } else {
          leftover -= this_leftover;
        }
      }
      add_score_stat(simmer->simmed_plays[i], best_play->score,
                     best_play->tiles_played == 7, ply);
    }

    int spread = gc->players[simmer->initial_player]->score -
                 gc->players[1 - simmer->initial_player]->score;
    add_equity_stat(simmer->simmed_plays[i], simmer->initial_spread, spread,
                    leftover);
    add_winpct_stat(
        simmer->simmed_plays[i], simmer->win_pcts, spread, leftover,
        gc->game_end_reason,
        // number of tiles unseen to us: bag tiles + tiles on opp rack.
        gc->gen->bag->last_tile_index + 1 +
            gc->players[1 - simmer->initial_player]->rack->number_of_letters,
        plies % 2);
    // reset to first state. we only need to restore one backup.
    unplay_last_move(gc);
  }
  atomic_fetch_add(&simmer->iteration_count, 1);
}

void set_stopping_condition(Simmer *simmer, int sc) {
  simmer->stopping_condition = sc;
}

void set_ucgi_mode(Simmer *simmer, int mode) { simmer->ucgi_mode = mode; }

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

void print_ucgi_stats(Simmer *simmer, int print_best_play) {
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
    store_move_ucgi(play->move, simmer->game->gen->board, move,
                    simmer->game->gen->letter_distribution);

    fprintf(stdout,
            "info currmove %s sc %d wp %.3f wpe %.3f eq %.3f eqe %.3f it %llu "
            "ig %d ",
            move, play->move->score, wp_mean, wp_se, eq_mean, eq_se, niters,
            play->ignore);
    fprintf(stdout, "plies ");
    for (int i = 0; i < simmer->max_plies; i++) {
      fprintf(stdout, "ply %d ", i + 1);
      fprintf(stdout, "scm %.3f scd %.3f bp %.3f ", play->score_stat[i]->mean,
              get_stdev(play->score_stat[i]),
              play->bingo_stat[i]->mean * 100.0);
    }
    fprintf(stdout, "\n");
  }
  if (print_best_play) {
    char move[30];
    SimmedPlay *play = simmer->simmed_plays[0];
    store_move_ucgi(play->move, simmer->game->gen->board, move,
                    simmer->game->gen->letter_distribution);
    fprintf(stdout, "bestmove %s\n", move);
  }
}

void print_sim_stats(Simmer *simmer) {
  pthread_mutex_lock(&simmer->simmed_plays_mutex);
  sort_plays_by_win_rate(simmer->simmed_plays, simmer->num_simmed_plays);
  pthread_mutex_unlock(&simmer->simmed_plays_mutex);

  printf("%-20s%-9s%-16s%-16s\n", "Play", "Score", "Win%", "Equity");
  for (int i = 0; i < simmer->num_simmed_plays; i++) {
    SimmedPlay *play = simmer->simmed_plays[i];
    double wp_mean = play->win_pct_stat->mean * 100.0;
    double wp_se = get_standard_error(play->win_pct_stat, STATS_Z99) * 100.0;

    double eq_mean = play->equity_stat->mean;
    double eq_se = get_standard_error(play->equity_stat, STATS_Z99);

    char wp[20];
    char eq[20];
    sprintf(wp, "%.3f±%.3f", wp_mean, wp_se);
    sprintf(eq, "%.3f±%.3f", eq_mean, eq_se);

    const char *ignore = play->ignore ? "❌" : "";
    char placeholder[80];
    store_move_description(play->move, placeholder,
                           simmer->game->gen->letter_distribution);
    printf("%-20s%-9d%-16s%-16s%s\n", placeholder, play->move->score, wp, eq,
           ignore);
  }
  printf("Iterations: %d\n", simmer->iteration_count);
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

void free_game_copies(Simmer *simmer) {
  for (int i = 0; i < simmer->threads; i++) {
    destroy_game(simmer->game_copies[i]);
  }
  free(simmer->game_copies);
}

void free_rack_placeholders(Simmer *simmer) {
  for (int i = 0; i < simmer->threads; i++) {
    destroy_rack(simmer->rack_placeholders[i]);
  }
  free(simmer->rack_placeholders);
}

void free_thread_controllers(Simmer *simmer) {
  for (int i = 0; i < simmer->threads; i++) {
    free(simmer->thread_control[i]);
  }
  free(simmer->thread_control);
}

void destroy_simmer(Simmer *simmer) {
  if (simmer->num_simmed_plays > 0) {
    free_simmed_plays(simmer);
  }
  if (simmer->known_opp_rack != NULL) {
    destroy_rack(simmer->known_opp_rack);
  }
  if (simmer->game_copies != NULL) {
    free_game_copies(simmer);
  }
  if (simmer->rack_placeholders != NULL) {
    free_rack_placeholders(simmer);
  }
  if (simmer->thread_control != NULL) {
    free_thread_controllers(simmer);
  }
  if (simmer->play_similarity_cache != NULL) {
    free(simmer->play_similarity_cache);
  }
  free(simmer);
}
