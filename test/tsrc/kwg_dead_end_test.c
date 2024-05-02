#include <assert.h>

#include "../../src/ent/config.h"
#include "../../src/ent/kwg.h"
#include "../../src/ent/kwg_dead_ends.h"
#include "../../src/ent/rack.h"

#include "../../src/impl/gameplay.h"

#include "../pi/move_gen_pi.h"

#include "test_util.h"

// Only works for english
static inline uint64_t string_to_tile_sequence(KWGDeadEnds *kwgde,
                                               const char *str) {
  int str_length = string_length(str);
  uint64_t tile_sequence_val = 0;
  for (int i = 0; i < str_length; i++) {
    char c = str[i];
    uint64_t letter_val;
    if (c == '^') {
      letter_val = kwgde->base_offset - 1;
    } else {
      letter_val = c + 1 - 'A';
    }
    tile_sequence_val += kwgde->dead_end_level_offsets[i] * letter_val;
  }
  return tile_sequence_val;
}

void assert_kwg_dead_ends_max_tiles_played(KWGDeadEnds *kwgde,
                                           const LetterDistribution *ld,
                                           const KWG *kwg, const char *rack_str,
                                           int expected_max_tiles_played) {
  const int ld_size = ld_get_size(ld);
  Rack *rack = rack_create(ld_size);
  rack_set_to_string(ld, rack, rack_str);
  kwgde_set_dead_ends(kwgde, kwg, rack, ld_size);
  int max_tiles_played = kwgde_get_max_tiles_played(kwgde);
  if (max_tiles_played != expected_max_tiles_played) {
    log_fatal("max tiles played are not equal for %s: %d != %d\n", rack_str,
              max_tiles_played, expected_max_tiles_played);
  }
  rack_destroy(rack);
}

void test_kwg_dead_ends_max_word_length() {
  Config *config = create_config_or_die(
      "setoptions lex CSW21 s1 equity s2 equity r1 all r2 all numplays 1");
  const KWG *kwg = players_data_get_kwg(config_get_players_data(config), 0);
  const LetterDistribution *ld = config_get_ld(config);

  KWGDeadEnds kwgde;

  assert_kwg_dead_ends_max_tiles_played(&kwgde, ld, kwg, "", 0);
  assert_kwg_dead_ends_max_tiles_played(&kwgde, ld, kwg, "A", 0);
  assert_kwg_dead_ends_max_tiles_played(&kwgde, ld, kwg, "AE", 2);
  assert_kwg_dead_ends_max_tiles_played(&kwgde, ld, kwg, "AET", 3);
  assert_kwg_dead_ends_max_tiles_played(&kwgde, ld, kwg, "AETR", 4);
  assert_kwg_dead_ends_max_tiles_played(&kwgde, ld, kwg, "AETRS", 5);
  assert_kwg_dead_ends_max_tiles_played(&kwgde, ld, kwg, "AETRSI", 6);
  assert_kwg_dead_ends_max_tiles_played(&kwgde, ld, kwg, "AEINRST", 7);

  assert_kwg_dead_ends_max_tiles_played(&kwgde, ld, kwg, "", 0);
  assert_kwg_dead_ends_max_tiles_played(&kwgde, ld, kwg, "?", 0);
  assert_kwg_dead_ends_max_tiles_played(&kwgde, ld, kwg, "A?", 2);
  assert_kwg_dead_ends_max_tiles_played(&kwgde, ld, kwg, "AE?", 3);
  assert_kwg_dead_ends_max_tiles_played(&kwgde, ld, kwg, "AET?", 4);
  assert_kwg_dead_ends_max_tiles_played(&kwgde, ld, kwg, "AETR?", 5);
  assert_kwg_dead_ends_max_tiles_played(&kwgde, ld, kwg, "AETRS?", 6);
  assert_kwg_dead_ends_max_tiles_played(&kwgde, ld, kwg, "AISERT?", 7);
  assert_kwg_dead_ends_max_tiles_played(&kwgde, ld, kwg, "AISER??", 7);
  assert_kwg_dead_ends_max_tiles_played(&kwgde, ld, kwg, "BUSUUTI", 7);
  assert_kwg_dead_ends_max_tiles_played(&kwgde, ld, kwg, "KARATES", 7);

  assert_kwg_dead_ends_max_tiles_played(&kwgde, ld, kwg, "UUUVVWW", 0);
  assert_kwg_dead_ends_max_tiles_played(&kwgde, ld, kwg, "QZJKLMN", 0);
  assert_kwg_dead_ends_max_tiles_played(&kwgde, ld, kwg, "UUUVVWD", 4);
  assert_kwg_dead_ends_max_tiles_played(&kwgde, ld, kwg, "QIUUUUO", 2);
  assert_kwg_dead_ends_max_tiles_played(&kwgde, ld, kwg, "MUUMUUS", 7);
  assert_kwg_dead_ends_max_tiles_played(&kwgde, ld, kwg, "OOOOOOO", 2);
  assert_kwg_dead_ends_max_tiles_played(&kwgde, ld, kwg, "EEEEEEW", 4);
  assert_kwg_dead_ends_max_tiles_played(&kwgde, ld, kwg, "AFGBEUW", 4);
  assert_kwg_dead_ends_max_tiles_played(&kwgde, ld, kwg, "AFGBEUH", 5);
  assert_kwg_dead_ends_max_tiles_played(&kwgde, ld, kwg, "KGOTLAT", 6);
  assert_kwg_dead_ends_max_tiles_played(&kwgde, ld, kwg, "KGOTLET", 5);
  config_destroy(config);
}

void print_kwgde(KWGDeadEnds *kwgde) {
  int number_of_dead_ends = kwgde_get_number_of_dead_ends(kwgde);
  printf("dead ends: %d\n", number_of_dead_ends);
  for (int i = 0; i < number_of_dead_ends; i++) {
    printf("%d: ", i);
    print_english_dead_end(kwgde, kwgde_get_dead_end(kwgde, i));
  }
}

void test_kwg_dead_ends_move_gen() {
  Config *config = create_config_or_die(
      "setoptions lex CSW21 s1 equity s2 equity r1 all r2 all numplays 1");
  Game *game = game_create(config);
  MoveList *move_list = move_list_create(1);
  Rack *rack = player_get_rack(game_get_player(game, 0));

  rack_set_to_string(game_get_ld(game), rack, "AET");
  generate_moves_for_game(game, 0, move_list);

  // Get the kwgde after calling generate
  // so a movegen internal struct is created.
  KWGDeadEnds *kwgde = gen_get_kwgde(0);

  assert(kwgde_get_number_of_dead_ends(kwgde) == 1);
  assert(kwgde_get_dead_end(kwgde, 0) == string_to_tile_sequence(kwgde, "A^E"));

  rack_set_to_string(game_get_ld(game), rack, "JQZ");
  generate_moves_for_game(game, 0, move_list);

  assert(kwgde_get_number_of_dead_ends(kwgde) == 3);
  assert(kwgde_get_dead_end(kwgde, 0) == string_to_tile_sequence(kwgde, "J"));
  assert(kwgde_get_dead_end(kwgde, 1) == string_to_tile_sequence(kwgde, "Q"));
  assert(kwgde_get_dead_end(kwgde, 2) == string_to_tile_sequence(kwgde, "Z"));

  game_destroy(game);
  move_list_destroy(move_list);
  config_destroy(config);
}

void test_kwg_dead_ends() {
  // test_kwg_dead_ends_max_word_length();
  test_kwg_dead_ends_move_gen();
}