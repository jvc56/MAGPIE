// WMP vs GPU comparison: sweep distinct bag-legal size-7 racks, lex-order.
// Measures cold-cache per-rack throughput for:
//   - WMP single-thread
//   - WMP multi-thread (pthread, 8 threads)
//   - GPU one batched dispatch per length 2..7, host-side count accumulation
//
// Validates that summed match counts agree exactly across all three.
//
// Universe size is env-capped (GPUMATCH_N=N for partial sweep, or 0 for the
// full 3,199,724 legal size-7 racks). Default 200,000 keeps wall time short.
//
// On non-Darwin builds the GPU portion is compiled out; CPU timings still run.

#include "gpu_match_test.h"

#include "../src/def/kwg_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/ent/bit_rack.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/player.h"
#include "../src/ent/wmp.h"
#include "../src/impl/config.h"
#include "../src/impl/flat_lex_maker.h"
#include "../src/metal/movegen.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__APPLE__)
#define HAVE_GPU 1
#else
#define HAVE_GPU 0
#endif

#define BITRACK_BYTES 16
#define MIN_WORD_LENGTH 2
#define MAX_NONPLAYTHROUGH_LENGTH 7
#define RACK_SIZE_FOR_BENCH 7

static double monotonic_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts); // NOLINT(misc-include-cleaner)
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int wmp_entry_word_count_for(const WMPEntry *entry, int word_length) {
  if (wmp_entry_is_inlined(entry)) {
    return wmp_entry_number_of_inlined_bytes(entry, word_length) / word_length;
  }
  return (int)entry->num_words;
}

static void enum_subracks_count(const WMP *wmp, const BitRack *parent,
                                BitRack *current, int next_ml, int current_size,
                                int min_size, int max_size,
                                uint32_t *out_total) {
  if (next_ml >= BIT_RACK_MAX_ALPHABET_SIZE) {
    if (current_size >= min_size && current_size <= max_size) {
      const WMPEntry *entry = wmp_get_word_entry(wmp, current, current_size);
      if (entry != NULL) {
        *out_total += (uint32_t)wmp_entry_word_count_for(entry, current_size);
      }
    }
    return;
  }
  const int max_count = bit_rack_get_letter(parent, next_ml);
  if (current_size > max_size) {
    return;
  }
  for (int c = 0; c <= max_count; c++) {
    enum_subracks_count(wmp, parent, current, next_ml + 1, current_size + c,
                        min_size, max_size, out_total);
    if (c < max_count) {
      bit_rack_add_letter(current, next_ml);
    }
  }
  for (int c = 0; c < max_count; c++) {
    bit_rack_take_letter(current, next_ml);
  }
}

static uint32_t wmp_count_one(const WMP *wmp, const BitRack *rack_br) {
  uint32_t total = 0;
  BitRack current = bit_rack_create_empty();
  enum_subracks_count(wmp, rack_br, &current, 0, 0, MIN_WORD_LENGTH,
                      MAX_NONPLAYTHROUGH_LENGTH, &total);
  return total;
}

// Bag-legal size-7 rack universe enumeration. Fills racks_buf with multisets
// in lex order, capped at max_racks. Returns the number written.
typedef struct {
  uint8_t *buf;
  uint64_t cap;
  uint64_t count;
  const uint8_t *bag; // per-letter cap
  uint16_t current[MAX_ALPHABET_SIZE];
  int alpha;
} EnumCtx;

static void enum_emit(EnumCtx *u) {
  if (u->count >= u->cap) {
    return;
  }
  uint64_t lo = 0;
  uint64_t hi = 0;
  for (int ml = 0; ml < u->alpha; ml++) {
    const int c = u->current[ml];
    if (c == 0) {
      continue;
    }
    const int shift = ml * BIT_RACK_BITS_PER_LETTER;
    if (shift < 64) {
      lo += (uint64_t)c << shift;
    } else {
      hi += (uint64_t)c << (shift - 64);
    }
  }
  uint8_t *dst = u->buf + u->count * BITRACK_BYTES;
  memcpy(dst, &lo, 8);
  memcpy(dst + 8, &hi, 8);
  u->count++;
}

static void enum_recurse(EnumCtx *u, int letter_idx, int remaining) {
  if (u->count >= u->cap) {
    return;
  }
  const int bag_max = u->bag[letter_idx];
  if (letter_idx == u->alpha - 1) {
    if (remaining > bag_max) {
      return;
    }
    u->current[letter_idx] = (uint16_t)remaining;
    enum_emit(u);
    return;
  }
  const int max_c = remaining < bag_max ? remaining : bag_max;
  for (int c = 0; c <= max_c; c++) {
    u->current[letter_idx] = (uint16_t)c;
    enum_recurse(u, letter_idx + 1, remaining - c);
    if (u->count >= u->cap) {
      return;
    }
  }
}

