#include "census_rollout_lab_test.h"

#include "../src/compat/ctime.h"
#include "../src/compat/memory_info.h"
#include "../src/def/board_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/players_data_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/sim_results.h"
#include "../src/impl/census.h"
#include "../src/impl/census_play_scores.h"
#include "../src/impl/census_rollout.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/util/io_util.h"
#include "census_rollout_lab.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  CRL_TEST_MAX_TURNS = 16,
  CRL_TEST_MAX_SIM_PLAYS = 64,
  CRL_TEST_MAX_PLIES = 8,
  CRL_TEST_TOPK = 4,
};

static int env_int(const char *name, int default_value) {
  const char *value = getenv(name);
  return value ? atoi(value) : default_value;
}

// A comma-separated list of integers from the environment, or the default.
static int env_int_list(const char *name, const char *default_value,
                        int *values, int max_values) {
  const char *value = getenv(name);
  char buffer[256];
  snprintf(buffer, sizeof(buffer), "%s", value ? value : default_value);
  int count = 0;
  for (const char *token = strtok(buffer, ","); token && count < max_values;
       token = strtok(NULL, ",")) {
    values[count++] = atoi(token);
  }
  return count;
}

typedef struct LabRun {
  const char *name;
  crl_mode_t mode;
  int variant;
  bool with_carry;
  // CRL_REPLAY_SKIPS | CRL_REPLAY_FLOOR, or 0 for both (see LAB_RUN_PARTS).
  int parts;
} LabRun;

// A run's replay parts: 0 means the default, both; -1 neither.
static int lab_run_parts(const LabRun *run) {
  if (run->parts == 0) {
    return CRL_REPLAY_SKIPS | CRL_REPLAY_FLOOR;
  }
  return run->parts < 0 ? 0 : run->parts;
}

static void add_stats(CrlPlyStats *sum, const CrlPlyStats *stats) {
  sum->queries += stats->queries;
  sum->eligible += stats->eligible;
  sum->generation_ns += stats->generation_ns;
  sum->mismatches += stats->mismatches;
  sum->replay_hits += stats->replay_hits;
  sum->incomplete += stats->incomplete;
  sum->lanes_with_anchors += stats->lanes_with_anchors;
  sum->hash_ns += stats->hash_ns;
  for (int board = 0; board < CRL_NUM_BOARDS; board++) {
    sum->dirty_lanes[board] += stats->dirty_lanes[board];
    sum->best_in_clean_lane[board] += stats->best_in_clean_lane[board];
    sum->self_check_violations[board] += stats->self_check_violations[board];
  }
  for (int variant = 0; variant < CRL_NUM_VARIANTS; variant++) {
    sum->variant_hits[variant] += stats->variant_hits[variant];
    sum->variant_searched_lanes[variant] +=
        stats->variant_searched_lanes[variant];
    sum->variant_searched_anchors[variant] +=
        stats->variant_searched_anchors[variant];
  }
  sum->anchors += stats->anchors;
  for (int board = 0; board < CRL_NUM_BOARDS; board++) {
    sum->clean_anchors_in_dirty_lanes[board] +=
        stats->clean_anchors_in_dirty_lanes[board];
    sum->best_in_known_anchor[board] += stats->best_in_known_anchor[board];
    sum->best_untouched[board] += stats->best_untouched[board];
    sum->anchor_self_check_violations[board] +=
        stats->anchor_self_check_violations[board];
  }
}

static double ratio(uint64_t num, uint64_t den) {
  return den ? (double)num / (double)den : 0.0;
}

static void print_record_stats(const CrlPlyStats *stats, int plies) {
  const char *board_names[CRL_NUM_BOARDS] = {"root", "cand"};
  printf("CRL   record: per rollout ply (0 = reply to the candidate)\n");
  printf("CRL   %-4s %8s %6s %7s", "ply", "queries", "elig", "anchrd");
  for (int board = 0; board < CRL_NUM_BOARDS; board++) {
    printf("  %s:dirty bestcln viol", board_names[board]);
  }
  printf("  hash_ns\n");
  for (int ply = 0; ply < plies; ply++) {
    const CrlPlyStats *s = &stats[ply];
    printf("CRL   %-4d %8llu %5.1f%% %7.2f", ply,
           (unsigned long long)s->queries, 100 * ratio(s->eligible, s->queries),
           ratio(s->lanes_with_anchors, s->eligible));
    for (int board = 0; board < CRL_NUM_BOARDS; board++) {
      printf("  %10.2f %6.1f%% %4llu",
             ratio(s->dirty_lanes[board], s->eligible),
             100 * ratio(s->best_in_clean_lane[board], s->eligible),
             (unsigned long long)s->self_check_violations[board]);
    }
    printf("  %7.0f\n", ratio((uint64_t)s->hash_ns, s->eligible));
  }
  printf("CRL   variant hit rate / lanes searched (of anchored), eligible "
         "queries\n");
  for (int board = 0; board < CRL_NUM_BOARDS; board++) {
    for (int topk_idx = 0; topk_idx < CRL_NUM_TOPK; topk_idx++) {
      const int variant = crl_variant(board, topk_idx);
      printf("CRL   %s k=%-2d", board_names[board], crl_topk_values[topk_idx]);
      for (int ply = 0; ply < plies; ply++) {
        const CrlPlyStats *s = &stats[ply];
        printf("  p%d %5.1f%% %5.2f", ply,
               100 * ratio(s->variant_hits[variant], s->eligible),
               ratio(s->variant_searched_lanes[variant], s->eligible));
      }
      printf("\n");
    }
  }
  printf("CRL   anchors: per ply, eligible queries\n");
  printf("CRL   %-4s %7s", "ply", "anchors");
  for (int board = 0; board < CRL_NUM_BOARDS; board++) {
    printf("  %s:clnanc bestanc untouch viol  lanes:anc  anch:anc",
           board_names[board]);
  }
  printf("\n");
  for (int ply = 0; ply < plies; ply++) {
    const CrlPlyStats *s = &stats[ply];
    printf("CRL   %-4d %7.2f", ply, ratio(s->anchors, s->eligible));
    for (int board = 0; board < CRL_NUM_BOARDS; board++) {
      printf(
          "  %11.2f %6.1f%% %6.1f%% %4llu %10.2f %9.2f",
          ratio(s->clean_anchors_in_dirty_lanes[board], s->eligible),
          100 * ratio(s->best_in_known_anchor[board], s->eligible),
          100 * ratio(s->best_untouched[board], s->eligible),
          (unsigned long long)s->anchor_self_check_violations[board],
          ratio(
              s->variant_searched_anchors[crl_variant(board, CRL_NUM_TOPK - 1)],
              s->eligible),
          ratio(s->variant_searched_anchors[crl_anchor_variant(board)],
                s->eligible));
    }
    printf("\n");
  }
}

static void print_timing_row(const LabRun *run, const CrlPlyStats *stats,
                             int plies, int64_t wall_ns,
                             double baseline_seconds) {
  int64_t total_ns = 0;
  uint64_t total_queries = 0;
  uint64_t mismatches = 0;
  for (int ply = 0; ply < plies; ply++) {
    total_ns += stats[ply].generation_ns;
    total_queries += stats[ply].queries;
    mismatches += stats[ply].mismatches;
  }
  const double seconds = (double)total_ns / 1e9;
  printf("CRL   %-22s gen %7.3f s (%5.2fx) %6.2f us/q", run->name, seconds,
         baseline_seconds > 0 ? baseline_seconds / seconds : 1.0,
         ratio((uint64_t)total_ns, total_queries) / 1000.0);
  for (int ply = 0; ply < plies; ply++) {
    printf("  p%d %6.2f", ply,
           ratio((uint64_t)stats[ply].generation_ns, stats[ply].queries) /
               1000.0);
  }
  printf("  wall %6.2f s  mism %llu\n", (double)wall_ns / 1e9,
         (unsigned long long)mismatches);
}

