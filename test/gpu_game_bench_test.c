// Phase 5.1c.3 v2: game-walk GPU benchmark with realistic rack enumeration.
//
// Plays a CSW24 game using CPU MAGPIE static-eval (best-by-equity at every
// ply, applied to the actual drawn rack). At each ply, before applying the
// move, enumerates ALL unique 7-multiset racks composable from the current
// bag's tiles, then runs three independently-timed loops:
//
//   1. CPU MAGPIE generate_moves on each rack individually
//   2. GPU WMPG top-1 (canonical) on the batch of all racks
//
// Reports per-rack µs for each loop at every ply.
//
// Blank handling: WMPG currently supports only blank-0 racks (no blank-1 or
// blank-2 lookup tables in the kernel yet — flattener writes them but the
// kernel doesn't use them). For honesty, this bench enumerates blank-free
// racks only, and reports the count of blank-containing racks that were
// skipped. The bag's blanks themselves are excluded from the enumeration
// pool — pending kernel updates to use the blank-1 and blank-2 tables.

#include "gpu_game_bench_test.h"

#include "../src/def/board_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/kwg_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/bonus_square.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/klv.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/wmp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/wmpg_maker.h"
#include "../src/metal/movegen.h"
#include "../src/util/io_util.h"
#include "test_util.h"
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
#define RACK_TILES 7

static double mono_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts); // NOLINT(misc-include-cleaner)
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

typedef struct SlotMeta {
  uint8_t row;
  uint8_t col_start;
  uint8_t length;
  uint8_t dir;
} SlotMeta;

typedef struct SlotCache {
  uint64_t cross_sets[BOARD_DIM];
  uint8_t fixed_letters[BOARD_DIM];
  uint8_t fixed_bitrack[BITRACK_BYTES];
  uint64_t fb_lo;
  uint64_t fb_hi;
  int32_t position_multipliers[BOARD_DIM];
  int32_t base_score_eq;
  int32_t bingo_eq;
  uint8_t length;
  uint8_t target_used_size;
  uint8_t row;
  uint8_t col;
  uint8_t dir;
} SlotCache;

// Helper: recursively enumerate multisets summing to target_sum where each
// digit `used[k]` lies in [0, max_per[distinct_ml[k]]]. Pruned: at each level
// we cap the iteration by `target_sum - sum_so_far` so we never overshoot,
// and we lower-bound by `target_sum - sum_remaining_max` so we never
// undershoot.
static void enum_recurse(int level, int distinct_count, int target_sum,
                         int sum_so_far, const int *suffix_max_sum,
                         const uint8_t *distinct_ml, const int *max_per_ml,
                         uint8_t *used, uint8_t *racks_out, uint32_t *produced,
                         uint32_t max_racks) {
  if (*produced >= max_racks) {
    return;
  }
  if (level == distinct_count) {
    if (sum_so_far == target_sum) {
      uint64_t lo = 0;
      uint64_t hi = 0;
      for (int k = 0; k < distinct_count; k++) {
        if (used[k] == 0) {
          continue;
        }
        const int shift = distinct_ml[k] * 4;
        if (shift < 64) {
          lo += (uint64_t)used[k] << shift;
        } else {
          hi += (uint64_t)used[k] << (shift - 64);
        }
      }
      memcpy(racks_out + (size_t)*produced * BITRACK_BYTES, &lo, 8);
      memcpy(racks_out + (size_t)*produced * BITRACK_BYTES + 8, &hi, 8);
      (*produced)++;
    }
    return;
  }
  const int remaining_target = target_sum - sum_so_far;
  if (remaining_target < 0) {
    return;
  }
  const int max_at_level = max_per_ml[distinct_ml[level]];
  // Lower bound: if even using the full remaining max, we can't reach target.
  if (sum_so_far + suffix_max_sum[level] < target_sum) {
    return;
  }
  const int hi_v =
      remaining_target < max_at_level ? remaining_target : max_at_level;
  // Lower-bound on used[level]: must be at least target - (suffix_max_sum
  // for level+1 onwards) so we don't undershoot.
  const int suffix_after =
      (level + 1 < distinct_count) ? suffix_max_sum[level + 1] : 0;
  const int lo_v_raw = remaining_target - suffix_after;
  const int lo_v = lo_v_raw < 0 ? 0 : lo_v_raw;
  for (int v = lo_v; v <= hi_v; v++) {
    used[level] = (uint8_t)v;
    enum_recurse(level + 1, distinct_count, target_sum, sum_so_far + v,
                 suffix_max_sum, distinct_ml, max_per_ml, used, racks_out,
                 produced, max_racks);
    if (*produced >= max_racks) {
      return;
    }
  }
}

