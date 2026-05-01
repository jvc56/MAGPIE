// Phase 4.1 stage A: unit-test the cross-check filtering kernel against a
// CPU baseline of the same algorithm, using synthetic cross-sets.
//
// For rack AABDELT and length 7 in CSW24 there are exactly two anagrams:
// ABLATED (A,B,L,A,T,E,D) and DATABLE (D,A,T,A,B,L,E). The tests below pick
// cross-sets that include or exclude these specifically.
//
// Skipped on non-Darwin builds.

#include "gpu_cross_test.h"

#include "../src/def/board_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/kwg_defs.h"
#include "../src/def/move_defs.h"
#include "../src/ent/board.h"
#include "../src/ent/bonus_square.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/players_data.h"
#include "../src/ent/rack.h"
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

// VsMatt board with no embedded lexicon override (so the active config's lex
// controls). Same content as VS_MATT in test_constants.h minus the trailing
// "-lex NWL20;".
#define VS_MATT_NO_LEX_CSW                                                     \
  "7ZEP1F3/1FLUKY3R1R3/5EX2A1U3/2SCARIEST1I3/9TOT3/6GO1LO4/6OR1ETA3/"          \
  "6JABS1b3/5QI4A3/5I1N3N3/3ReSPOND1D3/1HOE3V3O3/1ENCOMIA3N3/7T7/3VENGED6 "    \
  "/ 0/0 0"

#define BITRACK_BYTES 16

static double monotonic_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts); // NOLINT(misc-include-cleaner)
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int subset_test_byte_wise(const uint8_t *word, const uint8_t *rack) {
  for (int b = 0; b < BITRACK_BYTES; b++) {
    if ((word[b] & 0x0F) > (rack[b] & 0x0F)) {
      return 0;
    }
    if (((word[b] >> 4) & 0x0F) > ((rack[b] >> 4) & 0x0F)) {
      return 0;
    }
  }
  return 1;
}

// CPU baseline matching the count_with_cross_kernel.
static uint32_t cpu_count_with_cross(const uint8_t *bitracks,
                                     const uint8_t *letters, uint32_t n_words,
                                     uint32_t word_length, const uint8_t *rack,
                                     const uint64_t *cross_sets) {
  uint32_t count = 0;
  for (uint32_t i = 0; i < n_words; i++) {
    const uint8_t *w = bitracks + (size_t)i * BITRACK_BYTES;
    if (!subset_test_byte_wise(w, rack)) {
      continue;
    }
    const uint8_t *letter_seq = letters + (size_t)i * word_length;
    int ok = 1;
    for (uint32_t j = 0; j < word_length && ok; j++) {
      const uint64_t bit = ((uint64_t)1) << letter_seq[j];
      if ((cross_sets[j] & bit) == 0) {
        ok = 0;
      }
    }
    if (ok) {
      count++;
    }
  }
  return count;
}

#if HAVE_GPU
static void run_case(GpuMatcher *m, const char *label, uint32_t first_word_L7,
                     uint32_t n_words_L7, size_t letters_offset_L7,
                     const uint8_t *bitracks_L7, const uint8_t *letters_L7,
                     const uint8_t *rack_br, const uint64_t *cross_sets,
                     uint32_t expected) {
  const uint32_t cpu_c = cpu_count_with_cross(
      bitracks_L7, letters_L7, n_words_L7, 7, rack_br, cross_sets);
  uint32_t gpu_c = 0;
  gpu_matcher_count_with_cross(m, first_word_L7, n_words_L7, letters_offset_L7,
                               7, rack_br, 1, cross_sets, &gpu_c);
  printf("  %-40s expected=%u  cpu=%u  gpu=%u  %s\n", label, expected, cpu_c,
         gpu_c, (cpu_c == expected && gpu_c == expected) ? "ok" : "FAIL");
  assert(cpu_c == expected);
  assert(gpu_c == expected);
}
#endif