// The census of one lane: its play scores over the pool, then each full
// rack's best in it. Returns the seconds of each stage in seconds[0..1].
static void time_lane_census(const Game *game, const CensusLattice *lattice,
                             const uint8_t *pool, const Equity *leave,
                             Equity *play_scores, Equity *rack_best,
                             uint64_t lane_mask, double *seconds) {
  const uint32_t size = census_lattice_get_size(lattice);
  const uint32_t full_start = census_lattice_get_full_rack_start(lattice);
  int64_t start = ctimer_monotonic_ns();
  for (uint32_t index = 0; index < size; index++) {
    play_scores[index] = CENSUS_UNPLAYABLE;
  }
  census_play_scores_build_lanes(game, lattice, pool, play_scores, lane_mask);
  int64_t end = ctimer_monotonic_ns();
  seconds[0] += (double)(end - start) / 1e9;
  start = end;
  for (uint32_t index = full_start; index < size; index++) {
    rack_best[index] = CENSUS_UNPLAYABLE;
  }
  census_scatter_rack_best(lattice, play_scores, leave, pool, rack_best);
  end = ctimer_monotonic_ns();
  seconds[1] += (double)(end - start) / 1e9;
}

// Raises each full rack's top-k (value, lane) list by its best in lane.
static void merge_lane_into_topk(const Equity *rack_best, uint32_t full_start,
                                 uint32_t full_count, int lane, Equity *topk,
                                 uint8_t *topk_lanes) {
  for (uint32_t rack_idx = 0; rack_idx < full_count; rack_idx++) {
    const Equity value = rack_best[full_start + rack_idx];
    Equity *values = &topk[(size_t)rack_idx * CRL_TEST_TOPK];
    if (value <= values[CRL_TEST_TOPK - 1]) {
      continue;
    }
    uint8_t *lanes = &topk_lanes[(size_t)rack_idx * CRL_TEST_TOPK];
    int pos = CRL_TEST_TOPK - 1;
    while (pos > 0 && values[pos - 1] < value) {
      values[pos] = values[pos - 1];
      lanes[pos] = lanes[pos - 1];
      pos--;
    }
    values[pos] = value;
    lanes[pos] = (uint8_t)lane;
  }
}

static uint64_t lanes_with_anchors(const Game *game) {
  uint8_t row_anchors[2 * BOARD_DIM];
  board_load_number_of_row_anchors_cache(game_get_board(game), row_anchors);
  uint64_t lanes = 0;
  for (int lane = 0; lane < 2 * BOARD_DIM; lane++) {
    if (row_anchors[lane] != 0) {
      lanes |= (uint64_t)1 << lane;
    }
  }
  return lanes;
}

// The top number_of_candidates moves by static equity, best first.
static int top_static_moves(Game *game, Move *candidates,
                            int number_of_candidates) {
  MoveList *move_list = move_list_create(1 << 16);
  const MoveGenArgs args = {.game = game,
                            .move_list = move_list,
                            .move_record_type = MOVE_RECORD_ALL,
                            .move_sort_type = MOVE_SORT_EQUITY,
                            .override_kwg = NULL,
                            .eq_margin_movegen = 0,
                            .target_equity = EQUITY_MAX_VALUE};
  generate_moves(&args);
  move_list_sort_moves(move_list);
  int count = move_list_get_count(move_list);
  if (count > number_of_candidates) {
    count = number_of_candidates;
  }
  for (int move_idx = 0; move_idx < count; move_idx++) {
    move_copy(&candidates[move_idx], move_list_get_move(move_list, move_idx));
  }
  move_list_destroy(move_list);
  return count;
}

// What the lane-by-lane census costs: on the position's board, every lane
// with anchors, and on the boards after its best few moves, the lanes each
// changes.
static void time_lane_census_build(Game *game, int number_of_candidates) {
  const LetterDistribution *ld = game_get_ld(game);
  ErrorStack *error_stack = error_stack_create();
  CensusLattice *lattice =
      census_lattice_create(ld_get_size(ld), RACK_SIZE, error_stack);
  error_stack_destroy(error_stack);
  const uint32_t size = census_lattice_get_size(lattice);
  const uint32_t full_start = census_lattice_get_full_rack_start(lattice);
  const uint32_t full_count = size - full_start;
  Equity *leave = malloc_or_die(sizeof(Equity) * size);
  census_leaves_from_klv(lattice, player_get_klv(game_get_player(game, 0)),
                         leave);
  Equity *play_scores = malloc_or_die(sizeof(Equity) * size);
  Equity *rack_best = malloc_or_die(sizeof(Equity) * size);
  Equity *topk = malloc_or_die(sizeof(Equity) * full_count * CRL_TEST_TOPK);
  uint8_t *topk_lanes = malloc_or_die(full_count * CRL_TEST_TOPK);
  for (size_t idx = 0; idx < (size_t)full_count * CRL_TEST_TOPK; idx++) {
    topk[idx] = CENSUS_UNPLAYABLE;
    topk_lanes[idx] = 0xFF;
  }
  uint8_t pool[CENSUS_MAX_LETTERS];
  census_get_unseen_tally(game, pool);
  int pool_size = 0;
  for (int ml = 0; ml < ld_get_size(ld); ml++) {
    pool_size += pool[ml];
  }

  // The whole board at once, as census_sim builds it, for reference.
  double whole[2] = {0, 0};
  time_lane_census(game, lattice, pool, leave, play_scores, rack_best,
                   CENSUS_ALL_LANES, whole);

  const uint64_t anchored = lanes_with_anchors(game);
  double lanes[2] = {0, 0};
  double merge_seconds = 0;
  double max_lane_seconds = 0;
  for (int lane = 0; lane < 2 * BOARD_DIM; lane++) {
    if ((anchored & ((uint64_t)1 << lane)) == 0) {
      continue;
    }
    double lane_seconds[2] = {0, 0};
    time_lane_census(game, lattice, pool, leave, play_scores, rack_best,
                     (uint64_t)1 << lane, lane_seconds);
    lanes[0] += lane_seconds[0];
    lanes[1] += lane_seconds[1];
    if (lane_seconds[0] + lane_seconds[1] > max_lane_seconds) {
      max_lane_seconds = lane_seconds[0] + lane_seconds[1];
    }
    const int64_t start = ctimer_monotonic_ns();
    merge_lane_into_topk(rack_best, full_start, full_count, lane, topk,
                         topk_lanes);
    merge_seconds += (double)(ctimer_monotonic_ns() - start) / 1e9;
  }
  printf("CRL build: bag %d pool %d, %d lanes with anchors; whole board: "
         "plays %.3f s + rack bests %.3f s; by lane: plays %.3f s + rack "
         "bests %.3f s + top-%d merge %.3f s = %.3f s (slowest lane %.3f s)\n",
         bag_get_letters(game_get_bag(game)), pool_size,
         __builtin_popcountll(anchored), whole[0], whole[1], lanes[0], lanes[1],
         CRL_TEST_TOPK, merge_seconds, lanes[0] + lanes[1] + merge_seconds,
         max_lane_seconds);

  Move candidates[16];
  const int count = top_static_moves(
      game, candidates, number_of_candidates < 16 ? number_of_candidates : 16);
  Game *after = game_duplicate(game);
  for (int move_idx = 0; move_idx < count; move_idx++) {
    game_copy(after, game);
    play_move(&candidates[move_idx], after, NULL);
    const uint64_t changed = census_play_scores_changed_lanes(game, after) &
                             lanes_with_anchors(after);
    uint8_t after_pool[CENSUS_MAX_LETTERS];
    census_get_unseen_tally(after, after_pool);
    double changed_seconds[2] = {0, 0};
    for (uint64_t remaining = changed; remaining != 0;
         remaining &= remaining - 1) {
      time_lane_census(after, lattice, after_pool, leave, play_scores,
                       rack_best, (uint64_t)1 << __builtin_ctzll(remaining),
                       changed_seconds);
    }
    printf("CRL build:   candidate %d changes %d anchored lanes: plays %.3f s "
           "+ rack bests %.3f s\n",
           move_idx, __builtin_popcountll(changed), changed_seconds[0],
           changed_seconds[1]);
  }
  game_destroy(after);
  free(topk_lanes);
  free(topk);
  free(rack_best);
  free(play_scores);
  free(leave);
  census_lattice_destroy(lattice);
}

