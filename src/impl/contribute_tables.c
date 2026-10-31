#include "contribute_tables.h"

#include "../compat/cfile.h"
#include "../compat/cpath.h"
#include "../compat/ctime.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
  // How long a manifest update waits for another process's, in tenths of a
  // second. The lock is held only to read and rewrite a small file.
  MANIFEST_LOCK_ATTEMPTS = 100,
  // A table's last use is rewritten only once it is this old: what eviction
  // needs is an order, not a timestamp to the second.
  MANIFEST_TOUCH_INTERVAL_SECONDS = 600,
  MANIFEST_INITIAL_CAPACITY = 8,
};

#define MANIFEST_LOCK_NAP_SECONDS 0.1
// Marks the optional modification time on a manifest line. An absolute path
// cannot start with it, and a path relative to some data directory never does.
#define MANIFEST_MODIFIED_PREFIX "modified:"

static const contribute_table_role_t contribute_table_priority[] = {
    CONTRIBUTE_TABLE_WMP, CONTRIBUTE_TABLE_WIT, CONTRIBUTE_TABLE_RIT};

const char *contribute_table_role_name(contribute_table_role_t role) {
  switch (role) {
  case CONTRIBUTE_TABLE_WMP:
    return "wmp";
  case CONTRIBUTE_TABLE_WIT:
    return "wit";
  case CONTRIBUTE_TABLE_RIT:
    return "rit";
  case NUMBER_OF_CONTRIBUTE_TABLE_ROLES:
    break;
  }
  return "unknown";
}

const char *contribute_table_role_description(contribute_table_role_t role) {
  switch (role) {
  case CONTRIBUTE_TABLE_WMP:
    return "wordmap";
  case CONTRIBUTE_TABLE_WIT:
    return "word info table";
  case CONTRIBUTE_TABLE_RIT:
    return "rack info table";
  case NUMBER_OF_CONTRIBUTE_TABLE_ROLES:
    break;
  }
  return "table";
}

// ---------------------------------------------------------------------------
// The manifest
// ---------------------------------------------------------------------------

static ContributeTableManifest *manifest_create(void) {
  ContributeTableManifest *manifest =
      malloc_or_die(sizeof(ContributeTableManifest));
  manifest->count = 0;
  manifest->capacity = MANIFEST_INITIAL_CAPACITY;
  manifest->entries = malloc_or_die(sizeof(ContributeTableManifestEntry) *
                                    (size_t)manifest->capacity);
  return manifest;
}

void contribute_table_manifest_destroy(ContributeTableManifest *manifest) {
  if (!manifest) {
    return;
  }
  for (int entry_idx = 0; entry_idx < manifest->count; entry_idx++) {
    free(manifest->entries[entry_idx].role);
    free(manifest->entries[entry_idx].name);
    free(manifest->entries[entry_idx].path);
  }
  free(manifest->entries);
  free(manifest);
}

static int manifest_find(const ContributeTableManifest *manifest,
                         const char *role, const char *name) {
  for (int entry_idx = 0; entry_idx < manifest->count; entry_idx++) {
    if (strings_equal(manifest->entries[entry_idx].role, role) &&
        strings_equal(manifest->entries[entry_idx].name, name)) {
      return entry_idx;
    }
  }
  return -1;
}

void contribute_table_manifest_record(ContributeTableManifest *manifest,
                                      const char *role, const char *name,
                                      int64_t bytes, const char *path,
                                      int64_t now) {
  const int found = manifest_find(manifest, role, name);
  if (found >= 0) {
    ContributeTableManifestEntry *entry = &manifest->entries[found];
    entry->bytes = bytes;
    entry->last_used = now;
    free(entry->path);
    entry->path = string_duplicate(path);
    entry->modified_seconds = 0;
    entry->modified_nanoseconds = 0;
    return;
  }
  if (manifest->count == manifest->capacity) {
    manifest->capacity *= 2;
    manifest->entries =
        realloc_or_die(manifest->entries, sizeof(ContributeTableManifestEntry) *
                                              (size_t)manifest->capacity);
  }
  ContributeTableManifestEntry *entry = &manifest->entries[manifest->count++];
  entry->role = string_duplicate(role);
  entry->name = string_duplicate(name);
  entry->bytes = bytes;
  entry->last_used = now;
  entry->path = string_duplicate(path);
  entry->modified_seconds = 0;
  entry->modified_nanoseconds = 0;
}

