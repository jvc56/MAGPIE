#ifndef CONTRIBUTE_TABLES_H
#define CONTRIBUTE_TABLES_H

// Which of the derived tables a task's players use -- wordmaps, word info
// tables, rack info tables -- a contributor builds and keeps, given its disk.
//
// None of them is ever required. Each is an accelerator: the moves a static
// player plays and the move generations a task counts are the same with or
// without it. So a player's use_wordmap, use_wit and use_rit mean "use it if
// this contributor can": a table that does not fit in the disk space the
// contributor has beyond its `diskreserve`, or in the memory it has beyond
// its `memreserve` -- to build it, or to hold it while playing -- is skipped
// and the task runs without it, slower. A contributor never declines or fails a
// task for lack of space. A table that is built and does not match the hash the
// claim pins is a different matter, and is declined as before
// (derived_mismatch).
//
// The space a task may use is the free space on the filesystem holding the
// data directory, less the reserve, plus the tables this contributor built
// for other jobs, which it deletes -- least recently used first -- when a
// table a task needs does not fit otherwise. It deletes only what its own
// manifest, CONTRIBUTE_TABLE_MANIFEST_FILENAME in the data directory, says it
// built.

#include "../util/string_util.h"
#include <stdbool.h>
#include <stdint.h>

// In the order of their value per byte, which is the order a contributor
// keeps them in when it cannot keep them all (scripts/bench_tables.sh; CSW24,
// four threads, movegens/s against none):
//
//   static player: none 17.2k, WIT alone 18.2k, WMP 65.1k, WMP+WIT 63.0k,
//                  WMP+RIT 69.2k, all three 69.7k
//   2-ply simmer:  none 15.9k, WIT alone 15.9k, WMP 47.7k, WMP+WIT 51.9k,
//                  WMP+RIT 55.8k, all three 70.9k
//
// A wordmap (179 MB for CSW24) is 3-4x. A word info table (122 MB) is about
// 9% on a simmer, and nothing without a wordmap. A rack info table (1.9 GB)
// is about 17% on a simmer, and 6% on a static player; beside a word info
// table, 36%.
typedef enum {
  CONTRIBUTE_TABLE_WMP,
  CONTRIBUTE_TABLE_WIT,
  CONTRIBUTE_TABLE_RIT,
  NUMBER_OF_CONTRIBUTE_TABLE_ROLES,
} contribute_table_role_t;

#define CONTRIBUTE_TABLE_MANIFEST_FILENAME "contribute_tables.txt"

// Why contribute_choose_tables left a table out.
typedef enum {
  CONTRIBUTE_TABLE_KEPT,
  // Not enough disk beyond the diskreserve.
  CONTRIBUTE_TABLE_NO_DISK,
  // Not enough memory beyond the memreserve to build it.
  CONTRIBUTE_TABLE_NO_MEMORY_TO_BUILD,
  // Not enough memory beyond the memreserve to hold it while playing, after
  // the tables chosen before it.
  CONTRIBUTE_TABLE_NO_MEMORY_TO_HOLD,
  // The wordmap it goes with was skipped.
  CONTRIBUTE_TABLE_NO_WORDMAP,
} contribute_table_skip_t;

// The memory a table takes, from the size of its file. Building one peaks at
// about 710 MB for CSW24's 179 MB wordmap, 310 MB for its 122 MB word info
// table and 2.4 GB for its 1.9 GB rack info table (measured on the builds
// birdtest's workers run; the RIT figure is the one its build line states),
// so a build's peak is taken as its file's size times these ratios, in
// thousandths. Held while playing, each is loaded or mapped whole: its file's
// size. When the server states no size, CSW24's are assumed -- the largest a
// job uses -- since running out of memory, unlike disk, is not something a
// build finds out gracefully.
#define CONTRIBUTE_WMP_BUILD_PERMILLE 3966
#define CONTRIBUTE_WIT_BUILD_PERMILLE 2541
#define CONTRIBUTE_RIT_BUILD_PERMILLE 1273
#define CONTRIBUTE_WMP_ASSUMED_BYTES 178929167LL
#define CONTRIBUTE_WIT_ASSUMED_BYTES 121972344LL
#define CONTRIBUTE_RIT_ASSUMED_BYTES 1885416064LL