// The lazy census: for every placed multiset P, the units (lanes, or anchors
// of lanes) it is played in and its best score in each, with no rack-best
// scatter; a rack's best in each unit is gathered when the rack is drawn.
enum {
  UNIT_SCORES_MAX_UNITS = 2 * BOARD_DIM * BOARD_DIM,
};

typedef struct UnitScores {
  int number_of_units;
  // Each unit's lane and anchor column, -1 for a whole lane.
  int8_t unit_lanes[UNIT_SCORES_MAX_UNITS];
  int8_t unit_cols[UNIT_SCORES_MAX_UNITS];
  uint32_t *offsets;
  uint16_t *units;
  Equity *scores;
  uint32_t number_of_entries;
} UnitScores;

static const Square *board_lane_squares(const Game *game, int lane) {
  const int cross_index = board_get_cross_set_index(
      game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG), 0);
  return board_get_row_cache(
      board_get_readonly_lanes(game_get_board(game), cross_index),
      lane % BOARD_DIM, lane / BOARD_DIM);
}

static uint16_t board_lane_anchor_cols(const Game *game, int lane) {
  const Square *squares = board_lane_squares(game, lane);
  uint16_t anchor_cols = 0;
  for (int col = 0; col < BOARD_DIM; col++) {
    if (square_get_anchor(&squares[col])) {
      anchor_cols |= (uint16_t)(1U << col);
    }
  }
  return anchor_cols;
}

// by_anchor: a unit for each anchor of each lane with anchors, else one for
// each such lane.
static UnitScores *unit_scores_create(const Game *game,
                                      const CensusLattice *lattice,
                                      const uint8_t *pool, bool by_anchor,
                                      Equity *play_scores,
                                      double *build_seconds) {
  const uint32_t size = census_lattice_get_size(lattice);
  const int64_t start = ctimer_monotonic_ns();
  UnitScores *unit_scores = calloc_or_die(1, sizeof(UnitScores));
  unit_scores->offsets = calloc_or_die((size_t)size + 1, sizeof(uint32_t));
  uint32_t *unit_entries[UNIT_SCORES_MAX_UNITS] = {0};
  Equity *unit_entry_scores[UNIT_SCORES_MAX_UNITS] = {0};
  uint32_t unit_counts[UNIT_SCORES_MAX_UNITS] = {0};
  for (int lane = 0; lane < 2 * BOARD_DIM; lane++) {
    const uint16_t anchor_cols = board_lane_anchor_cols(game, lane);
    if (anchor_cols == 0) {
      continue;
    }
    for (uint32_t remaining = by_anchor ? anchor_cols : 1; remaining != 0;
         remaining &= remaining - 1) {
      const int unit = unit_scores->number_of_units++;
      const int col = by_anchor ? __builtin_ctz(remaining) : -1;
      unit_scores->unit_lanes[unit] = (int8_t)lane;
      unit_scores->unit_cols[unit] = (int8_t)col;
      for (uint32_t index = 0; index < size; index++) {
        play_scores[index] = CENSUS_UNPLAYABLE;
      }
      if (by_anchor) {
        census_play_scores_build_anchor(game, lattice, pool, play_scores, NULL,
                                        lane, col);
      } else {
        census_play_scores_build_lanes(game, lattice, pool, play_scores,
                                       (uint64_t)1 << lane);
      }
      uint32_t count = 0;
      for (uint32_t index = 0; index < size; index++) {
        count += play_scores[index] > 0;
      }
      unit_entries[unit] = malloc_or_die(sizeof(uint32_t) * (count + 1));
      unit_entry_scores[unit] = malloc_or_die(sizeof(Equity) * (count + 1));
      count = 0;
      for (uint32_t index = 0; index < size; index++) {
        if (play_scores[index] > 0) {
          unit_entries[unit][count] = index;
          unit_entry_scores[unit][count] = play_scores[index];
          unit_scores->offsets[index + 1]++;
          count++;
        }
      }
      unit_counts[unit] = count;
      unit_scores->number_of_entries += count;
    }
  }
  for (uint32_t index = 0; index < size; index++) {
    unit_scores->offsets[index + 1] += unit_scores->offsets[index];
  }
  unit_scores->units =
      malloc_or_die(sizeof(uint16_t) * (unit_scores->number_of_entries + 1));
  unit_scores->scores =
      malloc_or_die(sizeof(Equity) * (unit_scores->number_of_entries + 1));
  uint32_t *fill = malloc_or_die(sizeof(uint32_t) * size);
  memcpy(fill, unit_scores->offsets, sizeof(uint32_t) * size);
  for (int unit = 0; unit < unit_scores->number_of_units; unit++) {
    for (uint32_t entry = 0; entry < unit_counts[unit]; entry++) {
      const uint32_t slot = fill[unit_entries[unit][entry]]++;
      unit_scores->units[slot] = (uint16_t)unit;
      unit_scores->scores[slot] = unit_entry_scores[unit][entry];
    }
    free(unit_entries[unit]);
    free(unit_entry_scores[unit]);
  }
  free(fill);
  *build_seconds = (double)(ctimer_monotonic_ns() - start) / 1e9;
  return unit_scores;
}

static void unit_scores_destroy(UnitScores *unit_scores) {
  free(unit_scores->offsets);
  free(unit_scores->units);
  free(unit_scores->scores);
  free(unit_scores);
}

// Every unit's best score plus leave for the rack: the best over the rack's
// splits into a played multiset P and a kept one K. Returns the entries read.
static int unit_scores_gather(const UnitScores *unit_scores,
                              const CensusLattice *lattice, const Equity *leave,
                              const MachineLetter *letters,
                              const uint8_t *counts, int number_of_letters,
                              Equity *unit_bests) {
  for (int unit = 0; unit < unit_scores->number_of_units; unit++) {
    unit_bests[unit] = CENSUS_UNPLAYABLE;
  }
  uint8_t played[RACK_SIZE] = {0};
  uint8_t kept[RACK_SIZE];
  for (int letter_idx = 0; letter_idx < number_of_letters; letter_idx++) {
    kept[letter_idx] = counts[letter_idx];
  }
  int entries_read = 0;
  while (true) {
    // The next split: an odometer over the copies of each letter played.
    int letter_idx = 0;
    while (letter_idx < number_of_letters &&
           played[letter_idx] == counts[letter_idx]) {
      played[letter_idx] = 0;
      kept[letter_idx] = counts[letter_idx];
      letter_idx++;
    }
    if (letter_idx == number_of_letters) {
      break;
    }
    played[letter_idx]++;
    kept[letter_idx]--;
    const uint32_t played_index =
        census_lattice_rank_sparse(lattice, letters, played, number_of_letters);
    const uint32_t first = unit_scores->offsets[played_index];
    const uint32_t last = unit_scores->offsets[played_index + 1];
    if (first == last) {
      continue;
    }
    const Equity leave_value = leave[census_lattice_rank_sparse(
        lattice, letters, kept, number_of_letters)];
    for (uint32_t entry = first; entry < last; entry++) {
      const Equity value = unit_scores->scores[entry] + leave_value;
      const int unit = unit_scores->units[entry];
      if (value > unit_bests[unit]) {
        unit_bests[unit] = value;
      }
    }
    entries_read += (int)(last - first);
  }
  return entries_read;
}

