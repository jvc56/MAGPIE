#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>

#include "../def/rack_defs.h"
#include "../def/stats_defs.h"

#include "bag.h"
#include "game.h"
#include "move.h"
#include "movegen.h"
#include "player.h"
#include "rack.h"
#include "simmer.h"
#include "stats.h"
#include "thread_control.h"
#include "timer.h"
#include "win_pct.h"

#define MAX_STOPPING_ITERATION_CT 4000
#define PER_PLY_STOPPING_SCALING 1250
#define SIMILAR_PLAYS_ITER_CUTOFF 1000

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

Move *get_simmed_play_move(const SimmedPlay *simmed_play) {
  return simmed_play->move;
}

Stat *get_simmed_play_score_stat(const SimmedPlay *simmed_play,
                                 int stat_index) {
  return simmed_play->score_stat[stat_index];
}

Stat *get_simmed_play_bingo_stat(const SimmedPlay *simmed_play,
                                 int stat_index) {
  return simmed_play->bingo_stat[stat_index];
}

Stat *get_simmed_play_equity_stat(const SimmedPlay *simmed_play) {
  return simmed_play->equity_stat;
}

Stat *get_simmed_play_leftover_stat(const SimmedPlay *simmed_play) {
  return simmed_play->leftover_stat;
}

Stat *get_simmed_play_win_pct_stat(const SimmedPlay *simmed_play) {
  return simmed_play->win_pct_stat;
}

bool is_simmed_play_ignore(const SimmedPlay *simmed_play) {
  return simmed_play->ignore;
}

int get_simmed_play_id(const SimmedPlay *simmed_play) {
  return simmed_play->play_id;
}

pthread_mutex_t *get_simmed_play_mutex(const SimmedPlay *simmed_play) {
  return (pthread_mutex_t *)&simmed_play->mutex;
}

struct Simmer {
  int initial_spread;
  int max_plies;
  int initial_player;
  int iteration_count;
  pthread_mutex_t iteration_count_mutex;
  int max_iterations;
  int num_simmed_plays;
  struct timespec start_time;

  int stopping_condition;
  int threads;
  uint64_t seed;

  SimmedPlay **simmed_plays;
  pthread_mutex_t simmed_plays_mutex;

  Rack *known_opp_rack;
  Rack *similar_plays_rack;
  const WinPct *win_pcts;

  int *play_similarity_cache;
  atomic_int node_count;
  ThreadControl *thread_control;
};

typedef struct SimmerWorker {
  int thread_index;
  Game *game;
  Rack *rack_placeholder;
  Simmer *simmer;
} SimmerWorker;

int simmer_get_node_count(Simmer *simmer) {
  return atomic_load(&simmer->node_count);
}

int simmer_get_number_of_plays(Simmer *simmer) {
  return simmer->num_simmed_plays;
}

int simmer_get_max_plies(Simmer *simmer) { return simmer->max_plies; }

int simmer_get_iteration_count(Simmer *simmer) {
  return simmer->iteration_count;
}

SimmedPlay *simmer_get_simmed_play(Simmer *simmer, int simmed_play_index) {
  return simmer->simmed_plays[simmed_play_index];
}

