// What building the leaves of a pool costs (see PoolLeaves in pool_shift.h),
// and a hash of everything a build produces, to compare two builds of MAGPIE
// on the same positions.
//
// Positions come from static self-play: every position whose mover holds a
// full rack with at least a rack in the bag, which are the positions a sim
// builds the leaves of. Each position's leaves are built reps times.
//
// Spec: first=<game>,games=<n>,reps=<r>,private=<0 or 1>
//       followed by any of ,pos=<index> ,build=<0 or 1> ,cgp=<0 or 1>
//       ,threads=<n> ,root=<0 or 1>
// threads=<n> builds on n threads; 1, the default, on the caller's alone.
// root=1 times, in place of the build, the exact shifts of the mover's rack
// (pool_shift_fill_rack), which generating the root's moves with the exact
// leave values computes once.
// With private=1 the pool is the one the mover sees; otherwise it is the
// public pool, which is what a sim builds.
// pos=<index> builds at that position alone (the index of its PLB line) and
// stops the self-play there, so that a process of its own measures a build
// that nothing before it in the process has warmed; pos=-1, the default,
// builds at every position.
// build=0 goes through the same loading and self-play without building: the
// control a measurement of the process's memory subtracts. The peak resident
// memory of a build is measured from outside the process (/usr/bin/time -l),
// since src/compat has no portable query for it.
// cgp=1 prints each selected position as a CGP without the opponent's rack,
// so that its tiles are in the bag as a sim of that CGP sees them; such a sim
// builds the public pool of the position's PLB line.
// Lines printed:
//   PLB spec first= games= reps= private= pos= build= cgp= threads= root=
//   PLB cgp pos= <cgp>
//   PLB pos= game= bag= unseen= letters= min_ms= med_ms= klv_hash=
//       table_hash= offset=
//   PLB root pos= game= bag= min_ms= med_ms= shifts_hash=
//   PLB total positions= builds= sum_min_ms= mean_min_ms= mean_med_ms=
//   PLB bucket bag=<range> positions= mean_min_ms= mean_med_ms=

#include "pool_leaves_bench_test.h"

#include "../src/compat/ctime.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/klv.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/player.h"
#include "../src/ent/pool_shift.h"
#include "../src/ent/rack.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/util/fnv.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  PLB_SPEC_FIELDS = 4,
  PLB_MAX_REPS = 64,
  PLB_POSITIONS_PER_GAME = 100,
  PLB_FIRST_GAME_SEED = 5000,
  PLB_NUM_BUCKETS = 3,
  PLB_LOW_BAG_MAX = 20,
  PLB_MID_BAG_MAX = 50,
  PLB_ALL_POSITIONS = -1,
};

typedef struct PoolLeavesBenchSpec {
  int first_game;
  int num_games;
  int reps;
  int private_view;
  // The one position to build at, or PLB_ALL_POSITIONS.
  int position;
  int build;
  int print_cgp;
  int threads;
  int root;
} PoolLeavesBenchSpec;

typedef struct PoolLeavesBenchBucket {
  int positions;
  double sum_min_ms;
  double sum_med_ms;
} PoolLeavesBenchBucket;

typedef struct PoolLeavesBenchBuild {
  double min_ms;
  double med_ms;
} PoolLeavesBenchBuild;

static uint64_t plb_hash_equities(uint64_t hash, const Equity *values,
                                  size_t count) {
  for (size_t value_idx = 0; value_idx < count; value_idx++) {
    hash = fnv64a_step(hash, (uint64_t)(uint32_t)values[value_idx]);
  }
  return hash;
}

static uint64_t plb_hash_klv(const KLV *klv) {
  return plb_hash_equities(FNV_64_OFFSET_BASIS, klv->leave_values,
                           klv_get_number_of_leaves(klv));
}

// Everything of the table a rollout reads: its values, its reference pool
// and its offset.
static uint64_t plb_hash_table(const PoolShiftTable *table) {
  uint64_t hash =
      plb_hash_equities(FNV_64_OFFSET_BASIS, table->values,
                        (size_t)table->number_of_rows * (size_t)table->stride);
  hash = fnv64a_step(hash, (uint64_t)table->has_reference_pool);
  hash = fnv64a_step(hash, (uint64_t)(uint32_t)table->reference_exact_offset);
  hash = fnv64a_step(hash, (uint64_t)table->reference_total);
  for (int ml = 0; ml < table->ld_size; ml++) {
    hash = fnv64a_step(hash, (uint64_t)table->reference_counts[ml]);
  }
  return hash;
}