// The unit of a lane or anchor in unit_scores, -1 if none.
static int unit_scores_find(const UnitScores *unit_scores, int lane, int col) {
  for (int unit = 0; unit < unit_scores->number_of_units; unit++) {
    if (unit_scores->unit_lanes[unit] == lane &&
        unit_scores->unit_cols[unit] == col) {
      return unit;
    }
  }
  return -1;
}

// The gather's cost for lanes and for anchors, and a check of both against
// every play of the same racks on the same board: each rack's best must match
// movegen's, and no unit's best may be below that of its plays of two or more
// tiles (the census counts a play of one tile in both of its lanes).
static void time_census_gather(const Game *game, int number_of_racks) {
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
  ErrorStack *error_stack = error_stack_create();
  CensusLattice *lattice =
      census_lattice_create(ld_size, RACK_SIZE, error_stack);
  error_stack_destroy(error_stack);
  const uint32_t size = census_lattice_get_size(lattice);
  Equity *leave = malloc_or_die(sizeof(Equity) * size);
  census_leaves_from_klv(lattice, player_get_klv(game_get_player(game, 0)),
                         leave);
  Equity *play_scores = malloc_or_die(sizeof(Equity) * size);
  uint8_t pool[CENSUS_MAX_LETTERS];
  census_get_unseen_tally(game, pool);

  // Racks drawn from the pool, as a rollout draws them.
  MachineLetter tiles[128];
  int number_of_tiles = 0;
  for (int ml = 0; ml < ld_size; ml++) {
    for (int copy = 0; copy < pool[ml]; copy++) {
      tiles[number_of_tiles++] = (MachineLetter)ml;
    }
  }
  uint64_t state = UINT64_C(0x9E3779B97F4A7C15);
  MachineLetter *rack_letters =
      malloc_or_die(sizeof(MachineLetter) * RACK_SIZE * number_of_racks);
  uint8_t *rack_counts = malloc_or_die(RACK_SIZE * (size_t)number_of_racks);
  int *rack_sizes = malloc_or_die(sizeof(int) * (size_t)number_of_racks);
  for (int rack_idx = 0; rack_idx < number_of_racks; rack_idx++) {
    for (int draw = 0; draw < RACK_SIZE; draw++) {
      state = state * UINT64_C(6364136223846793005) + 1;
      const int pick =
          draw + (int)((state >> 33) % (uint64_t)(number_of_tiles - draw));
      const MachineLetter swap = tiles[draw];
      tiles[draw] = tiles[pick];
      tiles[pick] = swap;
    }
    uint8_t tally[CENSUS_MAX_LETTERS] = {0};
    for (int draw = 0; draw < RACK_SIZE; draw++) {
      tally[tiles[draw]]++;
    }
    int distinct = 0;
    for (int ml = 0; ml < ld_size; ml++) {
      if (tally[ml] > 0) {
        rack_letters[rack_idx * RACK_SIZE + distinct] = (MachineLetter)ml;
        rack_counts[rack_idx * RACK_SIZE + distinct] = tally[ml];
        distinct++;
      }
    }
    rack_sizes[rack_idx] = distinct;
  }

  Game *checked = game_duplicate(game);
  MoveList *move_list = move_list_create(1 << 17);
  const int player_index = game_get_player_on_turn_index(game);
  const int number_checked = number_of_racks < 300 ? number_of_racks : 300;
  uint16_t anchor_cols[2 * BOARD_DIM];
  for (int lane = 0; lane < 2 * BOARD_DIM; lane++) {
    anchor_cols[lane] = board_lane_anchor_cols(game, lane);
  }
  for (int by_anchor = 0; by_anchor < 2; by_anchor++) {
    double build_seconds = 0;
    UnitScores *unit_scores = unit_scores_create(
        game, lattice, pool, by_anchor != 0, play_scores, &build_seconds);
    Equity unit_bests[UNIT_SCORES_MAX_UNITS];
    uint64_t entries = 0;
    Equity checksum = 0;
    const int64_t start = ctimer_monotonic_ns();
    for (int rack_idx = 0; rack_idx < number_of_racks; rack_idx++) {
      entries += (uint64_t)unit_scores_gather(
          unit_scores, lattice, leave, &rack_letters[rack_idx * RACK_SIZE],
          &rack_counts[rack_idx * RACK_SIZE], rack_sizes[rack_idx], unit_bests);
      checksum += unit_bests[rack_idx % unit_scores->number_of_units];
    }
    const double gather_ns =
        (double)(ctimer_monotonic_ns() - start) / number_of_racks;

    int best_mismatches = 0;
    int unit_mismatches = 0;
    int units_compared = 0;
    for (int rack_idx = 0; rack_idx < number_checked; rack_idx++) {
      Rack *rack = player_get_rack(game_get_player(checked, player_index));
      rack_reset(rack);
      for (int letter_idx = 0; letter_idx < rack_sizes[rack_idx];
           letter_idx++) {
        rack_add_letters(rack, rack_letters[rack_idx * RACK_SIZE + letter_idx],
                         rack_counts[rack_idx * RACK_SIZE + letter_idx]);
      }
      unit_scores_gather(
          unit_scores, lattice, leave, &rack_letters[rack_idx * RACK_SIZE],
          &rack_counts[rack_idx * RACK_SIZE], rack_sizes[rack_idx], unit_bests);
      const MoveGenArgs args = {.game = checked,
                                .move_list = move_list,
                                .move_record_type = MOVE_RECORD_ALL,
                                .move_sort_type = MOVE_SORT_EQUITY,
                                .override_kwg = NULL,
                                .eq_margin_movegen = 0,
                                .target_equity = EQUITY_MAX_VALUE};
      generate_moves(&args);
      Equity movegen_units[UNIT_SCORES_MAX_UNITS];
      for (int unit = 0; unit < unit_scores->number_of_units; unit++) {
        movegen_units[unit] = CENSUS_UNPLAYABLE;
      }
      Equity movegen_best = CENSUS_UNPLAYABLE;
      for (int move_idx = 0; move_idx < move_list_get_count(move_list);
           move_idx++) {
        const Move *move = move_list_get_move(move_list, move_idx);
        if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE ||
            move_get_score(move) <= 0) {
          continue;
        }
        if (move_get_equity(move) > movegen_best) {
          movegen_best = move_get_equity(move);
        }
        if (move_get_tiles_played(move) < 2) {
          continue;
        }
        const int dir = move_get_dir(move);
        const bool vertical = board_is_dir_vertical(dir);
        const int lane =
            BOARD_DIM * dir +
            (vertical ? move_get_col_start(move) : move_get_row_start(move));
        const int move_start =
            vertical ? move_get_row_start(move) : move_get_col_start(move);
        const int move_end = move_start + move_get_tiles_length(move) - 1;
        int col = -1;
        if (by_anchor) {
          const uint32_t span = (((uint32_t)1 << (move_end + 1)) - 1) &
                                ~(((uint32_t)1 << move_start) - 1);
          const uint32_t held = anchor_cols[lane] & span;
          col = held != 0 ? __builtin_ctz(held) : -1;
        }
        const int unit = unit_scores_find(unit_scores, lane, col);
        if (unit < 0) {
          unit_mismatches++;
          continue;
        }
        if (move_get_equity(move) > movegen_units[unit]) {
          movegen_units[unit] = move_get_equity(move);
        }
      }
      Equity gathered_best = CENSUS_UNPLAYABLE;
      for (int unit = 0; unit < unit_scores->number_of_units; unit++) {
        if (unit_bests[unit] > gathered_best) {
          gathered_best = unit_bests[unit];
        }
        if (movegen_units[unit] != CENSUS_UNPLAYABLE) {
          units_compared++;
          if (unit_bests[unit] < movegen_units[unit]) {
            unit_mismatches++;
          }
        }
      }
      if (gathered_best != movegen_best) {
        best_mismatches++;
      }
    }
    printf("CRL gather: bag %d, %s: %d units, scores built in %.3f s (%u "
           "entries, %.1f MB); gather %.0f ns a rack, %.0f entries a rack; "
           "check of %d racks: best mismatches %d, units below movegen's %d "
           "of %d (checksum %d)\n",
           bag_get_letters(game_get_bag(game)), by_anchor ? "anchors" : "lanes",
           unit_scores->number_of_units, build_seconds,
           unit_scores->number_of_entries,
           (double)(sizeof(uint32_t) * (size + 1) +
                    (sizeof(Equity) + sizeof(uint16_t)) *
                        unit_scores->number_of_entries) /
               1e6,
           gather_ns, (double)entries / number_of_racks, number_checked,
           best_mismatches, unit_mismatches, units_compared, (int)checksum);
    unit_scores_destroy(unit_scores);
  }
  move_list_destroy(move_list);
  game_destroy(checked);
  free(rack_sizes);
  free(rack_counts);
  free(rack_letters);
  free(play_scores);
  free(leave);
  census_lattice_destroy(lattice);
}

