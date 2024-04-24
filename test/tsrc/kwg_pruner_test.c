#include <assert.h>

#include "../../src/ent/config.h"
#include "../../src/ent/kwg.h"
#include "../../src/ent/kwg_pruner.h"
#include "../../src/ent/rack.h"

#include "../../src/impl/gameplay.h"

#include "test_util.h"

void assert_kwg_pruner_init(KWGPruner *kwgp, const LetterDistribution *ld,
                            const KWG *const_kwg, KWG *mutable_kwg,
                            const char *rack_str,
                            int expected_max_tiles_played) {

  assert_kwgs_are_equal(mutable_kwg, const_kwg);
  Rack *rack = rack_create(ld_get_size(ld));
  rack_set_to_string(ld, rack, rack_str);
  kwgp_prune(kwgp, rack, mutable_kwg);
  int max_tiles_played = kwgp_get_max_tiles_played(kwgp);
  if (max_tiles_played != expected_max_tiles_played) {
    log_fatal("max tiles played are not equal for %s: %d != %d\n", rack_str,
              max_tiles_played, expected_max_tiles_played);
  }
  kwgp_unprune(kwgp, rack, mutable_kwg);
  assert_kwgs_are_equal(mutable_kwg, const_kwg);
  rack_destroy(rack);
}

void test_kwg_pruner() {
  Config *mutable_kwg_config = create_config_or_die(
      "setoptions lex CSW21 s1 equity s2 equity r1 all r2 all numplays 1");
  Config *const_kwg_config = create_config_or_die(
      "setoptions lex CSW21 s1 equity s2 equity r1 all r2 all numplays 1");
  KWG *mutable_kwg =
      players_data_get_kwg(config_get_players_data(mutable_kwg_config), 0);
  const KWG *const_kwg =
      players_data_get_kwg(config_get_players_data(const_kwg_config), 0);
  const LetterDistribution *ld = config_get_ld(const_kwg_config);

  KWGPruner kwgp;

  for (int i = 0; i < const_kwg->number_of_nodes; i++) {
    if (kwg_node_dead_end(kwg_node(const_kwg, i))) {
      log_fatal("dead node at index %d with value %d\n", i,
                kwg_node(const_kwg, i));
    }
  }

  assert_kwg_pruner_init(&kwgp, ld, const_kwg, mutable_kwg, "", 0);
  assert_kwg_pruner_init(&kwgp, ld, const_kwg, mutable_kwg, "A", 0);
  assert_kwg_pruner_init(&kwgp, ld, const_kwg, mutable_kwg, "AE", 2);
  assert_kwg_pruner_init(&kwgp, ld, const_kwg, mutable_kwg, "AET", 3);
  assert_kwg_pruner_init(&kwgp, ld, const_kwg, mutable_kwg, "AETR", 4);
  assert_kwg_pruner_init(&kwgp, ld, const_kwg, mutable_kwg, "AETRS", 5);
  assert_kwg_pruner_init(&kwgp, ld, const_kwg, mutable_kwg, "AETRSI", 6);
  assert_kwg_pruner_init(&kwgp, ld, const_kwg, mutable_kwg, "AEINRST", 7);

  assert_kwg_pruner_init(&kwgp, ld, const_kwg, mutable_kwg, "", 0);
  assert_kwg_pruner_init(&kwgp, ld, const_kwg, mutable_kwg, "?", 0);
  assert_kwg_pruner_init(&kwgp, ld, const_kwg, mutable_kwg, "A?", 2);
  assert_kwg_pruner_init(&kwgp, ld, const_kwg, mutable_kwg, "AE?", 3);
  assert_kwg_pruner_init(&kwgp, ld, const_kwg, mutable_kwg, "AET?", 4);
  assert_kwg_pruner_init(&kwgp, ld, const_kwg, mutable_kwg, "AETR?", 5);
  assert_kwg_pruner_init(&kwgp, ld, const_kwg, mutable_kwg, "AETRS?", 6);
  assert_kwg_pruner_init(&kwgp, ld, const_kwg, mutable_kwg, "AISERT?", 7);
  assert_kwg_pruner_init(&kwgp, ld, const_kwg, mutable_kwg, "AISER??", 7);
  assert_kwg_pruner_init(&kwgp, ld, const_kwg, mutable_kwg, "BUSUUTI", 7);
  assert_kwg_pruner_init(&kwgp, ld, const_kwg, mutable_kwg, "KARATES", 7);

  assert_kwg_pruner_init(&kwgp, ld, const_kwg, mutable_kwg, "UUUVVWW", 0);
  assert_kwg_pruner_init(&kwgp, ld, const_kwg, mutable_kwg, "QZJKLMN", 0);
  assert_kwg_pruner_init(&kwgp, ld, const_kwg, mutable_kwg, "UUUVVWD", 4);
  assert_kwg_pruner_init(&kwgp, ld, const_kwg, mutable_kwg, "QIUUUUO", 2);
  assert_kwg_pruner_init(&kwgp, ld, const_kwg, mutable_kwg, "MUUMUUS", 7);
  assert_kwg_pruner_init(&kwgp, ld, const_kwg, mutable_kwg, "OOOOOOO", 2);
  assert_kwg_pruner_init(&kwgp, ld, const_kwg, mutable_kwg, "EEEEEEW", 4);
  assert_kwg_pruner_init(&kwgp, ld, const_kwg, mutable_kwg, "AFGBEUW", 4);
  assert_kwg_pruner_init(&kwgp, ld, const_kwg, mutable_kwg, "AFGBEUH", 5);
  assert_kwg_pruner_init(&kwgp, ld, const_kwg, mutable_kwg, "KGOTLAT", 6);
  assert_kwg_pruner_init(&kwgp, ld, const_kwg, mutable_kwg, "KGOTLET", 5);

  config_destroy(mutable_kwg_config);
  config_destroy(const_kwg_config);
}