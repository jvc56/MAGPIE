// Validates the KWG -> flat lex converter on CSW24.
// Writes to /tmp/csw24.flatlex, parses the header back, prints per-length
// word counts, asserts the total matches the count enumerated directly from
// the KWG (so a regression in the converter would show up immediately).

#include "flat_lex_maker_test.h"

#include "../src/def/kwg_defs.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/player.h"
#include "../src/impl/config.h"
#include "../src/impl/flat_lex_maker.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void walk_count(const KWG *kwg, uint32_t node_index, int depth,
                       uint32_t *counts) {
  if (node_index == 0) {
    return;
  }
  for (uint32_t i = node_index;; i++) {
    const uint32_t node = kwg_node(kwg, i);
    if (kwg_node_accepts(node)) {
      counts[depth + 1]++;
    }
    const uint32_t child = kwg_node_arc_index(node);
    if (child != 0 && depth + 1 < MAX_KWG_STRING_LENGTH - 1) {
      walk_count(kwg, child, depth + 1, counts);
    }
    if (kwg_node_is_end(node)) {
      break;
    }
  }
}

void test_flat_lex_maker(void) {
  Config *config = config_create_or_die(
      "set -lex CSW24 -wmp false -s1 equity -s2 equity -r1 best -r2 best "
      "-numplays 1");
  Game *game = config_game_create(config);
  const LetterDistribution *ld = game_get_ld(game);
  const KWG *kwg = player_get_kwg(game_get_player(game, 0));

  // Reference counts straight from the KWG, for comparison with the file.
  uint32_t ref_counts[MAX_KWG_STRING_LENGTH] = {0};
  walk_count(kwg, kwg_get_dawg_root_node_index(kwg), 0, ref_counts);
  uint32_t ref_total = 0;
  for (int len = 0; len < MAX_KWG_STRING_LENGTH; len++) {
    ref_total += ref_counts[len];
  }

  const char *out_path = "/tmp/csw24.flatlex";
  ErrorStack *es = error_stack_create();
  flat_lex_make(kwg, ld, out_path, es);
  if (!error_stack_is_empty(es)) {
    error_stack_print_and_reset(es);
    log_fatal("flat_lex_make failed\n");
  }
  error_stack_destroy(es);

  // Read header + count table back.
  FILE *fp = fopen(out_path, "rb");
  assert(fp != NULL);
  uint8_t header[16];
  size_t got = fread(header, 1, sizeof(header), fp);
  assert(got == sizeof(header));
  assert(header[0] == 'F');
  assert(header[1] == 'L');
  assert(header[2] == 'E');
  assert(header[3] == 'X');
  assert(header[4] == 1); // version
  const int alpha = header[5];
  const int max_len_plus_one = header[6];
  const int br_bytes = header[7];
  assert(alpha == ld_get_size(ld));
  assert(max_len_plus_one == MAX_KWG_STRING_LENGTH);
  assert(br_bytes == 16);
  const uint32_t total = (uint32_t)header[8] | ((uint32_t)header[9] << 8) |
                         ((uint32_t)header[10] << 16) |
                         ((uint32_t)header[11] << 24);

  uint32_t counts[MAX_KWG_STRING_LENGTH] = {0};
  for (int len = 0; len < max_len_plus_one; len++) {
    uint8_t buf[4];
    got = fread(buf, 1, 4, fp);
    assert(got == 4);
    counts[len] = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                  ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
  }
  fseek(fp, 0, SEEK_END);
  const long file_bytes = ftell(fp);
  fclose(fp);

  printf("flat lex for CSW24 -> %s\n", out_path);
  printf("  alphabet=%d  max_len_plus_one=%d  bitrack_bytes=%d\n", alpha,
         max_len_plus_one, br_bytes);
  printf("  per-length word counts:\n");
  for (int len = 0; len < max_len_plus_one; len++) {
    if (counts[len] > 0 || ref_counts[len] > 0) {
      printf("    L=%2d  file=%-7u  kwg=%-7u  %s\n", len, counts[len],
             ref_counts[len],
             counts[len] == ref_counts[len] ? "ok" : "MISMATCH");
      assert(counts[len] == ref_counts[len]);
    }
  }
  assert(total == ref_total);

  size_t expected_letters = 0;
  for (int len = 0; len < max_len_plus_one; len++) {
    expected_letters += (size_t)counts[len] * (size_t)len;
  }
  const size_t expected_bytes =
      16 + (size_t)max_len_plus_one * 4 + (size_t)total * 16 + expected_letters;
  printf("  total words=%u  file size=%ld  expected=%zu  %s\n", total,
         file_bytes, expected_bytes,
         (size_t)file_bytes == expected_bytes ? "ok" : "SIZE-MISMATCH");
  assert((size_t)file_bytes == expected_bytes);

  game_destroy(game);
  config_destroy(config);
}