// End to end: the same sims timed with the carry alone and with the lazy
// per-anchor census steering the carry, the census's builds and gathers
// included, and a verified run that checks every census-steered generation
// chooses plain movegen's move.
static void time_lazy_census(Config *config, const int *turns,
                             int number_of_turns, const int *plies_list,
                             int number_of_plies, int iterations, int max_ply,
                             bool low_bag) {
  char command[256];
  char sim_command[64];
  snprintf(sim_command, sizeof(sim_command), "sim -seed 7 -threads %d",
           env_int("CRL_THREADS", 1));
  for (int plies_idx = 0; plies_idx < number_of_plies; plies_idx++) {
    const int plies = plies_list[plies_idx];
    snprintf(command, sizeof(command), "set -plies %d -it %d", plies,
             iterations);
    load_and_exec_config_or_die(config, command);
    printf("CRL e2e: plies %d, %d iterations, census at rollout plies 0-%d, "
           "bags of %s\n",
           plies, iterations, max_ply,
           low_bag ? "1 tile or more" : "13 tiles or more");
    printf("CRL e2e: %-4s %-4s %9s %9s %9s %9s %9s %9s %8s\n", "turn", "bag",
           "carry s", "lazy s", "speedup", "gen", "builds", "served", "mism");
    double carry_total = 0;
    double lazy_total = 0;
    for (int turn_idx = 0; turn_idx < number_of_turns; turn_idx++) {
      snprintf(command, sizeof(command), "goto %d", turns[turn_idx]);
      load_and_exec_config_or_die(config, command);
      load_and_exec_config_or_die(config, "gen");
      const int bag = bag_get_letters(game_get_bag(config_get_game(config)));
      double seconds[2] = {0, 0};
      int64_t generation_ns = 0;
      uint64_t served = 0;
      uint64_t queries = 0;
      CrlLazyStats lazy_stats = {0};
      // A warm-up, then carry and lazy, then the verified lazy run; long
      // runs can leave out the warm-up (CRL_WARMUP=0) and the verified run
      // (CRL_VERIFY=0).
      const int number_of_runs = env_int("CRL_PLAIN", 0) ? 6 : 4;
      for (int run = 0; run < number_of_runs; run++) {
        if (run == 5) {
          // The carry again, last: a control for the order of the runs.
          crl_set_mode(CRL_MODE_TIME_CARRY, 0, false);
          crl_clear_records();
          crl_new_sim();
          crl_reset_stats();
          const int64_t start = ctimer_monotonic_ns();
          load_and_exec_config_or_die(config, "sim -seed 7");
          const double elapsed = (double)(ctimer_monotonic_ns() - start) / 1e9;
          crl_set_mode(CRL_MODE_OFF, 0, false);
          printf("CRL e2e: %4d carry again %.3f s (first run %.3f s)\n",
                 turns[turn_idx], elapsed, seconds[0]);
          continue;
        }
        if (run == 4) {
          // Plain movegen, no carry, for scale.
          crl_set_mode(CRL_MODE_TIME_PLAIN, 0, false);
          crl_clear_records();
          crl_new_sim();
          crl_reset_stats();
          const int64_t start = ctimer_monotonic_ns();
          load_and_exec_config_or_die(config, "sim -seed 7");
          const double elapsed = (double)(ctimer_monotonic_ns() - start) / 1e9;
          crl_set_mode(CRL_MODE_OFF, 0, false);
          printf("CRL e2e: %4d plain (no carry) %.3f s: carry %.2fx, lazy "
                 "%.2fx of it\n",
                 turns[turn_idx], elapsed, elapsed / seconds[0],
                 elapsed / seconds[1]);
          continue;
        }
        if ((run == 0 && !env_int("CRL_WARMUP", 1)) ||
            (run == 3 && !env_int("CRL_VERIFY", 1))) {
          if (run == 3) {
            printf("CRL e2e: %4d %4d %9.3f %9.3f %8.2fx %8.3fs %8.3fs %8.1f%% "
                   "%8s\n",
                   turns[turn_idx], bag, seconds[0], seconds[1],
                   seconds[0] / seconds[1], (double)generation_ns / 1e9,
                   (double)lazy_stats.build_ns / 1e9,
                   100.0 * (double)served / (double)(queries ? queries : 1),
                   "-");
            printf("CRL e2e: root bests cached for %.1f%% of racks\n",
                   100.0 * (double)lazy_stats.root_gather_hits /
                       (double)(lazy_stats.root_gather_lookups
                                    ? lazy_stats.root_gather_lookups
                                    : 1));
          }
          continue;
        }
        const bool lazy_run = run == 2 || run == 3;
        crl_set_mode(lazy_run ? CRL_MODE_LAZY : CRL_MODE_TIME_CARRY, 0, false);
        crl_set_lazy_options(max_ply, run == 3, low_bag);
        crl_clear_records();
        crl_new_sim();
        crl_reset_stats();
        const int64_t start = ctimer_monotonic_ns();
        load_and_exec_config_or_die(config, sim_command);
        const double elapsed = (double)(ctimer_monotonic_ns() - start) / 1e9;
        crl_set_mode(CRL_MODE_OFF, 0, false);
        if (run == 1) {
          seconds[0] = elapsed;
        } else if (run == 2) {
          seconds[1] = elapsed;
          lazy_stats = *crl_get_lazy_stats();
          for (int ply = 0; ply < CRL_MAX_PLIES; ply++) {
            const CrlPlyStats *stats = crl_get_ply_stats(ply);
            generation_ns += stats->generation_ns;
            served += stats->replay_hits;
            queries += stats->queries;
          }
        } else if (run == 3) {
          uint64_t mismatches = 0;
          uint64_t plain_vs_exhaustive = 0;
          uint64_t census_vs_exhaustive = 0;
          uint64_t anchors_checked = 0;
          uint64_t anchor_value_violations = 0;
          for (int ply = 0; ply < CRL_MAX_PLIES; ply++) {
            anchors_checked += crl_get_ply_stats(ply)->anchors_checked;
            anchor_value_violations +=
                crl_get_ply_stats(ply)->anchor_value_violations;
            mismatches += crl_get_ply_stats(ply)->mismatches;
            plain_vs_exhaustive += crl_get_ply_stats(ply)->plain_vs_exhaustive;
            census_vs_exhaustive +=
                crl_get_ply_stats(ply)->census_vs_exhaustive;
          }
          printf("CRL e2e: verified against every play: plain differs %llu, "
                 "census-steered differs %llu; anchor values out of bounds "
                 "%llu of %llu\n",
                 (unsigned long long)plain_vs_exhaustive,
                 (unsigned long long)census_vs_exhaustive,
                 (unsigned long long)anchor_value_violations,
                 (unsigned long long)anchors_checked);
          printf("CRL e2e: root bests cached for %.1f%% of racks\n",
                 100.0 * (double)lazy_stats.root_gather_hits /
                     (double)(lazy_stats.root_gather_lookups
                                  ? lazy_stats.root_gather_lookups
                                  : 1));
          printf("CRL e2e: %4d %4d %9.3f %9.3f %8.2fx %8.3fs %8.3fs %8.1f%% "
                 "%8llu\n",
                 turns[turn_idx], bag, seconds[0], seconds[1],
                 seconds[0] / seconds[1], (double)generation_ns / 1e9,
                 (double)lazy_stats.build_ns / 1e9,
                 100.0 * (double)served / (double)(queries ? queries : 1),
                 (unsigned long long)mismatches);
        }
      }
      carry_total += seconds[0];
      lazy_total += seconds[1];
    }
    printf("CRL e2e: total carry %.3f s, lazy %.3f s: %.2fx\n", carry_total,
           lazy_total, carry_total / lazy_total);
    fflush(stdout);
  }
}