static uint64_t enumerate_universe(const LetterDistribution *ld, uint8_t *buf,
                                   uint64_t cap) {
  uint8_t bag[MAX_ALPHABET_SIZE] = {0};
  const int alpha = ld_get_size(ld);
  for (int ml = 0; ml < alpha; ml++) {
    bag[ml] = (uint8_t)ld_get_dist(ld, (MachineLetter)ml);
  }
  EnumCtx u = {.buf = buf, .cap = cap, .count = 0, .bag = bag, .alpha = alpha};
  memset(u.current, 0, sizeof(u.current));
  enum_recurse(&u, 0, RACK_SIZE_FOR_BENCH);
  return u.count;
}

// ---- CPU sweep (single-threaded) ----
static uint64_t cpu_sweep_serial(const WMP *wmp, const uint8_t *racks_buf,
                                 uint32_t n_racks) {
  uint64_t total = 0;
  for (uint32_t i = 0; i < n_racks; i++) {
    BitRack rack_br;
    memcpy(&rack_br, racks_buf + (size_t)i * BITRACK_BYTES, BITRACK_BYTES);
    total += wmp_count_one(wmp, &rack_br);
  }
  return total;
}

// ---- CPU sweep (multi-threaded) ----
typedef struct {
  const WMP *wmp;
  const uint8_t *racks_buf;
  uint32_t start;
  uint32_t end;
  uint64_t total;
} WmpWorkerArg;

static void *wmp_worker(void *arg) {
  WmpWorkerArg *a = (WmpWorkerArg *)arg;
  uint64_t total = 0;
  for (uint32_t i = a->start; i < a->end; i++) {
    BitRack rack_br;
    memcpy(&rack_br, a->racks_buf + (size_t)i * BITRACK_BYTES, BITRACK_BYTES);
    total += wmp_count_one(a->wmp, &rack_br);
  }
  a->total = total;
  return NULL;
}

static uint64_t cpu_sweep_parallel(const WMP *wmp, const uint8_t *racks_buf,
                                   uint32_t n_racks, int n_threads) {
  pthread_t *threads =
      (pthread_t *)malloc((size_t)n_threads * sizeof(pthread_t));
  WmpWorkerArg *args =
      (WmpWorkerArg *)malloc((size_t)n_threads * sizeof(WmpWorkerArg));
  const uint32_t per = n_racks / (uint32_t)n_threads;
  for (int t = 0; t < n_threads; t++) {
    args[t].wmp = wmp;
    args[t].racks_buf = racks_buf;
    args[t].start = (uint32_t)t * per;
    args[t].end = (t == n_threads - 1) ? n_racks : ((uint32_t)t + 1) * per;
    args[t].total = 0;
    pthread_create(&threads[t], NULL, wmp_worker, &args[t]);
  }
  uint64_t total = 0;
  for (int t = 0; t < n_threads; t++) {
    pthread_join(threads[t], NULL);
    total += args[t].total;
  }
  free(threads);
  free(args);
  return total;
}