void test_gpu_cross(void) {
  Config *config = config_create_or_die(
      "set -lex CSW24 -wmp false -s1 equity -s2 equity -r1 best -r2 best "
      "-numplays 1");
  Game *game = config_game_create(config);
  const LetterDistribution *ld = game_get_ld(game);
  const Player *player = game_get_player(game, 0);
  const KWG *kwg = player_get_kwg(player);

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

  const uint32_t n_L7 = per_length_count[7];
  const uint8_t *bitracks_L7 =
      all_bitracks + (size_t)first_word_for_length[7] * BITRACK_BYTES;
  const uint8_t *letters_L7 = all_letters + letters_byte_offset_for[7];

  // Build rack BitRack for AABDELT.
  const uint64_t lo_a = 1, lo_b = 1, lo_d = 1, lo_e = 1, lo_l = 1, lo_t = 1;
  (void)lo_a;
  (void)lo_b;
  (void)lo_d;
  (void)lo_e;
  (void)lo_l;
  (void)lo_t;
  uint8_t rack_br[BITRACK_BYTES] = {0};
  // ml encoding: A=1, B=2, ..., Z=26. Each letter takes 4 bits at ml*4.
  // AABDELT = A:2, B:1, D:1, E:1, L:1, T:1
  uint64_t lo = 0;
  uint64_t hi = 0;
  const uint8_t mls[] = {1 /*A*/, 2 /*B*/,  4 /*D*/,
                         5 /*E*/, 12 /*L*/, 20 /*T*/};
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

  printf("phase 4.1 cross-check filter unit test (CSW24, rack=AABDELT, L=7)\n");

#if HAVE_GPU
  if (!gpu_matcher_is_available()) {
    printf("  GPU not available, skipping\n");
    free(flatlex_bytes);
    game_destroy(game);
    config_destroy(config);
    return;
  }
  GpuMatcher *m =
      gpu_matcher_create("bin/movegen.metallib", all_bitracks, total_words,
                         all_letters, total_letters_bytes);
  if (m == NULL) {
    free(flatlex_bytes);
    game_destroy(game);
    config_destroy(config);
    log_fatal("gpu_matcher_create failed\n");
  }

  // Cross-set bit positions: ml=1..26 corresponds to A..Z.
  // bit (1ULL << ml) set means letter ml is allowed at that position.
  const uint64_t any_letter = 0x07FFFFFEULL; // bits 1..26
  uint64_t cross_sets[7];

  // Case 1: all "any letter" → both ABLATED and DATABLE match.
  for (int i = 0; i < 7; i++) {
    cross_sets[i] = any_letter;
  }
  run_case(m, "no constraints (any letter at all pos)",
           first_word_for_length[7], n_L7, letters_byte_offset_for[7],
           bitracks_L7, letters_L7, rack_br, cross_sets, 2);

  // Case 2: pos 0 must be 'A' (ml=1) → only ABLATED.
  cross_sets[0] = ((uint64_t)1) << 1;
  for (int i = 1; i < 7; i++) {
    cross_sets[i] = any_letter;
  }
  run_case(m, "pos 0 = A → ABLATED only", first_word_for_length[7], n_L7,
           letters_byte_offset_for[7], bitracks_L7, letters_L7, rack_br,
           cross_sets, 1);

  // Case 3: pos 0 must be 'D' (ml=4) → only DATABLE.
  cross_sets[0] = ((uint64_t)1) << 4;
  for (int i = 1; i < 7; i++) {
    cross_sets[i] = any_letter;
  }
  run_case(m, "pos 0 = D → DATABLE only", first_word_for_length[7], n_L7,
           letters_byte_offset_for[7], bitracks_L7, letters_L7, rack_br,
           cross_sets, 1);

  // Case 4: pos 0 must be 'Q' → no matches.
  cross_sets[0] = ((uint64_t)1) << 17; // Q
  for (int i = 1; i < 7; i++) {
    cross_sets[i] = any_letter;
  }
  run_case(m, "pos 0 = Q → no match", first_word_for_length[7], n_L7,
           letters_byte_offset_for[7], bitracks_L7, letters_L7, rack_br,
           cross_sets, 0);

  // Case 5: pos 1 must be 'A' (ml=1) → DATABLE matches (D,A,...), ABLATED
  // does not (A,B,...).
  cross_sets[0] = any_letter;
  cross_sets[1] = ((uint64_t)1) << 1; // A
  for (int i = 2; i < 7; i++) {
    cross_sets[i] = any_letter;
  }
  run_case(m, "pos 1 = A → DATABLE only", first_word_for_length[7], n_L7,
           letters_byte_offset_for[7], bitracks_L7, letters_L7, rack_br,
           cross_sets, 1);

  // Case 6: ABLATED-only template: A,B,L,A,T,E,D positions.
  cross_sets[0] = ((uint64_t)1) << 1;  // A
  cross_sets[1] = ((uint64_t)1) << 2;  // B
  cross_sets[2] = ((uint64_t)1) << 12; // L
  cross_sets[3] = ((uint64_t)1) << 1;  // A
  cross_sets[4] = ((uint64_t)1) << 20; // T
  cross_sets[5] = ((uint64_t)1) << 5;  // E
  cross_sets[6] = ((uint64_t)1) << 4;  // D
  run_case(m, "exact ABLATED template", first_word_for_length[7], n_L7,
           letters_byte_offset_for[7], bitracks_L7, letters_L7, rack_br,
           cross_sets, 1);

  gpu_matcher_destroy(m);
#else
  printf("  GPU section compiled out; non-Darwin build\n");
#endif

  free(flatlex_bytes);
  game_destroy(game);
  config_destroy(config);
}

// Print a cross-set bitvec as a human-readable letter list (e.g., "AEIO").
static void print_cross_set(const char *label, uint64_t cs) {
  printf("    %s 0x%016llx  letters: ", label, (unsigned long long)cs);
  for (int ml = 1; ml <= 26; ml++) {
    if (cs & ((uint64_t)1 << ml)) {
      printf("%c", 'A' + ml - 1);
    }
  }
  printf("\n");
}