// Iterations completed in a fixed time on every thread, as a timed game's
// move gets: the carry (the sim as it ships), plain movegen, the carry with
// more buckets, and the lazy census steering the carry and plain movegen.
typedef struct TimedVariant {
  const char *name;
  crl_mode_t mode;
  bool lazy_carry;
  int carry_buckets;
} TimedVariant;

static void time_timed_sims(Config *config, const int *turns,
                            int number_of_turns, const int *plies_list,
                            int number_of_plies, double seconds, int threads,
                            int big_buckets) {
  const TimedVariant variants[] = {
      {"carry", CRL_MODE_OFF, false, 0},
      {"plain", CRL_MODE_TIME_PLAIN, false, 0},
      {"carry, more buckets", CRL_MODE_OFF, false, big_buckets},
      {"lazy + carry", CRL_MODE_LAZY, true, 0},
      {"lazy + plain", CRL_MODE_LAZY, false, 0},
      // A control for drift (heat, other load) across a position's runs.
      {"carry (again)", CRL_MODE_OFF, false, 0},
  };
  enum { NUMBER_OF_VARIANTS = sizeof(variants) / sizeof(variants[0]) };
  char command[256];
  for (int plies_idx = 0; plies_idx < number_of_plies; plies_idx++) {
    snprintf(command, sizeof(command), "set -plies %d -it 1000000000",
             plies_list[plies_idx]);
    load_and_exec_config_or_die(config, command);
    printf("CRL timed: plies %d, %.0f s on %d threads, carry buckets %d / "
           "%d\n",
           plies_list[plies_idx], seconds, threads, SIM_BEST_MOVE_CARRY_BUCKETS,
           big_buckets);
    printf("CRL timed: %-4s %-4s", "turn", "bag");
    for (int variant_idx = 0; variant_idx < NUMBER_OF_VARIANTS; variant_idx++) {
      printf(" %22s", variants[variant_idx].name);
    }
    printf("\n");
    double totals[NUMBER_OF_VARIANTS] = {0};
    for (int turn_idx = 0; turn_idx < number_of_turns; turn_idx++) {
      snprintf(command, sizeof(command), "goto %d", turns[turn_idx]);
      load_and_exec_config_or_die(config, command);
      load_and_exec_config_or_die(config, "gen");
      const int bag = bag_get_letters(game_get_bag(config_get_game(config)));
      uint64_t iterations[NUMBER_OF_VARIANTS];
      for (int variant_idx = 0; variant_idx < NUMBER_OF_VARIANTS;
           variant_idx++) {
        const TimedVariant *variant = &variants[variant_idx];
        crl_carry_buckets = variant->carry_buckets;
        crl_set_mode(variant->mode, 0, false);
        crl_set_lazy_options(env_int("CRL_MAX_PLY", 3), false, true);
        crl_set_lazy_carry(variant->lazy_carry);
        crl_clear_records();
        crl_new_sim();
        crl_reset_stats();
        snprintf(command, sizeof(command),
                 "sim -seed 7 -threads %d -tlim %.3f -scond none", threads,
                 seconds);
        load_and_exec_config_or_die(config, command);
        crl_set_mode(CRL_MODE_OFF, 0, false);
        crl_carry_buckets = 0;
        iterations[variant_idx] =
            sim_results_get_iteration_count(config_get_sim_results(config));
        totals[variant_idx] += (double)iterations[variant_idx];
      }
      printf("CRL timed: %4d %4d", turns[turn_idx], bag);
      for (int variant_idx = 0; variant_idx < NUMBER_OF_VARIANTS;
           variant_idx++) {
        printf(" %12llu (%5.2fx)", (unsigned long long)iterations[variant_idx],
               (double)iterations[variant_idx] / (double)iterations[0]);
      }
      printf("\n");
      fflush(stdout);
    }
    printf("CRL timed: total");
    for (int variant_idx = 0; variant_idx < NUMBER_OF_VARIANTS; variant_idx++) {
      printf("  %s %.2fx", variants[variant_idx].name,
             totals[variant_idx] / totals[0]);
    }
    printf("\n");
    fflush(stdout);
  }
}

