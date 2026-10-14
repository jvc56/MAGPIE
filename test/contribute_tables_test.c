#include "contribute_tables_test.h"

#include "../src/compat/cfile.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/client_state.h"
#include "../src/ent/conversion_results.h"
#include "../src/ent/data_filepaths.h"
#include "../src/ent/thread_control.h"
#include "../src/impl/config.h"
#include "../src/impl/contribute.h"
#include "../src/impl/contribute_tables.h"
#include "../src/impl/convert.h"
#include "../src/util/hash.h"
#include "../src/util/io_util.h"
#include "../src/util/json.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>

// The sizes of CSW24's tables, in MB, which the chooser's cases are written
// in: what a contributor actually weighs.
#define MB 1000000LL

enum {
  WMP_MB = 179,
  WIT_MB = 122,
  RIT_MB = 1885,
};

#define TABLES_TEST_DIR "contribute_tables_test"
#define TABLES_TEST_LEXICON "CSW21_abtables"
#define TABLES_TEST_RIT TABLES_TEST_LEXICON "." TABLES_TEST_LEXICON
#define TABLES_TEST_MANIFEST                                                   \
  TABLES_TEST_DIR "/" CONTRIBUTE_TABLE_MANIFEST_FILENAME

static ContributeTable table(contribute_table_role_t role, const char *name,
                             const char *lexicon, int64_t megabytes,
                             bool requested) {
  ContributeTable result;
  memset(&result, 0, sizeof(result));
  result.role = role;
  result.name = name;
  result.lexicon = lexicon;
  result.bytes = megabytes < 0 ? -1 : megabytes * MB;
  result.requested = requested;
  return result;
}

// The three tables one CSW24 player uses.
static int csw24_player(ContributeTable *tables) {
  tables[0] = table(CONTRIBUTE_TABLE_WMP, "CSW24", "CSW24", WMP_MB, true);
  tables[1] = table(CONTRIBUTE_TABLE_WIT, "CSW24", "CSW24", WIT_MB, true);
  tables[2] = table(CONTRIBUTE_TABLE_RIT, "CSW24.CSW24", "CSW24", RIT_MB, true);
  return 3;
}

// Chooses with `free_mb` of disk free beyond `reserve_mb`, `memory_mb` of
// memory available beyond none, and the manifest, and checks the chosen
// tables are `expected` in that order (by index), as a string of digits.
static void assert_chosen_in(ContributeTable *tables, int count,
                             int64_t free_mb, int64_t reserve_mb,
                             int64_t memory_mb,
                             const ContributeTableManifest *manifest,
                             const char *expected) {
  int order[8];
  const ContributeTableBudget budget = {
      .free_disk_bytes = free_mb * MB,
      .disk_reserve_bytes = reserve_mb * MB,
      .free_memory_bytes = memory_mb < 0 ? INT64_MAX : memory_mb * MB,
      .memory_reserve_bytes = 0,
  };
  const int chosen =
      contribute_choose_tables(tables, count, &budget, manifest, order);
  char got[9] = {0};
  for (int rank = 0; rank < chosen; rank++) {
    got[rank] = (char)('0' + order[rank]);
  }
  if (!strings_equal(got, expected)) {
    log_fatal("chose '%s' with %lld MB free and %lld MB of memory, expected "
              "'%s'",
              got, (long long)free_mb, (long long)memory_mb, expected);
  }
  for (int table_idx = 0; table_idx < count; table_idx++) {
    assert(tables[table_idx].chosen ==
           (strchr(expected, '0' + table_idx) != NULL));
    // A wordmap built only for a rack info table has no reason of its own.
    assert(!tables[table_idx].requested ||
           tables[table_idx].chosen ==
               (tables[table_idx].skip == CONTRIBUTE_TABLE_KEPT));
  }
}

// The same with memory to spare.
static void assert_chosen(ContributeTable *tables, int count, int64_t free_mb,
                          int64_t reserve_mb,
                          const ContributeTableManifest *manifest,
                          const char *expected) {
  assert_chosen_in(tables, count, free_mb, reserve_mb, -1, manifest, expected);
}

