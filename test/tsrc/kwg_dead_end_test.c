#include <assert.h>

#include "../../src/ent/config.h"
#include "../../src/ent/kwg.h"
#include "../../src/ent/kwg_dead_ends.h"
#include "../../src/ent/rack.h"

#include "../../src/impl/gameplay.h"

#include "test_util.h"

static inline void print_english_dead_end(KWGDeadEnds *kwgde,
                                          uint64_t dead_end) {
  printf("%14lu: ", dead_end);
  for (int i = 0; i < RACK_SIZE + 1; i++) {
    int letter_val = dead_end / kwgde->dead_end_level_offsets[i];
    dead_end = dead_end % kwgde->dead_end_level_offsets[i];
    char c;
    if (letter_val == kwgde->base_offset - 1) {
      c = '^';
    } else {
      c = letter_val + 'A' - 1;
    }
    printf("%c", c);
    if (dead_end == 0) {
      break;
    }
  }
  printf("\n");
}

void assert_kwg_dead_ends_init(KWGDeadEnds *kwgde, const LetterDistribution *ld,
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

void test_kwg_dead_ends() {
  Config *config = create_config_or_die(
      "setoptions lex CSW21 s1 equity s2 equity r1 all r2 all numplays 1");
  const KWG *kwg = players_data_get_kwg(config_get_players_data(config), 0);
  const LetterDistribution *ld = config_get_ld(config);

  KWGDeadEnds kwgde;

  assert_kwg_dead_ends_init(&kwgde, ld, kwg, "", 0);
  assert_kwg_dead_ends_init(&kwgde, ld, kwg, "A", 0);
  assert_kwg_dead_ends_init(&kwgde, ld, kwg, "AE", 2);
  assert_kwg_dead_ends_init(&kwgde, ld, kwg, "AET", 3);
  assert_kwg_dead_ends_init(&kwgde, ld, kwg, "AETR", 4);
  assert_kwg_dead_ends_init(&kwgde, ld, kwg, "AETRS", 5);
  assert_kwg_dead_ends_init(&kwgde, ld, kwg, "AETRSI", 6);
  assert_kwg_dead_ends_init(&kwgde, ld, kwg, "AEINRST", 7);

  assert_kwg_dead_ends_init(&kwgde, ld, kwg, "", 0);
  assert_kwg_dead_ends_init(&kwgde, ld, kwg, "?", 0);
  assert_kwg_dead_ends_init(&kwgde, ld, kwg, "A?", 2);
  assert_kwg_dead_ends_init(&kwgde, ld, kwg, "AE?", 3);
  assert_kwg_dead_ends_init(&kwgde, ld, kwg, "AET?", 4);
  assert_kwg_dead_ends_init(&kwgde, ld, kwg, "AETR?", 5);
  assert_kwg_dead_ends_init(&kwgde, ld, kwg, "AETRS?", 6);
  assert_kwg_dead_ends_init(&kwgde, ld, kwg, "AISERT?", 7);
  assert_kwg_dead_ends_init(&kwgde, ld, kwg, "AISER??", 7);
  assert_kwg_dead_ends_init(&kwgde, ld, kwg, "BUSUUTI", 7);
  assert_kwg_dead_ends_init(&kwgde, ld, kwg, "KARATES", 7);

  assert_kwg_dead_ends_init(&kwgde, ld, kwg, "UUUVVWW", 0);
  assert_kwg_dead_ends_init(&kwgde, ld, kwg, "QZJKLMN", 0);
  assert_kwg_dead_ends_init(&kwgde, ld, kwg, "UUUVVWD", 4);
  assert_kwg_dead_ends_init(&kwgde, ld, kwg, "QIUUUUO", 2);
  assert_kwg_dead_ends_init(&kwgde, ld, kwg, "MUUMUUS", 7);
  assert_kwg_dead_ends_init(&kwgde, ld, kwg, "OOOOOOO", 2);
  assert_kwg_dead_ends_init(&kwgde, ld, kwg, "EEEEEEW", 4);
  assert_kwg_dead_ends_init(&kwgde, ld, kwg, "AFGBEUW", 4);
  assert_kwg_dead_ends_init(&kwgde, ld, kwg, "AFGBEUH", 5);
  assert_kwg_dead_ends_init(&kwgde, ld, kwg, "KGOTLAT", 6);
  assert_kwg_dead_ends_init(&kwgde, ld, kwg, "KGOTLET", 5);

  config_destroy(config);
}