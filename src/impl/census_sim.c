#include "census_sim.h"

#include "../compat/cpthread.h"
#include "../compat/cpthread_defs.h"
#include "../compat/ctime.h"
#include "../def/board_defs.h"
#include "../def/rack_defs.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/klv.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../util/io_util.h"
#include "census.h"
#include "census_play_scores.h"
#include "gameplay.h"
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

struct CensusSim {
  CensusLattice *lattice;
  CensusAddTable *add_table;
  // The opponent's leave values by rank, and their max over subsets, for
  // leaves_klv.
  Equity *leave;
  Equity *leave_max_over_subsets;
  const KLV *leaves_klv;
  Equity *play_scores;
  Equity *rack_best;
  CensusSimTimes times;
  int number_of_threads;
  // With a position set (census_sim_set_position), its board and the play
  // scores of each group of lanes on it, which a move's play scores take for
  // the lanes the move leaves as they were.
  Game *position;
  uint64_t group_lanes[CENSUS_SIM_LANE_GROUPS];
  Equity *group_scores[CENSUS_SIM_LANE_GROUPS];
};

// The pool of the player on turn in game: the board's unseen tiles less the
// mover's rack, which the opponent's rack is drawn from. Playing a move takes
// its tiles from that rack, so the pool is the same after it.
static void census_sim_get_pool(const Game *game, uint8_t *pool) {
  const int ld_size = ld_get_size(game_get_ld(game));
  census_get_unseen_tally(game, pool);
  const Rack *mover_rack = player_get_rack(
      game_get_player(game, game_get_player_on_turn_index(game)));
  for (int ml = 0; ml < ld_size; ml++) {
    pool[ml] = (uint8_t)(pool[ml] - rack_get_letter(mover_rack, ml));
  }
}

// One thread's share of census_sim_build: its slice of the lattice to fill,
// then the units of the build it claims.
typedef struct CensusSimBuildWorker {
  const CensusSim *sim;
  const Game *game;
  const uint8_t *pool;
  Equity *play_scores;
  const Equity *const *kept;
  int number_kept;
  uint64_t lanes;
  int thread_index;
  int number_of_threads;
  _Atomic int *next_unit;
  cpthread_t cpthread_id;
} CensusSimBuildWorker;

static void *census_sim_fill_worker(void *uncasted_worker) {
  const CensusSimBuildWorker *worker =
      (const CensusSimBuildWorker *)uncasted_worker;
  const uint64_t size = census_lattice_get_size(worker->sim->lattice);
  const uint32_t start = (uint32_t)(size * (uint64_t)worker->thread_index /
                                    (uint64_t)worker->number_of_threads);
  const uint32_t end = (uint32_t)(size * (uint64_t)(worker->thread_index + 1) /
                                  (uint64_t)worker->number_of_threads);
  Equity *play_scores = worker->play_scores;
  // 0, the value of exchanging the tiles away, is the floor of every score.
  for (uint32_t index = start; index < end; index++) {
    play_scores[index] = 0;
  }
  for (int kept_idx = 0; kept_idx < worker->number_kept; kept_idx++) {
    const Equity *kept = worker->kept[kept_idx];
    for (uint32_t index = start; index < end; index++) {
      play_scores[index] =
          kept[index] > play_scores[index] ? kept[index] : play_scores[index];
    }
  }
  return NULL;
}

static void *census_sim_lanes_worker(void *uncasted_worker) {
  const CensusSimBuildWorker *worker =
      (const CensusSimBuildWorker *)uncasted_worker;
  const CensusLattice *lattice = worker->sim->lattice;
  if (worker->number_of_threads == 1) {
    census_play_scores_build_lanes(worker->game, lattice, worker->pool,
                                   worker->play_scores, worker->lanes);
  } else {
    census_play_scores_build_claimed(worker->game, lattice, worker->pool,
                                     worker->play_scores,
                                     census_lattice_get_num_letters(lattice),
                                     worker->next_unit, worker->lanes);
  }
  return NULL;
}

