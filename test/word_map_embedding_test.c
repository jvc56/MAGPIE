#include "word_map_embedding_test.h"

#include "../src/def/board_defs.h"
#include "../src/def/wmp_defs.h"
#include "../src/ent/wmp.h"
#include "../src/ent/word_info_table.h"
#include "../src/impl/cmd_api.h"
#include "../src/impl/cmd_api_wmp.h"
#include "../src/impl/wmp_maker.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static void test_word_map_atomic_write(const WMP *wmp) {
  char directory[] = "/tmp/magpie-wmp-write-XXXXXX";
  assert(mkdtemp(directory));
  char *filename = get_formatted_string("%s/table.wmp", directory);
  assert(mkdir(filename, 0700) == 0);
  ErrorStack *errors = error_stack_create();
  // A directory cannot be replaced by the completed file. Preserve it and
  // clean up the temporary output on this failure path.
  wmp_write_to_file(wmp, filename, errors);
  assert(!error_stack_is_empty(errors));
  error_stack_destroy(errors);
  assert(rmdir(filename) == 0);
  assert(rmdir(directory) == 0);
  free(filename);
}

void test_word_map_embedding(void) {
  assert(cmd_api_build_wmp(NULL, 1, NULL) == NULL);
  assert(cmd_api_build_wit(NULL) == NULL);
  assert(!cmd_api_install_wmp(NULL, NULL, NULL));
  Magpie *failed = magpie_create("./nonexistent_data_path");
  assert(magpie_has_error(failed));
  assert(cmd_api_build_wmp(failed, 1, NULL) == NULL);
  assert(cmd_api_build_wit(failed) == NULL);
  assert(!cmd_api_install_wmp(failed, NULL, NULL));
  magpie_destroy(failed);
  Magpie *mp = magpie_create(DEFAULT_TEST_DATA_PATH);
  assert(!magpie_has_error(mp));
  WMPBuildProgress *progress = wmp_build_progress_create();
  assert(cmd_api_build_wmp(mp, 2, progress) == NULL);
  assert(cmd_api_build_wit(mp) == NULL);
  assert(!cmd_api_install_wmp(mp, NULL, NULL));
  free(magpie_get_and_clear_error(mp));
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
  wmp_build_progress_cancel(progress);
  assert(cmd_api_build_wmp(mp, 1, progress) == NULL);
  wmp_build_progress_destroy(progress);
  progress = wmp_build_progress_create();
  WMP *wmp = cmd_api_build_wmp(mp, 2, progress);
  assert(wmp != NULL);
  test_word_map_atomic_write(wmp);
  wmp_build_stage_t stage;
  int completed;
  int total;
  wmp_build_progress_snapshot(progress, &stage, &completed, &total);
  assert(stage == WMP_BUILD_INDEXES);
  assert(completed == BOARD_DIM - 1);
  assert(total == BOARD_DIM - 1);
  WordInfoTable *wit = cmd_api_build_wit(mp);
  assert(wit != NULL);
  // Failed installation leaves ownership with the caller and the engine usable.
  char *name = wmp->name;
  wmp->name = string_duplicate("wrong-lexicon");
  assert(!cmd_api_install_wmp(mp, wmp, wit));
  free(wmp->name);
  wmp->name = name;
  assert(magpie_has_error(mp));
  free(magpie_get_and_clear_error(mp));
  assert(magpie_run_sync(mp, "set -l2 NWL23") == MAGPIE_SUCCESS);
  assert(!cmd_api_install_wmp(mp, wmp, wit));
  assert(magpie_has_error(mp));
  free(magpie_get_and_clear_error(mp));
  assert(magpie_run_sync(mp, "generate") == MAGPIE_SUCCESS);
  assert(magpie_run_sync(mp, "set -l2 CSW24") == MAGPIE_SUCCESS);
  wmp->kwg_hash ^= 1;
  assert(!cmd_api_install_wmp(mp, wmp, wit));
  wmp->kwg_hash ^= 1;
  wmp->ld_fingerprint ^= 1;
  assert(!cmd_api_install_wmp(mp, wmp, wit));
  wmp->ld_fingerprint ^= 1;
  char small_output[32];
  FILE *limited_stream = fmemopen(small_output, sizeof(small_output), "wb");
  assert(limited_stream);
  const int unbuffered = setvbuf(limited_stream, NULL, _IONBF, 0);
  assert(unbuffered == 0);
  (void)unbuffered;
  const bool serialized = wmp_write_to_stream(wmp, limited_stream);
  assert(!serialized);
  (void)fclose(limited_stream);
  const uint64_t hash = wit->kwg_hash;
  wit->kwg_hash ^= 1;
  assert(!cmd_api_install_wmp(mp, wmp, wit));
  wit->kwg_hash = hash;
  assert(magpie_has_error(mp));
  free(magpie_get_and_clear_error(mp));
  assert(magpie_run_async(
             mp, "simulate -it 100000000 -plies 7 -scond none -threads 1") ==
         MAGPIE_SUCCESS);
  assert(cmd_api_build_wmp(mp, 2, progress) == NULL);
  assert(cmd_api_build_wit(mp) == NULL);
  assert(!cmd_api_install_wmp(mp, wmp, wit));
  magpie_stop_current_command(mp);
  assert(magpie_await(mp) == MAGPIE_SUCCESS);
  assert(cmd_api_install_wmp(mp, wmp, wit));
  assert(magpie_run_sync(mp, "set -wmp true -wit true") == MAGPIE_SUCCESS);
  assert(magpie_run_sync(mp, "generate") == MAGPIE_SUCCESS);
  char *accelerated = magpie_get_last_command_output(mp);
  assert(strings_equal(baseline, accelerated));
  free(accelerated);
  free(baseline);
  // The engine now owns both shared tables and destroys each only once.
  wmp_build_progress_destroy(progress);
  magpie_destroy(mp);
}
