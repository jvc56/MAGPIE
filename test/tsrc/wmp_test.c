#include <assert.h>
#include <unistd.h>

#include "../../src/ent/wmp.h"

#include "../../src/impl/config.h"
#include "../../src/impl/gameplay.h"

#include "../../src/str/rack_string.h"

#include "test_util.h"

void time_random_racks_with_wmp(Game *game, WMP *wmp) {
  const LetterDistribution *ld = game_get_ld(game);
  uint8_t *buffer = malloc_or_die(wmp->max_word_lookup_bytes);
  int bytes_written = 0;
  uint64_t lookups = 0;
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
        bytes_written = wmp_write_words_to_buffer(wmp, bit_rack, size, buffer);
        assert(bytes_written % size == 0);
        lookups++;
      }
    }
  }
  printf("performed %llu lookups\n", lookups);
}

void benchmark_csw_wmp(void) {
  WMP *wmp = wmp_create("testdata", "CSW21");
  assert(wmp != NULL);

  Config *config = config_create_or_die("set -lex CSW21");
  Game *game = config_game_create(config);
  sleep(10);
  time_random_racks_with_wmp(game, wmp);

  game_destroy(game);
  config_destroy(config);
  wmp_destroy(wmp);
}

void test_wmp(void) {
  benchmark_csw_wmp();
}