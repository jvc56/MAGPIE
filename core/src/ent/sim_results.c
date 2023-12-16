#include "sim_results.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>

#include "../def/game_defs.h"

#include "../util/util.h"

#include "move.h"
#include "stats.h"
#include "win_pct.h"

struct SimmedPlay {
  Move *move;
  Stat **score_stat;
  Stat **bingo_stat;
  Stat *equity_stat;
  Stat *leftover_stat;
  Stat *win_pct_stat;
  bool ignore;
  int play_id;
  pthread_mutex_t mutex;
};

struct SimResults {
  int max_plies;
  int num_simmed_plays;
  int iteration_count;
  atomic_int node_count;
  SimmedPlay **simmed_plays;
};

SimmedPlay **simmed_plays_create(MoveList *move_list, int num_simmed_plays,
                                 int max_plies) {
  SimmedPlay **simmed_plays =
      malloc_or_die((sizeof(SimmedPlay)) * num_simmed_plays);

  for (int i = 0; i < num_simmed_plays; i++) {
    SimmedPlay *sp = malloc_or_die(sizeof(SimmedPlay));
    sp->move = create_move();
    move_copy(sp->move, move_list_get_move(move_list, i));

    sp->score_stat = malloc_or_die(sizeof(Stat *) * max_plies);
    sp->bingo_stat = malloc_or_die(sizeof(Stat *) * max_plies);
    sp->equity_stat = create_stat();
    sp->leftover_stat = create_stat();
    sp->win_pct_stat = create_stat();
    for (int j = 0; j < max_plies; j++) {
      sp->score_stat[j] = create_stat();
      sp->bingo_stat[j] = create_stat();
    }
    sp->ignore = false;
    sp->play_id = i;
    pthread_mutex_init(&sp->mutex, NULL);
    simmed_plays[i] = sp;
  }
  return simmed_plays;
}

void simmed_plays_destroy(SimmedPlay **simmed_plays, int num_simmed_plays,
                          int max_plies) {
  for (int i = 0; i < num_simmed_plays; i++) {
    for (int j = 0; j < max_plies; j++) {
      destroy_stat(simmed_plays[i]->bingo_stat[j]);
      destroy_stat(simmed_plays[i]->score_stat[j]);
    }
    free(simmed_plays[i]->bingo_stat);
    free(simmed_plays[i]->score_stat);
    destroy_stat(simmed_plays[i]->equity_stat);
    destroy_stat(simmed_plays[i]->leftover_stat);
    destroy_stat(simmed_plays[i]->win_pct_stat);
    destroy_move(simmed_plays[i]->move);
    pthread_mutex_destroy(&simmed_plays[i]->mutex);
    free(simmed_plays[i]);
  }
  free(simmed_plays);
}

SimResults *sim_results_create() {
  SimResults *sim_results = malloc_or_die(sizeof(SimResults));
  sim_results->num_simmed_plays = 0;
  sim_results->max_plies = 0;
  sim_results->simmed_plays = NULL;
  return sim_results;
}

// destructors

void sim_results_destroy(SimResults *sim_results) {
  if (sim_results->simmed_plays) {
    simmed_plays_destroy(sim_results->simmed_plays,
                         sim_results->num_simmed_plays, sim_results->max_plies);
  }

  free(sim_results);
}

void sim_results_init(SimResults *sim_results, MoveList *move_list,
                      int num_simmed_plays, int max_plies) {
  // We need to destroy the simmed plays before
  // setting new values for num_simmed_plays and max_plies
  // because the destructor needs these old values to
  // properly free the simmed plays.
  if (sim_results->simmed_plays) {
    simmed_plays_destroy(sim_results->simmed_plays,
                         sim_results->num_simmed_plays, sim_results->max_plies);
  }
  sim_results->num_simmed_plays = num_simmed_plays;
  sim_results->max_plies = max_plies;
  sim_results->iteration_count = 0;
  atomic_init(&sim_results->node_count, 0);
  sim_results->simmed_plays =
      simmed_plays_create(move_list, num_simmed_plays, max_plies);
}

Move *simmed_play_get_move(const SimmedPlay *simmed_play) {
  return simmed_play->move;
}

