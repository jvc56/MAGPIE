// Phase 4.7a: GPU top-1 (best move per rack by equity).
//
// For each MAGPIE-enumerated slot, dispatch the GPU top1 kernel; receive a
// per-rack uint32 packed (equity_biased, word_idx) for that slot. Host-side,
// merge across slots: for each rack, track the best (equity, slot_id,
// word_idx). Compare against MAGPIE's MOVE_RECORD_BEST output for the same
// rack at the same position. Should match in equity (potentially tied at
// integer-points granularity since GPU stores plain points).

#include "gpu_top1_test.h"

#include "../src/def/board_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/kwg_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/ent/board.h"
#include "../src/ent/bonus_square.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/klv.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/players_data.h"
#include "../src/ent/rack.h"
#include "../src/ent/wmp.h"
#include "../src/impl/config.h"
#include "../src/impl/flat_lex_maker.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/wmpg_maker.h"
#include "../src/metal/movegen.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <assert.h>
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

static double monotonic_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts); // NOLINT(misc-include-cleaner)
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// Same VsMatt CGP without the embedded -lex.
#define VS_MATT_NO_LEX_CSW                                                     \
  "7ZEP1F3/1FLUKY3R1R3/5EX2A1U3/2SCARIEST1I3/9TOT3/6GO1LO4/6OR1ETA3/"          \
  "6JABS1b3/5QI4A3/5I1N3N3/3ReSPOND1D3/1HOE3V3O3/1ENCOMIA3N3/7T7/3VENGED6 "    \
  "/ 0/0 0"