bool contribute_table_manifest_touch(ContributeTableManifest *manifest,
                                     const char *role, const char *name,
                                     int64_t now) {
  const int found = manifest_find(manifest, role, name);
  if (found < 0) {
    return false;
  }
  manifest->entries[found].last_used = now;
  return true;
}

void contribute_table_manifest_forget(ContributeTableManifest *manifest,
                                      const char *role, const char *name) {
  const int found = manifest_find(manifest, role, name);
  if (found < 0) {
    return;
  }
  free(manifest->entries[found].role);
  free(manifest->entries[found].name);
  free(manifest->entries[found].path);
  for (int entry_idx = found; entry_idx < manifest->count - 1; entry_idx++) {
    manifest->entries[entry_idx] = manifest->entries[entry_idx + 1];
  }
  manifest->count--;
}

// A whole decimal number from 0, or false.
static bool parse_manifest_int64(const char *text, int64_t *value) {
  if (!text || !*text || strspn(text, "0123456789") != strlen(text)) {
    return false;
  }
  errno = 0;
  const long long parsed = strtoll(text, NULL, 10);
  if (errno == ERANGE) {
    return false;
  }
  *value = (int64_t)parsed;
  return true;
}

// "<role> <bytes> <last_used> <name> <path>", the path running to the end of
// the line. A line that is not exactly that is left out.
static void manifest_add_line(ContributeTableManifest *manifest, char *line) {
  char *fields[4];
  char *cursor = line;
  for (int field_idx = 0; field_idx < 4; field_idx++) {
    while (*cursor == ' ') {
      cursor++;
    }
    fields[field_idx] = cursor;
    char *space = strchr(cursor, ' ');
    if (!space) {
      return;
    }
    *space = '\0';
    cursor = space + 1;
  }
  // The modification time is optional and comes as a token of its own, so a
  // line from before it was recorded still reads.
  int64_t modified_seconds = 0;
  int64_t modified_nanoseconds = 0;
  if (has_prefix(MANIFEST_MODIFIED_PREFIX, cursor)) {
    char *token = cursor + strlen(MANIFEST_MODIFIED_PREFIX);
    char *dot = strchr(token, '.');
    char *space = strchr(token, ' ');
    if (!dot || !space || dot > space) {
      return;
    }
    *dot = '\0';
    *space = '\0';
    if (!parse_manifest_int64(token, &modified_seconds) ||
        !parse_manifest_int64(dot + 1, &modified_nanoseconds)) {
      return;
    }
    cursor = space + 1;
  }
  const char *path = cursor;
  int64_t bytes = 0;
  int64_t last_used = 0;
  if (!*fields[0] || !*fields[3] || !*path ||
      !parse_manifest_int64(fields[1], &bytes) ||
      !parse_manifest_int64(fields[2], &last_used)) {
    return;
  }
  contribute_table_manifest_record(manifest, fields[0], fields[3], bytes, path,
                                   last_used);
  ContributeTableManifestEntry *entry =
      &manifest->entries[manifest_find(manifest, fields[0], fields[3])];
  entry->modified_seconds = modified_seconds;
  entry->modified_nanoseconds = (long)modified_nanoseconds;
}

ContributeTableManifest *contribute_table_manifest_read(const char *path) {
  ContributeTableManifest *manifest = manifest_create();
  FILE *stream = path ? cfile_open(path, "rb", false) : NULL;
  if (!stream) {
    return manifest;
  }
  if (!cfile_is_regular_file(stream)) {
    (void)fclose(stream);
    return manifest;
  }
  ErrorStack *errors = error_stack_create();
  char *contents = get_string_from_file_handle(stream, path, errors);
  error_stack_destroy(errors);
  char *cursor = contents;
  while (cursor && *cursor) {
    char *newline = strchr(cursor, '\n');
    if (newline) {
      *newline = '\0';
    }
    char *line = trim(cursor);
    if (*line != '\0' && *line != '#') {
      manifest_add_line(manifest, line);
    }
    cursor = newline ? newline + 1 : NULL;
  }
  free(contents);
  return manifest;
}