void test_gpu_match(void) {
  Config *config = config_create_or_die(
      "set -lex CSW24 -wmp true -s1 equity -s2 equity -r1 best -r2 best "
      "-numplays 1");
  Game *game = config_game_create(config);
  const LetterDistribution *ld = game_get_ld(game);
  const Player *player = game_get_player(game, 0);
  const KWG *kwg = player_get_kwg(player);
  const WMP *wmp = player_get_wmp(player);
  assert(wmp != NULL);

  // Universe cap (default 200k for fast iter; 0 = full 3.2M)
  const char *n_env = getenv("GPUMATCH_N");
  uint64_t cap = (n_env != NULL) ? (uint64_t)strtoull(n_env, NULL, 10) : 200000;
  if (cap == 0) {
    cap = 4272048; // upper bound; bag constraint will limit further
  }

  uint8_t *racks_buf = (uint8_t *)malloc(cap * BITRACK_BYTES);
  const uint64_t n_racks_64 = enumerate_universe(ld, racks_buf, cap);
  if (n_racks_64 > UINT32_MAX) {
    log_fatal("rack universe too large for uint32 batch\n");
  }
  const uint32_t n_racks = (uint32_t)n_racks_64;
  printf("WMP vs GPU non-playthrough sweep (CSW24, lengths %d..%d)\n",
         MIN_WORD_LENGTH, MAX_NONPLAYTHROUGH_LENGTH);
  printf("  universe: %u distinct bag-legal racks of size %d\n", n_racks,
         RACK_SIZE_FOR_BENCH);

  // Build flat lex bytes in memory.
  uint8_t *flatlex_bytes = NULL;
  size_t flatlex_size = 0;
  ErrorStack *es = error_stack_create();
  flat_lex_build(kwg, ld, &flatlex_bytes, &flatlex_size, es);
  if (!error_stack_is_empty(es)) {
    error_stack_print_and_reset(es);
    log_fatal("flat_lex_build failed\n");
  }
  error_stack_destroy(es);

  const int max_len_plus_one = flatlex_bytes[6];
  const uint32_t total_words =
      (uint32_t)flatlex_bytes[8] | ((uint32_t)flatlex_bytes[9] << 8) |
      ((uint32_t)flatlex_bytes[10] << 16) | ((uint32_t)flatlex_bytes[11] << 24);
  uint32_t per_length_count[MAX_KWG_STRING_LENGTH] = {0};
  size_t off = 16;
  for (int len = 0; len < max_len_plus_one; len++) {
    per_length_count[len] = (uint32_t)flatlex_bytes[off] |
                            ((uint32_t)flatlex_bytes[off + 1] << 8) |
                            ((uint32_t)flatlex_bytes[off + 2] << 16) |
                            ((uint32_t)flatlex_bytes[off + 3] << 24);
    off += 4;
  }
  const size_t bitracks_block_offset = 16 + (size_t)max_len_plus_one * 4;
  const uint8_t *all_bitracks = flatlex_bytes + bitracks_block_offset;
  uint32_t first_word_for_length[MAX_KWG_STRING_LENGTH] = {0};
  uint32_t cum = 0;
  for (int len = 0; len < max_len_plus_one; len++) {
    first_word_for_length[len] = cum;
    cum += per_length_count[len];
  }

  // ---- CPU single-thread ----
  const double t_serial0 = monotonic_seconds();
  const uint64_t cpu_total_serial = cpu_sweep_serial(wmp, racks_buf, n_racks);
  const double t_serial = monotonic_seconds() - t_serial0;
  printf("  CPU 1-thread WMP   total=%llu  %.3f s  %.3f us/rack\n",
         (unsigned long long)cpu_total_serial, t_serial,
         1e6 * t_serial / n_racks);

  // ---- CPU multi-thread ----
  const int n_threads = 8;
  // warmup one pass to make any first-touch faulting fair
  (void)cpu_sweep_parallel(wmp, racks_buf, n_racks > 50000 ? 50000 : n_racks,
                           n_threads);
  const double t_par0 = monotonic_seconds();
  const uint64_t cpu_total_par =
      cpu_sweep_parallel(wmp, racks_buf, n_racks, n_threads);
  const double t_par = monotonic_seconds() - t_par0;
  printf("  CPU %d-thread WMP  total=%llu  %.3f s  %.3f us/rack  %s\n",
         n_threads, (unsigned long long)cpu_total_par, t_par,
         1e6 * t_par / n_racks,
         cpu_total_par == cpu_total_serial ? "ok" : "FAIL");

#if HAVE_GPU
  if (!gpu_matcher_is_available()) {
    printf("  GPU not available, skipping\n");
    free(flatlex_bytes);
    free(racks_buf);
    game_destroy(game);
    config_destroy(config);
    return;
  }

  GpuMatcher *m = gpu_matcher_create("bin/movegen.metallib", all_bitracks,
                                     total_words, NULL, 0);
  if (m == NULL) {
    free(flatlex_bytes);
    free(racks_buf);
    game_destroy(game);
    config_destroy(config);
    log_fatal("gpu_matcher_create failed\n");
  }

  uint32_t *out_counts = (uint32_t *)malloc((size_t)n_racks * sizeof(uint32_t));
  uint32_t *host_totals = (uint32_t *)calloc(n_racks, sizeof(uint32_t));

  // Warmup
  for (int len = MIN_WORD_LENGTH; len <= MAX_NONPLAYTHROUGH_LENGTH; len++) {
    if (per_length_count[len] == 0) {
      continue;
    }
    gpu_matcher_count(m, first_word_for_length[len], per_length_count[len],
                      racks_buf, n_racks, out_counts);
  }

  const double t_gpu0 = monotonic_seconds();
  for (int len = MIN_WORD_LENGTH; len <= MAX_NONPLAYTHROUGH_LENGTH; len++) {
    if (per_length_count[len] == 0) {
      continue;
    }
    gpu_matcher_count(m, first_word_for_length[len], per_length_count[len],
                      racks_buf, n_racks, out_counts);
    for (uint32_t i = 0; i < n_racks; i++) {
      host_totals[i] += out_counts[i];
    }
  }
  const double t_gpu = monotonic_seconds() - t_gpu0;

  uint64_t gpu_total = 0;
  for (uint32_t i = 0; i < n_racks; i++) {
    gpu_total += host_totals[i];
  }
  printf("  GPU brute-scan B=%-7u total=%llu  %.3f s  %.3f us/rack  %s\n",
         n_racks, (unsigned long long)gpu_total, t_gpu, 1e6 * t_gpu / n_racks,
         gpu_total == cpu_total_serial ? "ok" : "FAIL");

  free(host_totals);
  free(out_counts);
  gpu_matcher_destroy(m);
#endif

  free(flatlex_bytes);
  free(racks_buf);
  game_destroy(game);
  config_destroy(config);
}