void test_gpu_top1(void) {
  Config *config = config_create_or_die(
      "set -lex CSW24 -wmp true -s1 equity -s2 equity -r1 best -r2 best "
      "-numplays 1");
  Game *game = config_game_create(config);
  const LetterDistribution *ld = game_get_ld(game);
  Player *player = game_get_player(game, 0);
  const KWG *kwg = player_get_kwg(player);
  const KLV *klv = player_get_klv(player);
  const WMP *wmp = player_get_wmp(player);
  Board *board = game_get_board(game);

  load_cgp_or_die(game, VS_MATT_NO_LEX_CSW);
  Rack *rack = player_get_rack(player);
  rack_set_to_string(ld, rack, "AABDELT");

  // Build flat lex.
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
  const size_t letters_block_offset =
      bitracks_block_offset + (size_t)total_words * BITRACK_BYTES;
  const uint8_t *all_letters = flatlex_bytes + letters_block_offset;
  const size_t total_letters_bytes = flatlex_size - letters_block_offset;

  uint32_t first_word_for_length[MAX_KWG_STRING_LENGTH] = {0};
  size_t letters_byte_offset_for[MAX_KWG_STRING_LENGTH] = {0};
  uint32_t cum_words = 0;
  size_t cum_letters = 0;
  for (int len = 0; len < max_len_plus_one; len++) {
    first_word_for_length[len] = cum_words;
    letters_byte_offset_for[len] = cum_letters;
    cum_words += per_length_count[len];
    cum_letters += (size_t)per_length_count[len] * (size_t)len;
  }

  // Build rack BitRack for AABDELT.
  uint8_t rack_br[BITRACK_BYTES] = {0};
  {
    uint64_t lo = 0;
    uint64_t hi = 0;
    const uint8_t mls[] = {1, 2, 4, 5, 12, 20};
    const uint8_t cnts[] = {2, 1, 1, 1, 1, 1};
    for (int i = 0; i < 6; i++) {
      const int shift = mls[i] * 4;
      if (shift < 64) {
        lo += (uint64_t)cnts[i] << shift;
      } else {
        hi += (uint64_t)cnts[i] << (shift - 64);
      }
    }
    memcpy(rack_br, &lo, 8);
    memcpy(rack_br + 8, &hi, 8);
  }

  // Build per-rack leave table (96 entries for AABDELT).
  const int alpha = ld_get_size(ld);
  const int max_leaves = 512;
  uint8_t *leave_used =
      (uint8_t *)calloc((size_t)max_leaves * BITRACK_BYTES, 1);
  int32_t *leave_values =
      (int32_t *)calloc((size_t)max_leaves, sizeof(int32_t));
  uint32_t n_leaves = 0;
  {
    int stack[MAX_ALPHABET_SIZE] = {0};
    int max_count[MAX_ALPHABET_SIZE] = {0};
    for (int ml = 0; ml < alpha; ml++) {
      max_count[ml] = rack_get_letter(rack, ml);
    }
    int level = 0;
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
            rack_set_letter(&leave, ml, (uint16_t)rem);
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
        memcpy(leave_used + (size_t)n_leaves * BITRACK_BYTES, &lo, 8);
        memcpy(leave_used + (size_t)n_leaves * BITRACK_BYTES + 8, &hi, 8);
        leave_values[n_leaves] = (int32_t)lv;
        n_leaves++;
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
  }

  // Run MAGPIE movegen and find the BEST tile-placement move (by equity).
  MoveList *move_list = move_list_create(100000);
  const MoveGenArgs args = {
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
  generate_moves(&args);
  SortedMoveList *sml = sorted_move_list_create(move_list);
  // Find best tile-placement move (skip exchanges/passes).
  const Move *magpie_best = NULL;
  Equity magpie_best_eq = EQUITY_MIN_VALUE;
  for (int i = 0; i < sml->count; i++) {
    const Move *mv = sml->moves[i];
    if (move_get_type(mv) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      continue;
    }
    const Equity eq = move_get_equity(mv);
    if (eq > magpie_best_eq) {
      magpie_best_eq = eq;
      magpie_best = mv;
    }
  }
  assert(magpie_best != NULL);

  // Enumerate slots from MAGPIE's tile-placement plays (same as
  // gpucross_validate).
  typedef struct SlotMeta {
    uint8_t row, col_start, length, dir;
  } SlotMeta;
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
    if (!found) {
      assert(slot_count < max_slots);
      slots[slot_count++] = key;
    }
  }

  printf("phase 4.7a top-1 GPU vs MAGPIE (CSW24, VsMatt + AABDELT, %d slots)\n",
         slot_count);

#if HAVE_GPU
  if (!gpu_matcher_is_available()) {
    printf("  GPU not available, skipping\n");
    free(slots);
    sorted_move_list_destroy(sml);
    move_list_destroy(move_list);
    free(leave_used);
    free(leave_values);
    free(flatlex_bytes);
    game_destroy(game);
    config_destroy(config);
    return;
  }
  GpuMatcher *m =
      gpu_matcher_create("bin/movegen.metallib", all_bitracks, total_words,
                         all_letters, total_letters_bytes);
  if (m == NULL) {
    log_fatal("gpu_matcher_create failed\n");
  }

  // Build + upload WMPG so the WMPG top-1 kernels have a hash table to query.
  uint8_t *wmpg_bytes = NULL;
  size_t wmpg_size = 0;
  ErrorStack *es_wmpg = error_stack_create();
  wmpg_build(wmp, &wmpg_bytes, &wmpg_size, es_wmpg);
  if (!error_stack_is_empty(es_wmpg)) {
    error_stack_print_and_reset(es_wmpg);
    log_fatal("wmpg_build failed\n");
  }
  error_stack_destroy(es_wmpg);
  if (!gpu_matcher_load_wmpg(m, wmpg_bytes, wmpg_size)) {
    log_fatal("gpu_matcher_load_wmpg failed\n");
  }

  // letter_scores in millipoints (raw Equity), since equity also is.
  int32_t letter_scores_eq[32] = {0};
  for (int ml = 0; ml < 32; ml++) {
    if (ml < alpha) {
      letter_scores_eq[ml] = (int32_t)ld_get_score(ld, (MachineLetter)ml);
    }
  }
  const int32_t bingo_bonus_eq = (int32_t)game_get_bingo_bonus(game);

  PlayersData *pd = config_get_players_data(config);
  const KWG *p1_kwg = players_data_get_kwg(pd, 1);
  const int ci = board_get_cross_set_index(p1_kwg == kwg, 0);

  // GPU side: two-pass per slot. Pass 1 across all slots accumulates the
  // per-rack max equity_mp; pass 2 across all slots writes location of the
  // first match at max equity per rack.
  const uint32_t B = 1;
  gpu_matcher_top1_reset(m, B);

  // Cache slot data so we don't recompute between passes.
  typedef struct SlotCache {
    uint64_t cross_sets[BOARD_DIM];
    uint8_t fixed_letters[BOARD_DIM];
    uint8_t fixed_bitrack[BITRACK_BYTES];
    int32_t position_multipliers[BOARD_DIM];
    int32_t base_score_eq;
    int32_t bingo_eq;
    uint8_t length;
  } SlotCache;
  SlotCache *cache = (SlotCache *)calloc((size_t)slot_count, sizeof(SlotCache));

  for (int s = 0; s < slot_count; s++) {
    const SlotMeta k = slots[s];
    if (k.length < 2 || k.length > 15) {
      cache[s].length = 0;
      continue;
    }
    cache[s].length = k.length;
    int placed_count = 0;
    int prod_word_mult = 1;
    int hooked_cross_total_eq = 0;
    int playthrough_score_eq = 0;
    int letter_mult_arr[BOARD_DIM] = {0};
    int word_mult_arr[BOARD_DIM] = {0};
    int is_cross_word_arr[BOARD_DIM] = {0};
    {
      uint64_t flo = 0;
      uint64_t fhi = 0;
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
        }
      }
      memcpy(cache[s].fixed_bitrack, &flo, 8);
      memcpy(cache[s].fixed_bitrack + 8, &fhi, 8);
    }
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

  // Pass 1: equity_mp atomic_max across all slots.
  for (int s = 0; s < slot_count; s++) {
    if (cache[s].length == 0) {
      continue;
    }
    const SlotCache *sc = &cache[s];
    gpu_matcher_top1_pass1(
        m, first_word_for_length[sc->length], per_length_count[sc->length],
        letters_byte_offset_for[sc->length], (uint32_t)sc->length, rack_br, B,
        sc->cross_sets, sc->fixed_letters, sc->fixed_bitrack, letter_scores_eq,
        sc->position_multipliers, sc->base_score_eq, sc->bingo_eq, leave_used,
        leave_values, n_leaves, 0u);
  }
  // Pass 2: run BOTH tiebreak modes. Both should produce the same answer
  // unless there's an exact equity tie (rare).
  const GpuTop1Tiebreak modes[] = {GPU_TOP1_TIEBREAK_SPEEDY,
                                   GPU_TOP1_TIEBREAK_CANONICAL};
  uint32_t loc_per_mode[2] = {0, 0};
  int32_t eq_per_mode[2] = {0, 0};
  for (int mi = 0; mi < 2; mi++) {
    // Reset best_loc for each mode (best_eq_mp stays from pass 1).
    int32_t *eq_buf = (int32_t *)malloc(B * sizeof(int32_t));
    uint32_t *loc_buf = (uint32_t *)malloc(B * sizeof(uint32_t));
    // Reset best_loc only (keep best_eq_mp). Read+rewrite via reset+pass1
    // avoids needing a separate "reset_loc_only" API.
    gpu_matcher_top1_reset(m, B);
    for (int s = 0; s < slot_count; s++) {
      if (cache[s].length == 0) {
        continue;
      }
      const SlotCache *sc = &cache[s];
      gpu_matcher_top1_pass1(
          m, first_word_for_length[sc->length], per_length_count[sc->length],
          letters_byte_offset_for[sc->length], (uint32_t)sc->length, rack_br, B,
          sc->cross_sets, sc->fixed_letters, sc->fixed_bitrack,
          letter_scores_eq, sc->position_multipliers, sc->base_score_eq,
          sc->bingo_eq, leave_used, leave_values, n_leaves, 0u);
    }
    for (int s = 0; s < slot_count; s++) {
      if (cache[s].length == 0) {
        continue;
      }
      const SlotMeta k = slots[s];
      const SlotCache *sc = &cache[s];
      gpu_matcher_top1_pass2(
          m, first_word_for_length[sc->length], per_length_count[sc->length],
          letters_byte_offset_for[sc->length], (uint32_t)sc->length, rack_br, B,
          sc->cross_sets, sc->fixed_letters, sc->fixed_bitrack,
          letter_scores_eq, sc->position_multipliers, sc->base_score_eq,
          sc->bingo_eq, leave_used, leave_values, n_leaves, 0u, (uint32_t)k.row,
          (uint32_t)k.col_start, (uint32_t)k.dir, modes[mi]);
    }
    gpu_matcher_top1_read(m, B, eq_buf, loc_buf);
    eq_per_mode[mi] = eq_buf[0];
    loc_per_mode[mi] = loc_buf[0];
    free(eq_buf);
    free(loc_buf);
  }
  // Use canonical's result for the MAGPIE comparison.
  const int32_t global_best_equity_mp = eq_per_mode[1];
  const uint32_t best_loc = loc_per_mode[1];
  const uint32_t global_best_row =
      (best_loc != 0xFFFFFFFFu) ? ((best_loc >> 28) & 0xFu) : 0;
  const uint32_t global_best_col =
      (best_loc != 0xFFFFFFFFu) ? ((best_loc >> 24) & 0xFu) : 0;
  const uint32_t global_best_dir =
      (best_loc != 0xFFFFFFFFu) ? ((best_loc >> 23) & 0x1u) : 0;
  const uint32_t global_best_length =
      (best_loc != 0xFFFFFFFFu) ? ((best_loc >> 19) & 0xFu) : 0;
  const uint32_t global_best_word_idx = best_loc & 0x7FFFFu;
  free(cache);

  // Decode global best from the packed locator (length is now in the locator
  // itself rather than via a slot table).
  const uint8_t *best_word_letters =
      all_letters + letters_byte_offset_for[global_best_length] +
      (size_t)global_best_word_idx * (size_t)global_best_length;
  char gpu_word_buf[16] = {0};
  for (uint32_t i = 0; i < global_best_length; i++) {
    gpu_word_buf[i] = 'A' + best_word_letters[i] - 1;
  }

  // MAGPIE side: extract its best move's word.
  char magpie_word_buf[16] = {0};
  const int magpie_len = move_get_tiles_length(magpie_best);
  for (int i = 0; i < magpie_len && i < 15; i++) {
    const MachineLetter t = move_get_tile(magpie_best, i);
    if (t == 0) {
      magpie_word_buf[i] = '.';
    } else {
      const MachineLetter unblanked = (MachineLetter)(t & UNBLANK_MASK);
      magpie_word_buf[i] =
          (unblanked >= 1 && unblanked <= 26) ? ('A' + unblanked - 1) : '?';
    }
  }
  printf("  MAGPIE best: %s row=%d col=%d L=%d dir=%s eq=%d mp\n",
         magpie_word_buf, move_get_row_start(magpie_best),
         move_get_col_start(magpie_best), magpie_len,
         move_get_dir(magpie_best) == BOARD_HORIZONTAL_DIRECTION ? "H" : "V",
         (int)magpie_best_eq);
  printf("  GPU best:    %s row=%u col=%u L=%u dir=%s eq=%d mp (canonical)\n",
         gpu_word_buf, global_best_row, global_best_col, global_best_length,
         global_best_dir == 0 ? "H" : "V", global_best_equity_mp);

  // WMPG two-pass top-1: should produce the same best equity_mp. The packed
  // locator's word_idx field encodes a per-thread anagram counter rather than
  // a flat-lex word_idx, so we compare equity only (same row/col/dir/length
  // are still expected). Re-derive cache because the brute-force run already
  // freed it.
  {
    typedef struct SlotCacheWmpg {
      uint64_t cross_sets[BOARD_DIM];
      uint8_t fixed_letters[BOARD_DIM];
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
    } SlotCacheWmpg;
    SlotCacheWmpg *wcache =
        (SlotCacheWmpg *)calloc((size_t)slot_count, sizeof(SlotCacheWmpg));
    for (int s = 0; s < slot_count; s++) {
      const SlotMeta k = slots[s];
      if (k.length < 2 || k.length > 15) {
        wcache[s].length = 0;
        continue;
      }
      wcache[s].length = k.length;
      wcache[s].row = k.row;
      wcache[s].col = k.col_start;
      wcache[s].dir = k.dir;
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
          wcache[s].fixed_letters[i] = 0;
          wcache[s].cross_sets[i] =
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
          wcache[s].fixed_letters[i] = unblanked;
          wcache[s].cross_sets[i] = 0;
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
      wcache[s].fb_lo = flo;
      wcache[s].fb_hi = fhi;
      wcache[s].target_used_size = (uint8_t)(k.length - fixed_count);
      for (int i = 0; i < k.length; i++) {
        if (wcache[s].fixed_letters[i] != 0) {
          wcache[s].position_multipliers[i] = 0;
        } else {
          wcache[s].position_multipliers[i] =
              letter_mult_arr[i] *
              (prod_word_mult + is_cross_word_arr[i] * word_mult_arr[i]);
        }
      }
      wcache[s].base_score_eq =
          playthrough_score_eq * prod_word_mult + hooked_cross_total_eq;
      wcache[s].bingo_eq = (placed_count == 7) ? bingo_bonus_eq : 0;
    }

    int32_t wmpg_eq_per_mode[2] = {0, 0};
    uint32_t wmpg_loc_per_mode[2] = {0, 0};
    for (int mi = 0; mi < 2; mi++) {
      gpu_matcher_top1_reset(m, B);
      for (int s = 0; s < slot_count; s++) {
        if (wcache[s].length == 0) {
          continue;
        }
        const SlotCacheWmpg *sc = &wcache[s];
        gpu_matcher_top1_pass1_wmpg(
            m, (uint32_t)sc->length, (uint32_t)sc->target_used_size, rack_br, B,
            sc->cross_sets, sc->fixed_letters, sc->fb_lo, sc->fb_hi,
            letter_scores_eq, sc->position_multipliers, sc->base_score_eq,
            sc->bingo_eq, leave_used, leave_values, n_leaves, 0u);
      }
      for (int s = 0; s < slot_count; s++) {
        if (wcache[s].length == 0) {
          continue;
        }
        const SlotCacheWmpg *sc = &wcache[s];
        gpu_matcher_top1_pass2_wmpg(
            m, (uint32_t)sc->length, (uint32_t)sc->target_used_size, rack_br, B,
            sc->cross_sets, sc->fixed_letters, sc->fb_lo, sc->fb_hi,
            letter_scores_eq, sc->position_multipliers, sc->base_score_eq,
            sc->bingo_eq, leave_used, leave_values, n_leaves, 0u,
            (uint32_t)sc->row, (uint32_t)sc->col, (uint32_t)sc->dir, modes[mi]);
      }
      int32_t eq_buf2 = 0;
      uint32_t loc_buf2 = 0;
      gpu_matcher_top1_read(m, B, &eq_buf2, &loc_buf2);
      wmpg_eq_per_mode[mi] = eq_buf2;
      wmpg_loc_per_mode[mi] = loc_buf2;
    }
    free(wcache);

    const int32_t wmpg_best_eq = wmpg_eq_per_mode[1];
    const uint32_t wmpg_best_loc = wmpg_loc_per_mode[1];
    const uint32_t wmpg_row =
        (wmpg_best_loc != 0xFFFFFFFFu) ? ((wmpg_best_loc >> 28) & 0xFu) : 0;
    const uint32_t wmpg_col =
        (wmpg_best_loc != 0xFFFFFFFFu) ? ((wmpg_best_loc >> 24) & 0xFu) : 0;
    const uint32_t wmpg_dir =
        (wmpg_best_loc != 0xFFFFFFFFu) ? ((wmpg_best_loc >> 23) & 0x1u) : 0;
    const uint32_t wmpg_length =
        (wmpg_best_loc != 0xFFFFFFFFu) ? ((wmpg_best_loc >> 19) & 0xFu) : 0;
    printf("  GPU WMPG:    row=%u col=%u L=%u dir=%s eq=%d mp (canonical) %s\n",
           wmpg_row, wmpg_col, wmpg_length, wmpg_dir == 0 ? "H" : "V",
           wmpg_best_eq,
           wmpg_best_eq == global_best_equity_mp ? "ok" : "EQUITY-MISMATCH");
    if (wmpg_eq_per_mode[0] != wmpg_eq_per_mode[1]) {
      printf("  WMPG MODE-MISMATCH: speedy=%d canonical=%d\n",
             wmpg_eq_per_mode[0], wmpg_eq_per_mode[1]);
    }
  }

  // Batched perf comparison: top-1 (two-pass) vs equity (one-pass, all
  // matches). Same rack repeated B times per dispatch.
  printf("\n  Top-1 (two-pass) vs Equity (one-pass) batched throughput:\n");
  const int Bs[] = {1, 64, 1024, 4096, 18571};
  const int n_Bs = sizeof(Bs) / sizeof(Bs[0]);
  const int B_MAX = Bs[n_Bs - 1];
  uint8_t *batch_racks = (uint8_t *)malloc((size_t)B_MAX * BITRACK_BYTES);
  for (int i = 0; i < B_MAX; i++) {
    memcpy(batch_racks + (size_t)i * BITRACK_BYTES, rack_br, BITRACK_BYTES);
  }
  // Re-cache slots (we freed `cache` earlier; rebuild for the bench).
  typedef struct SlotCache2 {
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
  } SlotCache2;
  SlotCache2 *cache2 =
      (SlotCache2 *)calloc((size_t)slot_count, sizeof(SlotCache2));
  for (int s = 0; s < slot_count; s++) {
    const SlotMeta k = slots[s];
    if (k.length < 2 || k.length > 15) {
      cache2[s].length = 0;
      continue;
    }
    cache2[s].length = k.length;
    cache2[s].row = k.row;
    cache2[s].col = k.col_start;
    cache2[s].dir = k.dir;
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
        cache2[s].fixed_letters[i] = 0;
        cache2[s].cross_sets[i] =
            board_get_cross_set(board, r, c, (int)k.dir, ci);
        is_cross_word_arr[i] =
            board_get_is_cross_word(board, r, c, (int)k.dir) ? 1 : 0;
        const Equity cs_eq = board_get_cross_score(board, r, c, (int)k.dir, ci);
        hooked_cross_total_eq += (int)cs_eq * wm;
        prod_word_mult *= wm;
        placed_count++;
      } else {
        const MachineLetter unblanked = (MachineLetter)(raw & UNBLANK_MASK);
        cache2[s].fixed_letters[i] = unblanked;
        cache2[s].cross_sets[i] = 0;
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
    memcpy(cache2[s].fixed_bitrack, &flo, 8);
    memcpy(cache2[s].fixed_bitrack + 8, &fhi, 8);
    cache2[s].fb_lo = flo;
    cache2[s].fb_hi = fhi;
    cache2[s].target_used_size = (uint8_t)(k.length - fixed_count);
    for (int i = 0; i < k.length; i++) {
      if (cache2[s].fixed_letters[i] != 0) {
        cache2[s].position_multipliers[i] = 0;
      } else {
        cache2[s].position_multipliers[i] =
            letter_mult_arr[i] *
            (prod_word_mult + is_cross_word_arr[i] * word_mult_arr[i]);
      }
    }
    cache2[s].base_score_eq =
        playthrough_score_eq * prod_word_mult + hooked_cross_total_eq;
    cache2[s].bingo_eq = (placed_count == 7) ? bingo_bonus_eq : 0;
  }
  uint32_t *equity_pair_buf =
      (uint32_t *)malloc((size_t)B_MAX * 2 * sizeof(uint32_t));

  printf("    %-7s %-22s %-22s %-22s\n", "B", "equity-1pass wall|gpu us/r",
         "top1-speedy wall|gpu us/r", "top1-canon wall|gpu us/r");
  for (int bi = 0; bi < n_Bs; bi++) {
    const int B = Bs[bi];

    // Equity (one-pass)
    for (int warm = 0; warm < 2; warm++) {
      for (int s = 0; s < slot_count; s++) {
        if (cache2[s].length == 0) {
          continue;
        }
        const SlotCache2 *sc = &cache2[s];
        gpu_matcher_equity(
            m, first_word_for_length[sc->length], per_length_count[sc->length],
            letters_byte_offset_for[sc->length], (uint32_t)sc->length,
            batch_racks, (uint32_t)B, sc->cross_sets, sc->fixed_letters,
            sc->fixed_bitrack, letter_scores_eq, sc->position_multipliers,
            sc->base_score_eq, sc->bingo_eq, leave_used, leave_values, n_leaves,
            equity_pair_buf);
      }
    }
    {
      double total_eq_gpu_us = 0;
      const double t0 = monotonic_seconds();
      for (int s = 0; s < slot_count; s++) {
        if (cache2[s].length == 0) {
          continue;
        }
        const SlotCache2 *sc = &cache2[s];
        gpu_matcher_equity(
            m, first_word_for_length[sc->length], per_length_count[sc->length],
            letters_byte_offset_for[sc->length], (uint32_t)sc->length,
            batch_racks, (uint32_t)B, sc->cross_sets, sc->fixed_letters,
            sc->fixed_bitrack, letter_scores_eq, sc->position_multipliers,
            sc->base_score_eq, sc->bingo_eq, leave_used, leave_values, n_leaves,
            equity_pair_buf);
        total_eq_gpu_us += gpu_matcher_get_last_gpu_us(m);
      }
      const double dt_eq = monotonic_seconds() - t0;
      const double per_rack_eq_us = 1e6 * dt_eq / B;
      const double per_rack_eq_gpu_us = total_eq_gpu_us / B;

      // Top-1 each mode
      double per_rack_top1_us[2] = {0, 0};
      double per_rack_top1_gpu_us[2] = {0, 0};
      for (int mi = 0; mi < 2; mi++) {
        // Warmup
        gpu_matcher_top1_reset(m, B);
        for (int s = 0; s < slot_count; s++) {
          if (cache2[s].length == 0) {
            continue;
          }
          const SlotCache2 *sc = &cache2[s];
          gpu_matcher_top1_pass1(
              m, first_word_for_length[sc->length],
              per_length_count[sc->length], letters_byte_offset_for[sc->length],
              (uint32_t)sc->length, batch_racks, (uint32_t)B, sc->cross_sets,
              sc->fixed_letters, sc->fixed_bitrack, letter_scores_eq,
              sc->position_multipliers, sc->base_score_eq, sc->bingo_eq,
              leave_used, leave_values, n_leaves, 0u);
        }
        for (int s = 0; s < slot_count; s++) {
          if (cache2[s].length == 0) {
            continue;
          }
          const SlotCache2 *sc = &cache2[s];
          gpu_matcher_top1_pass2(
              m, first_word_for_length[sc->length],
              per_length_count[sc->length], letters_byte_offset_for[sc->length],
              (uint32_t)sc->length, batch_racks, (uint32_t)B, sc->cross_sets,
              sc->fixed_letters, sc->fixed_bitrack, letter_scores_eq,
              sc->position_multipliers, sc->base_score_eq, sc->bingo_eq,
              leave_used, leave_values, n_leaves, 0u, (uint32_t)sc->row,
              (uint32_t)sc->col, (uint32_t)sc->dir, modes[mi]);
        }
        // Timed
        double total_top1_gpu_us = 0;
        const double tt0 = monotonic_seconds();
        gpu_matcher_top1_reset(m, B);
        for (int s = 0; s < slot_count; s++) {
          if (cache2[s].length == 0) {
            continue;
          }
          const SlotCache2 *sc = &cache2[s];
          gpu_matcher_top1_pass1(
              m, first_word_for_length[sc->length],
              per_length_count[sc->length], letters_byte_offset_for[sc->length],
              (uint32_t)sc->length, batch_racks, (uint32_t)B, sc->cross_sets,
              sc->fixed_letters, sc->fixed_bitrack, letter_scores_eq,
              sc->position_multipliers, sc->base_score_eq, sc->bingo_eq,
              leave_used, leave_values, n_leaves, 0u);
          total_top1_gpu_us += gpu_matcher_get_last_gpu_us(m);
        }
        for (int s = 0; s < slot_count; s++) {
          if (cache2[s].length == 0) {
            continue;
          }
          const SlotCache2 *sc = &cache2[s];
          gpu_matcher_top1_pass2(
              m, first_word_for_length[sc->length],
              per_length_count[sc->length], letters_byte_offset_for[sc->length],
              (uint32_t)sc->length, batch_racks, (uint32_t)B, sc->cross_sets,
              sc->fixed_letters, sc->fixed_bitrack, letter_scores_eq,
              sc->position_multipliers, sc->base_score_eq, sc->bingo_eq,
              leave_used, leave_values, n_leaves, 0u, (uint32_t)sc->row,
              (uint32_t)sc->col, (uint32_t)sc->dir, modes[mi]);
          total_top1_gpu_us += gpu_matcher_get_last_gpu_us(m);
        }
        const double tt = monotonic_seconds() - tt0;
        per_rack_top1_us[mi] = 1e6 * tt / B;
        per_rack_top1_gpu_us[mi] = total_top1_gpu_us / B;
      }
      printf("    %-7d %9.1f|%-9.1f         %9.1f|%-9.1f         "
             "%9.1f|%-9.1f\n",
             B, per_rack_eq_us, per_rack_eq_gpu_us, per_rack_top1_us[0],
             per_rack_top1_gpu_us[0], per_rack_top1_us[1],
             per_rack_top1_gpu_us[1]);
    }
  }
  // WMPG batched throughput: same B values, same slots, but using the WMPG
  // (hash-table) kernels instead of the brute-force scan. One thread per rack;
  // each thread enumerates subracks and hash-looks-up. Crossover with
  // brute-force is expected somewhere as B grows.
  printf("\n  WMPG batched throughput (same rack B× per slot):\n");
  printf("    %-7s %-22s %-22s %-22s\n", "B", "eq-wmpg wall|gpu us/r",
         "top1-wmpg-spd wall|gpu us/r", "top1-wmpg-can wall|gpu us/r");
  uint32_t *wmpg_pair_buf =
      (uint32_t *)malloc((size_t)B_MAX * 2 * sizeof(uint32_t));
  for (int bi = 0; bi < n_Bs; bi++) {
    const int B = Bs[bi];

    // Equity-WMPG warmup
    for (int warm = 0; warm < 2; warm++) {
      for (int s = 0; s < slot_count; s++) {
        if (cache2[s].length == 0) {
          continue;
        }
        const SlotCache2 *sc = &cache2[s];
        gpu_matcher_equity_wmpg(
            m, (uint32_t)sc->length, (uint32_t)sc->target_used_size,
            batch_racks, (uint32_t)B, sc->cross_sets, sc->fixed_letters,
            sc->fb_lo, sc->fb_hi, letter_scores_eq, sc->position_multipliers,
            sc->base_score_eq, sc->bingo_eq, leave_used, leave_values, n_leaves,
            wmpg_pair_buf);
      }
    }
    double total_eq_wmpg_gpu_us = 0;
    const double e0 = monotonic_seconds();
    for (int s = 0; s < slot_count; s++) {
      if (cache2[s].length == 0) {
        continue;
      }
      const SlotCache2 *sc = &cache2[s];
      gpu_matcher_equity_wmpg(
          m, (uint32_t)sc->length, (uint32_t)sc->target_used_size, batch_racks,
          (uint32_t)B, sc->cross_sets, sc->fixed_letters, sc->fb_lo, sc->fb_hi,
          letter_scores_eq, sc->position_multipliers, sc->base_score_eq,
          sc->bingo_eq, leave_used, leave_values, n_leaves, wmpg_pair_buf);
      total_eq_wmpg_gpu_us += gpu_matcher_get_last_gpu_us(m);
    }
    const double dt_eq_wmpg = monotonic_seconds() - e0;
    const double per_rack_eq_wmpg_us = 1e6 * dt_eq_wmpg / B;
    const double per_rack_eq_wmpg_gpu_us = total_eq_wmpg_gpu_us / B;

    // Top1-WMPG each mode
    double per_rack_top1_wmpg_us[2] = {0, 0};
    double per_rack_top1_wmpg_gpu_us[2] = {0, 0};
    for (int mi = 0; mi < 2; mi++) {
      // Warmup
      gpu_matcher_top1_reset(m, (uint32_t)B);
      for (int s = 0; s < slot_count; s++) {
        if (cache2[s].length == 0) {
          continue;
        }
        const SlotCache2 *sc = &cache2[s];
        gpu_matcher_top1_pass1_wmpg(
            m, (uint32_t)sc->length, (uint32_t)sc->target_used_size,
            batch_racks, (uint32_t)B, sc->cross_sets, sc->fixed_letters,
            sc->fb_lo, sc->fb_hi, letter_scores_eq, sc->position_multipliers,
            sc->base_score_eq, sc->bingo_eq, leave_used, leave_values, n_leaves,
            0u);
      }
      for (int s = 0; s < slot_count; s++) {
        if (cache2[s].length == 0) {
          continue;
        }
        const SlotCache2 *sc = &cache2[s];
        gpu_matcher_top1_pass2_wmpg(
            m, (uint32_t)sc->length, (uint32_t)sc->target_used_size,
            batch_racks, (uint32_t)B, sc->cross_sets, sc->fixed_letters,
            sc->fb_lo, sc->fb_hi, letter_scores_eq, sc->position_multipliers,
            sc->base_score_eq, sc->bingo_eq, leave_used, leave_values, n_leaves,
            0u, (uint32_t)sc->row, (uint32_t)sc->col, (uint32_t)sc->dir,
            modes[mi]);
      }
      // Timed
      double total_t1_wmpg_gpu_us = 0;
      const double tt0 = monotonic_seconds();
      gpu_matcher_top1_reset(m, (uint32_t)B);
      for (int s = 0; s < slot_count; s++) {
        if (cache2[s].length == 0) {
          continue;
        }
        const SlotCache2 *sc = &cache2[s];
        gpu_matcher_top1_pass1_wmpg(
            m, (uint32_t)sc->length, (uint32_t)sc->target_used_size,
            batch_racks, (uint32_t)B, sc->cross_sets, sc->fixed_letters,
            sc->fb_lo, sc->fb_hi, letter_scores_eq, sc->position_multipliers,
            sc->base_score_eq, sc->bingo_eq, leave_used, leave_values, n_leaves,
            0u);
        total_t1_wmpg_gpu_us += gpu_matcher_get_last_gpu_us(m);
      }
      for (int s = 0; s < slot_count; s++) {
        if (cache2[s].length == 0) {
          continue;
        }
        const SlotCache2 *sc = &cache2[s];
        gpu_matcher_top1_pass2_wmpg(
            m, (uint32_t)sc->length, (uint32_t)sc->target_used_size,
            batch_racks, (uint32_t)B, sc->cross_sets, sc->fixed_letters,
            sc->fb_lo, sc->fb_hi, letter_scores_eq, sc->position_multipliers,
            sc->base_score_eq, sc->bingo_eq, leave_used, leave_values, n_leaves,
            0u, (uint32_t)sc->row, (uint32_t)sc->col, (uint32_t)sc->dir,
            modes[mi]);
        total_t1_wmpg_gpu_us += gpu_matcher_get_last_gpu_us(m);
      }
      const double tt = monotonic_seconds() - tt0;
      per_rack_top1_wmpg_us[mi] = 1e6 * tt / B;
      per_rack_top1_wmpg_gpu_us[mi] = total_t1_wmpg_gpu_us / B;
    }
    printf("    %-7d %9.1f|%-9.1f         %9.1f|%-9.1f         "
           "%9.1f|%-9.1f\n",
           B, per_rack_eq_wmpg_us, per_rack_eq_wmpg_gpu_us,
           per_rack_top1_wmpg_us[0], per_rack_top1_wmpg_gpu_us[0],
           per_rack_top1_wmpg_us[1], per_rack_top1_wmpg_gpu_us[1]);
  }
  free(wmpg_pair_buf);

  free(equity_pair_buf);
  free(cache2);
  free(batch_racks);
  // Speedy result for comparison.
  const uint32_t speedy_loc = loc_per_mode[0];
  if (speedy_loc != 0xFFFFFFFFu) {
    const uint32_t srow = (speedy_loc >> 28) & 0xFu;
    const uint32_t scol = (speedy_loc >> 24) & 0xFu;
    const uint32_t sdir = (speedy_loc >> 23) & 0x1u;
    const uint32_t slen = (speedy_loc >> 19) & 0xFu;
    const uint32_t swi = speedy_loc & 0x7FFFFu;
    const uint8_t *sw_letters = all_letters + letters_byte_offset_for[slen] +
                                (size_t)swi * (size_t)slen;
    char sbuf[16] = {0};
    for (uint32_t i = 0; i < slen && i < 15; i++) {
      sbuf[i] = 'A' + sw_letters[i] - 1;
    }
    const int both_agree = (loc_per_mode[0] == loc_per_mode[1] &&
                            eq_per_mode[0] == eq_per_mode[1]);
    printf("  GPU best:    %s row=%u col=%u L=%u dir=%s eq=%d mp (speedy)\n",
           sbuf, srow, scol, slen, sdir == 0 ? "H" : "V", eq_per_mode[0]);
    printf("  Mode agreement: %s\n",
           both_agree ? "ok (no equity ties)"
                      : "DIFFER — equity tie broken differently");
  }

  const int eq_match = ((int32_t)magpie_best_eq == global_best_equity_mp);
  const int slot_match =
      ((uint32_t)move_get_row_start(magpie_best) == global_best_row &&
       (uint32_t)move_get_col_start(magpie_best) == global_best_col &&
       (uint32_t)move_get_tiles_length(magpie_best) == global_best_length &&
       (uint32_t)move_get_dir(magpie_best) == global_best_dir);
  printf("  Equity match: %s   Slot match: %s\n", eq_match ? "ok" : "MISMATCH",
         slot_match ? "ok" : "differ-but-tied?");

  gpu_matcher_destroy(m);
#else
  printf("  GPU section compiled out; non-Darwin build\n");
#endif

  free(slots);
  sorted_move_list_destroy(sml);
  move_list_destroy(move_list);
  free(leave_used);
  free(leave_values);
  free(flatlex_bytes);
  game_destroy(game);
  config_destroy(config);
}