// Which tables a task keeps for the space it has: wordmaps first, then word
// info tables, then rack info tables, each while it fits; never a rack info
// table without its wordmap; no word info table whose wordmap was asked for
// and skipped.
static void test_the_chooser_keeps_the_tables_worth_most(void) {
  ContributeTable tables[8];
  int count = csw24_player(tables);

  // Room for all three, in priority order, which is not the request's.
  assert_chosen(tables, count, 10000, 0, NULL, "012");
  // A diskreserve comes off the top.
  assert_chosen(tables, count, 10000, 8000, NULL, "01");
  // Room for the wordmap alone: the others say what they needed.
  assert_chosen(tables, count, 200, 0, NULL, "0");
  assert(tables[1].needed_bytes == WIT_MB * MB);
  assert(tables[1].available_bytes == (200 - WMP_MB) * MB);
  assert(tables[2].needed_bytes == RIT_MB * MB);
  // The wordmap and the word info table, exactly.
  assert_chosen(tables, count, WMP_MB + WIT_MB, 0, NULL, "01");
  // No room for the wordmap: the word info table would fit, but does nothing
  // without it, and the rack info table is built from it.
  assert_chosen(tables, count, 150, 0, NULL, "");
  assert(tables[1].needed_bytes == 0 && tables[2].needed_bytes == 0);
  // Room for the wordmap and the rack info table but not the word info table
  // too: the word info table comes first, and the rack info table does not
  // fit after it.
  assert_chosen(tables, count, WMP_MB + RIT_MB + 50, 0, NULL, "01");

  // A table on disk already costs nothing, and is kept even past the
  // reserve.
  tables[0].present = true;
  tables[1].present = true;
  assert_chosen(tables, count, 0, 2000, NULL, "01");
  // ...but a rack info table on disk is still not used without its wordmap.
  tables[0].present = false;
  tables[2].present = true;
  assert_chosen(tables, count, 100, 0, NULL, "");
  tables[1].present = false;
  tables[2].present = false;

  // A rack info table whose player did not ask for the wordmap still needs
  // it built, and counts its bytes; the wordmap comes before it.
  count = 0;
  tables[count++] =
      table(CONTRIBUTE_TABLE_RIT, "CSW24.CSW24", "CSW24", RIT_MB, true);
  tables[count++] = table(CONTRIBUTE_TABLE_WMP, "CSW24", "CSW24", WMP_MB,
                          /*requested=*/false);
  assert_chosen(tables, count, WMP_MB + RIT_MB, 0, NULL, "10");
  assert_chosen(tables, count, RIT_MB + 1, 0, NULL, "");
  assert(tables[0].needed_bytes == (WMP_MB + RIT_MB) * MB);
  // The wordmap on disk: the table alone.
  tables[1].present = true;
  assert_chosen(tables, count, RIT_MB, 0, NULL, "10");

  // A word info table asked for without a wordmap is kept if it fits.
  count = 0;
  tables[count++] = table(CONTRIBUTE_TABLE_WIT, "CSW24", "CSW24", WIT_MB, true);
  assert_chosen(tables, count, WIT_MB, 0, NULL, "0");
  assert_chosen(tables, count, WIT_MB - 1, 0, NULL, "");

  // Two players on two lexicons: both wordmaps before either word info
  // table, and both word info tables before either rack info table.
  count = 0;
  tables[count++] =
      table(CONTRIBUTE_TABLE_RIT, "CSW24.CSW24", "CSW24", RIT_MB, true);
  tables[count++] = table(CONTRIBUTE_TABLE_WIT, "CSW24", "CSW24", WIT_MB, true);
  tables[count++] = table(CONTRIBUTE_TABLE_WMP, "CSW24", "CSW24", WMP_MB, true);
  tables[count++] = table(CONTRIBUTE_TABLE_WIT, "NWL23", "NWL23", 92, true);
  tables[count++] = table(CONTRIBUTE_TABLE_WMP, "NWL23", "NWL23", 128, true);
  assert_chosen(tables, count, WMP_MB + 128 + 92, 0, NULL, "243");
  assert_chosen(tables, count, WMP_MB + 128 + 92 + WIT_MB, 0, NULL, "2413");
  assert_chosen(tables, count, 10000, 0, NULL, "24130");
  // The second wordmap does not fit: its word info table goes with it, and
  // the first lexicon's word info table takes the room.
  assert_chosen(tables, count, WMP_MB + 127, 0, NULL, "21");

  // A size the server did not state is tried while anything is left, and
  // its build finds out.
  count = 0;
  tables[count++] = table(CONTRIBUTE_TABLE_WMP, "CSW24", "CSW24", -1, true);
  assert_chosen(tables, count, 1, 0, NULL, "0");
  assert_chosen(tables, count, 0, 0, NULL, "");
  assert_chosen(tables, count, 1, 2, NULL, "");

  // A table the claim pins no digest for is never skipped: the task is
  // declined for it, as it always was.
  tables[0].unpinned = true;
  assert_chosen(tables, count, 0, 2000, NULL, "0");
}

