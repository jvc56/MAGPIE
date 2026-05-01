// WMPG top-1 GPU vs MAGPIE: correctness test.
//
// For the VsMatt position with three test racks (AABDELT = blank-free,
// ?ABDELT = 1 blank, ??BDELT = 2 blanks), runs WMPG top-1 (two-pass
// canonical) across all slots produced by MAGPIE's r1=ALL move list and
// asserts the best equity matches MAGPIE's best move equity.

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
#include "../src/ent/rack.h"
#include "../src/ent/wmp.h"
#include "../src/impl/config.h"
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

#if defined(__APPLE__)
#define HAVE_GPU 1
#else
#define HAVE_GPU 0
#endif

#define BITRACK_BYTES 16

#define VS_MATT_NO_LEX_CSW                                                     \
  "7ZEP1F3/1FLUKY3R1R3/5EX2A1U3/2SCARIEST1I3/9TOT3/6GO1LO4/6OR1ETA3/"          \
  "6JABS1b3/5QI4A3/5I1N3N3/3ReSPOND1D3/1HOE3V3O3/1ENCOMIA3N3/7T7/3VENGED6 "    \
  "/ 0/0 0"

#if HAVE_GPU
typedef struct SlotMeta {
  uint8_t row, col_start, length, dir;
} SlotMeta;

typedef struct SlotCache {
  uint64_t cross_sets[BOARD_DIM];
  uint8_t fixed_letters[BOARD_DIM];
  uint64_t fb_lo;
  uint64_t fb_hi;
  int32_t position_multipliers[BOARD_DIM];
  int32_t base_score_eq;
  int32_t bingo_eq;
  uint8_t length;
  uint8_t target_used_size;
  uint8_t row, col, dir;
} SlotCache;

// Run MAGPIE generate_moves and find the best tile-placement move + the
// unique slot set that produced any tile-placement.
static const Move *find_magpie_best_and_slots(MoveList *move_list,
                                              const MoveGenArgs *args,
                                              SortedMoveList **out_sml,
                                              SlotMeta *slots_out,
                                              int max_slots, int *out_count) {
  generate_moves(args);
  SortedMoveList *sml = sorted_move_list_create(move_list);
  *out_sml = sml;
  const Move *best = NULL;
  Equity best_eq = EQUITY_MIN_VALUE;
  int slot_count = 0;
  for (int i = 0; i < sml->count; i++) {
    const Move *mv = sml->moves[i];
    if (move_get_type(mv) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      continue;
    }
    if (move_get_equity(mv) > best_eq) {
      best_eq = move_get_equity(mv);
      best = mv;
    }
    SlotMeta key = {.row = (uint8_t)move_get_row_start(mv),
                    .col_start = (uint8_t)move_get_col_start(mv),
                    .length = (uint8_t)move_get_tiles_length(mv),
                    .dir = (uint8_t)move_get_dir(mv)};
    int found = 0;
    for (int s = 0; s < slot_count; s++) {
      if (slots_out[s].row == key.row &&
          slots_out[s].col_start == key.col_start &&
          slots_out[s].length == key.length && slots_out[s].dir == key.dir) {
        found = 1;
        break;
      }
    }
    if (!found && slot_count < max_slots) {
      slots_out[slot_count++] = key;
    }
  }
  *out_count = slot_count;
  return best;
}