// Enumerate all unique 7-multiset racks composable from the current bag's
// letters (including blanks). Each rack is written as a 16-byte BitRack into
// racks_out (caller-allocated). Returns the number of racks generated,
// capped at max_racks. Includes the blank tile (ml=0) in the pool, so racks
// with up to 2 blanks (CSW limit) appear naturally.
static uint32_t enumerate_blank_free_7_racks(const Bag *bag,
                                             const Rack *current_rack,
                                             int alpha, uint8_t *racks_out,
                                             uint32_t max_racks) {
  // Build pool: bag's tiles + player's current rack tiles (since the rack
  // came from the bag and is a valid sample point). Include ml=0 (blank).
  int pool[BIT_RACK_MAX_ALPHABET_SIZE] = {0};
  for (int ml = 0; ml < alpha; ml++) {
    pool[ml] = bag_get_letter(bag, (MachineLetter)ml);
    if (current_rack != NULL) {
      pool[ml] += rack_get_letter(current_rack, (MachineLetter)ml);
    }
  }
  int max_per[BIT_RACK_MAX_ALPHABET_SIZE] = {0};
  for (int ml = 0; ml < alpha; ml++) {
    max_per[ml] = pool[ml] < RACK_TILES ? pool[ml] : RACK_TILES;
  }
  // Cap blanks at 2 (CSW has only 2 blank tiles total; rack can hold ≤ 2).
  if (max_per[0] > 2) {
    max_per[0] = 2;
  }
  uint8_t distinct_ml[BIT_RACK_MAX_ALPHABET_SIZE];
  int distinct_count = 0;
  for (int ml = 0; ml < alpha; ml++) {
    if (max_per[ml] > 0) {
      distinct_ml[distinct_count++] = (uint8_t)ml;
    }
  }
  if (distinct_count == 0) {
    return 0;
  }
  // Suffix-sum of max counts: suffix_max_sum[k] = sum of
  // max_per[distinct_ml[i]] for i in [k, distinct_count). Used to prune
  // branches that can't reach the target sum.
  int suffix_max_sum[BIT_RACK_MAX_ALPHABET_SIZE + 1] = {0};
  for (int k = distinct_count - 1; k >= 0; k--) {
    suffix_max_sum[k] = suffix_max_sum[k + 1] + max_per[distinct_ml[k]];
  }
  uint8_t used[BIT_RACK_MAX_ALPHABET_SIZE] = {0};
  uint32_t produced = 0;
  enum_recurse(0, distinct_count, RACK_TILES, 0, suffix_max_sum, distinct_ml,
               max_per, used, racks_out, &produced, max_racks);
  return produced;
}

// Build a Rack object from a 16-byte BitRack (writes per-letter counts).
// BitRack is 32 nibbles (4 bits per ml × 32 mls); loop is capped at 32 to
// avoid undefined shift behavior past the bitrack's range when `alpha` is
// larger than 32 (e.g., MAX_ALPHABET_SIZE=50).
static void rack_from_bitrack(Rack *rack, const uint8_t *br, int alpha) {
  rack_set_dist_size_and_reset(rack, alpha);
  uint64_t lo;
  uint64_t hi;
  memcpy(&lo, br, 8);
  memcpy(&hi, br + 8, 8);
  int total = 0;
  const int ml_max = alpha < 32 ? alpha : 32;
  for (int ml = 0; ml < ml_max; ml++) {
    int cnt;
    const int shift = ml * 4;
    if (shift < 64) {
      cnt = (int)((lo >> shift) & 0x0F);
    } else {
      cnt = (int)((hi >> (shift - 64)) & 0x0F);
    }
    if (cnt > 0) {
      rack_set_letter(rack, (MachineLetter)ml, (uint16_t)cnt);
      total += cnt;
    }
  }
  rack_set_total_letters(rack, total);
}