// Memory is the second budget, on the same terms: a table that cannot be
// built or held in the memory available is skipped like one with no disk,
// in the same priority order, never a rack info table without its wordmap.
// Building needs memory only for a table not on disk; holding, only for one
// this process has not loaded already.
static void test_the_chooser_weighs_memory_too(void) {
  ContributeTable tables[8];
  int count = csw24_player(tables);
  const int64_t wmp_build =
      contribute_table_build_memory(CONTRIBUTE_TABLE_WMP, (int64_t)WMP_MB * MB);
  const int64_t rit_build =
      contribute_table_build_memory(CONTRIBUTE_TABLE_RIT, (int64_t)RIT_MB * MB);
  // About the figures measured on CSW24's builds.
  assert(wmp_build > 700 * MB && wmp_build < 720 * MB);
  assert(rit_build > 2390 * MB && rit_build < 2410 * MB);
  assert(contribute_table_build_memory(CONTRIBUTE_TABLE_WIT, -1) > 300 * MB);
  assert(contribute_table_hold_memory(CONTRIBUTE_TABLE_RIT, -1) ==
         CONTRIBUTE_RIT_ASSUMED_BYTES);

  // Plenty of both.
  assert_chosen_in(tables, count, 10000, 0, 10000, NULL, "012");
  // Memory only: disk for all three, but not the memory to build the rack
  // info table.
  assert_chosen_in(tables, count, 10000, 0, 2300, NULL, "01");
  assert(tables[2].skip == CONTRIBUTE_TABLE_NO_MEMORY_TO_BUILD);
  assert(tables[2].needed_bytes == rit_build);
  assert(tables[2].available_bytes == 2300LL * MB);
  // Not the memory to build the wordmap: nothing that needs it either.
  assert_chosen_in(tables, count, 10000, 0, 700, NULL, "");
  assert(tables[0].skip == CONTRIBUTE_TABLE_NO_MEMORY_TO_BUILD);
  assert(tables[1].skip == CONTRIBUTE_TABLE_NO_WORDMAP);
  assert(tables[2].skip == CONTRIBUTE_TABLE_NO_WORDMAP);
  // Disk only: memory to spare, disk for the wordmap alone.
  assert_chosen_in(tables, count, 200, 0, 10000, NULL, "0");
  assert(tables[1].skip == CONTRIBUTE_TABLE_NO_DISK);
  assert(tables[2].skip == CONTRIBUTE_TABLE_NO_DISK);
  // Both together, each table its own reason: the rack info table on disk
  // but without the memory to hold it beside the wordmap, and no disk for
  // the word info table.
  tables[2].present = true;
  assert_chosen_in(tables, count, WMP_MB + 50, 0, 1900, NULL, "0");
  assert(tables[1].skip == CONTRIBUTE_TABLE_NO_DISK);
  assert(tables[2].skip == CONTRIBUTE_TABLE_NO_MEMORY_TO_HOLD);
  assert(tables[2].needed_bytes == (int64_t)RIT_MB * MB);
  assert(tables[2].available_bytes == (1900 - WMP_MB) * MB);
  tables[2].present = false;

  // On disk, nothing to build: only holding counts.
  for (int table_idx = 0; table_idx < count; table_idx++) {
    tables[table_idx].present = true;
  }
  assert_chosen_in(tables, count, 0, 0, WMP_MB + WIT_MB + RIT_MB, NULL, "012");
  assert_chosen_in(tables, count, 0, 0, WMP_MB + WIT_MB + RIT_MB - 1, NULL,
                   "01");
  // Loaded by this process already, it costs nothing more to hold.
  tables[2].loaded = true;
  assert_chosen_in(tables, count, 0, 0, WMP_MB + WIT_MB, NULL, "012");
  // ...but it is never used without its wordmap, which needs memory to hold.
  assert_chosen_in(tables, count, 0, 0, 0, NULL, "");
  assert(tables[0].skip == CONTRIBUTE_TABLE_NO_MEMORY_TO_HOLD);
  assert(tables[2].skip == CONTRIBUTE_TABLE_NO_WORDMAP);

  // A wordmap built only for a rack info table is built, not held: the
  // table's build counts the larger of the two peaks, and its holding only
  // its own size.
  count = 0;
  tables[count++] =
      table(CONTRIBUTE_TABLE_RIT, "CSW24.CSW24", "CSW24", RIT_MB, true);
  tables[count++] = table(CONTRIBUTE_TABLE_WMP, "CSW24", "CSW24", WMP_MB,
                          /*requested=*/false);
  assert_chosen_in(tables, count, 10000, 0, 2401, NULL, "10");
  assert_chosen_in(tables, count, 10000, 0, 2300, NULL, "");
  assert(tables[0].skip == CONTRIBUTE_TABLE_NO_MEMORY_TO_BUILD);
  assert(!tables[1].chosen);
  // The table on disk and its wordmap not: the wordmap's build is all.
  tables[0].present = true;
  assert_chosen_in(tables, count, 10000, 0, RIT_MB, NULL, "10");
  assert_chosen_in(tables, count, 10000, 0, RIT_MB - 1, NULL, "");
  assert(tables[0].skip == CONTRIBUTE_TABLE_NO_MEMORY_TO_HOLD);
  assert_chosen_in(tables, count, 10000, 0, 700, NULL, "");
  assert(tables[0].skip == CONTRIBUTE_TABLE_NO_MEMORY_TO_BUILD);

  // A memreserve comes off the top, like a diskreserve.
  count = csw24_player(tables);
  int order[8];
  const ContributeTableBudget budget = {
      .free_disk_bytes = INT64_MAX,
      .disk_reserve_bytes = 0,
      .free_memory_bytes = 3000LL * MB,
      .memory_reserve_bytes = 1000LL * MB,
  };
  assert(contribute_choose_tables(tables, count, &budget, NULL, order) == 2);
  assert(tables[2].skip == CONTRIBUTE_TABLE_NO_MEMORY_TO_BUILD);
  assert(tables[2].available_bytes == 2000LL * MB);
}

// Tables built for other jobs count as free space, since the build that
// needs it deletes them; the task's own do not.
static void test_tables_built_for_other_jobs_count_as_free(void) {
  ContributeTableManifest *manifest =
      contribute_table_manifest_read("contribute_tables_test_absent.txt");
  assert(manifest->count == 0);
  contribute_table_manifest_record(manifest, "rit", "NWL23.NWL23",
                                   (int64_t)RIT_MB * MB,
                                   "data/lexica/NWL23.NWL23.rit", 100);
  contribute_table_manifest_record(manifest, "wmp", "CSW24",
                                   (int64_t)WMP_MB * MB,
                                   "data/lexica/CSW24.wmp", 200);
  ContributeTable tables[8];
  const int count = csw24_player(tables);
  // The CSW24 wordmap is this task's, so only the other job's table counts.
  assert(contribute_table_manifest_evictable_bytes(manifest, tables, count) ==
         (int64_t)RIT_MB * MB);
  assert_chosen(tables, count, WMP_MB + WIT_MB, 0, manifest, "012");
  assert_chosen(tables, count, WMP_MB + WIT_MB - 1, 0, manifest, "01");
  contribute_table_manifest_destroy(manifest);
}