bool contribute_table_manifest_write(const ContributeTableManifest *manifest,
                                     const char *path) {
  char *temporary = temporary_sibling(path);
  FILE *stream = cfile_open(temporary, "wb", true);
  if (!stream) {
    free(temporary);
    return false;
  }
  bool written =
      fprintf(stream,
              "# The derived tables magpie contribute built in this data "
              "directory, which it\n# deletes, least recently used first, when "
              "a task needs the space. It deletes\n# nothing this file does "
              "not list. One per line: role, bytes, last use (Unix\n# "
              "seconds), name, the file's modification time if known "
              "(modified:<seconds>.<nanoseconds>),\n# path.\n") > 0;
  for (int entry_idx = 0; written && entry_idx < manifest->count; entry_idx++) {
    const ContributeTableManifestEntry *entry = &manifest->entries[entry_idx];
    char *modified = entry->modified_seconds != 0
                         ? get_formatted_string("%s%" PRId64 ".%ld ",
                                                MANIFEST_MODIFIED_PREFIX,
                                                entry->modified_seconds,
                                                entry->modified_nanoseconds)
                         : string_duplicate("");
    written = fprintf(stream, "%s %" PRId64 " %" PRId64 " %s %s%s\n",
                      entry->role, entry->bytes, entry->last_used, entry->name,
                      modified, entry->path) > 0;
    free(modified);
  }
  if (fclose(stream) != 0) {
    written = false;
  }
  if (written && cfile_rename(temporary, path) != 0) {
    written = false;
  }
  if (!written) {
    (void)remove(temporary);
  }
  free(temporary);
  return written;
}

static bool table_list_names(const ContributeTable *tables, int count,
                             const char *role, const char *name) {
  for (int table_idx = 0; table_idx < count; table_idx++) {
    if (strings_equal(contribute_table_role_name(tables[table_idx].role),
                      role) &&
        strings_equal(tables[table_idx].name, name)) {
      return true;
    }
  }
  return false;
}

int contribute_table_manifest_evictable(const ContributeTableManifest *manifest,
                                        const ContributeTable *tables,
                                        int count, int *order) {
  int evictable = 0;
  for (int entry_idx = 0; entry_idx < manifest->count; entry_idx++) {
    const ContributeTableManifestEntry *entry = &manifest->entries[entry_idx];
    if (table_list_names(tables, count, entry->role, entry->name)) {
      continue;
    }
    // Insertion by last use, oldest first; ties keep the manifest's order.
    int insert_at = evictable;
    while (insert_at > 0 && manifest->entries[order[insert_at - 1]].last_used >
                                entry->last_used) {
      order[insert_at] = order[insert_at - 1];
      insert_at--;
    }
    order[insert_at] = entry_idx;
    evictable++;
  }
  return evictable;
}

int64_t contribute_table_manifest_evictable_bytes(
    const ContributeTableManifest *manifest, const ContributeTable *tables,
    int count) {
  int64_t bytes = 0;
  for (int entry_idx = 0; entry_idx < manifest->count; entry_idx++) {
    const ContributeTableManifestEntry *entry = &manifest->entries[entry_idx];
    if (!table_list_names(tables, count, entry->role, entry->name)) {
      bytes += entry->bytes;
    }
  }
  return bytes;
}

// ---------------------------------------------------------------------------
// The choice
// ---------------------------------------------------------------------------

// The wordmap for `lexicon` among `tables`, or -1.
static int find_wordmap(const ContributeTable *tables, int count,
                        const char *lexicon) {
  for (int table_idx = 0; table_idx < count; table_idx++) {
    if (tables[table_idx].role == CONTRIBUTE_TABLE_WMP &&
        strings_equal(tables[table_idx].name, lexicon)) {
      return table_idx;
    }
  }
  return -1;
}

static int64_t assumed_bytes(contribute_table_role_t role) {
  switch (role) {
  case CONTRIBUTE_TABLE_WIT:
    return CONTRIBUTE_WIT_ASSUMED_BYTES;
  case CONTRIBUTE_TABLE_RIT:
    return CONTRIBUTE_RIT_ASSUMED_BYTES;
  case CONTRIBUTE_TABLE_WMP:
  case NUMBER_OF_CONTRIBUTE_TABLE_ROLES:
    break;
  }
  return CONTRIBUTE_WMP_ASSUMED_BYTES;
}