// Build per-slot cache for a given board state.
static void build_slot_cache(const Board *board, const SlotMeta *slots,
                             int slot_count, SlotCache *cache,
                             const int32_t *letter_scores_eq, int ci,
                             int32_t bingo_bonus_eq) {
  for (int s = 0; s < slot_count; s++) {
    const SlotMeta k = slots[s];
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
        const Equity cs_eq = board_get_cross_score(board, r, c, (int)k.dir, ci);
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
}

// Build the leave table for `rack` (every multiset subrack, paired with KLV
// leave value). Returns n_leaves.
static uint32_t build_leave_table(const Rack *rack, const KLV *klv, int alpha,
                                  uint8_t *leave_used, int32_t *leave_values,
                                  uint32_t max_leaves) {
  int max_count[MAX_ALPHABET_SIZE] = {0};
  for (int ml = 0; ml < alpha; ml++) {
    max_count[ml] = rack_get_letter(rack, ml);
  }
  int stack[MAX_ALPHABET_SIZE] = {0};
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

// Build a 16-byte BitRack from a Rack object. Caps the loop at 32 nibbles
// since the bitrack format only has 32 letter slots.
static void rack_to_bitrack(const Rack *rack, int alpha, uint8_t *out) {
  uint64_t lo = 0;
  uint64_t hi = 0;
  const int ml_max = alpha < 32 ? alpha : 32;
  for (int ml = 0; ml < ml_max; ml++) {
    const int c = rack_get_letter(rack, (MachineLetter)ml);
    if (c == 0) {
      continue;
    }
    const int shift = ml * 4;
    if (shift < 64) {
      lo += (uint64_t)c << shift;
    } else {
      hi += (uint64_t)c << (shift - 64);
    }
  }
  memcpy(out, &lo, 8);
  memcpy(out + 8, &hi, 8);
}

// Run a full WMPG top-1 (canonical, two-pass) on the cached slots for
// `rack`. Returns the best equity across all slots; populates *out_loc with
// the packed locator from pass-2.
static int32_t run_wmpg_top1_canonical(GpuMatcher *m, const SlotCache *cache,
                                       int slot_count, const uint8_t *rack_br,
                                       const int32_t *letter_scores_eq,
                                       const uint8_t *leave_used,
                                       const int32_t *leave_values,
                                       uint32_t n_leaves, uint32_t *out_loc) {
  gpu_matcher_top1_reset(m, 1);
  for (int s = 0; s < slot_count; s++) {
    const SlotCache *sc = &cache[s];
    gpu_matcher_top1_pass1_wmpg(
        m, (uint32_t)sc->length, (uint32_t)sc->target_used_size, rack_br, 1,
        sc->cross_sets, sc->fixed_letters, sc->fb_lo, sc->fb_hi,
        letter_scores_eq, sc->position_multipliers, sc->base_score_eq,
        sc->bingo_eq, leave_used, leave_values, n_leaves, 0u);
  }
  for (int s = 0; s < slot_count; s++) {
    const SlotCache *sc = &cache[s];
    gpu_matcher_top1_pass2_wmpg(
        m, (uint32_t)sc->length, (uint32_t)sc->target_used_size, rack_br, 1,
        sc->cross_sets, sc->fixed_letters, sc->fb_lo, sc->fb_hi,
        letter_scores_eq, sc->position_multipliers, sc->base_score_eq,
        sc->bingo_eq, leave_used, leave_values, n_leaves, 0u, (uint32_t)sc->row,
        (uint32_t)sc->col, (uint32_t)sc->dir, GPU_TOP1_TIEBREAK_CANONICAL);
  }
  int32_t eq_buf = 0;
  uint32_t loc_buf = 0;
  gpu_matcher_top1_read(m, 1, &eq_buf, &loc_buf);
  if (out_loc != NULL) {
    *out_loc = loc_buf;
  }
  return eq_buf;
}

// Validate one rack: run MAGPIE generate_moves to find best, run WMPG top-1,
// assert equities match.
static void validate_rack(GpuMatcher *m, Game *game, const Rack *rack_input,
                          const Player *player, const KWG *kwg, const KLV *klv,
                          int alpha, const int32_t *letter_scores_eq,
                          int32_t bingo_bonus_eq, MoveList *move_list,
                          const char *label) {
  Rack *rack = player_get_rack(player);
  rack_set_dist_size_and_reset(rack, alpha);
  for (int ml = 0; ml < alpha; ml++) {
    rack_set_letter(rack, ml, rack_get_letter(rack_input, ml));
  }
  rack_set_total_letters(rack, rack_get_total_letters(rack_input));
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
  SortedMoveList *sml = NULL;
  const int max_slots = 4096;
  SlotMeta *slots = (SlotMeta *)calloc((size_t)max_slots, sizeof(SlotMeta));
  int slot_count = 0;
  const Move *magpie_best = find_magpie_best_and_slots(
      move_list, &args, &sml, slots, max_slots, &slot_count);
  assert(magpie_best != NULL);
  const int32_t magpie_eq = (int32_t)move_get_equity(magpie_best);

  Board *board = game_get_board(game);
  const KWG *p1_kwg = player_get_kwg(player);
  const int ci = board_get_cross_set_index(p1_kwg == kwg, 0);
  SlotCache *cache = (SlotCache *)calloc((size_t)slot_count, sizeof(SlotCache));
  build_slot_cache(board, slots, slot_count, cache, letter_scores_eq, ci,
                   bingo_bonus_eq);

  const uint32_t max_leaves = 1024;
  uint8_t *leave_used =
      (uint8_t *)calloc((size_t)max_leaves * BITRACK_BYTES, 1);
  int32_t *leave_values =
      (int32_t *)calloc((size_t)max_leaves, sizeof(int32_t));
  const uint32_t n_leaves =
      build_leave_table(rack, klv, alpha, leave_used, leave_values, max_leaves);

  uint8_t rack_br[BITRACK_BYTES];
  rack_to_bitrack(rack, alpha, rack_br);
  uint32_t loc = 0;
  const int32_t wmpg_eq =
      run_wmpg_top1_canonical(m, cache, slot_count, rack_br, letter_scores_eq,
                              leave_used, leave_values, n_leaves, &loc);

  printf("  %s rack=%-8s slots=%-4d MAGPIE eq=%d  WMPG eq=%d  %s\n", label,
         (label[0] != '\0') ? "" : "", slot_count, magpie_eq, wmpg_eq,
         wmpg_eq == magpie_eq ? "ok" : "MISMATCH");
  assert(wmpg_eq == magpie_eq);

  free(leave_used);
  free(leave_values);
  free(cache);
  free(slots);
  sorted_move_list_destroy(sml);
}
#endif // HAVE_GPU

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

  load_cgp_or_die(game, VS_MATT_NO_LEX_CSW);

  const int alpha = ld_get_size(ld);
  int32_t letter_scores_eq[32] = {0};
  for (int ml = 0; ml < 32; ml++) {
    if (ml < alpha) {
      letter_scores_eq[ml] = (int32_t)ld_get_score(ld, (MachineLetter)ml);
    }
  }
  const int32_t bingo_bonus_eq = (int32_t)game_get_bingo_bonus(game);

  printf("WMPG top-1 vs MAGPIE on VsMatt\n");

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

  MoveList *move_list = move_list_create(100000);

  // Validate three rack categories: blank-free (0 blanks), single-blank
  // (1), double-blank (2). Each exercises a different code path in the
  // WMPG top-1 kernels.
  Rack rack_input;
  rack_set_dist_size_and_reset(&rack_input, alpha);

  rack_set_to_string(ld, &rack_input, "AABDELT");
  validate_rack(m, game, &rack_input, player, kwg, klv, alpha, letter_scores_eq,
                bingo_bonus_eq, move_list, "blank-0");

  rack_set_dist_size_and_reset(&rack_input, alpha);
  rack_set_to_string(ld, &rack_input, "?ABDELT");
  validate_rack(m, game, &rack_input, player, kwg, klv, alpha, letter_scores_eq,
                bingo_bonus_eq, move_list, "blank-1");

  rack_set_dist_size_and_reset(&rack_input, alpha);
  rack_set_to_string(ld, &rack_input, "??BDELT");
  validate_rack(m, game, &rack_input, player, kwg, klv, alpha, letter_scores_eq,
                bingo_bonus_eq, move_list, "blank-2");

  move_list_destroy(move_list);
  free(wmpg_bytes);
  gpu_matcher_destroy(m);
#else
  (void)wmp;
  (void)kwg;
  (void)klv;
  (void)bingo_bonus_eq;
  (void)letter_scores_eq;
  printf("  GPU not built (non-Apple), skipping\n");
#endif

  game_destroy(game);
  config_destroy(config);
}