// Generate the leave-value table for a given rack (all multiset subracks of
// the rack, paired with their KLV leave values). Returns n_leaves.
static uint32_t build_leave_table(const Rack *rack, const KLV *klv, int alpha,
                                  uint8_t *leave_used, int32_t *leave_values,
                                  uint32_t max_leaves) {
  int max_count[BIT_RACK_MAX_ALPHABET_SIZE] = {0};
  for (int ml = 0; ml < alpha; ml++) {
    max_count[ml] = rack_get_letter(rack, (MachineLetter)ml);
  }
  int stack[BIT_RACK_MAX_ALPHABET_SIZE] = {0};
  int level = 0;
  uint32_t n = 0;
  while (1) {
    if (level == alpha) {
      Rack leave;
      rack_set_dist_size_and_reset(&leave, alpha);
      int leave_total = 0;
      uint64_t lo = 0;
      uint64_t hi = 0;
      for (int ml = 0; ml < alpha; ml++) {
        const int used = stack[ml];
        const int rem = max_count[ml] - used;
        if (rem > 0) {
          rack_set_letter(&leave, (MachineLetter)ml, (uint16_t)rem);
          leave_total += rem;
        }
        if (used > 0) {
          const int shift = ml * 4;
          if (shift < 64) {
            lo += (uint64_t)used << shift;
          } else {
            hi += (uint64_t)used << (shift - 64);
          }
        }
      }
      rack_set_total_letters(&leave, leave_total);
      const Equity lv = klv_get_leave_value(klv, &leave);
      if (n < max_leaves) {
        memcpy(leave_used + (size_t)n * BITRACK_BYTES, &lo, 8);
        memcpy(leave_used + (size_t)n * BITRACK_BYTES + 8, &hi, 8);
        leave_values[n] = (int32_t)lv;
      }
      n++;
      level--;
      while (level >= 0 && stack[level] >= max_count[level]) {
        stack[level] = 0;
        level--;
      }
      if (level < 0) {
        break;
      }
      stack[level]++;
      level++;
      continue;
    }
    stack[level] = 0;
    level++;
  }
  return n;
}