static void plb_sort(double *values, int count) {
  for (int value_idx = 1; value_idx < count; value_idx++) {
    const double value = values[value_idx];
    int slot_idx = value_idx;
    while (slot_idx > 0 && values[slot_idx - 1] > value) {
      values[slot_idx] = values[slot_idx - 1];
      slot_idx--;
    }
    values[slot_idx] = value;
  }
}

static int plb_bucket_of_bag(int bag_size) {
  if (bag_size <= PLB_LOW_BAG_MAX) {
    return 0;
  }
  if (bag_size <= PLB_MID_BAG_MAX) {
    return 1;
  }
  return 2;
}

// The length of the field ",<name>=<integer>" at the start of text, whose
// integer goes to *value, or 0 if text does not start with that field.
static int plb_parse_field(const char *text, const char *name, int *value) {
  const size_t name_length = strlen(name);
  if (text[0] != ',' || strncmp(text + 1, name, name_length) != 0 ||
      text[1 + name_length] != '=') {
    return 0;
  }
  const char *number = text + name_length + 2;
  int number_length = 0;
  // The spec is test input, and the field count is checked.
  // NOLINTNEXTLINE(cert-err34-c,bugprone-unchecked-string-to-number-conversion)
  if (sscanf(number, "%d%n", value, &number_length) != 1) {
    return 0;
  }
  return (int)(number - text) + number_length;
}

// Reads the optional fields after the four the spec always has. Any other
// text is an error, so that a misspelled field is not silently ignored.
static void plb_parse_spec(const char *spec, PoolLeavesBenchSpec *parsed) {
  parsed->position = PLB_ALL_POSITIONS;
  parsed->build = 1;
  parsed->print_cgp = 0;
  parsed->threads = 1;
  parsed->root = 0;
  int consumed = 0;
  // The spec is test input, and the field count is checked.
  // NOLINTNEXTLINE(cert-err34-c,bugprone-unchecked-string-to-number-conversion)
  if (sscanf(spec, "first=%d,games=%d,reps=%d,private=%d%n",
             &parsed->first_game, &parsed->num_games, &parsed->reps,
             &parsed->private_view, &consumed) != PLB_SPEC_FIELDS) {
    log_fatal("pool leaves bench: bad spec: %s", spec);
  }
  const char *rest = spec + consumed;
  while (*rest != '\0') {
    int field_length = plb_parse_field(rest, "pos", &parsed->position);
    if (field_length == 0) {
      field_length = plb_parse_field(rest, "build", &parsed->build);
    }
    if (field_length == 0) {
      field_length = plb_parse_field(rest, "cgp", &parsed->print_cgp);
    }
    if (field_length == 0) {
      field_length = plb_parse_field(rest, "threads", &parsed->threads);
    }
    if (field_length == 0) {
      field_length = plb_parse_field(rest, "root", &parsed->root);
    }
    if (field_length == 0) {
      log_fatal("pool leaves bench: bad spec field: %s", rest);
    }
    rest += field_length;
  }
  if (parsed->reps < 1 || parsed->reps > PLB_MAX_REPS ||
      parsed->num_games < 0 || parsed->position < PLB_ALL_POSITIONS ||
      parsed->threads < 1 || (parsed->root != 0 && parsed->root != 1) ||
      (parsed->build != 0 && parsed->build != 1) ||
      (parsed->print_cgp != 0 && parsed->print_cgp != 1)) {
    log_fatal("pool leaves bench: bad spec: %s", spec);
  }
}

// The position as a sim of it sees it: the opponent's rack is unknown, so its
// tiles are in the bag.
static void plb_print_cgp(const Game *game, int pos_idx) {
  char *cgp = game_get_cgp(game, true);
  strip_cgp_opp_rack(cgp);
  printf("PLB cgp pos=%d %s\n", pos_idx, cgp);
  (void)fflush(stdout);
  free(cgp);
}

