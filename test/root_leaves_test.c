#include "root_leaves_test.h"

#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/root_leaves.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/root_candidates.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  TEST_ALPHABET_SIZE = 27,
  TEST_HEADS = 8,
  TEST_CANDIDATES = 15,
  TEST_ALL_MOVES_CAPACITY = 1000,
  // Machine letter S in the English distribution.
  TEST_S_MACHINE_LETTER = 19,
};

// Writes testdata/lexica/<name>.klv3: CSW21.klv2's body plus a one-bin KLV3
// trailer whose weights are zero and whose only bias is s_bias on every S
// held after a move that draws.
static void write_test_klv3(const char *name, float s_bias) {
  FILE *in = fopen("./testdata/lexica/CSW21.klv2", "rbe");
  assert(in);
  char *path = get_formatted_string("./testdata/lexica/%s.klv3", name);
  FILE *out = fopen(path, "wbe");
  assert(out);
  free(path);
  char buffer[1 << 16];
  size_t bytes;
  while ((bytes = fread(buffer, 1, sizeof(buffer), in)) > 0) {
    assert(fwrite(buffer, 1, bytes, out) == bytes);
  }
  (void)fclose(in);
  const uint8_t magic[8] = {'M', 'A', 'G', 'K', 'L', 'V', '3', '\0'};
  assert(fwrite(magic, sizeof(magic), 1, out) == 1);
  const uint32_t header[4] = {1, TEST_ALPHABET_SIZE, TEST_HEADS, 1};
  assert(fwrite(header, sizeof(uint32_t), 4, out) == 4);
  const uint16_t bin_upper_bound = 200;
  assert(fwrite(&bin_upper_bound, sizeof(uint16_t), 1, out) == 1);
  for (int draw_count = 0; draw_count < TEST_HEADS; draw_count++) {
    for (int held = 0; held < TEST_ALPHABET_SIZE; held++) {
      const float bias = held == TEST_S_MACHINE_LETTER ? s_bias : 0.0F;
      assert(fwrite(&bias, sizeof(float), 1, out) == 1);
    }
  }
  const float zero = 0.0F;
  for (int weight_idx = 0;
       weight_idx < TEST_HEADS * TEST_ALPHABET_SIZE * TEST_ALPHABET_SIZE;
       weight_idx++) {
    assert(fwrite(&zero, sizeof(float), 1, out) == 1);
  }
  (void)fclose(out);
}

static void remove_test_klv3(const char *name) {
  char *path = get_formatted_string("./testdata/lexica/%s.klv3", name);
  (void)remove(path);
  free(path);
}

static void generate_plain(Game *game, MoveList *move_list) {
  const MoveGenArgs args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&args);
}

static const Move *find_move(const MoveList *move_list, const Move *move) {
  for (int move_idx = 0; move_idx < move_list_get_count(move_list);
       move_idx++) {
    const Move *other = move_list_get_move(move_list, move_idx);
    if (compare_moves_without_equity(move, other, true) == -1) {
      return other;
    }
  }
  return NULL;
}