int64_t contribute_table_build_memory(contribute_table_role_t role,
                                      int64_t bytes) {
  int64_t permille = CONTRIBUTE_WMP_BUILD_PERMILLE;
  if (role == CONTRIBUTE_TABLE_WIT) {
    permille = CONTRIBUTE_WIT_BUILD_PERMILLE;
  } else if (role == CONTRIBUTE_TABLE_RIT) {
    permille = CONTRIBUTE_RIT_BUILD_PERMILLE;
  }
  const int64_t size = bytes < 0 ? assumed_bytes(role) : bytes;
  return size / 1000 * permille + size % 1000 * permille / 1000;
}

int64_t contribute_table_hold_memory(contribute_table_role_t role,
                                     int64_t bytes) {
  return bytes < 0 ? assumed_bytes(role) : bytes;
}

// The disk a table takes that it does not take already; unknown is 0 here,
// and handled by the caller.
static int64_t table_cost(const ContributeTable *table) {
  if (table->present || table->bytes < 0) {
    return 0;
  }
  return table->bytes;
}

static bool table_size_unknown(const ContributeTable *table) {
  return !table->present && table->bytes < 0;
}

// The memory building the table peaks at; none for one on disk.
static int64_t table_build_memory(const ContributeTable *table) {
  return table->present
             ? 0
             : contribute_table_build_memory(table->role, table->bytes);
}

// The memory holding the table while playing adds; none for one this process
// has loaded, or one no player loads.
static int64_t table_hold_memory(const ContributeTable *table) {
  return table->loaded || !table->requested
             ? 0
             : contribute_table_hold_memory(table->role, table->bytes);
}

static void skip_table(ContributeTable *table, contribute_table_skip_t skip,
                       int64_t needed, int64_t available) {
  table->skip = skip;
  table->needed_bytes = needed;
  table->available_bytes = available > 0 ? available : 0;
}

// Far past any machine, and far short of overflowing the sums below.
static int64_t capped(int64_t bytes) {
  const int64_t cap = INT64_MAX / 4;
  return bytes > cap ? cap : bytes;
}

