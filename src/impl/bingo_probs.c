// Computes opponent-bingo and self-bingo probabilities for a given
// position. See bingo_probs.h for the user-facing semantics.

#include "bingo_probs.h"

#include "../compat/cpthread.h"
#include "../def/cpthread_defs.h"
#include "../def/equity_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../def/rack_defs.h"
#include "../ent/bag.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/xoshiro.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "move_gen.h"
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------
// Combinatorics
// ---------------------------------------------------------------------

// C(n, k). Returns 0 if k > n. Uses uint64; safe up to roughly C(80, 40)
// or so before overflow becomes a concern. For our use case (k <=
// RACK_SIZE = 7, n up to ~100), we're well within range.
static uint64_t binomial_u64(uint32_t n, uint32_t k) {
  if (k > n) {
    return 0;
  }
  if (k > n - k) {
    k = n - k;
  }
  uint64_t result = 1;
  for (uint32_t i = 0; i < k; i++) {
    result = result * (n - i) / (i + 1);
  }
  return result;
}

static uint64_t gcd_u64(uint64_t a, uint64_t b) {
  while (b != 0) {
    const uint64_t t = b;
    b = a % b;
    a = t;
  }
  return a;
}

// ---------------------------------------------------------------------
// Tile pool — per-letter counts plus total
// ---------------------------------------------------------------------

typedef struct {
  uint16_t counts[MAX_ALPHABET_SIZE];
  int total;
  int ld_size;
} TilePool;

static void pool_zero(TilePool *pool, int ld_size) {
  memset(pool->counts, 0, sizeof(pool->counts));
  pool->total = 0;
  pool->ld_size = ld_size;
}

static void pool_add_rack(TilePool *pool, const Rack *rack) {
  if (rack == NULL) {
    return;
  }
  for (int ml = 0; ml < pool->ld_size; ml++) {
    const int n = rack_get_letter(rack, ml);
    pool->counts[ml] += (uint16_t)n;
    pool->total += n;
  }
}

static void pool_add_bag(TilePool *pool, const Bag *bag) {
  for (int ml = 0; ml < pool->ld_size; ml++) {
    const int n = bag_get_letter(bag, ml);
    pool->counts[ml] += (uint16_t)n;
    pool->total += n;
  }
}

// ---------------------------------------------------------------------
// Multiset enumeration into a flat list
// ---------------------------------------------------------------------

typedef struct {
  // Packed per-rack data. Each entry is (ld_size) uint8_t counts followed
  // by a uint64_t multinomial weight. Stride = entry_bytes.
  uint8_t *data;
  size_t entry_bytes;
  size_t count;
  size_t cap;
  int ld_size;
} RackList;

static void rack_list_init(RackList *list, int ld_size) {
  list->ld_size = ld_size;
  list->entry_bytes = (size_t)ld_size + sizeof(uint64_t);
  list->count = 0;
  list->cap = 256;
  list->data = malloc_or_die(list->cap * list->entry_bytes);
}

static void rack_list_destroy(RackList *list) { free(list->data); }

static void rack_list_push(RackList *list, const uint8_t *counts,
                           uint64_t weight) {
  if (list->count == list->cap) {
    list->cap *= 2;
    list->data = realloc_or_die(list->data, list->cap * list->entry_bytes);
  }
  uint8_t *slot = list->data + (list->count * list->entry_bytes);
  memcpy(slot, counts, (size_t)list->ld_size);
  memcpy(slot + list->ld_size, &weight, sizeof(uint64_t));
  list->count++;
}

static const uint8_t *rack_list_counts(const RackList *list, size_t i) {
  return list->data + (i * list->entry_bytes);
}

static uint64_t rack_list_weight(const RackList *list, size_t i) {
  uint64_t w;
  memcpy(&w, list->data + (i * list->entry_bytes) + list->ld_size,
         sizeof(uint64_t));
  return w;
}