// The manifest is what this contributor built, with each table's last use,
// and nothing else: a line that is not one is left out.
static void test_the_manifest_round_trips(void) {
  const char *path = "contribute_tables_test_manifest.txt";
  ContributeTableManifest *manifest = contribute_table_manifest_read(path);
  contribute_table_manifest_record(manifest, "wmp", "CSW24", 178929167,
                                   "data/lexica/CSW24.wmp", 300);
  contribute_table_manifest_record(manifest, "rit", "CSW24.CSW24", 1885416048,
                                   "my data/lexica/CSW24.CSW24.rit", 100);
  contribute_table_manifest_record(manifest, "wit", "CSW24", 121972344,
                                   "data/lexica/CSW24.wit", 200);
  // Recording again updates in place.
  contribute_table_manifest_record(manifest, "wmp", "CSW24", 178929168,
                                   "data/lexica/CSW24.wmp", 400);
  assert(manifest->count == 3);
  assert(contribute_table_manifest_write(manifest, path));
  contribute_table_manifest_destroy(manifest);

  // Malformed lines appended by hand are ignored.
  FILE *stream = fopen(path, "ae");
  assert(stream);
  (void)fputs("wmp notanumber 5 X data/X.wmp\nwmp 5\n\nrit -1 5 Y y.rit\n",
              stream);
  (void)fclose(stream);

  manifest = contribute_table_manifest_read(path);
  assert(manifest->count == 3);
  assert_strings_equal(manifest->entries[0].role, "wmp");
  assert(manifest->entries[0].bytes == 178929168);
  assert(manifest->entries[0].last_used == 400);
  // A path keeps its spaces.
  assert_strings_equal(manifest->entries[1].path,
                       "my data/lexica/CSW24.CSW24.rit");

  // Least recently used first, the task's own tables left out.
  int order[3];
  ContributeTable keep = table(CONTRIBUTE_TABLE_WIT, "CSW24", "CSW24", 1, true);
  assert(contribute_table_manifest_evictable(manifest, &keep, 1, order) == 2);
  assert(order[0] == 1 && order[1] == 0);
  assert(contribute_table_manifest_evictable(manifest, NULL, 0, order) == 3);
  assert(order[0] == 1 && order[1] == 2 && order[2] == 0);

  assert(contribute_table_manifest_touch(manifest, "rit", "CSW24.CSW24", 500));
  assert(!contribute_table_manifest_touch(manifest, "rit", "NWL23", 500));
  contribute_table_manifest_forget(manifest, "wmp", "CSW24");
  assert(manifest->count == 2);
  assert(contribute_table_manifest_evictable(manifest, NULL, 0, order) == 2);
  assert_strings_equal(manifest->entries[order[0]].role, "wit");
  contribute_table_manifest_destroy(manifest);
  (void)remove(path);
}

// A filesystem whose free space grows by what is deleted from it: `base`
// bytes, plus the size of each listed file that has gone.
typedef struct FakeDisk {
  int64_t base;
  const char *paths[4];
  int64_t sizes[4];
  int count;
} FakeDisk;

static bool fake_free_bytes(const char *directory, void *context,
                            int64_t *free_bytes) {
  (void)directory;
  const FakeDisk *disk = (const FakeDisk *)context;
  *free_bytes = disk->base;
  for (int path_idx = 0; path_idx < disk->count; path_idx++) {
    struct stat info;
    if (stat(disk->paths[path_idx], &info) != 0) {
      *free_bytes += disk->sizes[path_idx];
    }
  }
  return true;
}

static void write_bytes(const char *path, int64_t size) {
  FILE *stream = fopen(path, "we");
  assert(stream);
  for (int64_t byte_idx = 0; byte_idx < size; byte_idx++) {
    (void)fputc('x', stream);
  }
  (void)fclose(stream);
}

static bool file_exists(const char *path) {
  struct stat info;
  return stat(path, &info) == 0;
}

// Room is made by deleting the tables this contributor built for other jobs,
// least recently used first, and only as many as it takes. A file the
// manifest does not list is never deleted, nor one whose size is no longer
// what was built, nor the task's own.
static void test_room_is_made_from_the_manifest_alone(void) {
  (void)mkdir(TABLES_TEST_DIR, 0777);
  const char *old_path = TABLES_TEST_DIR "/OLD.rit";
  const char *newer_path = TABLES_TEST_DIR "/NEWER.wmp";
  const char *changed_path = TABLES_TEST_DIR "/CHANGED.wit";
  const char *own_path = TABLES_TEST_DIR "/OWN.wmp";
  const char *foreign_path = TABLES_TEST_DIR "/FOREIGN.wmp";
  write_bytes(old_path, 300);
  write_bytes(newer_path, 200);
  write_bytes(changed_path, 150);
  write_bytes(own_path, 100);
  write_bytes(foreign_path, 1000);

  ContributeTableManifest *manifest =
      contribute_table_manifest_read(TABLES_TEST_MANIFEST);
  for (int entry_idx = manifest->count - 1; entry_idx >= 0; entry_idx--) {
    contribute_table_manifest_forget(manifest, manifest->entries[0].role,
                                     manifest->entries[0].name);
  }
  contribute_table_manifest_record(manifest, "rit", "OLD", 300, old_path, 10);
  contribute_table_manifest_record(manifest, "wmp", "NEWER", 200, newer_path,
                                   30);
  // Built at 120 bytes; something has written it since.
  contribute_table_manifest_record(manifest, "wit", "CHANGED", 120,
                                   changed_path, 5);
  contribute_table_manifest_record(manifest, "wmp", "OWN", 100, own_path, 1);
  assert(contribute_table_manifest_write(manifest, TABLES_TEST_MANIFEST));
  contribute_table_manifest_destroy(manifest);

  FakeDisk disk = {.base = 1000,
                   .paths = {old_path, newer_path, changed_path, own_path},
                   .sizes = {300, 200, 150, 100},
                   .count = 4};
  ContributeTable keep = table(CONTRIBUTE_TABLE_WMP, "OWN", "OWN", 1, true);
  StringList *evicted = string_list_create();
  int64_t available = 0;

  // Fits already: nothing is deleted.
  assert(contribute_tables_make_room(TABLES_TEST_MANIFEST, TABLES_TEST_DIR, 800,
                                     200, &keep, 1, fake_free_bytes, &disk,
                                     &available, evicted));
  assert(available == 800 && string_list_get_count(evicted) == 0);

  // 250 more than there is: the least recently used table goes, and that is
  // enough.
  assert(contribute_tables_make_room(TABLES_TEST_MANIFEST, TABLES_TEST_DIR,
                                     1050, 200, &keep, 1, fake_free_bytes,
                                     &disk, &available, evicted));
  assert(available == 1100);
  assert(string_list_get_count(evicted) == 1);
  assert_strings_equal(string_list_get_string(evicted, 0), "rit OLD");
  assert(!file_exists(old_path) && file_exists(newer_path));

  // More than everything deletable: the next goes, the changed file and the
  // task's own stay, and it still does not fit.
  assert(!contribute_tables_make_room(TABLES_TEST_MANIFEST, TABLES_TEST_DIR,
                                      5000, 200, &keep, 1, fake_free_bytes,
                                      &disk, &available, evicted));
  assert(available == 1300);
  assert(string_list_get_count(evicted) == 2);
  assert(!file_exists(newer_path));
  assert(file_exists(changed_path) && file_exists(own_path) &&
         file_exists(foreign_path));

  // What was deleted is gone from the manifest; what was not is still there.
  manifest = contribute_table_manifest_read(TABLES_TEST_MANIFEST);
  assert(manifest->count == 2);
  assert(contribute_table_manifest_touch(manifest, "wit", "CHANGED", 1));
  assert(contribute_table_manifest_touch(manifest, "wmp", "OWN", 1));
  contribute_table_manifest_destroy(manifest);

  // An unknown size is left to its build.
  assert(contribute_tables_make_room(TABLES_TEST_MANIFEST, TABLES_TEST_DIR, -1,
                                     1000000, &keep, 1, fake_free_bytes, &disk,
                                     &available, NULL));

  string_list_destroy(evicted);
  (void)remove(changed_path);
  (void)remove(own_path);
  (void)remove(foreign_path);
  (void)remove(TABLES_TEST_MANIFEST);
  char *lock = get_formatted_string("%s.lock", TABLES_TEST_MANIFEST);
  (void)remove(lock);
  free(lock);
  const char *locks[] = {TABLES_TEST_DIR "/OLD.rit.lock",
                         TABLES_TEST_DIR "/NEWER.wmp.lock",
                         TABLES_TEST_DIR "/CHANGED.wit.lock"};
  for (size_t lock_idx = 0; lock_idx < sizeof(locks) / sizeof(locks[0]);
       lock_idx++) {
    (void)remove(locks[lock_idx]);
  }
}