// Builds the leaves of the position's pool reps times and prints its line.
static PoolLeavesBenchBuild plb_build_position(const PoolLeavesBenchSpec *spec,
                                               const Game *game,
                                               const PoolShiftTable *table,
                                               int game_idx, int pos_idx) {
  const LetterDistribution *ld = game_get_ld(game);
  const int mover_index = game_get_player_on_turn_index(game);
  const Rack *mover_rack = player_get_rack(game_get_player(game, mover_index));
  const Rack *opponent_rack =
      player_get_rack(game_get_player(game, 1 - mover_index));
  double times_ms[PLB_MAX_REPS];
  uint64_t klv_hash = 0;
  uint64_t table_hash = 0;
  int unseen = 0;
  int letters = 0;
  Equity offset = 0;
  for (int rep_idx = 0; rep_idx < spec->reps; rep_idx++) {
    Timer timer;
    ctimer_start(&timer);
    PoolLeaves *pool_leaves = pool_leaves_create(
        table, ld, game_get_bag(game), opponent_rack,
        spec->private_view != 0 ? NULL : mover_rack, spec->threads);
    times_ms[rep_idx] = ctimer_elapsed_seconds(&timer) * 1000.0;
    const uint64_t rep_klv_hash = plb_hash_klv(pool_leaves->klv);
    const uint64_t rep_table_hash = plb_hash_table(pool_leaves->table);
    if (rep_idx > 0 &&
        (rep_klv_hash != klv_hash || rep_table_hash != table_hash)) {
      log_fatal("pool leaves bench: builds of one pool differ at %d", pos_idx);
    }
    klv_hash = rep_klv_hash;
    table_hash = rep_table_hash;
    unseen = pool_leaves->table->reference_total;
    offset = pool_leaves->table->reference_exact_offset;
    letters = 0;
    for (int ml = 0; ml < ld_get_size(ld); ml++) {
      if (pool_leaves->table->reference_counts[ml] > 0) {
        letters++;
      }
    }
    pool_leaves_destroy(pool_leaves);
  }
  plb_sort(times_ms, spec->reps);
  PoolLeavesBenchBuild build;
  build.min_ms = times_ms[0];
  build.med_ms = times_ms[spec->reps / 2];
  printf("PLB pos=%d game=%d bag=%d unseen=%d letters=%d min_ms=%.3f "
         "med_ms=%.3f klv_hash=%016llx table_hash=%016llx offset=%d\n",
         pos_idx, game_idx, game_get_real_bag_size(game), unseen, letters,
         build.min_ms, build.med_ms, (unsigned long long)klv_hash,
         (unsigned long long)table_hash, (int)offset);
  (void)fflush(stdout);
  return build;
}

// Fills the exact shifts of the mover's rack reps times and prints its line.
static PoolLeavesBenchBuild plb_fill_root(const PoolLeavesBenchSpec *spec,
                                          const Game *game,
                                          const PoolShiftTable *table,
                                          int game_idx, int pos_idx) {
  const LetterDistribution *ld = game_get_ld(game);
  const int mover_index = game_get_player_on_turn_index(game);
  const Rack *mover_rack = player_get_rack(game_get_player(game, mover_index));
  const Rack *opponent_rack =
      player_get_rack(game_get_player(game, 1 - mover_index));
  double times_ms[PLB_MAX_REPS];
  uint64_t shifts_hash = 0;
  PoolShiftRack *rack_shift = malloc_or_die(sizeof(PoolShiftRack));
  for (int rep_idx = 0; rep_idx < spec->reps; rep_idx++) {
    Timer timer;
    ctimer_start(&timer);
    pool_shift_fill_rack(table, ld, game_get_bag(game), opponent_rack,
                         POOL_SHIFT_EXACT_WEIGHT_MILLI, mover_rack, rack_shift);
    times_ms[rep_idx] = ctimer_elapsed_seconds(&timer) * 1000.0;
    const uint64_t rep_hash = plb_hash_equities(
        FNV_64_OFFSET_BASIS, rack_shift->shifts, (size_t)rack_shift->count);
    if (rep_idx > 0 && rep_hash != shifts_hash) {
      log_fatal("pool leaves bench: shifts of one rack differ at %d", pos_idx);
    }
    shifts_hash = rep_hash;
  }
  free(rack_shift);
  plb_sort(times_ms, spec->reps);
  PoolLeavesBenchBuild build;
  build.min_ms = times_ms[0];
  build.med_ms = times_ms[spec->reps / 2];
  printf("PLB root pos=%d game=%d bag=%d min_ms=%.3f med_ms=%.3f "
         "shifts_hash=%016llx\n",
         pos_idx, game_idx, game_get_real_bag_size(game), build.min_ms,
         build.med_ms, (unsigned long long)shifts_hash);
  (void)fflush(stdout);
  return build;
}