static void enumerate_recursive(const TilePool *pool, int draw_size,
                                uint8_t *current, int ml, uint64_t weight,
                                RackList *out) {
  if (draw_size == 0) {
    rack_list_push(out, current, weight);
    return;
  }
  if (ml >= pool->ld_size) {
    return;
  }
  int max_n = pool->counts[ml];
  if (max_n > draw_size) {
    max_n = draw_size;
  }
  for (int n = 0; n <= max_n; n++) {
    current[ml] = (uint8_t)n;
    const uint64_t w = weight * binomial_u64(pool->counts[ml], (uint32_t)n);
    enumerate_recursive(pool, draw_size - n, current, ml + 1, w, out);
  }
  current[ml] = 0;
}

static void enumerate_multisets(const TilePool *pool, int draw_size,
                                RackList *out) {
  if (draw_size > pool->total) {
    return;
  }
  uint8_t *current = calloc_or_die((size_t)pool->ld_size, sizeof(uint8_t));
  enumerate_recursive(pool, draw_size, current, /*ml=*/0, /*weight=*/1, out);
  free(current);
}

// ---------------------------------------------------------------------
// Workers
// ---------------------------------------------------------------------

typedef struct {
  // Inputs
  const RackList *list;
  size_t start_idx;
  size_t end_idx;
  Game *game;            // per-thread game copy
  MoveList *move_list;   // per-thread scratch
  const Rack *base_rack; // tiles already in our rack (NULL = empty)
  int player_on_turn_index;
  int thread_index;

  // Outputs
  uint64_t bingo_distinct;
  uint64_t bingo_weight;
} ExhaustiveWorker;

static void apply_rack_counts(Rack *target, const Rack *base,
                              const uint8_t *drawn_counts, int ld_size) {
  rack_set_dist_size(target, ld_size);
  rack_reset(target);
  if (base != NULL) {
    for (int ml = 0; ml < ld_size; ml++) {
      const int n = rack_get_letter(base, ml);
      if (n > 0) {
        rack_add_letters(target, (MachineLetter)ml, n);
      }
    }
  }
  for (int ml = 0; ml < ld_size; ml++) {
    if (drawn_counts[ml] > 0) {
      rack_add_letters(target, (MachineLetter)ml, drawn_counts[ml]);
    }
  }
}

static void *exhaustive_worker_run(void *arg) {
  ExhaustiveWorker *w = (ExhaustiveWorker *)arg;
  const RackList *list = w->list;
  const Player *player = game_get_player(w->game, w->player_on_turn_index);
  Rack *player_rack = player_get_rack(player);
  const int ld_size = list->ld_size;

  const MoveGenArgs args = {
      .game = w->game,
      .move_list = w->move_list,
      .move_record_type = MOVE_RECORD_BINGO_EXISTS,
      .move_sort_type = MOVE_SORT_SCORE,
      .thread_index = w->thread_index,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };

  for (size_t i = w->start_idx; i < w->end_idx; i++) {
    const uint8_t *counts = rack_list_counts(list, i);
    apply_rack_counts(player_rack, w->base_rack, counts, ld_size);
    if (bingo_exists(&args)) {
      w->bingo_distinct++;
      w->bingo_weight += rack_list_weight(list, i);
    }
  }
  return NULL;
}

typedef struct {
  // Inputs
  const TilePool *pool;
  int draw_size;
  Game *game;
  MoveList *move_list;
  const Rack *base_rack;
  int player_on_turn_index;
  int thread_index;
  uint64_t samples;
  uint64_t seed;

  // Outputs
  uint64_t bingo_count;
  uint64_t total_count;
} SampleWorker;

// Draws `draw_size` tiles from the pool without replacement using the
// given PRNG, writing the result counts into `out_counts`. Pool counts
// are not modified (we replenish after the draw).
static void sample_one(const TilePool *pool, int draw_size, XoshiroPRNG *prng,
                       uint8_t *out_counts, uint16_t *scratch_pool) {
  memcpy(scratch_pool, pool->counts, sizeof(pool->counts));
  int remaining = pool->total;
  memset(out_counts, 0, (size_t)pool->ld_size);
  for (int i = 0; i < draw_size; i++) {
    uint64_t pick = prng_get_random_number(prng, (uint64_t)remaining);
    for (int ml = 0; ml < pool->ld_size; ml++) {
      if (scratch_pool[ml] == 0) {
        continue;
      }
      if (pick < scratch_pool[ml]) {
        out_counts[ml]++;
        scratch_pool[ml]--;
        remaining--;
        break;
      }
      pick -= scratch_pool[ml];
    }
  }
}