// `diskreserve` and `memreserve` are numbers of GB, fractions allowed, 2 and
// 1 when unstated; a value that is not one is refused, naming the line and
// never quoting it.
static void test_the_reserve_settings(void) {
  const char *path = "contribute_tables_test_settings.txt";
  ErrorStack *error_stack = error_stack_create();
  const struct {
    const char *contents;
    int64_t bytes;
  } accepted[] = {
      {"server https://birdtest.example\n", 2000000000LL},
      {"diskreserve 0\n", 0},
      {"diskreserve 0.5\n", 500000000LL},
      {"diskreserve 10\n", 10000000000LL},
      {"diskreserve   3.25  \n", 3250000000LL},
      {"diskreserve 1000000\n", 1000000LL * 1000000000LL},
      {"memreserve 0.25\ndiskreserve 3\n", 3000000000LL},
  };
  for (size_t case_idx = 0; case_idx < sizeof(accepted) / sizeof(accepted[0]);
       case_idx++) {
    FILE *stream = fopen(path, "we");
    assert(stream);
    (void)fputs(accepted[case_idx].contents, stream);
    (void)fclose(stream);
    ClientState *state = client_state_load(path, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_print_and_reset(error_stack);
      log_fatal("refused '%s'", accepted[case_idx].contents);
    }
    assert(state->disk_reserve_bytes == accepted[case_idx].bytes);
    client_state_destroy(state);
  }
  const struct {
    const char *contents;
    int64_t bytes;
  } memory[] = {
      {"server https://birdtest.example\n", 1000000000LL},
      {"memreserve 0\n", 0},
      {"memreserve 0.25\n", 250000000LL},
      {"diskreserve 9\nmemreserve 4\n", 4000000000LL},
  };
  for (size_t case_idx = 0; case_idx < sizeof(memory) / sizeof(memory[0]);
       case_idx++) {
    FILE *stream = fopen(path, "we");
    assert(stream);
    (void)fputs(memory[case_idx].contents, stream);
    (void)fclose(stream);
    ClientState *state = client_state_load(path, error_stack);
    assert(error_stack_is_empty(error_stack));
    assert(state->memory_reserve_bytes == memory[case_idx].bytes);
    client_state_destroy(state);
  }
  const char *refused[] = {
      "diskreserve -1\n",  "diskreserve abc\n",     "diskreserve 1e3\n",
      "diskreserve inf\n", "diskreserve 1.2.3\n",   "diskreserve\n",
      "diskreserve .\n",   "diskreserve 1000001\n", "diskreserve 0x10\n",
      "memreserve -1\n",   "memreserve abc\n",      "memreserve 1e3\n",
  };
  for (size_t case_idx = 0; case_idx < sizeof(refused) / sizeof(refused[0]);
       case_idx++) {
    FILE *stream = fopen(path, "we");
    assert(stream);
    (void)fputs("server https://birdtest.example\n", stream);
    (void)fputs(refused[case_idx], stream);
    (void)fclose(stream);
    assert(!client_state_load(path, error_stack));
    char *message = error_stack_get_string_and_reset(error_stack);
    assert(strstr(message, "line 2"));
    assert(strstr(message, strncmp(refused[case_idx], "mem", 3) == 0
                               ? "memreserve"
                               : "diskreserve"));
    assert(!strstr(message, "abc") && !strstr(message, "1e3"));
    free(message);
  }
  // A setting's name in the wrong case is named, with why.
  FILE *stream = fopen(path, "we");
  assert(stream);
  (void)fputs("DiskReserve 2\n", stream);
  (void)fclose(stream);
  assert(!client_state_load(path, error_stack));
  char *message = error_stack_get_string_and_reset(error_stack);
  assert(strstr(message, "settings are lowercase"));
  assert(strstr(message, "diskreserve"));
  free(message);
  error_stack_destroy(error_stack);
  (void)remove(path);
}