void pool_leaves_bench_run_spec(const char *spec_string) {
  PoolLeavesBenchSpec spec;
  plb_parse_spec(spec_string, &spec);
  printf("PLB spec first=%d games=%d reps=%d private=%d pos=%d build=%d "
         "cgp=%d threads=%d root=%d\n",
         spec.first_game, spec.num_games, spec.reps, spec.private_view,
         spec.position, spec.build, spec.print_cgp, spec.threads, spec.root);
  (void)fflush(stdout);
  Config *config = config_create_or_die(
      "set -lex CSW24 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 15 -plies 2 -threads 1 -scond none -tlim 0 "
      "-savesettings false -poolshift 1.2");
  Game *game = config_game_create(config);
  PoolLeavesBenchBucket buckets[PLB_NUM_BUCKETS] = {{0}};
  int positions = 0;
  double sum_min_ms = 0;
  double sum_med_ms = 0;
  bool target_done = false;
  for (int game_idx = spec.first_game;
       game_idx < spec.first_game + spec.num_games && !target_done;
       game_idx++) {
    game_reset(game);
    game_seed(game, (uint64_t)PLB_FIRST_GAME_SEED + (uint64_t)game_idx);
    draw_starting_racks(game);
    int pos_idx = game_idx * PLB_POSITIONS_PER_GAME;
    while (!game_over(game) && !target_done) {
      const Player *mover =
          game_get_player(game, game_get_player_on_turn_index(game));
      const int bag_size = game_get_real_bag_size(game);
      if (rack_get_total_letters(player_get_rack(mover)) == RACK_SIZE &&
          bag_size >= RACK_SIZE) {
        const PoolShiftTable *table = player_get_pool_shift_table(mover);
        if (table == NULL || table->exact_values == NULL) {
          log_fatal("pool leaves bench: no exact leave values (CSW24.rvt)");
        }
        if (spec.position == PLB_ALL_POSITIONS || spec.position == pos_idx) {
          if (spec.print_cgp != 0) {
            plb_print_cgp(game, pos_idx);
          }
          if (spec.build != 0) {
            const PoolLeavesBenchBuild build =
                spec.root != 0
                    ? plb_fill_root(&spec, game, table, game_idx, pos_idx)
                    : plb_build_position(&spec, game, table, game_idx, pos_idx);
            PoolLeavesBenchBucket *bucket =
                &buckets[plb_bucket_of_bag(bag_size)];
            bucket->positions++;
            bucket->sum_min_ms += build.min_ms;
            bucket->sum_med_ms += build.med_ms;
            positions++;
            sum_min_ms += build.min_ms;
            sum_med_ms += build.med_ms;
          }
          target_done = spec.position == pos_idx;
        }
        pos_idx++;
      }
      if (!target_done) {
        play_top_n_equity_move(game, 0);
      }
    }
  }
  if (positions > 0) {
    printf("PLB total positions=%d builds=%d sum_min_ms=%.1f mean_min_ms=%.3f "
           "mean_med_ms=%.3f\n",
           positions, positions * spec.reps, sum_min_ms,
           sum_min_ms / (double)positions, sum_med_ms / (double)positions);
  }
  static const char *const bucket_names[PLB_NUM_BUCKETS] = {"7-20", "21-50",
                                                            "51+"};
  for (int bucket_idx = 0; bucket_idx < PLB_NUM_BUCKETS; bucket_idx++) {
    const PoolLeavesBenchBucket *bucket = &buckets[bucket_idx];
    if (bucket->positions > 0) {
      printf("PLB bucket bag=%s positions=%d mean_min_ms=%.3f "
             "mean_med_ms=%.3f\n",
             bucket_names[bucket_idx], bucket->positions,
             bucket->sum_min_ms / (double)bucket->positions,
             bucket->sum_med_ms / (double)bucket->positions);
    }
  }
  game_destroy(game);
  config_destroy(config);
}