// The memory building a table of `role` and `bytes` (-1 unknown) peaks at,
// and the memory holding it takes.
int64_t contribute_table_build_memory(contribute_table_role_t role,
                                      int64_t bytes);
int64_t contribute_table_hold_memory(contribute_table_role_t role,
                                     int64_t bytes);

// What a task's tables have to fit in. INT64_MAX for a free amount that
// cannot be found: unconstrained.
typedef struct ContributeTableBudget {
  // Free on the filesystem builds are written to.
  int64_t free_disk_bytes;
  int64_t disk_reserve_bytes;
  // Available memory (MemAvailable on Linux).
  int64_t free_memory_bytes;
  int64_t memory_reserve_bytes;
} ContributeTableBudget;

// "wmp", "wit" or "rit": the role birdtest's expected_data.derived names.
const char *contribute_table_role_name(contribute_table_role_t role);
// "wordmap", "word info table" or "rack info table".
const char *contribute_table_role_description(contribute_table_role_t role);

// One table a task's players use, or one a rack info table needs built.
typedef struct ContributeTable {
  contribute_table_role_t role;
  // The name it is built and loaded under: a wordmap's and a word info
  // table's is the lexicon's, a rack info table's the one the server pins.
  const char *name;
  // The lexicon whose wordmap the table goes with: its own name for a wordmap
  // or word info table, its player's lexicon for a rack info table, which is
  // built from that wordmap.
  const char *lexicon;
  // The size of the server's reference build, or -1 when it did not say.
  int64_t bytes;
  // On disk already with the digest the claim pins, so it costs no space,
  // and needs no memory to build.
  bool present;
  // Loaded (or mapped) by this process already, so holding it costs no more
  // memory. A table another process maps is counted in full.
  bool loaded;
  // Some player loads it. False for a wordmap no player asked for, which a
  // rack info table needs built.
  bool requested;
  // The claim pins no digest for it. Never skipped: the task is declined for
  // it as before.
  bool unpinned;
  // Set by contribute_choose_tables: whether to build (if absent) and use it.
  bool chosen;
  // Set by contribute_choose_tables for a table it skipped: why, and for a
  // lack of disk or memory, the bytes it needed (with its wordmap's, for a
  // rack info table) and the bytes there were for it, beyond the reserve.
  contribute_table_skip_t skip;
  int64_t needed_bytes;
  int64_t available_bytes;
} ContributeTable;

// One line of the manifest: a table this contributor built.
typedef struct ContributeTableManifestEntry {
  char *role;
  char *name;
  int64_t bytes;
  // Unix seconds.
  int64_t last_used;
  // Where it was written; the only file eviction ever removes for it.
  char *path;
} ContributeTableManifestEntry;

typedef struct ContributeTableManifest {
  ContributeTableManifestEntry *entries;
  int count;
  int capacity;
} ContributeTableManifest;

// Reads the manifest at `path`; a missing or unreadable file is an empty
// manifest, and a malformed line is left out. Never NULL.
ContributeTableManifest *contribute_table_manifest_read(const char *path);
// Writes it in full, beside `path` and renamed into place. Returns false if
// it could not be written: what was built is then forgotten, and never
// evicted, which is the safe way to lose it.
bool contribute_table_manifest_write(const ContributeTableManifest *manifest,
                                     const char *path);
void contribute_table_manifest_destroy(ContributeTableManifest *manifest);
// Adds the table (role, name), or updates it if it is already listed.
void contribute_table_manifest_record(ContributeTableManifest *manifest,
                                      const char *role, const char *name,
                                      int64_t bytes, const char *path,
                                      int64_t now);
// Moves the listed table (role, name) to `now`. Returns whether it is listed.
bool contribute_table_manifest_touch(ContributeTableManifest *manifest,
                                     const char *role, const char *name,
                                     int64_t now);
// Drops the table (role, name) from the manifest, if it is listed.
void contribute_table_manifest_forget(ContributeTableManifest *manifest,
                                      const char *role, const char *name);
// The listed tables `tables` does not name, as indexes into the manifest,
// least recently used first, in `order` (room for manifest->count); returns
// how many. What eviction may delete for this task.
int contribute_table_manifest_evictable(const ContributeTableManifest *manifest,
                                        const ContributeTable *tables,
                                        int count, int *order);