// Validate the cross-check kernel against MAGPIE's movegen on a real slot.
// Picks slot row=13 cols=8..14 horizontal length=7 on VsMatt (a fully-empty
// non-playthrough slot). Compares matched-word count between:
//   - CPU brute-force using MAGPIE-computed cross-sets
//   - GPU kernel using same cross-sets
//   - MAGPIE full movegen, filtered to plays at exactly this slot
void test_gpu_cross_real(void) {
  Config *config = config_create_or_die(
      "set -lex CSW24 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 10000");
  Game *game = config_game_create(config);
  const LetterDistribution *ld = game_get_ld(game);
  Player *player = game_get_player(game, 0);
  const KWG *kwg = player_get_kwg(player);
  Board *board = game_get_board(game);

  load_cgp_or_die(game, VS_MATT_NO_LEX_CSW);
  // Re-set rack since CGP has empty racks. DEIOSTU spells OUTSIDE which fits
  // this slot (col 8 needs A/E/I/O, col 11 needs S).
  Rack *rack = player_get_rack(player);
  rack_set_to_string(ld, rack, "DEIOSTU");

  // Slot: row 13 (0-indexed), cols 8..14, horizontal, length 7.
  const int slot_row = 13;
  const int slot_col_start = 8;
  const int slot_length = 7;

  // Verify the slot is fully empty (no playthrough).
  for (int c = slot_col_start; c < slot_col_start + slot_length; c++) {
    const MachineLetter ml = board_get_letter(board, slot_row, c);
    if (ml != ALPHABET_EMPTY_SQUARE_MARKER) {
      log_fatal("slot (%d,%d..%d) has tile at col %d ml=%d\n", slot_row,
                slot_col_start, slot_col_start + slot_length - 1, c, ml);
    }
  }

  // Cross-set index: 0 if both players share a KWG.
  PlayersData *players_data = config_get_players_data(config);
  const KWG *p1_kwg = players_data_get_kwg(players_data, 1);
  const int ci = board_get_cross_set_index(p1_kwg == kwg, 0);

  // Pull MAGPIE-computed cross-sets for vertical direction at each slot square.
  uint64_t cross_sets[7];
  printf("phase 4.1B real-position test: VsMatt slot row=%d cols=%d..%d L=%d "
         "(horizontal)\n",
         slot_row, slot_col_start, slot_col_start + slot_length - 1,
         slot_length);
  for (int i = 0; i < slot_length; i++) {
    // For a horizontal play, cross_set(HORIZONTAL) is the bitvec of letters
    // that, placed at this square, form a valid vertical (perpendicular)
    // word. Per endgame.c:848: "cross_set(H) validates the vertical word".
    cross_sets[i] = board_get_cross_set(board, slot_row, slot_col_start + i,
                                        BOARD_HORIZONTAL_DIRECTION, ci);
    char label[8];
    snprintf(label, sizeof(label), "col %2d", slot_col_start + i);
    print_cross_set(label, cross_sets[i]);
  }

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
  const uint32_t n_L7 = per_length_count[slot_length];
  const uint8_t *bitracks_L7 =
      all_bitracks + (size_t)first_word_for_length[slot_length] * BITRACK_BYTES;
  const uint8_t *letters_L7 =
      all_letters + letters_byte_offset_for[slot_length];

  // Build rack BitRack for DEIOSTU.
  uint8_t rack_br[BITRACK_BYTES] = {0};
  {
    uint64_t lo = 0;
    uint64_t hi = 0;
    // D=4, E=5, I=9, O=15, S=19, T=20, U=21
    const uint8_t mls[] = {4, 5, 9, 15, 19, 20, 21};
    for (int i = 0; i < 7; i++) {
      const int shift = mls[i] * 4;
      if (shift < 64) {
        lo += (uint64_t)1 << shift;
      } else {
        hi += (uint64_t)1 << (shift - 64);
      }
    }
    memcpy(rack_br, &lo, 8);
    memcpy(rack_br + 8, &hi, 8);
  }

  const uint32_t cpu_c = cpu_count_with_cross(bitracks_L7, letters_L7, n_L7,
                                              slot_length, rack_br, cross_sets);
  printf("  CPU brute-force (cross+subset): %u matches\n", cpu_c);

  // Print the matching words from CPU pass.
  printf("  CPU matches: ");
  for (uint32_t i = 0; i < n_L7; i++) {
    const uint8_t *w = bitracks_L7 + (size_t)i * BITRACK_BYTES;
    if (!subset_test_byte_wise(w, rack_br)) {
      continue;
    }
    const uint8_t *letter_seq = letters_L7 + (size_t)i * slot_length;
    int ok = 1;
    for (int j = 0; j < slot_length && ok; j++) {
      if ((cross_sets[j] & ((uint64_t)1 << letter_seq[j])) == 0) {
        ok = 0;
      }
    }
    if (ok) {
      char buf[16] = {0};
      for (int j = 0; j < slot_length; j++) {
        buf[j] = 'A' + letter_seq[j] - 1;
      }
      printf("%s ", buf);
    }
  }
  printf("\n");

#if HAVE_GPU
  if (gpu_matcher_is_available()) {
    GpuMatcher *m =
        gpu_matcher_create("bin/movegen.metallib", all_bitracks, total_words,
                           all_letters, total_letters_bytes);
    if (m == NULL) {
      log_fatal("gpu_matcher_create failed\n");
    }
    uint32_t gpu_c = 0;
    gpu_matcher_count_with_cross(m, first_word_for_length[slot_length], n_L7,
                                 letters_byte_offset_for[slot_length],
                                 (uint32_t)slot_length, rack_br, 1, cross_sets,
                                 &gpu_c);
    printf("  GPU kernel (cross+subset):       %u matches  %s\n", gpu_c,
           gpu_c == cpu_c ? "ok" : "MISMATCH-vs-CPU");
    assert(gpu_c == cpu_c);
    gpu_matcher_destroy(m);
  }
#endif

  // Validate against MAGPIE's full movegen, filtered to this slot.
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
  uint32_t magpie_count = 0;
  printf("  MAGPIE total moves generated: %d\n", sml->count);
  printf("  MAGPIE plays:\n");
  for (int i = 0; i < sml->count; i++) {
    const Move *mv = sml->moves[i];
    if (move_get_type(mv) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      continue;
    }
    const int row = move_get_row_start(mv);
    const int dir = move_get_dir(mv);
    const int len = move_get_tiles_length(mv);
    char buf[16] = {0};
    for (int j = 0; j < len && j < 15; j++) {
      const MachineLetter t = move_get_tile(mv, j);
      const MachineLetter unblanked = t & 0x7F;
      if (t == 0) {
        buf[j] = '.';
      } else {
        buf[j] =
            (unblanked >= 1 && unblanked <= 26) ? ('A' + unblanked - 1) : '?';
      }
    }
    printf("    %s row=%d col=%d L=%d played=%d  %s\n",
           dir == BOARD_HORIZONTAL_DIRECTION ? "H" : "V", row,
           move_get_col_start(mv), len, move_get_tiles_played(mv), buf);
    if (dir == BOARD_HORIZONTAL_DIRECTION && row == slot_row &&
        move_get_col_start(mv) == slot_col_start &&
        move_get_tiles_length(mv) == slot_length &&
        move_get_tiles_played(mv) == slot_length) {
      magpie_count++;
    }
  }
  printf("  MAGPIE count at exact slot:      %u  %s\n", magpie_count,
         magpie_count == cpu_c ? "ok" : "MISMATCH-vs-kernel");

  sorted_move_list_destroy(sml);
  move_list_destroy(move_list);
  free(flatlex_bytes);
  game_destroy(game);
  config_destroy(config);
}