Simmer *create_simmer(const Config *config) {
  Simmer *simmer = malloc_or_die(sizeof(Simmer));
  simmer->threads = get_number_of_threads(config_get_thread_control(config));
  simmer->win_pcts = config_get_win_pcts(config);
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

void create_simmed_plays(const Game *game, Simmer *simmer) {
  simmer->simmed_plays =
      malloc_or_die((sizeof(SimmedPlay)) * simmer->num_simmed_plays);
  for (int i = 0; i < simmer->num_simmed_plays; i++) {
    SimmedPlay *sp = malloc_or_die(sizeof(SimmedPlay));
    sp->move = create_move();
    move_copy(sp->move,
              move_list_get_move(gen_get_move_list(game_get_gen(game)), i));

    sp->score_stat = malloc_or_die(sizeof(Stat *) * simmer->max_plies);
    sp->bingo_stat = malloc_or_die(sizeof(Stat *) * simmer->max_plies);
    sp->equity_stat = create_stat();
    sp->leftover_stat = create_stat();
    sp->win_pct_stat = create_stat();
    for (int j = 0; j < simmer->max_plies; j++) {
      sp->score_stat[j] = create_stat();
      sp->bingo_stat[j] = create_stat();
    }
    sp->ignore = false;
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

  free(simmer->play_similarity_cache);
  free(simmer);
}

SimmerWorker *create_simmer_worker(const Game *game, Simmer *simmer,
                                   int worker_index) {
  SimmerWorker *simmer_worker = malloc_or_die(sizeof(SimmerWorker));

  simmer_worker->simmer = simmer;
  simmer_worker->thread_index = worker_index;
  Game *new_game = game_duplicate(game, 1);
  simmer_worker->game = new_game;
  set_backup_mode(new_game, BACKUP_MODE_SIMULATION);
  for (int j = 0; j < 2; j++) {
    // Simmer only needs to record top equity plays:
    player_set_move_record_type(game_get_player(new_game, j), MOVE_RECORD_BEST);
  }

  simmer_worker->rack_placeholder = create_rack(
      letter_distribution_get_size(gen_get_ld(game_get_gen(new_game))));

  seed_bag_for_worker(gen_get_bag(game_get_gen(new_game)), simmer->seed,
                      worker_index);

  return simmer_worker;
}

void destroy_simmer_worker(SimmerWorker *simmer_worker) {
  destroy_game(simmer_worker->game);
  destroy_rack(simmer_worker->rack_placeholder);
  free(simmer_worker);
}

bool is_multithreaded(const Simmer *simmer) { return simmer->threads > 1; }

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

void ignore_play(SimmedPlay *sp, bool lock) {
  if (lock) {
    pthread_mutex_lock(&sp->mutex);
  }
  sp->ignore = true;
  if (lock) {
    pthread_mutex_unlock(&sp->mutex);
  }
}

bool handle_potential_stopping_condition(Simmer *simmer) {
  pthread_mutex_lock(&simmer->simmed_plays_mutex);
  sort_plays_by_win_rate_while_locked(simmer->simmed_plays,
                                      simmer->num_simmed_plays);

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

  const SimmedPlay *tentative_winner = simmer->simmed_plays[0];
  double mu = get_mean(tentative_winner->win_pct_stat);
  double stderr = get_standard_error(tentative_winner->win_pct_stat, zval);
  int total_ignored = 0;
  for (int i = 1; i < simmer->num_simmed_plays; i++) {
    if (simmer->simmed_plays[i]->ignore) {
      total_ignored++;
      continue;
    }
    double mu_i = get_mean(simmer->simmed_plays[i]->win_pct_stat);
    double stderr_i =
        get_standard_error(simmer->simmed_plays[i]->win_pct_stat, zval);

    if ((mu - stderr) > (mu_i + stderr_i)) {
      ignore_play(simmer->simmed_plays[i], is_multithreaded(simmer));
      total_ignored++;
    } else if (simmer->iteration_count > SIMILAR_PLAYS_ITER_CUTOFF) {
      if (plays_are_similar(tentative_winner, simmer->simmed_plays[i],
                            simmer)) {
        ignore_play(simmer->simmed_plays[i], is_multithreaded(simmer));
        total_ignored++;
      }
    }
  }

  pthread_mutex_unlock(&simmer->simmed_plays_mutex);
  log_debug("total ignored: %d\n", total_ignored);
  if (total_ignored >= simmer->num_simmed_plays - 1) {
    // if there is only 1 unignored play, exit.
    return true;
  }
  return false;
}

void sim_single_iteration(SimmerWorker *simmer_worker) {
  Game *game = simmer_worker->game;
  Rack *rack_placeholder = simmer_worker->rack_placeholder;
  Simmer *simmer = simmer_worker->simmer;
  int plies = simmer->max_plies;

  // set random rack for opponent (throw in rack, shuffle, draw new tiles).
  set_random_rack(game, 1 - game_get_player_on_turn_index(game),
                  simmer->known_opp_rack);
  // need a new shuffle for every iteration:
  Bag *bag = gen_get_bag(game_get_gen(game));
  shuffle(bag);

  for (int i = 0; i < simmer->num_simmed_plays; i++) {
    if (simmer->simmed_plays[i]->ignore) {
      continue;
    }

    double leftover = 0.0;
    set_backup_mode(game, BACKUP_MODE_SIMULATION);
    // play move
    play_move(simmer->simmed_plays[i]->move, game);
    atomic_fetch_add(&simmer->node_count, 1);
    set_backup_mode(game, BACKUP_MODE_OFF);
    // further plies will NOT be backed up.
    for (int ply = 0; ply < plies; ply++) {
      int player_on_turn_index = game_get_player_on_turn_index(game);
      Player *player_on_turn = game_get_player(game, player_on_turn_index);
      Player *opponent = game_get_player(game, 1 - player_on_turn_index);
      game_end_reason_t game_end_reason = game_get_game_end_reason(game);

      if (game_end_reason != GAME_END_REASON_NONE) {
        // game is over.
        break;
      }

      const Move *best_play = get_top_equity_move(game);
      rack_copy(rack_placeholder, player_get_rack(player_on_turn));
      play_move(best_play, game);
      atomic_fetch_add(&simmer->node_count, 1);
      if (ply == plies - 2 || ply == plies - 1) {
        double this_leftover = get_leave_value_for_move(
            player_get_klv(player_on_turn), best_play, rack_placeholder);
        if (player_on_turn_index == simmer->initial_player) {
          leftover += this_leftover;
        } else {
          leftover -= this_leftover;
        }
      }
      add_score_stat(simmer->simmed_plays[i], get_score(best_play),
                     get_tiles_played(best_play) == RACK_SIZE, ply,
                     is_multithreaded(simmer));
    }

    int spread =
        player_get_score(game_get_player(game, simmer->initial_player)) -
        player_get_score(game_get_player(game, 1 - simmer->initial_player));
    add_equity_stat(simmer->simmed_plays[i], simmer->initial_spread, spread,
                    leftover, is_multithreaded(simmer));
    add_win_pct_stat(
        simmer->win_pcts, simmer->simmed_plays[i], spread, leftover,
        game_get_game_end_reason(game),
        // number of tiles unseen to us: bag tiles + tiles on opp rack.
        get_tiles_remaining(bag) +
            get_number_of_letters(player_get_rack(
                game_get_player(game, 1 - simmer->initial_player))),
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
    bool reached_max_iteration = false;
    if (is_multithreaded(simmer)) {
      pthread_mutex_lock(&simmer->iteration_count_mutex);
    }
    if (simmer->iteration_count == simmer->max_iterations) {
      reached_max_iteration = true;
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

    int print_info_interval = get_print_info_interval(thread_control);
    if (print_info_interval > 0 && current_iteration_count > 0 &&
        current_iteration_count % print_info_interval == 0) {
      print_ucgi_sim_stats(simmer_worker->game, simmer, 0);
    }

    int check_stopping_condition_interval =
        get_check_stopping_condition_interval(thread_control);
    if (check_stopping_condition_interval > 0 &&
        current_iteration_count % check_stopping_condition_interval == 0 &&
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

bool plays_are_similar(const SimmedPlay *m1, const SimmedPlay *m2,
                       Simmer *simmer) {
  // look in the cache first
  int cache_value =
      simmer->play_similarity_cache[m1->play_id +
                                    simmer->num_simmed_plays * m2->play_id];
  assert(cache_value != PLAYS_IDENTICAL);
  if (cache_value == PLAYS_SIMILAR) {
    return true;
  } else if (cache_value == PLAYS_NOT_SIMILAR) {
    return false;
  }
  int cache_index1 = m1->play_id + simmer->num_simmed_plays * m2->play_id;
  int cache_index2 = m2->play_id + simmer->num_simmed_plays * m1->play_id;

  if (get_move_type(m1->move) != get_move_type(m2->move)) {
    simmer->play_similarity_cache[cache_index1] = PLAYS_NOT_SIMILAR;
    simmer->play_similarity_cache[cache_index2] = PLAYS_NOT_SIMILAR;
    return false;
  }

  // Otherwise, we must compute play similarity and fill in the cache.
  // two plays are "similar" if they use the same tiles, and they start at
  // the same square.
  if (!(get_dir(m1->move) == get_dir(m2->move) &&
        get_col_start(m1->move) == get_col_start(m2->move) &&
        get_row_start(m1->move) == get_row_start(m2->move))) {

    simmer->play_similarity_cache[cache_index1] = PLAYS_NOT_SIMILAR;
    simmer->play_similarity_cache[cache_index2] = PLAYS_NOT_SIMILAR;
    return false;
  }
  if (!(get_tiles_played(m1->move) == get_tiles_played(m2->move) &&
        get_tiles_length(m1->move) == get_tiles_length(m2->move))) {
    simmer->play_similarity_cache[cache_index1] = PLAYS_NOT_SIMILAR;
    simmer->play_similarity_cache[cache_index2] = PLAYS_NOT_SIMILAR;
    return false;
  }

  // Create a rack from m1, then subtract the rack from m2. The final
  // rack should have all zeroes.
  reset_rack(simmer->similar_plays_rack);
  for (int i = 0; i < get_tiles_length(m1->move); i++) {
    uint8_t tile = get_tile(m1->move, i);
    if (tile == PLAYED_THROUGH_MARKER) {
      continue;
    }
    int ml = tile;
    if (is_blanked(ml)) {
      ml = 0;
    }
    add_letter_to_rack(simmer->similar_plays_rack, ml);
  }

  for (int i = 0; i < get_tiles_length(m2->move); i++) {
    uint8_t tile = get_tile(m1->move, i);
    if (tile == PLAYED_THROUGH_MARKER) {
      continue;
    }
    int ml = tile;
    if (is_blanked(ml)) {
      ml = 0;
    }
    take_letter_from_rack(simmer->similar_plays_rack, ml);
  }

  for (int i = 0; i < get_array_size(simmer->similar_plays_rack); i++) {
    if (get_number_of_letter(simmer->similar_plays_rack, i) != 0) {
      simmer->play_similarity_cache[cache_index1] = PLAYS_NOT_SIMILAR;
      simmer->play_similarity_cache[cache_index2] = PLAYS_NOT_SIMILAR;
      return false;
    }
  }
  simmer->play_similarity_cache[cache_index1] = PLAYS_SIMILAR;
  simmer->play_similarity_cache[cache_index2] = PLAYS_SIMILAR;

  return true;
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

// Must be called with a lock on the sorted plays mutex
void sort_plays_by_win_rate(Simmer *simmer) {
  pthread_mutex_lock(&simmer->simmed_plays_mutex);
  sort_plays_by_win_rate_while_locked(simmer->simmed_plays,
                                      simmer->num_simmed_plays);
  pthread_mutex_unlock(&simmer->simmed_plays_mutex);
}

// Must be called with a lock on the sorted plays mutex
void sort_plays_by_win_rate_while_locked(SimmedPlay **simmed_plays,
                                         int num_simmed_plays) {
  qsort(simmed_plays, num_simmed_plays, sizeof(SimmedPlay *),
        compare_simmed_plays);
}

sim_status_t simulate(const Config *config, const Game *game, Simmer *simmer) {
  ThreadControl *thread_control = config_get_thread_control(config);
  unhalt(thread_control);

  Generator *gen = game_get_gen(game);
  Bag *bag = gen_get_bag(gen);
  MoveList *move_list = gen_get_move_list(gen);
  int player_on_turn_index = game_get_player_on_turn_index(game);
  Player *player_on_turn = game_get_player(game, player_on_turn_index);
  Player *opponent = game_get_player(game, 1 - player_on_turn_index);

  reset_move_list(move_list);
  generate_moves(player_get_rack(opponent), gen,
                 player_get_rack(player_on_turn),
                 get_tiles_remaining(bag) >= RACK_SIZE, MOVE_RECORD_ALL,
                 MOVE_SORT_EQUITY, true);
  int number_of_moves_generated = move_list_get_count(move_list);
  sort_moves(move_list);

  int num_plays = config_get_num_plays(config);
  if (config_get_static_search_only(config)) {
    print_ucgi_static_moves(game, num_plays, thread_control);
    return SIM_STATUS_SUCCESS;
  }

  // It is important that we first destroy the simmed plays
  // then set the new values for the simmer. The destructor
  // relies on the previous values of the simmer to properly
  // free everything.
  if (simmer->simmed_plays) {
    destroy_simmed_plays(simmer);
  }

  int number_of_threads = get_number_of_threads(thread_control);
  // Prepare the shared simmer attributes
  simmer->thread_control = thread_control;
  simmer->max_plies = config_get_plies(config);
  simmer->threads = number_of_threads;
  simmer->seed = config_get_seed(config);
  simmer->max_iterations = config_get_max_iterations(config);
  simmer->stopping_condition = config_get_stopping_condition(config);

  simmer->num_simmed_plays = config_get_num_plays(config);
  if (number_of_moves_generated < simmer->num_simmed_plays) {
    simmer->num_simmed_plays = number_of_moves_generated;
  }
  simmer->iteration_count = 0;
  simmer->initial_player = player_on_turn_index;
  simmer->initial_spread =
      player_get_score(player_on_turn) - player_get_score(opponent);
  atomic_init(&simmer->node_count, 0);
  create_simmed_plays(game, simmer);

  // The letter distribution may have changed,
  // so we might need to recreate the rack.
  update_or_create_rack(
      &simmer->similar_plays_rack,
      letter_distribution_get_size(config_get_letter_distribution(config)));

  Rack *config_rack = config_get_rack(config);
  if (simmer->num_simmed_plays > 0) {
    if (config_rack) {
      if (simmer->known_opp_rack) {
        destroy_rack(simmer->known_opp_rack);
      }
      simmer->known_opp_rack = rack_duplicate(config_rack);
    } else {
      simmer->known_opp_rack = NULL;
    }

    free(simmer->play_similarity_cache);
    simmer->play_similarity_cache =
        malloc_or_die(sizeof(int) * num_plays * num_plays);
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

    SimmerWorker **simmer_workers =
        malloc_or_die((sizeof(SimmerWorker *)) * (number_of_threads));
    pthread_t *worker_ids =
        malloc_or_die((sizeof(pthread_t)) * (number_of_threads));

    Timer *timer = get_timer(thread_control);
    timer_start(timer);

    for (int thread_index = 0; thread_index < number_of_threads;
         thread_index++) {
      simmer_workers[thread_index] =
          create_simmer_worker(game, simmer, thread_index);
      pthread_create(&worker_ids[thread_index], NULL, simmer_worker,
                     simmer_workers[thread_index]);
    }

    for (int thread_index = 0; thread_index < number_of_threads;
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