static void *sample_worker_run(void *arg) {
  SampleWorker *w = (SampleWorker *)arg;
  const Player *player = game_get_player(w->game, w->player_on_turn_index);
  Rack *player_rack = player_get_rack(player);
  const int ld_size = w->pool->ld_size;
  XoshiroPRNG *prng = prng_create(w->seed);
  uint8_t *drawn_counts = calloc_or_die((size_t)ld_size, sizeof(uint8_t));
  uint16_t scratch_pool[MAX_ALPHABET_SIZE];

  const MoveGenArgs args = {
      .game = w->game,
      .move_list = w->move_list,
      .move_record_type = MOVE_RECORD_BINGO_EXISTS,
      .move_sort_type = MOVE_SORT_SCORE,
      .thread_index = w->thread_index,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };

  for (uint64_t s = 0; s < w->samples; s++) {
    sample_one(w->pool, w->draw_size, prng, drawn_counts, scratch_pool);
    apply_rack_counts(player_rack, w->base_rack, drawn_counts, ld_size);
    if (bingo_exists(&args)) {
      w->bingo_count++;
    }
    w->total_count++;
  }

  free(drawn_counts);
  prng_destroy(prng);
  return NULL;
}

// ---------------------------------------------------------------------
// Scenario runners
// ---------------------------------------------------------------------

typedef struct {
  uint64_t bingo_distinct;
  uint64_t total_distinct;
  uint64_t bingo_weight;
  uint64_t total_weight;
  uint64_t sample_bingo;
  uint64_t sample_total;
  bool sampled;
} ScenarioResult;

static void run_exhaustive(const Game *base_game, const TilePool *pool,
                           int draw_size, const Rack *base_rack,
                           int num_threads, ScenarioResult *out) {
  RackList list;
  rack_list_init(&list, pool->ld_size);
  enumerate_multisets(pool, draw_size, &list);
  out->total_distinct = list.count;
  out->total_weight = binomial_u64((uint32_t)pool->total, (uint32_t)draw_size);
  out->bingo_distinct = 0;
  out->bingo_weight = 0;

  if (list.count == 0) {
    rack_list_destroy(&list);
    return;
  }

  if (num_threads < 1) {
    num_threads = 1;
  }
  if ((size_t)num_threads > list.count) {
    num_threads = (int)list.count;
  }

  const int player_on_turn = game_get_player_on_turn_index(base_game);

  ExhaustiveWorker *workers =
      calloc_or_die((size_t)num_threads, sizeof(ExhaustiveWorker));
  Game **games = calloc_or_die((size_t)num_threads, sizeof(Game *));
  MoveList **move_lists =
      calloc_or_die((size_t)num_threads, sizeof(MoveList *));
  cpthread_t *threads = calloc_or_die((size_t)num_threads, sizeof(cpthread_t));

  const size_t per = list.count / (size_t)num_threads;
  const size_t rem = list.count % (size_t)num_threads;
  size_t cursor = 0;
  for (int t = 0; t < num_threads; t++) {
    const size_t this_size = per + (size_t)(t < (int)rem ? 1 : 0);
    games[t] = game_duplicate(base_game);
    move_lists[t] = move_list_create(1);
    workers[t].list = &list;
    workers[t].start_idx = cursor;
    workers[t].end_idx = cursor + this_size;
    workers[t].game = games[t];
    workers[t].move_list = move_lists[t];
    workers[t].base_rack = base_rack;
    workers[t].player_on_turn_index = player_on_turn;
    workers[t].thread_index = t;
    cursor += this_size;
  }

  for (int t = 0; t < num_threads; t++) {
    cpthread_create(&threads[t], exhaustive_worker_run, &workers[t]);
  }
  for (int t = 0; t < num_threads; t++) {
    cpthread_join(threads[t]);
    out->bingo_distinct += workers[t].bingo_distinct;
    out->bingo_weight += workers[t].bingo_weight;
    move_list_destroy(move_lists[t]);
    game_destroy(games[t]);
  }

  free(threads);
  free(move_lists);
  free(games);
  free(workers);
  rack_list_destroy(&list);
}