// Phase 4.2: GPU vs MAGPIE on every non-playthrough slot MAGPIE enumerates.
// For each unique slot (row, col_start, length, dir) where MAGPIE generated
// at least one tile-only-no-playthrough placement, dispatch the GPU kernel
// with that slot's cross-sets and compare match count to MAGPIE's count.
//
// Cross-sets are computed CPU-side from MAGPIE's already-populated Board,
// passed to GPU per-dispatch. One dispatch per slot. (This is the
// "lots of back and forth" path Cesar warned about — ~150 µs per dispatch
// times slot count = the next perf wall after correctness.)

typedef struct SlotKey {
  uint8_t row;
  uint8_t col_start;
  uint8_t length;
  uint8_t dir;
} SlotKey;

typedef struct SlotEntry {
  SlotKey key;
  uint32_t magpie_count;
  uint32_t gpu_count;
  int64_t magpie_score_sum;
  int64_t gpu_score_sum;
  int64_t magpie_equity_sum;
  int64_t gpu_equity_sum;
} SlotEntry;

static int slot_key_equal(SlotKey a, SlotKey b) {
  return a.row == b.row && a.col_start == b.col_start && a.length == b.length &&
         a.dir == b.dir;
}

void test_gpu_cross_validate(void) {
  Config *config = config_create_or_die(
      "set -lex CSW24 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 100000");
  Game *game = config_game_create(config);
  const LetterDistribution *ld = game_get_ld(game);
  Player *player = game_get_player(game, 0);
  const KWG *kwg = player_get_kwg(player);
  const WMP *wmp = player_get_wmp(player);
  Board *board = game_get_board(game);

  load_cgp_or_die(game, VS_MATT_NO_LEX_CSW);
  Rack *rack = player_get_rack(player);
  rack_set_to_string(ld, rack, "AABDELT");

  // Build flat lex once.
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

  // Run MAGPIE movegen.
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

  // Bucket MAGPIE plays by slot. Allocate generously.
  const int max_slots = 4096;
  SlotEntry *slots = (SlotEntry *)calloc(max_slots, sizeof(SlotEntry));
  int slot_count = 0;
  uint32_t magpie_total_nonplaythrough = 0;
  uint32_t magpie_total_playthrough = 0;
  uint32_t magpie_non_tile = 0;

  PlayersData *pd = config_get_players_data(config);
  const KWG *p1_kwg = players_data_get_kwg(pd, 1);
  const int ci = board_get_cross_set_index(p1_kwg == kwg, 0);

  for (int i = 0; i < sml->count; i++) {
    const Move *mv = sml->moves[i];
    if (move_get_type(mv) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      magpie_non_tile++;
      continue;
    }
    if (move_get_tiles_played(mv) != move_get_tiles_length(mv)) {
      magpie_total_playthrough++;
    } else {
      magpie_total_nonplaythrough++;
    }
    SlotKey key = {.row = (uint8_t)move_get_row_start(mv),
                   .col_start = (uint8_t)move_get_col_start(mv),
                   .length = (uint8_t)move_get_tiles_length(mv),
                   .dir = (uint8_t)move_get_dir(mv)};
    int found = -1;
    for (int s = 0; s < slot_count; s++) {
      if (slot_key_equal(slots[s].key, key)) {
        found = s;
        break;
      }
    }
    if (found < 0) {
      assert(slot_count < max_slots);
      slots[slot_count].key = key;
      slots[slot_count].magpie_count = 0;
      slots[slot_count].gpu_count = 0;
      slots[slot_count].magpie_score_sum = 0;
      slots[slot_count].gpu_score_sum = 0;
      slots[slot_count].magpie_equity_sum = 0;
      slots[slot_count].gpu_equity_sum = 0;
      found = slot_count;
      slot_count++;
    }
    slots[found].magpie_count++;
    slots[found].magpie_score_sum += (int64_t)equity_to_int(move_get_score(mv));
    slots[found].magpie_equity_sum += (int64_t)move_get_equity(mv);
  }

  printf("phase 4.2 slot-by-slot validation (CSW24, VsMatt + AABDELT)\n");
  printf("  MAGPIE: total moves=%d, non-tile=%u, playthrough=%u, "
         "non-playthrough tile placements=%u in %d unique slots\n",
         sml->count, magpie_non_tile, magpie_total_playthrough,
         magpie_total_nonplaythrough, slot_count);

#if HAVE_GPU
  if (!gpu_matcher_is_available()) {
    printf("  GPU not available, skipping\n");
    free(slots);
    sorted_move_list_destroy(sml);
    move_list_destroy(move_list);
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

  // Build WMPG and upload to matcher (for the WMPG count kernel comparison).
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

  // For each unique slot, dispatch GPU score kernel and compare both count
  // and score_sum to MAGPIE.
  uint32_t slots_count_match = 0;
  uint32_t slots_score_match = 0;
  uint32_t slots_equity_match = 0;
  uint32_t slots_full_match = 0;
  uint32_t slots_wmpg_match = 0;
  uint32_t slots_wmpg_equity_match = 0;
  uint32_t slots_skipped = 0;
  uint32_t gpu_count_total = 0;
  uint32_t gpu_wmpg_count_total = 0;
  int64_t gpu_score_total = 0;
  int64_t gpu_equity_total = 0;
  int64_t gpu_wmpg_equity_total = 0;
  int64_t magpie_score_total = 0;
  int64_t magpie_equity_total = 0;
  for (int s = 0; s < slot_count; s++) {
    magpie_score_total += slots[s].magpie_score_sum;
    magpie_equity_total += slots[s].magpie_equity_sum;
  }

  // Time MAGPIE generate_moves for comparison.
  const int magpie_iters = 1000;
  const double mt0 = monotonic_seconds();
  for (int i = 0; i < magpie_iters; i++) {
    move_list_set_rack(move_list, rack);
    generate_moves(&args);
  }
  const double magpie_per_iter_us =
      1e6 * (monotonic_seconds() - mt0) / magpie_iters;
  printf("  MAGPIE generate_moves: %.1f us per (position, rack)\n",
         magpie_per_iter_us);

  double total_gpu_score_dispatch_s = 0;
  double total_gpu_equity_dispatch_s = 0;
  double total_gpu_wmpg_equity_dispatch_s = 0;

  // Score test units: plain int points (rounded). Equity test uses raw Equity
  // millipoints (because leave values aren't integer-points).
  int32_t letter_scores_plain[32] = {0};
  int32_t letter_scores_eq[32] = {0};
  for (int ml = 0; ml < 32; ml++) {
    if (ml < ld_get_size(ld)) {
      const Equity raw = ld_get_score(ld, ml);
      letter_scores_eq[ml] = (int32_t)raw;
      letter_scores_plain[ml] = (int32_t)equity_to_int(raw);
    }
  }
  const int32_t bingo_bonus_eq =
      (int32_t)game_get_bingo_bonus(game); // millipoints
  const int32_t bingo_bonus_plain =
      (int32_t)equity_to_int(game_get_bingo_bonus(game));

  // Build per-rack leave table: for each subrack ⊆ rack, used_bitrack +
  // leave_value (= klv_get_leave_value of rack-subrack). Used in equity kernel.
  const KLV *klv = player_get_klv(player);
  const int alpha = ld_get_size(ld);
  const int max_leaves = 512;
  uint8_t *leave_used =
      (uint8_t *)calloc((size_t)max_leaves * BITRACK_BYTES, 1);
  int32_t *leave_values =
      (int32_t *)calloc((size_t)max_leaves, sizeof(int32_t));
  uint32_t n_leaves = 0;
  {
    Rack current;
    rack_set_dist_size_and_reset(&current, alpha);
    // Non-recursive enumeration via stack of counters per letter.
    int stack[MAX_ALPHABET_SIZE] = {0};
    int max_count[MAX_ALPHABET_SIZE] = {0};
    for (int ml = 0; ml < alpha; ml++) {
      max_count[ml] = rack_get_letter(rack, ml);
    }
    int level = 0;
    while (1) {
      if (level == alpha) {
        // Visit: build used bitrack + leave rack
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
        if (n_leaves >= (uint32_t)max_leaves) {
          log_fatal("leave table overflow\n");
        }
        memcpy(leave_used + (size_t)n_leaves * BITRACK_BYTES, &lo, 8);
        memcpy(leave_used + (size_t)n_leaves * BITRACK_BYTES + 8, &hi, 8);
        leave_values[n_leaves] = (int32_t)lv;
        n_leaves++;
        // Backtrack
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
  printf("  leave table: %u entries (subracks of rack)\n", n_leaves);

  for (int s = 0; s < slot_count; s++) {
    const SlotKey k = slots[s].key;
    if (k.length < 2 || k.length > 15) {
      slots[s].gpu_count = slots[s].magpie_count;
      slots[s].gpu_score_sum = slots[s].magpie_score_sum;
      slots_skipped++;
      continue;
    }
    uint64_t cross_sets[BOARD_DIM] = {0};
    uint8_t fixed_letters[BOARD_DIM] = {0};
    uint8_t fixed_bitrack[BITRACK_BYTES] = {0};
    int32_t position_multipliers[BOARD_DIM] = {0};
    int placed_count = 0;
    int prod_word_mult = 1;
    int hooked_cross_total_plain = 0;
    int hooked_cross_total_eq = 0;
    int playthrough_score_plain = 0;
    int playthrough_score_eq = 0;

    // First pass: collect per-position metadata + compute prod_word_mult,
    // hooked_cross_total, playthrough_score, fixed_bitrack.
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
          fixed_letters[i] = 0;
          cross_sets[i] = board_get_cross_set(board, r, c, (int)k.dir, ci);
          is_cross_word_arr[i] =
              board_get_is_cross_word(board, r, c, (int)k.dir) ? 1 : 0;
          const Equity cs_eq =
              board_get_cross_score(board, r, c, (int)k.dir, ci);
          const int cs_plain = (int)equity_to_int(cs_eq);
          hooked_cross_total_plain += cs_plain * wm;
          hooked_cross_total_eq += (int)cs_eq * wm;
          prod_word_mult *= wm;
          placed_count++;
        } else {
          const MachineLetter unblanked = (MachineLetter)(raw & UNBLANK_MASK);
          fixed_letters[i] = unblanked;
          cross_sets[i] = 0;
          // Playthrough tile: blanked → 0 score, else its letter score.
          if ((raw & BLANK_MASK) == 0) {
            playthrough_score_plain += letter_scores_plain[unblanked];
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
      memcpy(fixed_bitrack, &flo, 8);
      memcpy(fixed_bitrack + 8, &fhi, 8);
    }
    // Second pass: per-position multipliers.
    for (int i = 0; i < k.length; i++) {
      if (fixed_letters[i] != 0) {
        position_multipliers[i] = 0;
      } else {
        position_multipliers[i] =
            letter_mult_arr[i] *
            (prod_word_mult + is_cross_word_arr[i] * word_mult_arr[i]);
      }
    }
    const int32_t base_score_plain =
        playthrough_score_plain * prod_word_mult + hooked_cross_total_plain;
    const int32_t base_score_eq =
        playthrough_score_eq * prod_word_mult + hooked_cross_total_eq;
    const int32_t bingo_plain = (placed_count == 7) ? bingo_bonus_plain : 0;
    const int32_t bingo_eq = (placed_count == 7) ? bingo_bonus_eq : 0;

    uint32_t pair[2] = {0, 0};
    total_gpu_score_dispatch_s += gpu_matcher_score(
        m, first_word_for_length[k.length], per_length_count[k.length],
        letters_byte_offset_for[k.length], (uint32_t)k.length, rack_br, 1,
        cross_sets, fixed_letters, fixed_bitrack, letter_scores_plain,
        position_multipliers, base_score_plain, bingo_plain, pair);
    slots[s].gpu_count = pair[0];
    slots[s].gpu_score_sum = (int64_t)pair[1];
    gpu_count_total += pair[0];
    gpu_score_total += (int64_t)pair[1];

    uint32_t epair[2] = {0, 0};
    total_gpu_equity_dispatch_s += gpu_matcher_equity(
        m, first_word_for_length[k.length], per_length_count[k.length],
        letters_byte_offset_for[k.length], (uint32_t)k.length, rack_br, 1,
        cross_sets, fixed_letters, fixed_bitrack, letter_scores_eq,
        position_multipliers, base_score_eq, bingo_eq, leave_used, leave_values,
        n_leaves, epair);
    slots[s].gpu_equity_sum = (int64_t)(int32_t)epair[1]; // signed
    gpu_equity_total += (int64_t)(int32_t)epair[1];

    // WMPG count: per-rack hash-table lookup (not brute-force).
    int fixed_count = 0;
    for (int i = 0; i < k.length; i++) {
      if (fixed_letters[i] != 0) {
        fixed_count++;
      }
    }
    const uint32_t target_used_size = (uint32_t)(k.length - fixed_count);
    uint64_t fb_lo;
    uint64_t fb_hi;
    memcpy(&fb_lo, fixed_bitrack, 8);
    memcpy(&fb_hi, fixed_bitrack + 8, 8);
    uint32_t wmpg_c = 0;
    gpu_matcher_count_wmpg(m, (uint32_t)k.length, target_used_size, rack_br, 1,
                           cross_sets, fixed_letters, fb_lo, fb_hi, &wmpg_c);
    gpu_wmpg_count_total += wmpg_c;
    if (wmpg_c == slots[s].magpie_count) {
      slots_wmpg_match++;
    }

    // WMPG equity: same matching as count_wmpg + score+leave per match.
    uint32_t wmpg_epair[2] = {0, 0};
    total_gpu_wmpg_equity_dispatch_s += gpu_matcher_equity_wmpg(
        m, (uint32_t)k.length, target_used_size, rack_br, 1, cross_sets,
        fixed_letters, fb_lo, fb_hi, letter_scores_eq, position_multipliers,
        base_score_eq, bingo_eq, leave_used, leave_values, n_leaves,
        wmpg_epair);
    const int64_t wmpg_eq = (int64_t)(int32_t)wmpg_epair[1]; // signed
    gpu_wmpg_equity_total += wmpg_eq;
    if (wmpg_eq == slots[s].magpie_equity_sum) {
      slots_wmpg_equity_match++;
    }

    const int count_ok = (pair[0] == slots[s].magpie_count);
    const int score_ok = ((int64_t)pair[1] == slots[s].magpie_score_sum);
    const int equity_ok =
        (slots[s].gpu_equity_sum == slots[s].magpie_equity_sum);
    if (count_ok) {
      slots_count_match++;
    }
    if (score_ok) {
      slots_score_match++;
    }
    if (equity_ok) {
      slots_equity_match++;
    }
    if (count_ok && score_ok && equity_ok) {
      slots_full_match++;
    }
    if (!count_ok || !score_ok || !equity_ok) {
      static int reported = 0;
      if (reported < 10) {
        printf("    MISMATCH slot %s row=%u col=%u L=%u: MAGPIE "
               "count=%u score=%lld eq=%lld | GPU count=%u score=%u "
               "eq=%lld\n",
               k.dir == BOARD_HORIZONTAL_DIRECTION ? "H" : "V", k.row,
               k.col_start, k.length, slots[s].magpie_count,
               (long long)slots[s].magpie_score_sum,
               (long long)slots[s].magpie_equity_sum, pair[0], pair[1],
               (long long)slots[s].gpu_equity_sum);
        reported++;
      }
    }
  }
  const uint32_t magpie_total_tile =
      magpie_total_nonplaythrough + magpie_total_playthrough;
  printf("  Slots full-match: %u (count=%u, score=%u, equity=%u, "
         "skipped=%u, of %d)\n",
         slots_full_match, slots_count_match, slots_score_match,
         slots_equity_match, slots_skipped, slot_count);
  printf("  Counts:        MAGPIE=%u    GPU=%u    %s\n", magpie_total_tile,
         gpu_count_total,
         gpu_count_total == magpie_total_tile ? "ok" : "MISMATCH");
  printf("  Scores (pts):  MAGPIE=%lld   GPU=%lld   %s\n",
         (long long)magpie_score_total, (long long)gpu_score_total,
         gpu_score_total == magpie_score_total ? "ok" : "MISMATCH");
  printf("  Equity (mp):   MAGPIE=%lld   GPU=%lld   %s\n",
         (long long)magpie_equity_total, (long long)gpu_equity_total,
         gpu_equity_total == magpie_equity_total ? "ok" : "MISMATCH");
  printf("  WMPG counts:   MAGPIE=%u   GPU-WMPG=%u   slots-match=%u/%d   %s\n",
         magpie_total_tile, gpu_wmpg_count_total, slots_wmpg_match, slot_count,
         gpu_wmpg_count_total == magpie_total_tile ? "ok" : "MISMATCH");
  printf(
      "  WMPG equity:   MAGPIE=%lld   GPU-WMPG=%lld   slots-match=%u/%d   %s\n",
      (long long)magpie_equity_total, (long long)gpu_wmpg_equity_total,
      slots_wmpg_equity_match, slot_count,
      gpu_wmpg_equity_total == magpie_equity_total ? "ok" : "MISMATCH");
  printf("\n");
  printf("  Per-(position,rack) wall time:\n");
  printf("    MAGPIE generate_moves:        %8.1f us\n", magpie_per_iter_us);
  printf(
      "    GPU score-only sum (B=1):     %8.1f us  (%.1f us/slot, %d slots)\n",
      1e6 * total_gpu_score_dispatch_s,
      1e6 * total_gpu_score_dispatch_s / slot_count, slot_count);
  printf(
      "    GPU equity sum (B=1):         %8.1f us  (%.1f us/slot, %d slots)\n",
      1e6 * total_gpu_equity_dispatch_s,
      1e6 * total_gpu_equity_dispatch_s / slot_count, slot_count);
  printf(
      "    GPU WMPG equity sum (B=1):    %8.1f us  (%.1f us/slot, %d slots)\n",
      1e6 * total_gpu_wmpg_equity_dispatch_s,
      1e6 * total_gpu_wmpg_equity_dispatch_s / slot_count, slot_count);
  printf("    GPU score+equity slowdown:    %8.1fx vs MAGPIE\n",
         (1e6 * (total_gpu_score_dispatch_s + total_gpu_equity_dispatch_s)) /
             magpie_per_iter_us);

  // Batched throughput: equity kernel only, B racks at a time per slot.
  // Same rack repeated for simplicity; measures dispatch+kernel scaling.
  printf("\n  Equity-kernel batched throughput (same rack B× per slot):\n");
  const int Bs[] = {1, 16, 256, 1024, 4096, 18571};
  const int n_Bs = sizeof(Bs) / sizeof(Bs[0]);
  const int B_MAX = Bs[n_Bs - 1];
  uint8_t *batch_racks = (uint8_t *)malloc((size_t)B_MAX * BITRACK_BYTES);
  for (int i = 0; i < B_MAX; i++) {
    memcpy(batch_racks + (size_t)i * BITRACK_BYTES, rack_br, BITRACK_BYTES);
  }
  uint32_t *batch_pairs =
      (uint32_t *)malloc((size_t)B_MAX * 2 * sizeof(uint32_t));

  // Pre-cache per-slot constants (cross_sets, fixed, multipliers, base, bingo)
  // to keep host work out of the timed loop. Slot 0 only — bench treats one
  // position as N parallel racks at the same slots.
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
    const SlotKey k = slots[s].key;
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
      }
    }
    memcpy(cache[s].fixed_bitrack, &flo, 8);
    memcpy(cache[s].fixed_bitrack + 8, &fhi, 8);
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

  printf("    %-7s %-12s %-14s %-14s %-12s\n", "B", "wall ms", "us/slot",
         "us/rack", "vs MAGPIE");
  for (int bi = 0; bi < n_Bs; bi++) {
    const int B = Bs[bi];
    // Warmup
    for (int s = 0; s < slot_count; s++) {
      if (cache[s].length == 0) {
        continue;
      }
      const SlotCache *sc = &cache[s];
      gpu_matcher_equity(
          m, first_word_for_length[sc->length], per_length_count[sc->length],
          letters_byte_offset_for[sc->length], (uint32_t)sc->length,
          batch_racks, (uint32_t)B, sc->cross_sets, sc->fixed_letters,
          sc->fixed_bitrack, letter_scores_eq, sc->position_multipliers,
          sc->base_score_eq, sc->bingo_eq, leave_used, leave_values, n_leaves,
          batch_pairs);
    }
    const double bt0 = monotonic_seconds();
    for (int s = 0; s < slot_count; s++) {
      if (cache[s].length == 0) {
        continue;
      }
      const SlotCache *sc = &cache[s];
      gpu_matcher_equity(
          m, first_word_for_length[sc->length], per_length_count[sc->length],
          letters_byte_offset_for[sc->length], (uint32_t)sc->length,
          batch_racks, (uint32_t)B, sc->cross_sets, sc->fixed_letters,
          sc->fixed_bitrack, letter_scores_eq, sc->position_multipliers,
          sc->base_score_eq, sc->bingo_eq, leave_used, leave_values, n_leaves,
          batch_pairs);
    }
    const double dt = monotonic_seconds() - bt0;
    const double per_slot_us = 1e6 * dt / slot_count;
    const double per_rack_us = 1e6 * dt / B;
    const double slowdown = per_rack_us / magpie_per_iter_us;
    printf("    %-7d %-12.3f %-14.1f %-14.3f %-12.2fx\n", B, 1e3 * dt,
           per_slot_us, per_rack_us, slowdown);
  }

  free(cache);
  free(batch_pairs);
  free(batch_racks);
  free(leave_used);
  free(leave_values);

  gpu_matcher_destroy(m);
#endif

  free(slots);
  sorted_move_list_destroy(sml);
  move_list_destroy(move_list);
  free(flatlex_bytes);
  free(wmpg_bytes);
  game_destroy(game);
  config_destroy(config);
}