// Their bytes in total.
int64_t contribute_table_manifest_evictable_bytes(
    const ContributeTableManifest *manifest, const ContributeTable *tables,
    int count);

// Decides which of `tables` (the tables a task's players use, and the
// wordmaps their rack info tables need built) to use, given the `budget` --
// the free disk and memory, and the reserves of each to keep -- and the
// manifest's tables this task does not need, whose bytes count as free disk.
// Sets each table's `chosen` (and `skip` for one left out), and writes the
// chosen ones' indexes to `order` (room for `count`) in the order to build
// them; returns how many.
//
// Tables are taken in priority order -- wordmaps, then word info tables, then
// rack info tables (see contribute_table_role_t) -- each while it fits what
// the earlier ones left, on both counts:
//
// - disk: one on disk already takes none; one of unknown size fits while any
//   space is left, and its build finds out;
// - memory: building one not on disk must fit in the memory available (builds
//   run one at a time, before the task loads anything), and holding the ones
//   its players load must fit beside those chosen before it -- nothing for
//   one this process has loaded already. A wordmap built only for a rack info
//   table is built, not held. A rack info table is
// never chosen without the wordmap for its lexicon, whose bytes it counts if
// that wordmap is not chosen yet; a word info table is not chosen when the
// wordmap for its lexicon was asked for and skipped, since it does nothing
// without one. A table the claim pins no digest for is always chosen, so the
// task is declined for it as before.
int contribute_choose_tables(ContributeTable *tables, int count,
                             const ContributeTableBudget *budget,
                             const ContributeTableManifest *manifest,
                             int *order);

// The free bytes on the filesystem holding a directory, or false if they
// cannot be found. cfile_free_bytes, or a stand-in under test.
typedef bool (*contribute_free_bytes_fn)(const char *directory, void *context,
                                         int64_t *free_bytes);
bool contribute_tables_real_free_bytes(const char *directory, void *context,
                                       int64_t *free_bytes);

// Makes room in `directory` for a table of `bytes` (-1 unknown) about to be
// built there, keeping `reserve_bytes` free: deletes the tables the manifest
// at `manifest_path` lists and `keep` (the task's tables) does not, least
// recently used first, until it fits. A table whose build lock another
// process holds, or whose file is no longer the size it was built at, is not
// deleted. Returns whether the table fits now -- true for an unknown size,
// whose build finds out -- with the space there is for it in *available.
// `evicted` (may be NULL) gets a line per table deleted.
bool contribute_tables_make_room(const char *manifest_path,
                                 const char *directory, int64_t bytes,
                                 int64_t reserve_bytes,
                                 const ContributeTable *keep, int keep_count,
                                 contribute_free_bytes_fn free_bytes_fn,
                                 void *free_bytes_context, int64_t *available,
                                 StringList *evicted);

// Deletes the tables the manifest at `manifest_path` lists and `keep` does
// not, least recently used first, until the tables it lists take at most
// `cap_bytes`, under the same rules as make_room. Returns the bytes the
// listed tables take afterwards. `evicted` (may be NULL) gets a line per
// table deleted.
int64_t contribute_tables_trim_to(const char *manifest_path, int64_t cap_bytes,
                                  const ContributeTable *keep, int keep_count,
                                  StringList *evicted);

// Records a table just built at `path` in the manifest at `manifest_path`,
// with the size it has on disk, under the manifest's lock.
void contribute_tables_record_built(const char *manifest_path, const char *role,
                                    const char *name, const char *path);

// Marks the manifest's tables among `tables` that are chosen as used now,
// under the manifest's lock -- only when the last mark is a while old, so a
// run does not rewrite the file every task.
void contribute_tables_touch_used(const char *manifest_path,
                                  const ContributeTable *tables, int count);

// The manifest's path for `data_paths` (its first directory, where builds
// are written). The caller frees.
char *contribute_tables_manifest_path(const char *data_paths);
// The same for a manifest of another name: the command line program keeps
// its own, of the tables it built on first use. The caller frees.
char *contribute_tables_manifest_path_named(const char *data_paths,
                                            const char *filename);

#endif