// Runs one phase on every worker's thread, the first in the calling thread.
static void census_sim_run_phase(CensusSimBuildWorker *workers,
                                 int number_of_threads,
                                 void *(*phase)(void *)) {
  for (int thread_idx = 1; thread_idx < number_of_threads; thread_idx++) {
    cpthread_create(&workers[thread_idx].cpthread_id, phase,
                    &workers[thread_idx]);
  }
  phase(&workers[0]);
  for (int thread_idx = 1; thread_idx < number_of_threads; thread_idx++) {
    cpthread_join(workers[thread_idx].cpthread_id);
  }
}

// Sets play_scores to the max of 0 and the number_kept arrays of kept, then
// raises it by the plays of the lanes in `lanes` on game's board, on the
// sim's threads.
static void census_sim_build(const CensusSim *sim, const Game *game,
                             const uint8_t *pool, Equity *play_scores,
                             const Equity *const *kept, int number_kept,
                             uint64_t lanes) {
  const int number_of_threads = sim->number_of_threads;
  CensusSimBuildWorker workers[CENSUS_SIM_MAX_THREADS];
  _Atomic int next_unit;
  atomic_init(&next_unit, 0);
  for (int thread_idx = 0; thread_idx < number_of_threads; thread_idx++) {
    workers[thread_idx] = (CensusSimBuildWorker){
        .sim = sim,
        .game = game,
        .pool = pool,
        .play_scores = play_scores,
        .kept = kept,
        .number_kept = number_kept,
        .lanes = lanes,
        .thread_index = thread_idx,
        .number_of_threads = number_of_threads,
        .next_unit = &next_unit,
    };
  }
  census_sim_run_phase(workers, number_of_threads, census_sim_fill_worker);
  census_sim_run_phase(workers, number_of_threads, census_sim_lanes_worker);
}

void census_sim_set_position(CensusSim *sim, const Game *game) {
  if (sim->position == NULL) {
    sim->position = game_duplicate(game);
  } else {
    game_copy(sim->position, game);
  }
  uint8_t pool[CENSUS_MAX_LETTERS];
  census_sim_get_pool(game, pool);
  const uint32_t size = census_lattice_get_size(sim->lattice);
  for (int group_idx = 0; group_idx < CENSUS_SIM_LANE_GROUPS; group_idx++) {
    if (sim->group_scores[group_idx] == NULL) {
      sim->group_scores[group_idx] = malloc_or_die(sizeof(Equity) * size);
    }
    census_sim_build(sim, game, pool, sim->group_scores[group_idx], NULL, 0,
                     sim->group_lanes[group_idx]);
  }
}

void census_sim_clear_position(CensusSim *sim) {
  game_destroy(sim->position);
  sim->position = NULL;
}

CensusSim *census_sim_create(const Game *game, int number_of_threads) {
  CensusSim *sim = calloc_or_die(1, sizeof(CensusSim));
  ErrorStack *error_stack = error_stack_create();
  sim->lattice = census_lattice_create(ld_get_size(game_get_ld(game)),
                                       RACK_SIZE, error_stack);
  if (sim->lattice == NULL) {
    log_fatal("cannot create a census lattice for this alphabet");
  }
  error_stack_destroy(error_stack);
  sim->add_table = census_add_table_create(sim->lattice, number_of_threads);
  census_sim_set_threads(sim, number_of_threads);
  // Each direction's lanes in bands of neighbors, since a move changes a few
  // neighboring lanes of each direction.
  const int bands = CENSUS_SIM_LANE_GROUPS / 2;
  for (int group_idx = 0; group_idx < CENSUS_SIM_LANE_GROUPS; group_idx++) {
    const int dir = group_idx / bands;
    const int band = group_idx % bands;
    for (int row = BOARD_DIM * band / bands;
         row < BOARD_DIM * (band + 1) / bands; row++) {
      sim->group_lanes[group_idx] |= (uint64_t)1 << (dir * BOARD_DIM + row);
    }
  }
  const uint32_t size = census_lattice_get_size(sim->lattice);
  sim->leave = malloc_or_die(sizeof(Equity) * size);
  sim->leave_max_over_subsets = malloc_or_die(sizeof(Equity) * size);
  sim->play_scores = malloc_or_die(sizeof(Equity) * size);
  sim->rack_best = malloc_or_die(sizeof(Equity) * size);
  return sim;
}