int contribute_choose_tables(ContributeTable *tables, int count,
                             const ContributeTableBudget *budget,
                             const ContributeTableManifest *manifest,
                             int *order) {
  int64_t disk_left =
      capped(budget->free_disk_bytes) - capped(budget->disk_reserve_bytes);
  if (manifest) {
    disk_left +=
        contribute_table_manifest_evictable_bytes(manifest, tables, count);
  }
  const int64_t memory =
      capped(budget->free_memory_bytes) - capped(budget->memory_reserve_bytes);
  int64_t memory_left = memory;
  for (int table_idx = 0; table_idx < count; table_idx++) {
    tables[table_idx].chosen = false;
    skip_table(&tables[table_idx], CONTRIBUTE_TABLE_KEPT, 0, 0);
  }
  int chosen = 0;
  const int num_roles = (int)(sizeof(contribute_table_priority) /
                              sizeof(contribute_table_priority[0]));
  for (int rank = 0; rank < num_roles; rank++) {
    const contribute_table_role_t role = contribute_table_priority[rank];
    for (int table_idx = 0; table_idx < count; table_idx++) {
      ContributeTable *table = &tables[table_idx];
      if (table->role != role || table->chosen) {
        continue;
      }
      if (table->unpinned) {
        table->chosen = true;
        order[chosen++] = table_idx;
        continue;
      }
      // A wordmap no player loads is built only for a rack info table, and
      // is decided with it.
      if (role == CONTRIBUTE_TABLE_WMP && !table->requested) {
        continue;
      }
      const int wordmap = role == CONTRIBUTE_TABLE_WMP
                              ? -1
                              : find_wordmap(tables, count, table->lexicon);
      const bool wordmap_skipped =
          wordmap >= 0 && !tables[wordmap].chosen && tables[wordmap].requested;
      if (role == CONTRIBUTE_TABLE_WIT && wordmap_skipped) {
        skip_table(table, CONTRIBUTE_TABLE_NO_WORDMAP, 0, 0);
        continue;
      }
      ContributeTable *dependency = NULL;
      if (role == CONTRIBUTE_TABLE_RIT) {
        if (wordmap < 0 || wordmap_skipped) {
          skip_table(table, CONTRIBUTE_TABLE_NO_WORDMAP, 0, 0);
          continue;
        }
        if (!tables[wordmap].chosen) {
          dependency = &tables[wordmap];
        }
      }
      // Disk. A table on disk takes nothing, and fits whatever is left; one
      // of unknown size fits while there is anything left after the rest.
      const int64_t cost =
          table_cost(table) + (dependency ? table_cost(dependency) : 0);
      const bool unknown = table_size_unknown(table) ||
                           (dependency && table_size_unknown(dependency));
      bool disk_fits = cost <= disk_left;
      if (unknown) {
        disk_fits = cost < disk_left;
      } else if (cost == 0) {
        disk_fits = true;
      }
      if (!disk_fits) {
        skip_table(table, CONTRIBUTE_TABLE_NO_DISK, cost, disk_left);
        continue;
      }
      // Memory to build: one build at a time, so each on its own.
      int64_t build = table_build_memory(table);
      if (dependency && table_build_memory(dependency) > build) {
        build = table_build_memory(dependency);
      }
      if (build > 0 && build > memory) {
        skip_table(table, CONTRIBUTE_TABLE_NO_MEMORY_TO_BUILD, build, memory);
        continue;
      }
      // Memory to hold, beside what is chosen already.
      const int64_t hold = table_hold_memory(table);
      if (hold > 0 && hold > memory_left) {
        skip_table(table, CONTRIBUTE_TABLE_NO_MEMORY_TO_HOLD, hold,
                   memory_left);
        continue;
      }
      disk_left -= cost;
      memory_left -= hold;
      if (dependency) {
        dependency->chosen = true;
        order[chosen++] = wordmap;
      }
      table->chosen = true;
      order[chosen++] = table_idx;
    }
  }
  return chosen;
}

// ---------------------------------------------------------------------------
// Eviction and the manifest on disk
// ---------------------------------------------------------------------------

bool contribute_tables_real_free_bytes(const char *directory, void *context,
                                       int64_t *free_bytes) {
  (void)context;
  return cfile_free_bytes(directory, free_bytes);
}

// Takes the manifest's lock, waiting a while for another process's; -1 if it
// cannot be had, and the caller does without.
static int lock_manifest(const char *manifest_path) {
  char *lock_path = get_formatted_string("%s.lock", manifest_path);
  const int lock = cfile_lock_open(lock_path);
  free(lock_path);
  if (lock < 0) {
    return -1;
  }
  for (int attempt = 0; attempt < MANIFEST_LOCK_ATTEMPTS; attempt++) {
    bool busy = false;
    if (cfile_lock_try(lock, &busy)) {
      return lock;
    }
    if (!busy) {
      break;
    }
    ctime_nap(MANIFEST_LOCK_NAP_SECONDS);
  }
  cfile_lock_close(lock);
  return -1;
}

// Deletes the table `entry` names, unless another process is building it or
// its file is no longer the one that was built. Returns whether it is gone
// (or was already).
static bool evict_entry(const ContributeTableManifestEntry *entry) {
  CFileIdentity identity;
  if (!cfile_identity(entry->path, &identity)) {
    return true;
  }
  if (identity.size != entry->bytes) {
    return false;
  }
  if (entry->modified_seconds != 0 &&
      (identity.mtime_seconds != entry->modified_seconds ||
       identity.mtime_nanoseconds != entry->modified_nanoseconds)) {
    return false;
  }
  char *lock_path = get_formatted_string("%s.lock", entry->path);
  const int lock = cfile_lock_open(lock_path);
  free(lock_path);
  bool busy = false;
  if (lock >= 0 && !cfile_lock_try(lock, &busy) && busy) {
    cfile_lock_close(lock);
    return false;
  }
  const bool removed = remove(entry->path) == 0;
  cfile_lock_close(lock);
  return removed;
}

// Whether eviction has made enough room, after `freed_bytes` of listed
// tables were deleted.
typedef bool (*eviction_done_fn)(void *context, int64_t freed_bytes);