static void run_sampled(const Game *base_game, const TilePool *pool,
                        int draw_size, const Rack *base_rack, int num_threads,
                        uint64_t total_samples, ScenarioResult *out) {
  out->total_distinct = 0;
  out->bingo_distinct = 0;
  out->total_weight = binomial_u64((uint32_t)pool->total, (uint32_t)draw_size);
  out->bingo_weight = 0;
  out->sampled = true;
  out->sample_total = 0;
  out->sample_bingo = 0;

  if (num_threads < 1) {
    num_threads = 1;
  }
  if ((uint64_t)num_threads > total_samples) {
    num_threads = (int)total_samples;
  }
  if (num_threads < 1) {
    return;
  }

  const int player_on_turn = game_get_player_on_turn_index(base_game);

  SampleWorker *workers =
      calloc_or_die((size_t)num_threads, sizeof(SampleWorker));
  Game **games = calloc_or_die((size_t)num_threads, sizeof(Game *));
  MoveList **move_lists =
      calloc_or_die((size_t)num_threads, sizeof(MoveList *));
  cpthread_t *threads = calloc_or_die((size_t)num_threads, sizeof(cpthread_t));

  // Seed each worker with a distinct value derived from a fixed mix of
  // the per-process clock and the worker index. We don't need
  // cross-process reproducibility here.
  const uint64_t base_seed =
      (uint64_t)((uintptr_t)workers ^ 0x9E3779B97F4A7C15ULL);

  const uint64_t per = total_samples / (uint64_t)num_threads;
  const uint64_t rem = total_samples % (uint64_t)num_threads;
  for (int t = 0; t < num_threads; t++) {
    const uint64_t this_samples = per + (uint64_t)(t < (int)rem ? 1 : 0);
    games[t] = game_duplicate(base_game);
    move_lists[t] = move_list_create(1);
    workers[t].pool = pool;
    workers[t].draw_size = draw_size;
    workers[t].game = games[t];
    workers[t].move_list = move_lists[t];
    workers[t].base_rack = base_rack;
    workers[t].player_on_turn_index = player_on_turn;
    workers[t].thread_index = t;
    workers[t].samples = this_samples;
    workers[t].seed = base_seed + (uint64_t)t * 0x9E3779B97F4A7C15ULL;
  }

  for (int t = 0; t < num_threads; t++) {
    cpthread_create(&threads[t], sample_worker_run, &workers[t]);
  }
  for (int t = 0; t < num_threads; t++) {
    cpthread_join(threads[t]);
    out->sample_bingo += workers[t].bingo_count;
    out->sample_total += workers[t].total_count;
    move_list_destroy(move_lists[t]);
    game_destroy(games[t]);
  }

  free(threads);
  free(move_lists);
  free(games);
  free(workers);
}

// ---------------------------------------------------------------------
// Output formatting
// ---------------------------------------------------------------------

static void format_scenario(StringBuilder *sb, const char *label,
                            const char *desc, int pool_size, int draw_size,
                            const ScenarioResult *r) {
  string_builder_add_formatted_string(sb, "%s (%s, %d of %d tiles drawn):\n",
                                      label, desc, draw_size, pool_size);
  if (r->sampled) {
    const double pct = r->sample_total == 0 ? 0.0
                                            : 100.0 * (double)r->sample_bingo /
                                                  (double)r->sample_total;
    const double p = pct / 100.0;
    const double se =
        r->sample_total == 0
            ? 0.0
            : 100.0 * sqrt(p * (1.0 - p) / (double)r->sample_total);
    string_builder_add_formatted_string(
        sb,
        "  sampled: %" PRIu64 " bingo / %" PRIu64
        " samples = %.3f%%  (SE %.3f%%)\n",
        r->sample_bingo, r->sample_total, pct, se);
    return;
  }
  const uint64_t no_bingo = r->total_distinct - r->bingo_distinct;
  string_builder_add_formatted_string(sb,
                                      "  raw racks: %" PRIu64 " bingo, %" PRIu64
                                      " no-bingo (%" PRIu64 " distinct)\n",
                                      r->bingo_distinct, no_bingo,
                                      r->total_distinct);
  uint64_t num = r->bingo_weight;
  uint64_t den = r->total_weight;
  if (den == 0) {
    string_builder_add_string(sb,
                              "  weighted: 0/0 (pool too small for draw)\n");
    return;
  }
  const double pct = 100.0 * (double)num / (double)den;
  const uint64_t g = num == 0 ? den : gcd_u64(num, den);
  string_builder_add_formatted_string(
      sb, "  weighted: %" PRIu64 "/%" PRIu64 " = %.3f%%\n", num / g, den / g,
      pct);
}

