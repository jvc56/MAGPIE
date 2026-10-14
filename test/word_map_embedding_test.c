#include "word_map_embedding_test.h"

#include "../src/compat/cfile.h"
#include "../src/def/board_defs.h"
#include "../src/def/wmp_defs.h"
#include "../src/ent/wmp.h"
#include "../src/ent/word_info_table.h"
#include "../src/impl/cmd_api.h"
#include "../src/impl/cmd_api_wmp.h"
#include "../src/impl/wmp_maker.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_platform.h"
#include "test_util.h"
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_word_map_atomic_write(const WMP *wmp) {
  char *directory = test_platform_make_temporary_directory("magpie-wmp-write-");
  char *filename = get_formatted_string("%s/table.wmp", directory);
  assert(cfile_mkdir(filename) == 0);
  ErrorStack *errors = error_stack_create();
  // A directory cannot be replaced by the completed file. Preserve it and
  // clean up the temporary output on this failure path.
  wmp_write_to_file(wmp, filename, errors);
  assert(!error_stack_is_empty(errors));
  error_stack_destroy(errors);
  assert(cfile_rmdir(filename) == 0);
  assert(cfile_rmdir(directory) == 0);
  free(filename);
  free(directory);
}

static void write_map(const WMP *wmp, const WordInfoTable *wit,
                      const char *filename, ErrorStack *errors) {
  if (wit) {
    word_info_table_write_to_file(wit, filename, errors);
  } else {
    wmp_write_to_file(wmp, filename, errors);
  }
}

// That `path`'s permission bits are `expected`, where they are POSIX's;
// Windows has none to compare.
static void assert_permissions(const char *path, unsigned int expected) {
  TestPlatformFileStatus status;
  assert(test_platform_stat(path, &status));
  if (test_platform_has_posix_permissions()) {
    assert((status.mode & 0777) == expected);
  }
}

static int64_t file_size(const char *path) {
  CFileIdentity identity;
  assert(cfile_identity(path, &identity));
  return identity.size;
}

// Writes through symlinks in `directory`, where `linkname` already links to
// the not yet written `filename`'s base name, "table".
static void test_map_symlinks(const WMP *wmp, const WordInfoTable *wit,
                              const char *directory, const char *filename,
                              const char *linkname, ErrorStack *errors) {
  // A relative symlink must survive and update the actual target.
  write_string_to_file(filename, "w", "stale", errors);
  write_map(wmp, wit, linkname, errors);
  assert(error_stack_is_empty(errors));
  assert(test_platform_is_symlink(linkname));
  assert(file_size(filename) > 5);
  assert_permissions(filename, 0664);
  remove_or_die(linkname);
  remove_or_die(filename);
  // Both relative and absolute dangling targets are created through the link.
  char *working_directory = test_platform_current_directory();
  char *absolute_filename =
      get_formatted_string("%s/%s", working_directory, filename);
  const char *targets[] = {"table", absolute_filename};
  for (int target_idx = 0; target_idx < 2; target_idx++) {
    assert(test_platform_symlink(targets[target_idx], linkname));
    write_map(wmp, wit, linkname, errors);
    assert(error_stack_is_empty(errors));
    assert(test_platform_is_symlink(linkname));
    assert(file_size(filename) > 5);
    assert_permissions(filename, 0644);
    remove_or_die(linkname);
    remove_or_die(filename);
  }
  free(absolute_filename);
  free(working_directory);
  char *chain = get_formatted_string("%s/chain", directory);
  assert(test_platform_symlink("link", chain));
  assert(test_platform_symlink("table", linkname));
  write_map(wmp, wit, chain, errors);
  assert(error_stack_is_empty(errors));
  assert(file_size(filename) > 5);
  remove_or_die(filename);
  remove_or_die(linkname);
  assert(test_platform_symlink("chain", linkname));
  write_map(wmp, wit, chain, errors);
  assert(!error_stack_is_empty(errors));
  remove_or_die(chain);
  remove_or_die(linkname);
  free(chain);
}

static void test_map_permissions(const WMP *wmp, const WordInfoTable *wit) {
  char *directory = test_platform_make_temporary_directory("magpie-map-mode-");
  char *filename = get_formatted_string("%s/table", directory);
  char *linkname = get_formatted_string("%s/link", directory);
  ErrorStack *errors = error_stack_create();
  if (!test_platform_has_posix_permissions()) {
    printf("word_map_embedding: skipped the permission bits: none here\n");
  }
  const unsigned int original_mask = test_platform_set_umask(0022);
  write_map(wmp, wit, filename, errors);
  assert(error_stack_is_empty(errors));
  assert_permissions(filename, 0644);
  assert(test_platform_set_permissions(filename, 0664));
  write_map(wmp, wit, filename, errors);
  assert(error_stack_is_empty(errors));
  assert_permissions(filename, 0664);
  if (test_platform_symlink("table", linkname)) {
    test_map_symlinks(wmp, wit, directory, filename, linkname, errors);
  } else {
    printf("word_map_embedding: skipped the symlink cases: this process may "
           "not create symlinks\n");
    remove_or_die(filename);
  }
  assert(cfile_rmdir(directory) == 0);
  (void)test_platform_set_umask(original_mask);
  error_stack_destroy(errors);
  free(linkname);
  free(filename);
  free(directory);
}

static void test_word_map_write_errno(const WMP *wmp) {
  if (!test_platform_can_limit_file_size()) {
    printf("word_map_embedding: skipped the full-disk case: no file size "
           "limit here\n");
    return;
  }
  char *directory = test_platform_make_temporary_directory("magpie-map-error-");
  char *filename = get_formatted_string("%s/table", directory);
  ErrorStack *errors = error_stack_create();
  write_string_to_file(filename, "w", "preserved", errors);
  assert(error_stack_is_empty(errors));
  test_platform_limit_file_size(1024);
  wmp_write_to_file(wmp, filename, errors);
  test_platform_unlimit_file_size();
  char *message = error_stack_get_string_and_reset(errors);
  const char *expected_error = strerror(EFBIG);
  assert(strstr(message, expected_error));
  free(message);
  char *contents = get_string_from_file(filename, errors);
  assert(strings_equal(contents, "preserved"));
  free(contents);
  remove_or_die(filename);
  assert(cfile_rmdir(directory) == 0);
  error_stack_destroy(errors);
  free(filename);
  free(directory);
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
  test_map_permissions(wmp, NULL);
  test_word_map_write_errno(wmp);
  wmp_build_stage_t stage;
  int completed;
  int total;
  wmp_build_progress_snapshot(progress, &stage, &completed, &total);
  assert(stage == WMP_BUILD_INDEXES);
  assert(completed == BOARD_DIM - 1);
  assert(total == BOARD_DIM - 1);
  WordInfoTable *wit = cmd_api_build_wit(mp);
  assert(wit != NULL);
  test_map_permissions(wmp, wit);
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
  FILE *limited_stream =
      test_platform_open_bounded_stream(small_output, sizeof(small_output));
  if (limited_stream) {
    const int unbuffered = setvbuf(limited_stream, NULL, _IONBF, 0);
    assert(unbuffered == 0);
    (void)unbuffered;
    const bool serialized = wmp_write_to_stream(wmp, limited_stream);
    assert(!serialized);
    (void)fclose(limited_stream);
  } else {
    printf("word_map_embedding: skipped the full-stream case: no bounded "
           "streams here\n");
  }
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
