#include <assert.h>
#include <unistd.h>

#include "../../src/ent/wmp.h"

#include "../../src/impl/config.h"
#include "../../src/impl/gameplay.h"

#include "../../src/str/rack_string.h"

#include "test_util.h"

void time_wmp_buffer_writes(Game *game, WMP *wmp) {
  const LetterDistribution *ld = game_get_ld(game);
  uint8_t *buffer = malloc_or_die(wmp->max_word_lookup_bytes);
  int bytes_written = 0;
  uint64_t lookups = 0;
  uint64_t total_bytes_written = 0;
  for (int i = 0; i < 1e6; i++) {
    game_reset(game);
    draw_to_full_rack(game, 0);
    Player *player = game_get_player(game, 0);

    Rack *full_rack = player_get_rack(player);
    BitRack full_bit_rack = bit_rack_create_from_rack(ld, full_rack);

    BitRackPowerSet set;
    bit_rack_power_set_init(&set, &full_bit_rack);
    for (int size = 2; size <= RACK_SIZE; size++) {
      const int offset = bit_rack_get_combination_offset(size);
      const int count = set.count_by_size[size];
      for (int idx_for_size = 0; idx_for_size < count; idx_for_size++) {
        BitRack *bit_rack = &set.subracks[offset + idx_for_size];
        bytes_written = wmp_write_words_to_buffer(wmp, bit_rack, size, buffer); assert(bytes_written % size == 0);
        total_bytes_written += bytes_written;
        lookups++;
      }
    }
  }
  printf("performed %llu lookups, %llu bytes written\n", lookups,
          total_bytes_written);
}

void time_wmp_existence_checks(Game *game, WMP *wmp) {
  const LetterDistribution *ld = game_get_ld(game);
  uint64_t lookups = 0;
  uint64_t num_with_solution = 0;
  for (int i = 0; i < 1e6; i++) {
    game_reset(game);
    draw_to_full_rack(game, 0);
    Player *player = game_get_player(game, 0);

    Rack *full_rack = player_get_rack(player);
    BitRack full_bit_rack = bit_rack_create_from_rack(ld, full_rack);

    BitRackPowerSet set;
    bit_rack_power_set_init(&set, &full_bit_rack);
    for (int size = 2; size <= RACK_SIZE; size++) {
      const int offset = bit_rack_get_combination_offset(size);
      const int count = set.count_by_size[size];
      for (int idx_for_size = 0; idx_for_size < count; idx_for_size++) {
        BitRack *bit_rack = &set.subracks[offset + idx_for_size];
        const bool has_solution = wmp_has_word(wmp, bit_rack, size);
        num_with_solution += has_solution;
        lookups++;
      }
    }
  }
  printf("performed %llu lookups, %llu having solutions\n", lookups,
         num_with_solution);
}

void benchmark_csw_wmp(void) {
  WMP *wmp = wmp_create("testdata", "CSW21");
  assert(wmp != NULL);

  Config *config = config_create_or_die("set -lex CSW21");
  Game *game = config_game_create(config);
  sleep(10);
  time_wmp_buffer_writes(game, wmp);
  time_wmp_existence_checks(game, wmp);

  game_destroy(game);
  config_destroy(config);
  wmp_destroy(wmp);
}

void test_short_and_long_words(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  const LetterDistribution *ld = config_get_ld(config);

  WMP *wmp = wmp_create("testdata", "CSW21_3or15");
  assert(wmp != NULL);

  uint8_t *buffer = malloc_or_die(wmp->max_word_lookup_bytes);
  BitRack inq = string_to_bit_rack(ld, "INQ");
  int bytes_written = wmp_write_words_to_buffer(wmp, &inq, 3, buffer);
  assert(bytes_written == 3);
  assert_word_in_buffer(buffer, "QIN", ld, 0, 3);

  BitRack vv_blank = string_to_bit_rack(ld, "VV?");
  bytes_written = wmp_write_words_to_buffer(wmp, &vv_blank, 3, buffer);
  assert(bytes_written == 3);
  assert_word_in_buffer(buffer, "VAV", ld, 0, 3);

  BitRack q_blank_blank = string_to_bit_rack(ld, "Q??");
  bytes_written = wmp_write_words_to_buffer(wmp, &q_blank_blank, 3, buffer);
  assert(bytes_written == 15);

  assert_word_in_buffer(buffer, "QAT", ld, 0, 3);
  assert_word_in_buffer(buffer, "QUA", ld, 1, 3);
  assert_word_in_buffer(buffer, "QIN", ld, 2, 3);
  assert_word_in_buffer(buffer, "QIS", ld, 3, 3);
  assert_word_in_buffer(buffer, "SUQ", ld, 4, 3);

  BitRack quarterbackin_double_blank =
      string_to_bit_rack(ld, "QUARTERBACKIN??");
  bytes_written =
      wmp_write_words_to_buffer(wmp, &quarterbackin_double_blank, 15, buffer);
  assert(bytes_written == 15);
  assert_word_in_buffer(buffer, "QUARTERBACKINGS", ld, 0, 15);

  config_destroy(config);
}

void test_wmp(void) {
  benchmark_csw_wmp();
  //test_short_and_long_words();
}