Stat *simmed_play_get_score_stat(const SimmedPlay *simmed_play,
                                 int stat_index) {
  return simmed_play->score_stat[stat_index];
}

Stat *simmed_play_get_bingo_stat(const SimmedPlay *simmed_play,
                                 int stat_index) {
  return simmed_play->bingo_stat[stat_index];
}

Stat *simmed_play_get_equity_stat(const SimmedPlay *simmed_play) {
  return simmed_play->equity_stat;
}

Stat *simmed_play_get_leftover_stat(const SimmedPlay *simmed_play) {
  return simmed_play->leftover_stat;
}

Stat *simmed_play_get_win_pct_stat(const SimmedPlay *simmed_play) {
  return simmed_play->win_pct_stat;
}

bool simmed_play_get_ignore(const SimmedPlay *simmed_play) {
  return simmed_play->ignore;
}

int simmed_play_get_id(const SimmedPlay *simmed_play) {
  return simmed_play->play_id;
}

pthread_mutex_t *simmed_play_get_mutex(const SimmedPlay *simmed_play) {
  return (pthread_mutex_t *)&simmed_play->mutex;
}

void simmed_play_set_ignore(SimmedPlay *sp, bool lock) {
  if (lock) {
    pthread_mutex_lock(&sp->mutex);
  }
  sp->ignore = true;
  if (lock) {
    pthread_mutex_unlock(&sp->mutex);
  }
}

int sim_results_get_number_of_plays(SimResults *sim_results) {
  return sim_results->num_simmed_plays;
}

int sim_results_get_max_plies(SimResults *sim_results) {
  return sim_results->max_plies;
}

int sim_results_get_node_count(SimResults *sim_results) {
  return atomic_load(&sim_results->node_count);
}

// Not thread safe. Caller is responsible for ensuring thread
// safety
int sim_results_get_iteration_count(SimResults *sim_results) {
  return sim_results->iteration_count;
}

// Not thread safe. Caller is responsible for ensuring thread
// safety
void sim_results_increment_iteration_count(SimResults *sim_results) {
  sim_results->iteration_count++;
}

SimmedPlay *sim_results_get_simmed_play(SimResults *sim_results, int index) {
  return sim_results->simmed_plays[index];
}

void sim_results_increment_node_count(SimResults *sim_results) {
  atomic_fetch_add(&sim_results->node_count, 1);
}

void add_score_stat(SimmedPlay *sp, int score, bool is_bingo, int ply,
                    bool lock) {
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
                     float leftover, bool lock) {
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

void add_win_pct_stat(const WinPct *wp, SimmedPlay *sp, int spread,
                      float leftover, game_end_reason_t game_end_reason,
                      int tiles_unseen, bool plies_are_odd, bool lock) {
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

int compare_simmed_plays(const void *a, const void *b) {
  const SimmedPlay *play_a = *(const SimmedPlay **)a;
  const SimmedPlay *play_b = *(const SimmedPlay **)b;

  if (play_a->ignore && !play_b->ignore) {
    return 1;
  } else if (play_b->ignore && !play_a->ignore) {
    return -1;
  }

  // Compare the mean values of win_pct_stat
  double mean_a = get_mean(play_a->win_pct_stat);
  double mean_b = get_mean(play_b->win_pct_stat);

  if (mean_a > mean_b) {
    return -1;
  } else if (mean_a < mean_b) {
    return 1;
  } else {
    // If win_pct_stat->mean values are equal, compare equity_stat->mean
    double equity_mean_a = get_mean(play_a->equity_stat);
    double equity_mean_b = get_mean(play_b->equity_stat);

    if (equity_mean_a > equity_mean_b) {
      return -1;
    } else if (equity_mean_a < equity_mean_b) {
      return 1;
    } else {
      return 0;
    }
  }
}

// Not thread safe. Must be called with a lock on the sorted plays mutex
// during multithreaded simming.
void sim_results_sort_plays_by_win_rate(SimResults *sim_results) {
  qsort(sim_results->simmed_plays, sim_results->num_simmed_plays,
        sizeof(SimmedPlay *), compare_simmed_plays);
}