void test_gpu_game_bench(void) {
  Config *config = config_create_or_die(
      "set -lex CSW24 -wmp true -s1 equity -s2 equity -r1 best -r2 best "
      "-numplays 1");
  Game *game = config_game_create(config);
  const LetterDistribution *ld = game_get_ld(game);
  Player *player0 = game_get_player(game, 0);
  const KWG *kwg = player_get_kwg(player0);
  const KLV *klv = player_get_klv(player0);
  const WMP *wmp = player_get_wmp(player0);
  Board *board = game_get_board(game);
  game_reset(game);
  draw_starting_racks(game);

#if HAVE_GPU
  if (!gpu_matcher_is_available()) {
    printf("  GPU not available, skipping\n");
    game_destroy(game);
    config_destroy(config);
    return;
  }
  GpuMatcher *m = gpu_matcher_create("bin/movegen.metallib");
  if (m == NULL) {
    log_fatal("gpu_matcher_create failed\n");
  }
  uint8_t *wmpg_bytes = NULL;
  size_t wmpg_size = 0;
  ErrorStack *es_w = error_stack_create();
  wmpg_build(wmp, &wmpg_bytes, &wmpg_size, es_w);
  if (!error_stack_is_empty(es_w)) {
    error_stack_print_and_reset(es_w);
    log_fatal("wmpg_build failed\n");
  }
  error_stack_destroy(es_w);
  if (!gpu_matcher_load_wmpg(m, wmpg_bytes, wmpg_size)) {
    log_fatal("gpu_matcher_load_wmpg failed\n");
  }
#else
  (void)kwg;
  (void)wmp;
  printf("  GPU not built (non-Apple), skipping\n");
  game_destroy(game);
  config_destroy(config);
  return;
#endif

#if HAVE_GPU
  const int alpha = ld_get_size(ld);
  int32_t letter_scores_eq[32] = {0};
  for (int ml = 0; ml < 32; ml++) {
    if (ml < alpha) {
      letter_scores_eq[ml] = (int32_t)ld_get_score(ld, (MachineLetter)ml);
    }
  }
  const int32_t bingo_bonus_eq = (int32_t)game_get_bingo_bonus(game);

  // Per-rack reusable buffers. Each rack has up to ~512 leaves (8 distinct
  // letters → 256–512). The GPU API takes a single shared leave table per
  // dispatch — so we need one leave table per rack and dispatch per-rack
  // for top-1 (since each rack has its own leave values). For the BATCH
  // dispatches we have a problem: BF/WMPG kernels share one leave table per
  // dispatch. To avoid that, we time per-rack-dispatch loops.

  MoveList *move_list = move_list_create(1000000);

  printf("phase 5.1c.3-v3 game-walk bench (CSW24, every 7-rack from bag at "
         "each turn — apples-to-apples diverse racks on both sides)\n");
  printf("  CPU column: generate_moves with MoveGenArgs.override_rack on "
         "each of n_rack distinct enumerated racks\n");
  printf("  WMPG column: top-1 over n_rack DISTINCT enumerated racks in a "
         "single batched dispatch (per-rack leave tables)\n");
  printf("  Enumeration includes 0/1/2-blank racks; WMPG uses the blank-1 "
         "and blank-2 sub-tables for blank substitution\n");
  printf("  %-3s %-5s %-7s %-7s %-9s %-13s %-13s\n", "trn", "slot", "rack",
         "n_rack", "score", "cpu us/r", "WMPG us/r");

  const int max_turns = 30;
  // With batched dispatch (single kernel call covers all B distinct racks
  // via leave_stride), the bottleneck is now CPU-side enumeration time and
  // GPU compute, not dispatch overhead. We can run many more racks per turn.
  // Empty bag has 86 blank-free tiles — multiset 7-rack count is ~1M; we
  // cap at 50k to keep memory reasonable (50k * 256 * 16 = 200 MB for
  // the leave tables alone).
  const uint32_t max_racks_per_turn = 5000;

  // Aggregates
  double cpu_total_us = 0;
  double wmpg_total_us = 0;
  uint64_t total_racks = 0;
  int valid_turns = 0;

  // Pre-allocate rack buffer big enough.
  uint8_t *batch_racks =
      (uint8_t *)malloc((size_t)max_racks_per_turn * BITRACK_BYTES);

  for (int turn = 0; turn < max_turns && !game_over(game); turn++) {
    const int player_idx = game_get_player_on_turn_index(game);
    Player *player = game_get_player(game, player_idx);
    Rack *rack_real = player_get_rack(player);
    const KWG *p_kwg = player_get_kwg(player);
    const int ci = board_get_cross_set_index(p_kwg == kwg, 0);

    char rack_str[64] = {0};
    {
      int pos = 0;
      for (int ml = 0; ml < alpha && pos < 60; ml++) {
        const int cnt = rack_get_letter(rack_real, (MachineLetter)ml);
        for (int c = 0; c < cnt && pos < 60; c++) {
          if (ml == 0) {
            rack_str[pos++] = '?';
          } else if (ml >= 1 && ml <= 26) {
            rack_str[pos++] = 'A' + ml - 1;
          } else {
            rack_str[pos++] = '?';
          }
        }
      }
    }

    // Run MAGPIE generate_moves on the actual rack to get slot list and
    // best move (for game advancement).
    const MoveGenArgs gen_args = {
        .game = game,
        .move_list = move_list,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_EQUITY,
        .override_kwg = NULL,
        .thread_index = 0,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    move_list_set_rack(move_list, rack_real);
    generate_moves(&gen_args);
    SortedMoveList *sml = sorted_move_list_create(move_list);
    const Move *magpie_best_ptr = NULL;
    Equity magpie_best_eq = EQUITY_MIN_VALUE;
    for (int i = 0; i < sml->count; i++) {
      const Move *mv = sml->moves[i];
      if (move_get_equity(mv) > magpie_best_eq) {
        magpie_best_eq = move_get_equity(mv);
        magpie_best_ptr = mv;
      }
    }
    // sml->moves[i] are pointers into move_list's owned Move objects, which
    // get overwritten by subsequent generate_moves(...) calls. Copy the best
    // Move now so play_move() at end of turn doesn't read stale data.
    Move *magpie_best = move_create();
    if (magpie_best_ptr != NULL) {
      move_copy(magpie_best, magpie_best_ptr);
    }

    // Build slot list from MAGPIE's tile-placement moves. (Slots depend on
    // rack-induced moves, which is rack-dependent. With a different rack,
    // some slots may not produce any move at all. For perf-bench purposes
    // this is an acceptable approximation — the slot set covers every
    // playable position the actual rack saw.)
    const int max_slots = 4096;
    SlotMeta *slots = (SlotMeta *)calloc((size_t)max_slots, sizeof(SlotMeta));
    int slot_count = 0;
    for (int i = 0; i < sml->count; i++) {
      const Move *mv = sml->moves[i];
      if (move_get_type(mv) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
        continue;
      }
      SlotMeta key = {.row = (uint8_t)move_get_row_start(mv),
                      .col_start = (uint8_t)move_get_col_start(mv),
                      .length = (uint8_t)move_get_tiles_length(mv),
                      .dir = (uint8_t)move_get_dir(mv)};
      int found = 0;
      for (int s = 0; s < slot_count; s++) {
        if (slots[s].row == key.row && slots[s].col_start == key.col_start &&
            slots[s].length == key.length && slots[s].dir == key.dir) {
          found = 1;
          break;
        }
      }
      if (!found && slot_count < max_slots) {
        slots[slot_count++] = key;
      }
    }

    // Build per-slot caches (board state at this ply).
    SlotCache *cache =
        (SlotCache *)calloc((size_t)slot_count, sizeof(SlotCache));
    for (int s = 0; s < slot_count; s++) {
      const SlotMeta k = slots[s];
      if (k.length < 2 || k.length > 15) {
        cache[s].length = 0;
        continue;
      }
      cache[s].length = k.length;
      cache[s].row = k.row;
      cache[s].col = k.col_start;
      cache[s].dir = k.dir;
      int placed_count = 0;
      int prod_word_mult = 1;
      int hooked_cross_total_eq = 0;
      int playthrough_score_eq = 0;
      int letter_mult_arr[BOARD_DIM] = {0};
      int word_mult_arr[BOARD_DIM] = {0};
      int is_cross_word_arr[BOARD_DIM] = {0};
      uint64_t flo = 0;
      uint64_t fhi = 0;
      int fixed_count = 0;
      for (int i = 0; i < k.length; i++) {
        int r = k.row;
        int c = k.col_start + i;
        if (k.dir == BOARD_VERTICAL_DIRECTION) {
          r = k.row + i;
          c = k.col_start;
        }
        const MachineLetter raw = board_get_letter(board, r, c);
        BonusSquare bsq = board_get_bonus_square(board, r, c);
        const int lm = bonus_square_get_letter_multiplier(bsq);
        const int wm = bonus_square_get_word_multiplier(bsq);
        letter_mult_arr[i] = lm;
        word_mult_arr[i] = wm;
        if (raw == ALPHABET_EMPTY_SQUARE_MARKER) {
          cache[s].fixed_letters[i] = 0;
          cache[s].cross_sets[i] =
              board_get_cross_set(board, r, c, (int)k.dir, ci);
          is_cross_word_arr[i] =
              board_get_is_cross_word(board, r, c, (int)k.dir) ? 1 : 0;
          const Equity cs_eq =
              board_get_cross_score(board, r, c, (int)k.dir, ci);
          hooked_cross_total_eq += (int)cs_eq * wm;
          prod_word_mult *= wm;
          placed_count++;
        } else {
          const MachineLetter unblanked = (MachineLetter)(raw & UNBLANK_MASK);
          cache[s].fixed_letters[i] = unblanked;
          cache[s].cross_sets[i] = 0;
          if ((raw & BLANK_MASK) == 0) {
            playthrough_score_eq += letter_scores_eq[unblanked];
          }
          const int shift = unblanked * 4;
          if (shift < 64) {
            flo += (uint64_t)1 << shift;
          } else {
            fhi += (uint64_t)1 << (shift - 64);
          }
          fixed_count++;
        }
      }
      memcpy(cache[s].fixed_bitrack, &flo, 8);
      memcpy(cache[s].fixed_bitrack + 8, &fhi, 8);
      cache[s].fb_lo = flo;
      cache[s].fb_hi = fhi;
      cache[s].target_used_size = (uint8_t)(k.length - fixed_count);
      for (int i = 0; i < k.length; i++) {
        if (cache[s].fixed_letters[i] != 0) {
          cache[s].position_multipliers[i] = 0;
        } else {
          cache[s].position_multipliers[i] =
              letter_mult_arr[i] *
              (prod_word_mult + is_cross_word_arr[i] * word_mult_arr[i]);
        }
      }
      cache[s].base_score_eq =
          playthrough_score_eq * prod_word_mult + hooked_cross_total_eq;
      cache[s].bingo_eq = (placed_count == 7) ? bingo_bonus_eq : 0;
    }

    // Enumerate all unique blank-free 7-multiset racks from the bag.
    const Bag *bag = game_get_bag(game);
    const uint32_t n_racks = enumerate_blank_free_7_racks(
        bag, rack_real, alpha, batch_racks, max_racks_per_turn);

    double cpu_us_per_rack = -1;
    double wmpg_us_per_rack = -1;
    if (slot_count > 0 && n_racks > 0) {
      // 1) CPU MAGPIE per-rack loop. Use MoveGenArgs.override_rack to make
      // generate_moves run on each enumerated rack without mutating the
      // player's actual rack on the game (which would break MAGPIE's
      // caches). This is now an apples-to-apples comparison: CPU and GPU
      // both process n_racks DISTINCT racks at this position.
      Rack tmp_rack;
      rack_set_dist_size_and_reset(&tmp_rack, alpha);
      MoveGenArgs gen_args_override = gen_args;
      gen_args_override.override_rack = &tmp_rack;
      const double t_cpu_0 = mono_s();
      for (uint32_t i = 0; i < n_racks; i++) {
        rack_from_bitrack(&tmp_rack, batch_racks + (size_t)i * BITRACK_BYTES,
                          alpha);
        generate_moves(&gen_args_override);
      }
      const double t_cpu = mono_s() - t_cpu_0;
      cpu_us_per_rack = 1e6 * t_cpu / (double)n_racks;

      // GPU batched dispatch with per-rack leave tables (leave_stride =
      // max_leaves_per_rack). Build a single concatenated leave_used /
      // leave_values buffer of size n_racks * stride, with sentinel rows
      // (all-0xFF bitrack) padding any rack with fewer than stride leaves.
      // n_leaves bound to stride for each rack — the kernel scans the full
      // stride but sentinel rows simply never match.
      const uint32_t max_leaves_per_rack = 256;
      uint8_t *leave_used = (uint8_t *)malloc(
          (size_t)n_racks * max_leaves_per_rack * BITRACK_BYTES);
      int32_t *leave_values = (int32_t *)malloc(
          (size_t)n_racks * max_leaves_per_rack * sizeof(int32_t));
      // Fill with 0xFF sentinel (will not match any 4-bit-per-letter
      // used-bitrack since real counts are 0..7 per letter).
      memset(leave_used, 0xFF,
             (size_t)n_racks * max_leaves_per_rack * BITRACK_BYTES);
      memset(leave_values, 0,
             (size_t)n_racks * max_leaves_per_rack * sizeof(int32_t));
      for (uint32_t i = 0; i < n_racks; i++) {
        rack_from_bitrack(&tmp_rack, batch_racks + (size_t)i * BITRACK_BYTES,
                          alpha);
        build_leave_table(&tmp_rack, klv, alpha,
                          leave_used +
                              (size_t)i * max_leaves_per_rack * BITRACK_BYTES,
                          leave_values + (size_t)i * max_leaves_per_rack,
                          max_leaves_per_rack);
      }

      // 2) GPU WMPG top-1 (canonical) — single batched dispatch per slot
      // per pass, all n_racks distinct racks at once.
      const double t_wm_0 = mono_s();
      gpu_matcher_top1_reset(m, n_racks);
      for (int s = 0; s < slot_count; s++) {
        if (cache[s].length == 0) {
          continue;
        }
        const SlotCache *sc = &cache[s];
        gpu_matcher_top1_pass1_wmpg(
            m, (uint32_t)sc->length, (uint32_t)sc->target_used_size,
            batch_racks, n_racks, sc->cross_sets, sc->fixed_letters, sc->fb_lo,
            sc->fb_hi, letter_scores_eq, sc->position_multipliers,
            sc->base_score_eq, sc->bingo_eq, leave_used, leave_values,
            max_leaves_per_rack, max_leaves_per_rack);
      }
      for (int s = 0; s < slot_count; s++) {
        if (cache[s].length == 0) {
          continue;
        }
        const SlotCache *sc = &cache[s];
        gpu_matcher_top1_pass2_wmpg(
            m, (uint32_t)sc->length, (uint32_t)sc->target_used_size,
            batch_racks, n_racks, sc->cross_sets, sc->fixed_letters, sc->fb_lo,
            sc->fb_hi, letter_scores_eq, sc->position_multipliers,
            sc->base_score_eq, sc->bingo_eq, leave_used, leave_values,
            max_leaves_per_rack, max_leaves_per_rack, (uint32_t)sc->row,
            (uint32_t)sc->col, (uint32_t)sc->dir, GPU_TOP1_TIEBREAK_CANONICAL);
      }
      const double t_wm = mono_s() - t_wm_0;
      wmpg_us_per_rack = 1e6 * t_wm / (double)n_racks;

      free(leave_used);
      free(leave_values);

      cpu_total_us += t_cpu * 1e6;
      wmpg_total_us += t_wm * 1e6;
      total_racks += n_racks;
      valid_turns++;
    }

    const int score_p0 = (int)player_get_score(game_get_player(game, 0));
    const int score_p1 = (int)player_get_score(game_get_player(game, 1));
    char score_buf[16];
    snprintf(score_buf, sizeof(score_buf), "%d-%d", score_p0, score_p1);
    if (slot_count > 0 && n_racks > 0) {
      printf("  %-3d %-5d %-7s %-7u %-9s %11.1f %11.1f\n", turn, slot_count,
             rack_str, n_racks, score_buf, cpu_us_per_rack, wmpg_us_per_rack);
    } else {
      printf("  %-3d %-5d %-7s %-7u %-9s         (skip: no slots or racks)\n",
             turn, slot_count, rack_str, n_racks, score_buf);
    }
    fflush(stdout);

    Rack leave;
    rack_set_dist_size_and_reset(&leave, alpha);
    if (magpie_best != NULL) {
      play_move(magpie_best, game, &leave);
    } else {
      sorted_move_list_destroy(sml);
      free(slots);
      free(cache);
      break;
    }

    sorted_move_list_destroy(sml);
    free(slots);
    free(cache);
  }

  printf("\n  Aggregate over %d plies, %llu rack-evaluations:\n", valid_turns,
         (unsigned long long)total_racks);
  if (valid_turns > 0 && total_racks > 0) {
    printf("    CPU MAGPIE generate_moves:    %9.1f us/rack (sum %.1f s)\n",
           cpu_total_us / (double)total_racks, cpu_total_us / 1e6);
    printf("    GPU WMPG top-1:               %9.1f us/rack (sum %.1f s, "
           "%.2fx vs CPU)\n",
           wmpg_total_us / (double)total_racks, wmpg_total_us / 1e6,
           cpu_total_us / wmpg_total_us);
  }

  free(batch_racks);
  free(wmpg_bytes);
  gpu_matcher_destroy(m);
  move_list_destroy(move_list);
  game_destroy(game);
  config_destroy(config);
#endif
}