// Fixed work for a profiler: one single-threaded sim of each position and
// plies in one mode (lazy, plain or carry), and nothing else.
static void run_profile_work(Config *config, const int *turns,
                             int number_of_turns, const int *plies_list,
                             int number_of_plies, int iterations,
                             const char *mode) {
  char command[256];
  const bool lazy_mode = strcmp(mode, "lazy") == 0;
  const bool plain_mode = strcmp(mode, "plain") == 0;
  int64_t total_ns = 0;
  uint64_t total_iterations = 0;
  for (int plies_idx = 0; plies_idx < number_of_plies; plies_idx++) {
    snprintf(command, sizeof(command), "set -plies %d -it %d",
             plies_list[plies_idx], iterations);
    load_and_exec_config_or_die(config, command);
    for (int turn_idx = 0; turn_idx < number_of_turns; turn_idx++) {
      snprintf(command, sizeof(command), "goto %d", turns[turn_idx]);
      load_and_exec_config_or_die(config, command);
      load_and_exec_config_or_die(config, "gen");
      crl_set_mode(lazy_mode    ? CRL_MODE_LAZY
                   : plain_mode ? CRL_MODE_TIME_PLAIN
                                : CRL_MODE_OFF,
                   0, false);
      crl_set_lazy_options(env_int("CRL_MAX_PLY", 3), false, true);
      crl_set_lazy_carry(false);
      crl_clear_records();
      crl_new_sim();
      crl_reset_stats();
      const int64_t start = ctimer_monotonic_ns();
      load_and_exec_config_or_die(config, "sim -seed 7 -threads 1");
      total_ns += ctimer_monotonic_ns() - start;
      crl_set_mode(CRL_MODE_OFF, 0, false);
      total_iterations +=
          sim_results_get_iteration_count(config_get_sim_results(config));
    }
  }
  printf("CRL profile: mode %s, %llu iterations in %.3f s\n", mode,
         (unsigned long long)total_iterations, (double)total_ns / 1e9);
}

// A sim's results, play by play, for comparing two sims of the same work.
typedef struct SimSnapshot {
  int number_of_plays;
  uint64_t node_count;
  uint64_t signatures[CRL_TEST_MAX_SIM_PLAYS];
  uint64_t rollouts[CRL_TEST_MAX_SIM_PLAYS];
  double equity_means[CRL_TEST_MAX_SIM_PLAYS];
  double win_pct_means[CRL_TEST_MAX_SIM_PLAYS];
} SimSnapshot;

static void take_sim_snapshot(const SimResults *sim_results,
                              SimSnapshot *snapshot) {
  snapshot->number_of_plays = sim_results_get_number_of_plays(sim_results);
  assert(snapshot->number_of_plays <= CRL_TEST_MAX_SIM_PLAYS);
  snapshot->node_count = sim_results_get_node_count(sim_results);
  for (int play_idx = 0; play_idx < snapshot->number_of_plays; play_idx++) {
    const SimmedPlay *simmed_play =
        sim_results_get_simmed_play(sim_results, play_idx);
    snapshot->signatures[play_idx] =
        census_rollout_move_signature(simmed_play_get_move(simmed_play));
    snapshot->rollouts[play_idx] = simmed_play_get_num_rollouts(simmed_play);
    snapshot->equity_means[play_idx] =
        simmed_play_get_exact_equity_mean(simmed_play);
    snapshot->win_pct_means[play_idx] =
        simmed_play_get_exact_win_pct_mean(simmed_play);
  }
}

static bool sim_snapshots_equal(const SimSnapshot *a, const SimSnapshot *b) {
  if (a->number_of_plays != b->number_of_plays ||
      a->node_count != b->node_count) {
    return false;
  }
  for (int play_idx = 0; play_idx < a->number_of_plays; play_idx++) {
    if (a->signatures[play_idx] != b->signatures[play_idx] ||
        a->rollouts[play_idx] != b->rollouts[play_idx] ||
        a->equity_means[play_idx] != b->equity_means[play_idx] ||
        a->win_pct_means[play_idx] != b->win_pct_means[play_idx]) {
      return false;
    }
  }
  return true;
}

// The census as sims use it (-scensus true): single-threaded with a fixed
// seed, a sim with it must give the same results as one without it, to the
// last rollout, since every census-steered move is the plain one. Also times
// both.
static void check_sim_census(Config *config, const int *turns,
                             int number_of_turns, const int *plies_list,
                             int number_of_plies, int iterations) {
  char command[256];
  printf("CRL scensus: %-5s %-4s %-4s %9s %9s %9s %s\n", "plies", "turn", "bag",
         "off s", "on s", "speedup", "results");
  double off_total = 0;
  double on_total = 0;
  for (int plies_idx = 0; plies_idx < number_of_plies; plies_idx++) {
    snprintf(command, sizeof(command), "set -plies %d -it %d",
             plies_list[plies_idx], iterations);
    load_and_exec_config_or_die(config, command);
    for (int turn_idx = 0; turn_idx < number_of_turns; turn_idx++) {
      snprintf(command, sizeof(command), "goto %d", turns[turn_idx]);
      load_and_exec_config_or_die(config, command);
      load_and_exec_config_or_die(config, "gen");
      SimSnapshot snapshots[2];
      double seconds[2];
      for (int census_on = 0; census_on < 2; census_on++) {
        const int64_t start = ctimer_monotonic_ns();
        load_and_exec_config_or_die(config, census_on
                                                ? "sim -seed 7 -scensus true"
                                                : "sim -seed 7 -scensus false");
        seconds[census_on] = (double)(ctimer_monotonic_ns() - start) / 1e9;
        take_sim_snapshot(config_get_sim_results(config),
                          &snapshots[census_on]);
      }
      const bool same = sim_snapshots_equal(&snapshots[0], &snapshots[1]);
      printf("CRL scensus: %-5d %-4d %-4d %9.3f %9.3f %8.2fx %s\n",
             plies_list[plies_idx], turns[turn_idx],
             bag_get_letters(game_get_bag(config_get_game(config))), seconds[0],
             seconds[1], seconds[0] / seconds[1], same ? "same" : "DIFFER");
      assert(same);
      off_total += seconds[0];
      on_total += seconds[1];
    }
  }
  load_and_exec_config_or_die(config, "set -scensus false");
  printf("CRL scensus: total off %.3f s, on %.3f s: %.2fx\n", off_total,
         on_total, off_total / on_total);
}