// Deletes the tables the manifest at `manifest_path` lists and `keep` does
// not, least recently used first, until `done` says there is room, and drops
// them from the manifest. A table evict_entry will not delete is passed over.
// Returns false, deleting nothing, if the manifest's lock cannot be had.
static bool evict_least_recently_used(const char *manifest_path,
                                      const ContributeTable *keep,
                                      int keep_count, eviction_done_fn done,
                                      void *done_context, StringList *evicted) {
  const int lock = lock_manifest(manifest_path);
  if (lock < 0) {
    return false;
  }
  ContributeTableManifest *manifest =
      contribute_table_manifest_read(manifest_path);
  int *order = malloc_or_die(sizeof(int) * (size_t)(manifest->count + 1));
  const int evictable =
      contribute_table_manifest_evictable(manifest, keep, keep_count, order);
  StringList *gone = string_list_create();
  int64_t freed_bytes = 0;
  for (int rank = 0; rank < evictable && !done(done_context, freed_bytes);
       rank++) {
    const ContributeTableManifestEntry *entry = &manifest->entries[order[rank]];
    if (!evict_entry(entry)) {
      continue;
    }
    freed_bytes += entry->bytes;
    if (evicted) {
      char *line = get_formatted_string("%s %s", entry->role, entry->name);
      string_list_add_string(evicted, line);
      free(line);
    }
    string_list_add_string(gone, entry->role);
    string_list_add_string(gone, entry->name);
  }
  // Measured once more, so the caller sees the room the last deletion made.
  (void)done(done_context, freed_bytes);
  free(order);
  const int gone_count = string_list_get_count(gone);
  for (int gone_idx = 0; gone_idx + 1 < gone_count; gone_idx += 2) {
    contribute_table_manifest_forget(
        manifest, string_list_get_string(gone, gone_idx),
        string_list_get_string(gone, gone_idx + 1));
  }
  if (gone_count > 0) {
    (void)contribute_table_manifest_write(manifest, manifest_path);
  }
  string_list_destroy(gone);
  contribute_table_manifest_destroy(manifest);
  cfile_lock_close(lock);
  return true;
}

// make_room's measure of room: the free space the filesystem reports.
typedef struct DiskRoom {
  const char *directory;
  contribute_free_bytes_fn free_bytes_fn;
  void *free_bytes_context;
  int64_t reserve_bytes;
  int64_t bytes;
  int64_t *available;
} DiskRoom;

static bool disk_room_done(void *context, int64_t freed_bytes) {
  (void)freed_bytes;
  const DiskRoom *room = (const DiskRoom *)context;
  int64_t free_bytes = 0;
  if (room->free_bytes_fn(room->directory, room->free_bytes_context,
                          &free_bytes)) {
    *room->available = free_bytes - room->reserve_bytes;
  }
  return room->bytes <= *room->available;
}

bool contribute_tables_make_room(const char *manifest_path,
                                 const char *directory, int64_t bytes,
                                 int64_t reserve_bytes,
                                 const ContributeTable *keep, int keep_count,
                                 contribute_free_bytes_fn free_bytes_fn,
                                 void *free_bytes_context, int64_t *available,
                                 StringList *evicted) {
  int64_t free_bytes = 0;
  if (!free_bytes_fn(directory, free_bytes_context, &free_bytes)) {
    // A filesystem that will not say: the build finds out.
    *available = INT64_MAX;
    return true;
  }
  *available = free_bytes - reserve_bytes;
  if (bytes < 0 || bytes <= *available) {
    return true;
  }
  DiskRoom room = {.directory = directory,
                   .free_bytes_fn = free_bytes_fn,
                   .free_bytes_context = free_bytes_context,
                   .reserve_bytes = reserve_bytes,
                   .bytes = bytes,
                   .available = available};
  if (!evict_least_recently_used(manifest_path, keep, keep_count,
                                 disk_room_done, &room, evicted)) {
    return false;
  }
  return bytes <= *available;
}

// trim_to's measure of room: what the listed tables still take.
typedef struct ListedRoom {
  int64_t listed_bytes;
  int64_t cap_bytes;
} ListedRoom;

