#include "word_map_embedding_test.h"

#include "../src/def/board_defs.h"
#include "../src/ent/wmp.h"
#include "../src/ent/word_info_table.h"
#include "../src/impl/cmd_api.h"
#include "../src/impl/cmd_api_wmp.h"
#include "../src/impl/wmp_maker.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

void test_word_map_embedding(void) {
  assert(magpie_build_wmp(NULL, 1, NULL) == NULL);
  assert(magpie_build_wit(NULL) == NULL);
  assert(!magpie_install_wmp(NULL, NULL, NULL));
  Magpie *failed = magpie_create("./nonexistent_data_path");
  assert(magpie_has_error(failed));
  magpie_set_peg_json(failed, true);
  assert(magpie_build_wmp(failed, 1, NULL) == NULL);
  assert(magpie_build_wit(failed) == NULL);
  assert(!magpie_install_wmp(failed, NULL, NULL));
  magpie_destroy(failed);
  Magpie *mp = magpie_create(DEFAULT_TEST_DATA_PATH);
  assert(!magpie_has_error(mp));
  WMPBuildProgress progress = {0};
  assert(magpie_build_wmp(mp, 2, &progress) == NULL);
  assert(magpie_build_wit(mp) == NULL);
  assert(!magpie_install_wmp(mp, NULL, NULL));
  assert(magpie_run_sync(mp,
                         "set -lex CSW24 -wmp false -wit false -numplays 20") ==
         MAGPIE_SUCCESS);
  assert(
      magpie_run_sync(
          mp,
          "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 AEINRST/ 0/0 0") ==
      MAGPIE_SUCCESS);
  assert(magpie_run_sync(mp, "generate") == MAGPIE_SUCCESS);
  char *baseline = magpie_get_last_command_output(mp);
  atomic_store(&progress.cancelled, true);
  assert(magpie_build_wmp(mp, 1, &progress) == NULL);
  atomic_store(&progress.cancelled, false);
  WMP *wmp = magpie_build_wmp(mp, 2, &progress);
  assert(wmp != NULL);
  assert(atomic_load(&progress.stage) == 4);
  assert(atomic_load(&progress.completed) == BOARD_DIM - 1);
  assert(atomic_load(&progress.total) == BOARD_DIM - 1);
  WordInfoTable *wit = magpie_build_wit(mp);
  assert(wit != NULL);
  // Failed installation leaves ownership with the caller and the engine usable.
  char *name = wmp->name;
  wmp->name = string_duplicate("wrong-lexicon");
  assert(!magpie_install_wmp(mp, wmp, wit));
  free(wmp->name);
  wmp->name = name;
  assert(magpie_run_sync(mp, "set -l2 NWL23") == MAGPIE_SUCCESS);
  assert(!magpie_install_wmp(mp, wmp, wit));
  assert(magpie_run_sync(mp, "generate") == MAGPIE_SUCCESS);
  assert(magpie_run_sync(mp, "set -l2 CSW24") == MAGPIE_SUCCESS);
  wmp->kwg_hash ^= 1;
  assert(!magpie_install_wmp(mp, wmp, wit));
  wmp->kwg_hash ^= 1;
  wmp->ld_fingerprint ^= 1;
  assert(!magpie_install_wmp(mp, wmp, wit));
  wmp->ld_fingerprint ^= 1;
  const uint64_t hash = wit->kwg_hash;
  wit->kwg_hash ^= 1;
  assert(!magpie_install_wmp(mp, wmp, wit));
  wit->kwg_hash = hash;
  assert(magpie_run_async(
             mp, "simulate -it 100000000 -plies 7 -scond none -threads 1") ==
         MAGPIE_SUCCESS);
  assert(magpie_build_wmp(mp, 2, &progress) == NULL);
  assert(magpie_build_wit(mp) == NULL);
  assert(!magpie_install_wmp(mp, wmp, wit));
  magpie_stop_current_command(mp);
  assert(magpie_await(mp) == MAGPIE_SUCCESS);
  assert(magpie_install_wmp(mp, wmp, wit));
  assert(magpie_run_sync(mp, "set -wmp true -wit true") == MAGPIE_SUCCESS);
  assert(magpie_run_sync(mp, "generate") == MAGPIE_SUCCESS);
  char *accelerated = magpie_get_last_command_output(mp);
  assert(strings_equal(baseline, accelerated));
  free(accelerated);
  free(baseline);
  // The engine now owns both shared tables and destroys each only once.
  magpie_destroy(mp);
}