void test_census_rollout_lab(void) {
  int turns[CRL_TEST_MAX_TURNS];
  const int number_of_turns = env_int_list("CRL_TURNS", "6,10,14,18,22,26,30",
                                           turns, CRL_TEST_MAX_TURNS);
  int plies_list[CRL_TEST_MAX_PLIES];
  const int number_of_plies =
      env_int_list("CRL_PLIES", "1,2,4,6", plies_list, CRL_TEST_MAX_PLIES);
  const int iterations = env_int("CRL_IT", 2000);
  const int numplays = env_int("CRL_NUMPLAYS", 8);
  const int build_candidates = env_int("CRL_BUILD_CANDIDATES", 3);
  char command[512];
  snprintf(command, sizeof(command),
           "set -lex CSW24 -wmp true -rit true -ritmmap %s -wit true "
           "-threads 1 -numplays %d -scond none -seed 7 -patrollout false "
           "-sinfer false",
           env_int("CRL_RITMMAP", 1) ? "true" : "false", numplays);
  Config *config = config_create_or_die(command);
  load_and_exec_config_or_die(config,
                              "load testdata/gcgs/success_long_game.gcg");

  crl_set_lazy_carry(env_int("CRL_LAZY_CARRY", 1) != 0);
  crl_set_build_threads(env_int("CRL_BUILD_THREADS", 1));
  if (env_int("CRL_SCENSUS", 0)) {
    check_sim_census(config, turns, number_of_turns, plies_list,
                     number_of_plies, iterations);
    config_destroy(config);
    return;
  }
  if (getenv("CRL_PROFILE") != NULL) {
    run_profile_work(config, turns, number_of_turns, plies_list,
                     number_of_plies, iterations, getenv("CRL_PROFILE"));
    config_destroy(config);
    return;
  }
  if (env_int("CRL_TIMED", 0)) {
    time_timed_sims(config, turns, number_of_turns, plies_list, number_of_plies,
                    (double)env_int("CRL_TLIM", 14), env_int("CRL_THREADS", 10),
                    env_int("CRL_BIG_BUCKETS", 16384));
    config_destroy(config);
    return;
  }
  if (env_int("CRL_E2E", 0)) {
    time_lazy_census(config, turns, number_of_turns, plies_list,
                     number_of_plies, iterations, env_int("CRL_MAX_PLY", 3),
                     env_int("CRL_LOW_BAG", 1) != 0);
    config_destroy(config);
    return;
  }

  if (env_int("CRL_GATHER", 0)) {
    for (int turn_idx = 0; turn_idx < number_of_turns; turn_idx++) {
      snprintf(command, sizeof(command), "goto %d", turns[turn_idx]);
      load_and_exec_config_or_die(config, command);
      printf("CRL gather: turn %d\n", turns[turn_idx]);
      time_census_gather(config_get_game(config),
                         env_int("CRL_GATHER_RACKS", 200000));
    }
  }

  if (env_int("CRL_BUILD", 1)) {
    for (int turn_idx = 0; turn_idx < number_of_turns; turn_idx++) {
      snprintf(command, sizeof(command), "goto %d", turns[turn_idx]);
      load_and_exec_config_or_die(config, command);
      printf("CRL build: turn %d\n", turns[turn_idx]);
      time_lane_census_build(config_get_game(config), build_candidates);
    }
  }

  // Lane and anchor caches as a lazy census would give them (every lane and
  // anchor known), applying skips only: the floor buys nothing.
  const LabRun runs[] = {
      {"record", CRL_MODE_RECORD, 0, false, 0},
      {"carry", CRL_MODE_TIME_CARRY, 0, false, 0},
      {"plain", CRL_MODE_TIME_PLAIN, 0, false, 0},
      {"root lanes", CRL_MODE_REPLAY, crl_variant(CRL_BOARD_ROOT, 4), false,
       CRL_REPLAY_SKIPS},
      {"root anchors", CRL_MODE_REPLAY, crl_anchor_variant(CRL_BOARD_ROOT),
       false, CRL_REPLAY_SKIPS},
      {"cand lanes", CRL_MODE_REPLAY, crl_variant(CRL_BOARD_CAND, 4), false,
       CRL_REPLAY_SKIPS},
      {"cand anchors", CRL_MODE_REPLAY, crl_anchor_variant(CRL_BOARD_CAND),
       false, CRL_REPLAY_SKIPS},
      {"cand lanes + carry", CRL_MODE_REPLAY, crl_variant(CRL_BOARD_CAND, 4),
       true, CRL_REPLAY_SKIPS},
      {"cand anchors + carry", CRL_MODE_REPLAY,
       crl_anchor_variant(CRL_BOARD_CAND), true, CRL_REPLAY_SKIPS},
      {"plain (again)", CRL_MODE_TIME_PLAIN, 0, false, 0},
      {"carry (again)", CRL_MODE_TIME_CARRY, 0, false, 0},
  };
  // Controls (CRL_CONTROLS=1): a replay of cand k=4 that applies nothing must
  // time as plain does, and the skips and the floor alone show which one
  // costs what.
  const LabRun control_runs[] = {
      {"record", CRL_MODE_RECORD, 0, false, 0},
      {"carry", CRL_MODE_TIME_CARRY, 0, false, 0},
      {"plain", CRL_MODE_TIME_PLAIN, 0, false, 0},
      {"cand k=4 nothing", CRL_MODE_REPLAY, crl_variant(CRL_BOARD_CAND, 2),
       false, -1},
      {"cand k=4 skips only", CRL_MODE_REPLAY, crl_variant(CRL_BOARD_CAND, 2),
       false, CRL_REPLAY_SKIPS},
      {"cand k=4 floor only", CRL_MODE_REPLAY, crl_variant(CRL_BOARD_CAND, 2),
       false, CRL_REPLAY_FLOOR},
      {"cand k=4", CRL_MODE_REPLAY, crl_variant(CRL_BOARD_CAND, 2), false, 0},
      {"cand k=all skips only", CRL_MODE_REPLAY, crl_variant(CRL_BOARD_CAND, 4),
       false, CRL_REPLAY_SKIPS},
      {"cand k=all", CRL_MODE_REPLAY, crl_variant(CRL_BOARD_CAND, 4), false, 0},
      {"plain (again)", CRL_MODE_TIME_PLAIN, 0, false, 0},
      {"carry (again)", CRL_MODE_TIME_CARRY, 0, false, 0},
  };
  enum {
    MAX_RUNS = 16,
  };
  const bool controls = env_int("CRL_CONTROLS", 0) != 0;
  const LabRun *lab_runs = controls ? control_runs : runs;
  int number_of_runs = (int)(sizeof(runs) / sizeof(runs[0]));
  if (controls) {
    number_of_runs = (int)(sizeof(control_runs) / sizeof(control_runs[0]));
  }
  assert(number_of_runs <= MAX_RUNS);

  // Every run of a position replays the same draws.
  const char *sim_command = "sim -seed 7";
  for (int plies_idx = 0; plies_idx < number_of_plies; plies_idx++) {
    const int plies = plies_list[plies_idx];
    snprintf(command, sizeof(command), "set -plies %d -it %d", plies,
             iterations);
    load_and_exec_config_or_die(config, command);
    CrlPlyStats sums[MAX_RUNS][CRL_MAX_PLIES];
    memset(sums, 0, sizeof(sums));
    int64_t wall_ns[MAX_RUNS] = {0};
    for (int turn_idx = 0; turn_idx < number_of_turns; turn_idx++) {
      snprintf(command, sizeof(command), "goto %d", turns[turn_idx]);
      load_and_exec_config_or_die(config, command);
      load_and_exec_config_or_die(config, "gen");
      crl_clear_records();
      // Untimed: faults in the RIT pages and warms the caches for the racks
      // every run below draws, so that no run pays for them first.
      crl_set_mode(CRL_MODE_TIME_CARRY, 0, false);
      crl_new_sim();
      load_and_exec_config_or_die(config, sim_command);
      crl_set_mode(CRL_MODE_OFF, 0, false);
      for (int run_idx = 0; run_idx < number_of_runs; run_idx++) {
        crl_set_mode(lab_runs[run_idx].mode, lab_runs[run_idx].variant,
                     lab_runs[run_idx].with_carry);
        crl_set_replay_parts(lab_run_parts(&lab_runs[run_idx]));
        crl_new_sim();
        crl_reset_stats();
        const int64_t start = ctimer_monotonic_ns();
        load_and_exec_config_or_die(config, sim_command);
        wall_ns[run_idx] += ctimer_monotonic_ns() - start;
        crl_set_mode(CRL_MODE_OFF, 0, false);
        for (int ply = 0; ply < CRL_MAX_PLIES; ply++) {
          add_stats(&sums[run_idx][ply], crl_get_ply_stats(ply));
        }
      }
    }
    printf("CRL plies %d, %d iterations, %d candidates, %d positions\n", plies,
           iterations, numplays, number_of_turns);
    print_record_stats(sums[0], plies);
    double baseline_seconds = 0;
    for (int ply = 0; ply < plies; ply++) {
      baseline_seconds += (double)sums[1][ply].generation_ns / 1e9;
    }
    for (int run_idx = 1; run_idx < number_of_runs; run_idx++) {
      print_timing_row(&lab_runs[run_idx], sums[run_idx], plies,
                       wall_ns[run_idx], baseline_seconds);
    }
    fflush(stdout);
  }
  config_destroy(config);
}