static bool listed_room_done(void *context, int64_t freed_bytes) {
  const ListedRoom *room = (const ListedRoom *)context;
  return room->listed_bytes - freed_bytes <= room->cap_bytes;
}

int64_t contribute_tables_trim_to(const char *manifest_path, int64_t cap_bytes,
                                  const ContributeTable *keep, int keep_count,
                                  StringList *evicted) {
  ContributeTableManifest *manifest =
      contribute_table_manifest_read(manifest_path);
  ListedRoom room = {.listed_bytes = 0, .cap_bytes = cap_bytes};
  for (int entry_idx = 0; entry_idx < manifest->count; entry_idx++) {
    room.listed_bytes += manifest->entries[entry_idx].bytes;
  }
  contribute_table_manifest_destroy(manifest);
  if (room.listed_bytes > cap_bytes) {
    (void)evict_least_recently_used(manifest_path, keep, keep_count,
                                    listed_room_done, &room, evicted);
  }
  manifest = contribute_table_manifest_read(manifest_path);
  int64_t listed_bytes = 0;
  for (int entry_idx = 0; entry_idx < manifest->count; entry_idx++) {
    listed_bytes += manifest->entries[entry_idx].bytes;
  }
  contribute_table_manifest_destroy(manifest);
  return listed_bytes;
}

void contribute_tables_record_built(const char *manifest_path, const char *role,
                                    const char *name, const char *path) {
  CFileIdentity identity;
  if (!cfile_identity(path, &identity)) {
    return;
  }
  const int lock = lock_manifest(manifest_path);
  if (lock < 0) {
    return;
  }
  ContributeTableManifest *manifest =
      contribute_table_manifest_read(manifest_path);
  // Eviction may run from another directory, where a relative path would
  // name some other file or none.
  char *absolute_path = cfile_absolute_path(path);
  contribute_table_manifest_record(manifest, role, name, identity.size,
                                   absolute_path ? absolute_path : path,
                                   (int64_t)time(NULL));
  free(absolute_path);
  ContributeTableManifestEntry *entry =
      &manifest->entries[manifest_find(manifest, role, name)];
  entry->modified_seconds = identity.mtime_seconds;
  entry->modified_nanoseconds = identity.mtime_nanoseconds;
  (void)contribute_table_manifest_write(manifest, manifest_path);
  contribute_table_manifest_destroy(manifest);
  cfile_lock_close(lock);
}

void contribute_tables_touch_used(const char *manifest_path,
                                  const ContributeTable *tables, int count) {
  const int64_t now = (int64_t)time(NULL);
  // Read without the lock first: most tasks have nothing to mark, and need
  // not take it.
  ContributeTableManifest *manifest =
      contribute_table_manifest_read(manifest_path);
  bool stale = false;
  for (int table_idx = 0; table_idx < count && !stale; table_idx++) {
    const ContributeTable *table = &tables[table_idx];
    if (!table->chosen) {
      continue;
    }
    const int found = manifest_find(
        manifest, contribute_table_role_name(table->role), table->name);
    stale = found >= 0 && now - manifest->entries[found].last_used >=
                              MANIFEST_TOUCH_INTERVAL_SECONDS;
  }
  contribute_table_manifest_destroy(manifest);
  if (!stale) {
    return;
  }
  const int lock = lock_manifest(manifest_path);
  if (lock < 0) {
    return;
  }
  manifest = contribute_table_manifest_read(manifest_path);
  for (int table_idx = 0; table_idx < count; table_idx++) {
    if (tables[table_idx].chosen) {
      (void)contribute_table_manifest_touch(
          manifest, contribute_table_role_name(tables[table_idx].role),
          tables[table_idx].name, now);
    }
  }
  (void)contribute_table_manifest_write(manifest, manifest_path);
  contribute_table_manifest_destroy(manifest);
  cfile_lock_close(lock);
}

char *contribute_tables_manifest_path(const char *data_paths) {
  return contribute_tables_manifest_path_named(
      data_paths, CONTRIBUTE_TABLE_MANIFEST_FILENAME);
}

char *contribute_tables_manifest_path_named(const char *data_paths,
                                            const char *filename) {
  char *first = cpath_list_first(data_paths ? data_paths : ".");
  char *path = get_formatted_string("%s/%s", first, filename);
  free(first);
  return path;
}