// ---------------------------------------------------------------------------
// A task's tables, end to end
// ---------------------------------------------------------------------------

// A static player on the test lexicon, asking for every table or none.
static char *tables_test_player(bool use_tables) {
  return get_formatted_string(
      "{\"lexicon\": \"" TABLES_TEST_LEXICON "\", \"leaves\": "
      "\"" TABLES_TEST_LEXICON "\", "
      "\"recorder_type\": \"best\", \"sort_strategy\": \"equity\", "
      "\"num_plies\": 0, \"num_plays\": 100, \"num_plies_recorded\": 2, "
      "\"num_plays_recorded\": 10, \"movegens_per_move\": null, "
      "\"movegen_margin\": 5.0, \"use_wordmap\": %s, \"use_rit\": %s, "
      "\"rit_name\": %s, \"use_wit\": %s, \"endgame_plies\": 0, "
      "\"peg_max_bag\": 0}",
      use_tables ? "true" : "false", use_tables ? "true" : "false",
      use_tables ? "\"" TABLES_TEST_RIT "\"" : "null",
      use_tables ? "true" : "false");
}

static char *tables_test_request(bool use_tables) {
  char *player = tables_test_player(use_tables);
  char *request = get_formatted_string(
      "{\"job_type\": \"games\", \"variant\": \"classic\", "
      "\"letter_distribution\": \"english_ab\", "
      "\"board_layout\": \"standard15\", \"seed\": \"5\", "
      "\"num_games\": 4, \"capture_positions\": true, "
      "\"capture_first_divergence\": false, \"bingo_bonus\": 50, "
      "\"sim_cutoff\": 0.005, \"player1\": %s, \"player2\": %s}",
      player, player);
  free(player);
  return request;
}

static char *tables_test_path(data_filepath_t type, const char *name) {
  ErrorStack *error_stack = error_stack_create();
  char *path = data_filepaths_get_writable_filename(
      TABLES_TEST_DIR ":./testdata:./data", name, type, error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  return path;
}

static void remove_tables_test_tables(void) {
  const data_filepath_t types[] = {DATA_FILEPATH_TYPE_WORDMAP,
                                   DATA_FILEPATH_TYPE_WORD_INFO_TABLE,
                                   DATA_FILEPATH_TYPE_RACK_INFO_TABLE};
  const char *names[] = {TABLES_TEST_LEXICON, TABLES_TEST_LEXICON,
                         TABLES_TEST_RIT};
  for (int type_idx = 0; type_idx < 3; type_idx++) {
    char *path = tables_test_path(types[type_idx], names[type_idx]);
    (void)remove(path);
    free(path);
  }
}

// Whether the test directory's lexica hold anything left from a write that
// did not finish.
static bool tables_test_has_temporaries(void) {
  ErrorStack *error_stack = error_stack_create();
  int num_files = 0;
  char **files = get_files_in_directory(TABLES_TEST_DIR "/lexica", ".tmp",
                                        &num_files, error_stack);
  assert(error_stack_is_empty(error_stack));
  for (int file_idx = 0; file_idx < num_files; file_idx++) {
    free(files[file_idx]);
  }
  free(files);
  error_stack_destroy(error_stack);
  return num_files > 0;
}

#ifndef __EMSCRIPTEN__
// Caps the size of any file this process writes at `bytes`, as a full disk
// would stop it, with writes past it failing (EFBIG) rather than killing the
// process. Returns false where it cannot be done.
static bool limit_file_size(rlim_t bytes, struct rlimit *previous,
                            void (**previous_handler)(int)) {
  if (getrlimit(RLIMIT_FSIZE, previous) != 0) {
    return false;
  }
  struct rlimit limited = *previous;
  limited.rlim_cur = bytes;
  *previous_handler = signal(SIGXFSZ, SIG_IGN);
  return setrlimit(RLIMIT_FSIZE, &limited) == 0;
}

static void unlimit_file_size(const struct rlimit *previous,
                              void (*previous_handler)(int)) {
  assert(setrlimit(RLIMIT_FSIZE, previous) == 0);
  (void)signal(SIGXFSZ, previous_handler);
}
#endif

// Runs the games task with the tables asked for or not, on a contributor
// whose contribute.txt is `settings`, returning its result, and setting
// *movegens and *skipped (the roles the result would name). With
// `file_size_limit` above 0, no file the task writes may grow past that many
// bytes, as on a full disk.
static char *run_tables_task(const char *assignment_json, bool use_tables,
                             const char *settings, long file_size_limit,
                             uint64_t *movegens, unsigned int *skipped) {
  const char *settings_path = "contribute_tables_test_contribute.txt";
  FILE *stream = fopen(settings_path, "we");
  assert(stream);
  (void)fputs(settings, stream);
  (void)fclose(stream);

  ErrorStack *error_stack = error_stack_create();
  ThreadControl *thread_control = thread_control_create();
  ContributeState *state =
      contribute_state_create(settings_path, thread_control, error_stack);
  assert(error_stack_is_empty(error_stack));
  contribute_state_set_assignment(state,
                                  json_parse(assignment_json, error_stack));
  assert(error_stack_is_empty(error_stack));

  Config *config = config_create_default_test();
  load_and_exec_config_or_die(config, "set -path " TABLES_TEST_DIR
                                      ":./testdata:./data");
  Config *task_config = config_create_for_contribute(config, error_stack);
  assert(error_stack_is_empty(error_stack));
  thread_control_set_status(config_get_thread_control(task_config),
                            THREAD_CONTROL_STATUS_STARTED);
  char *request_json = tables_test_request(use_tables);
  const JsonValue *request = json_parse(request_json, error_stack);
  free(request_json);
  assert(error_stack_is_empty(error_stack));
  char *result_json = NULL;
#ifndef __EMSCRIPTEN__
  struct rlimit previous;
  void (*previous_handler)(int) = SIG_DFL;
  // The limit would stop this process's own output too, where it goes to a
  // file: it goes nowhere while the limit holds.
  FILE *nowhere = NULL;
  if (file_size_limit > 0) {
    nowhere = fopen("/dev/null", "we");
    assert(nowhere);
    io_set_stream_out(nowhere);
    io_set_stream_err(nowhere);
    assert(
        limit_file_size((rlim_t)file_size_limit, &previous, &previous_handler));
  }
#else
  (void)file_size_limit;
#endif
  // One thread, so the captured positions come in one order every run.
  const bool ran =
      config_contribute_execute(task_config, "games", request, 1, state,
                                &result_json, movegens, error_stack);
#ifndef __EMSCRIPTEN__
  if (file_size_limit > 0) {
    unlimit_file_size(&previous, previous_handler);
    io_reset_stream_out();
    io_reset_stream_err();
    (void)fclose(nowhere);
  }
#endif
  assert(ran);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    assert(false);
  }
  assert(result_json);
  *skipped = contribute_get_derived_skipped(state);
  json_destroy(request);
  config_destroy_for_contribute(task_config);
  config_destroy(config);
  contribute_state_destroy(state);
  thread_control_destroy(thread_control);
  error_stack_destroy(error_stack);
  (void)remove(settings_path);
  return result_json;
}