void test_root_leaves(void) {
  write_test_klv3("root_leaves_zero", 0.0F);
  write_test_klv3("root_leaves_s_bias", -25.0F);
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1 "
      "-wmp true");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 SSDEIRT/ 0/0 "
              "0");
  Game *game = config_get_game(config);
  MoveList *plain = move_list_create(TEST_CANDIDATES);
  MoveList *root = move_list_create(TEST_CANDIDATES);
  MoveList *all_moves = move_list_create(TEST_ALL_MOVES_CAPACITY);
  generate_plain(game, plain);

  // No root leaves: plain generation.
  generate_root_candidates(NULL, game, root);
  assert(move_list_get_count(root) == move_list_get_count(plain));
  for (int move_idx = 0; move_idx < move_list_get_count(plain); move_idx++) {
    const Move *a = move_list_get_move(plain, move_idx);
    const Move *b = move_list_get_move(root, move_idx);
    assert(compare_moves_without_equity(a, b, true) == -1);
    assert(move_get_equity(a) == move_get_equity(b));
  }

  // A zero-coefficient KLV3 ranks exactly as the KLV2: the same equities in
  // the same order, and the same moves up to ties. Move generation leaves its
  // list in heap order and the root candidates come back sorted, so sort the
  // plain list to compare.
  move_list_sort_moves(plain);
  ErrorStack *error_stack = error_stack_create();
  RootLeaves *zero = root_leaves_create(DEFAULT_TEST_DATA_PATH,
                                        "root_leaves_zero", error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(root_leaves_get_alphabet_size(zero) == TEST_ALPHABET_SIZE);
  generate_root_candidates(zero, game, root);
  assert(move_list_get_count(root) == move_list_get_count(plain));
  const int num_candidates = move_list_get_count(plain);
  const Equity last_equity =
      move_get_equity(move_list_get_move(plain, num_candidates - 1));
  for (int move_idx = 0; move_idx < num_candidates; move_idx++) {
    const Move *a = move_list_get_move(plain, move_idx);
    assert(move_get_equity(a) ==
           move_get_equity(move_list_get_move(root, move_idx)));
    if (move_get_equity(a) != last_equity) {
      assert(find_move(root, a) != NULL);
    }
  }
  root_leaves_destroy(zero);

  // A -25 bias per held S after a draw: every root candidate's equity is its
  // KLV2 equity less 25 per S it keeps, and the list holds the best of those.
  RootLeaves *s_bias = root_leaves_create(DEFAULT_TEST_DATA_PATH,
                                          "root_leaves_s_bias", error_stack);
  assert(error_stack_is_empty(error_stack));
  generate_root_candidates(s_bias, game, root);
  generate_plain(game, all_moves);
  const int num_all = move_list_get_count(all_moves);
  Equity *adjusted = malloc_or_die(sizeof(Equity) * num_all);
  int kept_s_changes = 0;
  for (int move_idx = 0; move_idx < num_all; move_idx++) {
    const Move *move = move_list_get_move(all_moves, move_idx);
    Rack leave;
    get_leave_for_move(move, game, &leave);
    const int draws = move_get_type(move) == GAME_EVENT_PASS
                          ? 0
                          : move_get_tiles_played(move);
    const Equity expected =
        draws > 0 ? int_to_equity(-25) *
                        rack_get_letter(&leave, TEST_S_MACHINE_LETTER)
                  : 0;
    adjusted[move_idx] = move_get_equity(move) + expected;
    if (expected != 0) {
      kept_s_changes++;
    }
    const Move *in_root = find_move(root, move);
    if (in_root) {
      assert(move_get_equity(in_root) == adjusted[move_idx]);
    }
  }
  assert(kept_s_changes > 0);
  // The root list is sorted and nothing outside it beats its last entry.
  for (int move_idx = 1; move_idx < move_list_get_count(root); move_idx++) {
    assert(move_get_equity(move_list_get_move(root, move_idx - 1)) >=
           move_get_equity(move_list_get_move(root, move_idx)));
  }
  const Equity root_last =
      move_get_equity(move_list_get_move(root, move_list_get_count(root) - 1));
  for (int move_idx = 0; move_idx < num_all; move_idx++) {
    if (!find_move(root, move_list_get_move(all_moves, move_idx))) {
      assert(adjusted[move_idx] <= root_last);
    }
  }
  free(adjusted);
  // Rollouts evaluate with the player's own leaves, which root ranking never
  // touches: static move generation afterwards matches before.
  MoveList *after = move_list_create(TEST_ALL_MOVES_CAPACITY);
  generate_plain(game, after);
  assert(move_list_get_count(after) == num_all);
  for (int move_idx = 0; move_idx < num_all; move_idx++) {
    assert(move_get_equity(move_list_get_move(after, move_idx)) ==
           move_get_equity(move_list_get_move(all_moves, move_idx)));
  }
  move_list_destroy(after);
  root_leaves_destroy(s_bias);

  // Through the config: -rootleaves loads per player, none unloads, and a
  // missing file is an error.
  load_and_exec_config_or_die(config, "set -rootleaves1 root_leaves_s_bias");
  assert_config_exec_status(config, "set -rootleaves2 no_such_klv3",
                            ERROR_STATUS_FILEPATH_FILE_NOT_FOUND);
  load_and_exec_config_or_die(config, "set -rootleaves none");

  error_stack_destroy(error_stack);
  move_list_destroy(plain);
  move_list_destroy(root);
  move_list_destroy(all_moves);
  config_destroy(config);
  remove_test_klv3("root_leaves_zero");
  remove_test_klv3("root_leaves_s_bias");
}
