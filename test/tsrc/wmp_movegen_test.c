#include <assert.h>

#include "../../src/ent/bit_rack.h"
#include "../../src/ent/wmp.h"

#include "../../src/impl/config.h"
#include "../../src/impl/gameplay.h"
#include "../../src/impl/wmp_movegen.h"

#include "../../src/str/rack_string.h"

#include "test_util.h"

void time_look_up_nonplaythrough(Game *game, WMPMoveGen *wgen, const WMP *wmp) {
  const LetterDistribution *ld = game_get_ld(game);
  LeaveMap leave_map;
  for (int i = 0; i < (1 << RACK_SIZE); i++) {
    leave_map.leave_values[i] = 0;
  }
  const clock_t start = clock();
  const int num_racks = 1e3;
  int has_word_of_length[RACK_SIZE + 1];
  memset(has_word_of_length, 0, sizeof(has_word_of_length));
  for (int i = 0; i < num_racks; i++) {
    game_reset(game);
    draw_to_full_rack(game, 0);
    Player *player = game_get_player(game, 0);
    Rack *full_rack = player_get_rack(player);
    leave_map_init(full_rack, &leave_map);
    wmp_move_gen_init(wgen, ld, full_rack, wmp);
    wmp_move_gen_check_nonplaythrough_existence(wgen, false, &leave_map);
    for (int length = MINIMUM_WORD_LENGTH; length <= RACK_SIZE; length++) {
      if (wmp_move_gen_nonplaythrough_word_of_length_exists(wgen, length)) {
        has_word_of_length[length]++;
      }
    }
  }
  const clock_t end = clock();
  printf("Existence checks for %d racks in %f seconds\n", num_racks,
         (double)(end - start) / CLOCKS_PER_SEC);
  for (int length = MINIMUM_WORD_LENGTH; length <= RACK_SIZE; length++) {
    printf("Racks with words of length %d: %.2f%%\n", length,
           100.0 * has_word_of_length[length] / num_racks);
  }
  assert(has_word_of_length[2] > 0.95 * num_racks);
  assert(has_word_of_length[3] > 0.95 * num_racks);
  assert(has_word_of_length[7] > 0.08 * num_racks);
  assert(has_word_of_length[7] < 0.23 * num_racks);
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

  WMPMoveGen wgen;
  time_look_up_nonplaythrough(game, &wgen, wmp);

  free(buffer);
  game_destroy(game);
  config_destroy(config);
}

void test_wmp_movegen(void) { benchmark_csw_wmp(); }