// The assignment a claim would carry for the test lexicon's three tables,
// each pinned to the digest of a build of it made here, with its size.
static char *tables_test_assignment(void) {
  Config *config = config_create_default_test();
  load_and_exec_config_or_die(config, "set -path " TABLES_TEST_DIR
                                      ":./testdata:./data");
  ErrorStack *error_stack = error_stack_create();
  const struct {
    const char *conversion;
    const char *name;
    const char *klv;
    const char *wmp;
  } builds[] = {
      {"dawg2wordmap", TABLES_TEST_LEXICON, NULL, NULL},
      {"kwg2wit", TABLES_TEST_LEXICON, NULL, NULL},
      {"klvwmp2rit", TABLES_TEST_RIT, TABLES_TEST_LEXICON, TABLES_TEST_LEXICON},
  };
  const data_filepath_t types[] = {DATA_FILEPATH_TYPE_WORDMAP,
                                   DATA_FILEPATH_TYPE_WORD_INFO_TABLE,
                                   DATA_FILEPATH_TYPE_RACK_INFO_TABLE};
  const char *roles[] = {"wmp", "wit", "rit"};
  StringBuilder *sb = string_builder_create();
  string_builder_add_string(sb, "{\"expected_data\": {\"algorithm\": "
                                "\"sha256\", \"files\": [], \"derived\": [");
  for (int build_idx = 0; build_idx < 3; build_idx++) {
    ConversionArgs args = {
        .conversion_type_string = builds[build_idx].conversion,
        .data_paths = config_get_data_paths(config),
        .input_and_output_name = builds[build_idx].name,
        .ld_name = "english_ab",
        .klv_name = builds[build_idx].klv,
        .wmp_name = builds[build_idx].wmp,
        .num_threads = 1,
    };
    ConversionResults *results = conversion_results_create();
    convert(&args, results, error_stack);
    conversion_results_destroy(results);
    assert(error_stack_is_empty(error_stack));
    char *path = tables_test_path(types[build_idx], builds[build_idx].name);
    char *digest = sha256_hash_file(path, error_stack);
    assert(error_stack_is_empty(error_stack));
    struct stat info;
    assert(stat(path, &info) == 0);
    string_builder_add_formatted_string(
        sb,
        "%s{\"role\": \"%s\", \"name\": \"%s\", \"sha256\": \"%s\", "
        "\"builder\": \"test\", \"build_target\": \"test\", \"bytes\": %lld}",
        build_idx ? ", " : "", roles[build_idx], builds[build_idx].name, digest,
        (long long)info.st_size);
    free(digest);
    free(path);
  }
  string_builder_add_string(sb, "]}}");
  error_stack_destroy(error_stack);
  config_destroy(config);
  remove_tables_test_tables();
  return string_builder_dump_and_destroy(sb, NULL);
}

// Copies a file byte for byte.
static void copy_file(const char *from, const char *to) {
  FILE *in = fopen(from, "rbe");
  assert(in);
  FILE *out = fopen(to, "wbe");
  assert(out);
  int byte = 0;
  while ((byte = fgetc(in)) != EOF) {
    (void)fputc(byte, out);
  }
  (void)fclose(in);
  (void)fclose(out);
}

