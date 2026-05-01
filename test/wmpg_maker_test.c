// Validates the WMP-to-GPU flattener. Builds a WMPG buffer from CSW24's
// loaded WMP, parses the header and per-length meta back, asserts that the
// flattened layout's stats exactly equal MAGPIE's in-memory WFLs, and
// spot-checks a hash lookup for AABDELT (length 7).

#include "wmpg_maker_test.h"

#include "../src/def/board_defs.h"
#include "../src/ent/bit_rack.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/player.h"
#include "../src/ent/wmp.h"
#include "../src/impl/config.h"
#include "../src/impl/wmpg_maker.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WMPG_HEADER_BYTES 32
#define WMPG_META_BYTES_PER_LENGTH 56
#define WMPG_ENTRY_BYTES 32

static uint32_t read_u32_le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

void test_wmpg_maker(void) {
  Config *config = config_create_or_die(
      "set -lex CSW24 -wmp true -s1 equity -s2 equity -r1 best -r2 best "
      "-numplays 1");
  Game *game = config_game_create(config);
  const LetterDistribution *ld = game_get_ld(game);
  const Player *player = game_get_player(game, 0);
  const WMP *wmp = player_get_wmp(player);
  assert(wmp != NULL);

  uint8_t *buf = NULL;
  size_t size = 0;
  ErrorStack *es = error_stack_create();
  wmpg_build(wmp, &buf, &size, es);
  if (!error_stack_is_empty(es)) {
    error_stack_print_and_reset(es);
    log_fatal("wmpg_build failed\n");
  }
  error_stack_destroy(es);

  // Header
  assert(buf[0] == 'W' && buf[1] == 'M' && buf[2] == 'P' && buf[3] == 'G');
  assert(buf[4] == 2);
  const int max_len_plus_one = buf[6];
  const int br_bytes = buf[7];
  assert(max_len_plus_one == BOARD_DIM + 1);
  assert(br_bytes == 16);
  const uint32_t total_bucket_starts = read_u32_le(buf + 8);
  const uint32_t total_entries = read_u32_le(buf + 12);
  const uint32_t total_letter_bytes = read_u32_le(buf + 16);
  const uint32_t sections_offset = read_u32_le(buf + 20);
  assert(sections_offset == WMPG_HEADER_BYTES + (uint32_t)max_len_plus_one *
                                                    WMPG_META_BYTES_PER_LENGTH);

  printf("WMPG for CSW24: %zu bytes\n", size);
  printf("  total_bucket_starts=%u  total_entries=%u  total_letter_bytes=%u\n",
         total_bucket_starts, total_entries, total_letter_bytes);

  // Per-length meta walk + comparison to in-memory WFL (all 3 tables).
  uint32_t total_entries_check = 0;
  uint32_t kwg_total_uninlined_check = 0;
  printf("  per-length (word | b1 | b2):\n");
  for (int len = 0; len < max_len_plus_one; len++) {
    const WMPForLength *wfl = &wmp->wfls[len];
    const uint8_t *m =
        buf + WMPG_HEADER_BYTES + (size_t)len * WMPG_META_BYTES_PER_LENGTH;
    const uint32_t w_nb = read_u32_le(m + 0);
    const uint32_t w_ne = read_u32_le(m + 8);
    const uint32_t w_nu = read_u32_le(m + 16);
    const uint32_t b1_nb = read_u32_le(m + 24);
    const uint32_t b1_ne = read_u32_le(m + 32);
    const uint32_t b2_nb = read_u32_le(m + 40);
    const uint32_t b2_ne = read_u32_le(m + 48);
    if (w_ne == 0 && b1_ne == 0 && b2_ne == 0 && wfl->num_word_entries == 0 &&
        wfl->num_blank_entries == 0 && wfl->num_double_blank_entries == 0) {
      continue;
    }
    printf("    L=%2d  W:%u/%u  b1:%u/%u  b2:%u/%u\n", len, w_nb, w_ne, b1_nb,
           b1_ne, b2_nb, b2_ne);
    assert(w_nb == (w_ne == 0 ? 0 : wfl->num_word_buckets));
    assert(w_ne == wfl->num_word_entries);
    assert(w_nu == wfl->num_uninlined_words);
    assert(b1_nb == (b1_ne == 0 ? 0 : wfl->num_blank_buckets));
    assert(b1_ne == wfl->num_blank_entries);
    assert(b2_nb == (b2_ne == 0 ? 0 : wfl->num_double_blank_buckets));
    assert(b2_ne == wfl->num_double_blank_entries);
    total_entries_check += w_ne + b1_ne + b2_ne;
    kwg_total_uninlined_check += w_nu * (uint32_t)len;
  }
  assert(total_entries_check == total_entries);
  assert(kwg_total_uninlined_check == total_letter_bytes);

  // Spot-check: lookup AABDELT in length 7. Use the SAME hash and bucket-walk
  // logic the GPU kernel will use.
  {
    Rack rack;
    rack_set_dist_size_and_reset(&rack, ld_get_size(ld));
    rack_set_to_string(ld, &rack, "AABDELT");
    const BitRack target = bit_rack_create_from_rack(ld, &rack);

    const uint8_t *m = buf + WMPG_HEADER_BYTES + 7 * WMPG_META_BYTES_PER_LENGTH;
    const uint32_t num_buckets = read_u32_le(m + 0);
    const uint32_t buckets_offset = read_u32_le(m + 4);
    const uint32_t entries_offset = read_u32_le(m + 12);

    const uint8_t *L_buckets = buf + sections_offset + buckets_offset;
    const uint32_t total_buckets_bytes =
        total_bucket_starts * (uint32_t)sizeof(uint32_t);
    const uint8_t *L_entries =
        buf + sections_offset + total_buckets_bytes + entries_offset;

    const uint64_t hash = bit_rack_mix_to_64(&target);
    const uint32_t bucket_idx = (uint32_t)hash & (num_buckets - 1);
    const uint32_t start =
        read_u32_le(L_buckets + (size_t)bucket_idx * sizeof(uint32_t));
    const uint32_t end =
        read_u32_le(L_buckets + ((size_t)bucket_idx + 1) * sizeof(uint32_t));
    int found = 0;
    int probes = 0;
    for (uint32_t i = start; i < end; i++) {
      const uint8_t *entry = L_entries + (size_t)i * WMPG_ENTRY_BYTES;
      uint64_t lo;
      uint64_t hi;
      memcpy(&lo, entry + 16, 8);
      memcpy(&hi, entry + 24, 8);
      probes++;
      if (lo == bit_rack_get_low_64(&target) &&
          hi == bit_rack_get_high_64(&target)) {
        // Decode word count.
        const uint8_t b0 = entry[0];
        if (b0 != 0) {
          // inlined: count L-byte chunks until a zero or end.
          int wc = 0;
          for (int w = 0; w < (16 / 7); w++) {
            if (entry[w * 7] == 0) {
              break;
            }
            wc++;
          }
          printf("  AABDELT found inlined in bucket %u after %d probe(s); "
                 "anagram count = %d\n",
                 bucket_idx, probes, wc);
          assert(wc == 2);
        } else {
          const uint32_t num_words = read_u32_le(entry + 12);
          printf("  AABDELT found non-inlined in bucket %u after %d "
                 "probe(s); num_words = %u\n",
                 bucket_idx, probes, num_words);
          assert(num_words == 2);
        }
        found = 1;
        break;
      }
    }
    assert(found);
  }

  free(buf);
  game_destroy(game);
  config_destroy(config);
}
