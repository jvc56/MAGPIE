#include <assert.h>

#include "../../src/ent/bit_rack.h"
#include "../../src/ent/wmp.h"

#include "../../src/impl/config.h"
#include "../../src/impl/gameplay.h"
#include "../../src/impl/wmp_movegen.h"

#include "test_util.h"

void time_look_up_words(Game *game, const WMP *wmp) {
  const LetterDistribution *ld = game_get_ld(game);
  LeaveMap leave_map;
  const clock_t start = clock();
  const int num_racks = 1e3;
  for (int i = 0; i < num_racks; i++) {
    game_reset(game);
    draw_to_full_rack(game, 0);
    Player *player = game_get_player(game, 0);
    Rack *full_rack = player_get_rack(player);
    leave_map_init(full_rack, &leave_map);
    for (int leave_map_idx = 0; leave_map_idx < 1 << RACK_SIZE;
         leave_map_idx++) {
      leave_map.leave_values[leave_map_idx] = 0.001 * leave_map_idx;
    }
    BitRack full_bit_rack = bit_rack_create_from_rack(ld, full_rack);
    Subracks subracks;
    subracks_init(&subracks, &full_bit_rack, &leave_map);
    subracks_look_up_words(&subracks, 2, false, wmp);
  }
  const clock_t end = clock();
  printf("Existence checks for %d racks in %f seconds\n", num_racks,
         (double)(end - start) / CLOCKS_PER_SEC);
}

void benchmark_csw_wmp(void) {
  Config *config = config_create_or_die("set -lex CSW21 -wmp1 true");
  Game *game = config_game_create(config);
  const LetterDistribution *ld = game_get_ld(game);
  const Player *player = game_get_player(game, 0);
  const WMP *wmp = player_get_wmp(player);

  uint8_t *buffer = malloc_or_die(wmp->max_word_lookup_bytes);

  BitRack doghare_ = string_to_bit_rack(ld, "HAGRODE?");
  int bytes_written = wmp_write_words_to_buffer(wmp, &doghare_, 8, buffer);
  assert(bytes_written == 16);
  assert_word_in_buffer(buffer, "GHERAOED", ld, 0, 8);
  assert_word_in_buffer(buffer, "GOATHERD", ld, 1, 8);

  time_look_up_words(game, wmp);

  game_destroy(game);
  config_destroy(config);
}

void test_wmp_movegen(void) { benchmark_csw_wmp(); }