void census_sim_destroy(CensusSim *sim) {
  if (!sim) {
    return;
  }
  census_add_table_destroy(sim->add_table);
  census_lattice_destroy(sim->lattice);
  free(sim->leave);
  free(sim->leave_max_over_subsets);
  free(sim->play_scores);
  free(sim->rack_best);
  for (int group_idx = 0; group_idx < CENSUS_SIM_LANE_GROUPS; group_idx++) {
    free(sim->group_scores[group_idx]);
  }
  game_destroy(sim->position);
  free(sim);
}

void census_sim_set_threads(CensusSim *sim, int number_of_threads) {
  sim->number_of_threads = number_of_threads < CENSUS_SIM_MAX_THREADS
                               ? number_of_threads
                               : CENSUS_SIM_MAX_THREADS;
}

const CensusSimTimes *census_sim_get_times(const CensusSim *sim) {
  return &sim->times;
}

Equity census_sim_value_move(CensusSim *sim, const Game *game, const Move *move,
                             Game *scratch, Equity *expected_reply) {
  const int mover_index = game_get_player_on_turn_index(game);
  const Player *mover = game_get_player(game, mover_index);
  const KLV *opponent_klv =
      player_get_klv(game_get_player(game, 1 - mover_index));
  if (sim->leaves_klv != opponent_klv) {
    census_leaves_from_klv(sim->lattice, opponent_klv, sim->leave);
    census_max_over_subsets(sim->lattice, sim->add_table, sim->leave,
                            sim->leave_max_over_subsets);
    sim->leaves_klv = opponent_klv;
  }

  uint8_t pool[CENSUS_MAX_LETTERS];
  census_sim_get_pool(game, pool);
  const Rack *mover_rack = player_get_rack(mover);

  Rack leave_rack;
  rack_copy(&leave_rack, mover_rack);
  const Equity move_value =
      move_get_score(move) +
      get_leave_value_for_move(player_get_klv(mover), move, &leave_rack);

  Timer timer;
  ctimer_start(&timer);
  game_copy(scratch, game);
  play_move(move, scratch, NULL);
  if (sim->position == NULL) {
    census_sim_build(sim, scratch, pool, sim->play_scores, NULL, 0,
                     CENSUS_ALL_LANES);
  } else {
    // The groups of lanes the move leaves as they were keep their play
    // scores from the position; the rest are found again.
    const uint64_t changed =
        census_play_scores_changed_lanes(sim->position, scratch);
    uint64_t rebuild = 0;
    const Equity *kept[CENSUS_SIM_LANE_GROUPS];
    int number_kept = 0;
    for (int group_idx = 0; group_idx < CENSUS_SIM_LANE_GROUPS; group_idx++) {
      if ((sim->group_lanes[group_idx] & changed) != 0) {
        rebuild |= sim->group_lanes[group_idx];
      } else {
        kept[number_kept++] = sim->group_scores[group_idx];
      }
    }
    census_sim_build(sim, scratch, pool, sim->play_scores, kept, number_kept,
                     rebuild);
  }
  ctimer_stop(&timer);
  sim->times.play_scores_seconds += ctimer_elapsed_seconds(&timer);

  ctimer_start(&timer);
  const CensusApportionArgs args = {
      .lattice = sim->lattice,
      .add_table = sim->add_table,
      .play_scores = sim->play_scores,
      .leave = sim->leave,
      .leave_max_over_subsets = sim->leave_max_over_subsets,
      .unseen_tally = pool,
      .null_leave = false,
      .superset_sum = true,
      .scatter = true,
  };
  CensusSums total;
  census_expected_best(&args, sim->rack_best, &total, sim->number_of_threads);
  ctimer_stop(&timer);
  sim->times.expected_best_seconds += ctimer_elapsed_seconds(&timer);
  sim->times.number_of_moves++;

  const Equity reply =
      total.den > 0 ? (Equity)llround(total.num / total.den) : 0;
  if (expected_reply != NULL) {
    *expected_reply = reply;
  }
  return move_value - reply;
}