// ---------------------------------------------------------------------
// Public entry
// ---------------------------------------------------------------------

char *bingo_probs_run(const Game *game, int num_threads, uint64_t sample_count,
                      ErrorStack *error_stack) {
  const int player_on_turn = game_get_player_on_turn_index(game);
  const Player *us = game_get_player(game, player_on_turn);
  const Player *opp = game_get_player(game, 1 - player_on_turn);

  if (player_get_wmp(us) == NULL) {
    error_stack_push(
        error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
        string_duplicate(
            "bingoprobs requires a WMP-enabled lexicon (use -wmp true)"));
    return NULL;
  }

  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
  const Bag *bag = game_get_bag(game);
  const Rack *opp_rack = player_get_rack(opp);
  const Rack *our_rack = player_get_rack(us);

  // Scenario 1: opp's hidden rack is drawn from bag + opp's actual rack.
  TilePool opp_pool;
  pool_zero(&opp_pool, ld_size);
  pool_add_bag(&opp_pool, bag);
  pool_add_rack(&opp_pool, opp_rack);
  const int opp_draw_size = RACK_SIZE;

  ScenarioResult opp_result = {0};
  if (sample_count > 0) {
    run_sampled(game, &opp_pool, opp_draw_size, /*base_rack=*/NULL, num_threads,
                sample_count, &opp_result);
  } else {
    run_exhaustive(game, &opp_pool, opp_draw_size, /*base_rack=*/NULL,
                   num_threads, &opp_result);
  }

  // Scenario 2: opp passes, we draw from bag to refill our rack.
  TilePool self_pool;
  pool_zero(&self_pool, ld_size);
  pool_add_bag(&self_pool, bag);
  const int our_size = rack_get_total_letters(our_rack);
  const int self_draw_size = RACK_SIZE - our_size;

  ScenarioResult self_result = {0};
  if (self_draw_size < 0) {
    // Shouldn't happen for legal racks.
    error_stack_push(
        error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
        string_duplicate("on-turn player rack has more than RACK_SIZE tiles"));
    return NULL;
  }
  if (self_draw_size == 0) {
    // No replenish needed; one trivial outcome — check it directly.
    self_result.total_distinct = 1;
    self_result.total_weight = 1;
    Game *gd = game_duplicate(game);
    MoveList *ml = move_list_create(1);
    const MoveGenArgs args = {
        .game = gd,
        .move_list = ml,
        .move_record_type = MOVE_RECORD_BINGO_EXISTS,
        .move_sort_type = MOVE_SORT_SCORE,
        .thread_index = 0,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    if (bingo_exists(&args)) {
      self_result.bingo_distinct = 1;
      self_result.bingo_weight = 1;
    }
    move_list_destroy(ml);
    game_destroy(gd);
  } else if (sample_count > 0) {
    run_sampled(game, &self_pool, self_draw_size, our_rack, num_threads,
                sample_count, &self_result);
  } else {
    run_exhaustive(game, &self_pool, self_draw_size, our_rack, num_threads,
                   &self_result);
  }

  // Format report.
  StringBuilder *sb = string_builder_create();
  format_scenario(sb, "opp_bingo", "drawn from bag + opp rack (unseen to us)",
                  opp_pool.total, opp_draw_size, &opp_result);
  string_builder_add_char(sb, '\n');
  if (self_draw_size == 0) {
    string_builder_add_formatted_string(
        sb, "self_bingo (rack already full, no replenish needed):\n");
    string_builder_add_formatted_string(
        sb, "  bingo: %s\n", self_result.bingo_distinct > 0 ? "yes" : "no");
  } else {
    format_scenario(sb, "self_bingo", "after opp pass, replenish from bag",
                    self_pool.total, self_draw_size, &self_result);
  }
  char *result = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  return result;
}