// A table is never required. A task whose players ask for all three runs the
// same games, with the same move generations, whether this machine has room
// for them or not; skipped, the result names them. A disk that fills while a
// table is written -- the rack info table's writer used to end the process
// here -- skips the table and leaves nothing half-written behind.
static void test_a_task_runs_without_the_tables_it_has_no_room_for(void) {
  (void)mkdir(TABLES_TEST_DIR, 0777);
  (void)mkdir(TABLES_TEST_DIR "/lexica", 0777);
  copy_file("data/lexica/CSW21_ab.kwg",
            TABLES_TEST_DIR "/lexica/" TABLES_TEST_LEXICON ".kwg");
  copy_file("data/lexica/CSW21_ab.klv2",
            TABLES_TEST_DIR "/lexica/" TABLES_TEST_LEXICON ".klv2");
  (void)remove(TABLES_TEST_MANIFEST);
  char *assignment = tables_test_assignment();
  const unsigned int all_three = (1U << CONTRIBUTE_TABLE_WMP) |
                                 (1U << CONTRIBUTE_TABLE_WIT) |
                                 (1U << CONTRIBUTE_TABLE_RIT);
  char *wmp_path =
      tables_test_path(DATA_FILEPATH_TYPE_WORDMAP, TABLES_TEST_LEXICON);
  char *wit_path =
      tables_test_path(DATA_FILEPATH_TYPE_WORD_INFO_TABLE, TABLES_TEST_LEXICON);
  char *rit_path =
      tables_test_path(DATA_FILEPATH_TYPE_RACK_INFO_TABLE, TABLES_TEST_RIT);

  // The games without tables, which every other run must match.
  uint64_t plain_movegens = 0;
  unsigned int skipped = 99;
  char *plain = run_tables_task(assignment, false, "diskreserve 2\n", 0,
                                &plain_movegens, &skipped);
  assert(skipped == 0 && plain_movegens > 0);

  // Room for all three: built, used, recorded in the manifest.
  uint64_t movegens = 0;
  char *result = run_tables_task(assignment, true, "diskreserve 0\n", 0,
                                 &movegens, &skipped);
  assert(skipped == 0);
  assert_strings_equal(result, plain);
  assert(movegens == plain_movegens);
  free(result);
  assert(file_exists(wmp_path) && file_exists(wit_path) &&
         file_exists(rit_path));
  ContributeTableManifest *manifest =
      contribute_table_manifest_read(TABLES_TEST_MANIFEST);
  assert(manifest->count == 3);
  assert(
      contribute_table_manifest_touch(manifest, "wmp", TABLES_TEST_LEXICON, 0));
  assert(contribute_table_manifest_touch(manifest, "rit", TABLES_TEST_RIT, 0));
  contribute_table_manifest_destroy(manifest);
  // On disk, they are used again whatever the reserve: they take no room.
  result = run_tables_task(assignment, true, "diskreserve 1000000\n", 0,
                           &movegens, &skipped);
  assert(skipped == 0);
  assert_strings_equal(result, plain);
  free(result);

  // No room for any: the same games, all three named, nothing built.
  remove_tables_test_tables();
  result = run_tables_task(assignment, true, "diskreserve 1000000\n", 0,
                           &movegens, &skipped);
  assert(skipped == all_three);
  assert_strings_equal(result, plain);
  assert(movegens == plain_movegens);
  free(result);
  assert(!file_exists(wmp_path) && !file_exists(wit_path) &&
         !file_exists(rit_path));

  // Disk to spare and no memory: the same again.
  result =
      run_tables_task(assignment, true, "diskreserve 0\nmemreserve 1000000\n",
                      0, &movegens, &skipped);
  assert(skipped == all_three);
  assert_strings_equal(result, plain);
  assert(movegens == plain_movegens);
  free(result);
  assert(!file_exists(wmp_path) && !file_exists(wit_path) &&
         !file_exists(rit_path));

#ifndef __EMSCRIPTEN__
  // The disk fills during the builds: the same games again, every table
  // skipped, no partial file left.
  result = run_tables_task(assignment, true, "diskreserve 0\n", 1, &movegens,
                           &skipped);
  assert(skipped == all_three);
  assert_strings_equal(result, plain);
  assert(movegens == plain_movegens);
  free(result);
  assert(!file_exists(wmp_path) && !file_exists(wit_path) &&
         !file_exists(rit_path));
  assert(!tables_test_has_temporaries());

  // The rack info table alone fills it: the wordmap and word info table are
  // on disk, and the table's write stops part-way, where it used to end the
  // process. Only it is skipped.
  free(run_tables_task(assignment, true, "diskreserve 0\n", 0, &movegens,
                       &skipped));
  assert(skipped == 0);
  (void)remove(rit_path);
  result = run_tables_task(assignment, true, "diskreserve 0\n", 1024, &movegens,
                           &skipped);
  assert(skipped == (1U << CONTRIBUTE_TABLE_RIT));
  assert_strings_equal(result, plain);
  free(result);
  assert(!file_exists(rit_path) && file_exists(wmp_path));
  assert(!tables_test_has_temporaries());
#endif

  free(plain);
  free(assignment);
  remove_tables_test_tables();
  (void)remove(TABLES_TEST_DIR "/lexica/" TABLES_TEST_LEXICON ".kwg");
  (void)remove(TABLES_TEST_DIR "/lexica/" TABLES_TEST_LEXICON ".klv2");
  const char *leftovers[] = {
      TABLES_TEST_MANIFEST,
      TABLES_TEST_MANIFEST ".lock",
      TABLES_TEST_DIR "/lexica/" TABLES_TEST_LEXICON ".wmp.lock",
      TABLES_TEST_DIR "/lexica/" TABLES_TEST_LEXICON ".wit.lock",
      TABLES_TEST_DIR "/lexica/" TABLES_TEST_RIT ".rit.lock",
  };
  for (size_t leftover_idx = 0;
       leftover_idx < sizeof(leftovers) / sizeof(leftovers[0]);
       leftover_idx++) {
    (void)remove(leftovers[leftover_idx]);
  }
  (void)rmdir(TABLES_TEST_DIR "/lexica");
  (void)rmdir(TABLES_TEST_DIR);
  free(wmp_path);
  free(wit_path);
  free(rit_path);
}

void test_contribute_tables(void) {
  test_the_chooser_keeps_the_tables_worth_most();
  test_the_chooser_weighs_memory_too();
  test_tables_built_for_other_jobs_count_as_free();
  test_the_manifest_round_trips();
  test_room_is_made_from_the_manifest_alone();
  test_the_reserve_settings();
  test_a_task_runs_without_the_tables_it_has_no_room_for();
}
