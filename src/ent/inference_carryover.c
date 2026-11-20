#include "inference_carryover.h"

#include "../def/klv_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../str/rack_string.h"
#include "../util/io_util.h"
#include "kwg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  INFERENCE_CARRYOVER_MAX_KEPT = (RACK_SIZE)-1,
  INFERENCE_CARRYOVER_LINE_LENGTH = 256,
};

typedef struct InferenceCarryoverEntry {
  uint32_t leave_index;
  float weight;
  // The kept tiles in increasing order; the table's `kept` of them are set.
  MachineLetter tiles[INFERENCE_CARRYOVER_MAX_KEPT];
} InferenceCarryoverEntry;

struct InferenceCarryoverTable {
  int kept;
  uint32_t num_leaves;
  float max;
  uint32_t count;
  // In increasing order of leave_index.
  InferenceCarryoverEntry *entries;
};

static float inference_carryover_slot_load(const InferenceCarryoverSlot *slot) {
  const uint32_t bits = atomic_load(slot);
  float value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

static void inference_carryover_slot_store(InferenceCarryoverSlot *slot,
                                           float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  atomic_store(slot, bits);
}

// The tiles of the leave with this KLV word index, the inverse of
// klv_get_word_index: a node's index is the number of leaves before it in the
// order the word graph lists them. Returns the number of tiles, or 0 when the
// index is no leave of at most INFERENCE_CARRYOVER_MAX_KEPT tiles.
static int inference_carryover_leave_tiles(const KLV *klv, uint32_t leave_index,
                                           MachineLetter *tiles) {
  const KWG *kwg = klv_get_kwg(klv);
  uint32_t node_index = kwg_get_dawg_root_node_index(kwg);
  uint32_t index_at_node = 0;
  int num_tiles = 0;
  while (node_index != 0 && num_tiles < INFERENCE_CARRYOVER_MAX_KEPT) {
    // The sibling whose leaves include the index.
    bool found_sibling = false;
    while (!found_sibling) {
      const uint32_t node = kwg_node(kwg, node_index);
      const bool is_end = kwg_node_is_end(node);
      const uint32_t leaves_through_node =
          klv->word_counts[node_index] -
          (is_end ? 0 : klv->word_counts[node_index + 1]);
      if (leave_index < index_at_node + leaves_through_node) {
        found_sibling = true;
      } else if (is_end) {
        return 0;
      } else {
        index_at_node += leaves_through_node;
        node_index++;
      }
    }
    const uint32_t node = kwg_node(kwg, node_index);
    tiles[num_tiles++] = (MachineLetter)kwg_node_tile(node);
    if (leave_index == index_at_node) {
      return num_tiles;
    }
    index_at_node++;
    node_index = kwg_node_arc_index(node);
  }
  return 0;
}

static InferenceCarryoverTable *
inference_carryover_table_create(int kept, uint32_t num_leaves,
                                 uint32_t count) {
  InferenceCarryoverTable *table =
      malloc_or_die(sizeof(InferenceCarryoverTable));
  table->kept = kept;
  table->num_leaves = num_leaves;
  table->max = 0;
  table->count = count;
  table->entries =
      calloc_or_die(count > 0 ? count : 1, sizeof(InferenceCarryoverEntry));
  return table;
}

InferenceCarryoverTable *
inference_carryover_table_create_from_slots(const InferenceCarryoverSlot *slots,
                                            const KLV *klv, int kept) {
  if (slots == NULL || klv == NULL || kept <= 0 ||
      kept > INFERENCE_CARRYOVER_MAX_KEPT) {
    return NULL;
  }
  const uint32_t num_leaves = klv_get_number_of_leaves(klv);
  uint32_t count = 0;
  for (uint32_t leave_index = 0; leave_index < num_leaves; leave_index++) {
    if (inference_carryover_slot_load(&slots[leave_index]) > 0) {
      count++;
    }
  }
  if (count == 0) {
    return NULL;
  }
  InferenceCarryoverTable *table =
      inference_carryover_table_create(kept, num_leaves, count);
  uint32_t entry_idx = 0;
  for (uint32_t leave_index = 0; leave_index < num_leaves; leave_index++) {
    const float weight = inference_carryover_slot_load(&slots[leave_index]);
    if (weight <= 0) {
      continue;
    }
    InferenceCarryoverEntry *entry = &table->entries[entry_idx];
    if (inference_carryover_leave_tiles(klv, leave_index, entry->tiles) !=
        kept) {
      // Only leaves of the kept size are recorded; anything else is not a
      // table of this turn.
      inference_carryover_table_destroy(table);
      return NULL;
    }
    entry->leave_index = leave_index;
    entry->weight = weight;
    if (weight > table->max) {
      table->max = weight;
    }
    entry_idx++;
  }
  return table;
}

InferenceCarryoverTable *
inference_carryover_table_duplicate(const InferenceCarryoverTable *table) {
  if (table == NULL) {
    return NULL;
  }
  InferenceCarryoverTable *duplicate = inference_carryover_table_create(
      table->kept, table->num_leaves, table->count);
  duplicate->max = table->max;
  memcpy(duplicate->entries, table->entries,
         sizeof(InferenceCarryoverEntry) * (size_t)table->count);
  return duplicate;
}

void inference_carryover_table_destroy(InferenceCarryoverTable *table) {
  if (table == NULL) {
    return;
  }
  free(table->entries);
  free(table);
}

int inference_carryover_table_get_kept(const InferenceCarryoverTable *table) {
  return table->kept;
}

uint32_t
inference_carryover_table_get_count(const InferenceCarryoverTable *table) {
  return table->count;
}

uint32_t
inference_carryover_table_get_num_leaves(const InferenceCarryoverTable *table) {
  return table->num_leaves;
}

float inference_carryover_table_get_max(const InferenceCarryoverTable *table) {
  return table->max;
}

uint32_t
inference_carryover_table_get_entry_index(const InferenceCarryoverTable *table,
                                          uint32_t entry_idx) {
  return table->entries[entry_idx].leave_index;
}

float inference_carryover_table_get_entry_weight(
    const InferenceCarryoverTable *table, uint32_t entry_idx) {
  return table->entries[entry_idx].weight;
}

void inference_carryover_table_get_entry_leave(
    const InferenceCarryoverTable *table, uint32_t entry_idx, int dist_size,
    Rack *leave) {
  rack_set_dist_size_and_reset(leave, dist_size);
  for (int tile_idx = 0; tile_idx < table->kept; tile_idx++) {
    rack_add_letter(leave, table->entries[entry_idx].tiles[tile_idx]);
  }
}

float inference_carryover_table_get_weight(const InferenceCarryoverTable *table,
                                           uint32_t leave_index) {
  uint32_t low = 0;
  uint32_t high = table->count;
  while (low < high) {
    const uint32_t middle = low + (high - low) / 2;
    const uint32_t middle_index = table->entries[middle].leave_index;
    if (middle_index == leave_index) {
      return table->entries[middle].weight;
    }
    if (middle_index < leave_index) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  return 0;
}

static bool
inference_carryover_entry_within(const InferenceCarryoverEntry *entry, int kept,
                                 const Rack *pool) {
  int run_length = 0;
  for (int tile_idx = 0; tile_idx < kept; tile_idx++) {
    run_length =
        tile_idx > 0 && entry->tiles[tile_idx] == entry->tiles[tile_idx - 1]
            ? run_length + 1
            : 1;
    if (entry->tiles[tile_idx] >= rack_get_dist_size(pool) ||
        rack_get_letter(pool, entry->tiles[tile_idx]) < run_length) {
      return false;
    }
  }
  return true;
}

float inference_carryover_table_get_max_within(
    const InferenceCarryoverTable *table, const Rack *pool) {
  float max_within = 0;
  for (uint32_t entry_idx = 0; entry_idx < table->count; entry_idx++) {
    const InferenceCarryoverEntry *entry = &table->entries[entry_idx];
    if (entry->weight > max_within &&
        inference_carryover_entry_within(entry, table->kept, pool)) {
      max_within = entry->weight;
    }
  }
  return max_within;
}

void inference_carryover_table_fill_slots(const InferenceCarryoverTable *table,
                                          InferenceCarryoverSlot *slots) {
  for (uint32_t leave_index = 0; leave_index < table->num_leaves;
       leave_index++) {
    atomic_store(&slots[leave_index], 0);
  }
  for (uint32_t entry_idx = 0; entry_idx < table->count; entry_idx++) {
    inference_carryover_slot_store(
        &slots[table->entries[entry_idx].leave_index],
        table->entries[entry_idx].weight);
  }
}

static int inference_carryover_compare_by_weight(const void *left,
                                                 const void *right) {
  const InferenceCarryoverEntry *left_entry =
      (const InferenceCarryoverEntry *)left;
  const InferenceCarryoverEntry *right_entry =
      (const InferenceCarryoverEntry *)right;
  if (left_entry->weight != right_entry->weight) {
    return left_entry->weight > right_entry->weight ? -1 : 1;
  }
  if (left_entry->leave_index != right_entry->leave_index) {
    return left_entry->leave_index < right_entry->leave_index ? -1 : 1;
  }
  return 0;
}

static int inference_carryover_compare_by_index(const void *left,
                                                const void *right) {
  const InferenceCarryoverEntry *left_entry =
      (const InferenceCarryoverEntry *)left;
  const InferenceCarryoverEntry *right_entry =
      (const InferenceCarryoverEntry *)right;
  if (left_entry->leave_index != right_entry->leave_index) {
    return left_entry->leave_index < right_entry->leave_index ? -1 : 1;
  }
  return 0;
}

bool inference_carryover_table_write(const InferenceCarryoverTable *table,
                                     const LetterDistribution *ld,
                                     const char *label, const char *filename) {
  FILE *file = fopen(filename, "w");
  if (file == NULL) {
    return false;
  }
  InferenceCarryoverEntry *by_weight =
      malloc_or_die(sizeof(InferenceCarryoverEntry) * (size_t)table->count);
  memcpy(by_weight, table->entries,
         sizeof(InferenceCarryoverEntry) * (size_t)table->count);
  qsort(by_weight, table->count, sizeof(InferenceCarryoverEntry),
        inference_carryover_compare_by_weight);
  bool success = fprintf(file, "# kept\t%d\n# max\t%.9g\n# entries\t%u\n",
                         table->kept, (double)table->max, table->count) > 0;
  if (success && label != NULL) {
    success = fprintf(file, "# label\t%s\n", label) > 0;
  }
  Rack leave;
  for (uint32_t entry_idx = 0; success && entry_idx < table->count;
       entry_idx++) {
    rack_set_dist_size_and_reset(&leave, ld_get_size(ld));
    for (int tile_idx = 0; tile_idx < table->kept; tile_idx++) {
      rack_add_letter(&leave, by_weight[entry_idx].tiles[tile_idx]);
    }
    char leave_string[INFERENCE_CARRYOVER_LINE_LENGTH];
    rack_get_string(&leave, ld, false, leave_string, sizeof(leave_string));
    success = fprintf(file, "%s\t%.9g\n", leave_string,
                      (double)by_weight[entry_idx].weight) > 0;
  }
  free(by_weight);
  return fclose(file) == 0 && success;
}

// Reads one "<leave><TAB><weight>" line into the entry. False when the line is
// not an entry of the table.
static bool inference_carryover_parse_entry(const KLV *klv,
                                            const LetterDistribution *ld,
                                            int kept, char *line,
                                            InferenceCarryoverEntry *entry) {
  char *separator = strchr(line, '\t');
  if (separator == NULL) {
    return false;
  }
  *separator = '\0';
  Rack leave;
  if (rack_set_to_string(ld, &leave, line) < 0 ||
      rack_get_total_letters(&leave) != kept) {
    return false;
  }
  char *weight_end = NULL;
  const float weight = strtof(separator + 1, &weight_end);
  if (weight_end == separator + 1 || !(weight > 0)) {
    return false;
  }
  entry->leave_index = klv_get_word_index(klv, &leave);
  if (entry->leave_index == KLV_UNFOUND_INDEX) {
    return false;
  }
  entry->weight = weight;
  int tile_idx = 0;
  for (int ml = 0; ml < ld_get_size(ld); ml++) {
    for (int copy = 0; copy < rack_get_letter(&leave, ml); copy++) {
      entry->tiles[tile_idx++] = (MachineLetter)ml;
    }
  }
  return true;
}

InferenceCarryoverTable *
inference_carryover_table_read(const KLV *klv, const LetterDistribution *ld,
                               const char *filename) {
  FILE *file = fopen(filename, "r");
  if (file == NULL) {
    return NULL;
  }
  int kept = 0;
  unsigned int declared_entries = 0;
  bool has_entries_line = false;
  InferenceCarryoverTable *table = NULL;
  uint32_t entries_read = 0;
  bool valid = true;
  char line[INFERENCE_CARRYOVER_LINE_LENGTH];
  while (valid && fgets(line, sizeof(line), file) != NULL) {
    line[strcspn(line, "\r\n")] = '\0';
    if (line[0] == '\0') {
      continue;
    }
    if (line[0] == '#') {
      if (sscanf(line, "# kept\t%d", &kept) == 1) {
        continue;
      }
      if (sscanf(line, "# entries\t%u", &declared_entries) == 1) {
        has_entries_line = true;
      }
      continue;
    }
    if (table == NULL) {
      valid = has_entries_line && declared_entries > 0 && kept > 0 &&
              kept <= INFERENCE_CARRYOVER_MAX_KEPT;
      if (valid) {
        table = inference_carryover_table_create(
            kept, klv_get_number_of_leaves(klv), declared_entries);
      }
    }
    valid = valid && entries_read < declared_entries &&
            inference_carryover_parse_entry(klv, ld, kept, line,
                                            &table->entries[entries_read]);
    if (valid) {
      if (table->entries[entries_read].weight > table->max) {
        table->max = table->entries[entries_read].weight;
      }
      entries_read++;
    }
  }
  (void)fclose(file);
  if (!valid || table == NULL || entries_read != declared_entries) {
    inference_carryover_table_destroy(table);
    return NULL;
  }
  qsort(table->entries, table->count, sizeof(InferenceCarryoverEntry),
        inference_carryover_compare_by_index);
  for (uint32_t entry_idx = 1; entry_idx < table->count; entry_idx++) {
    if (table->entries[entry_idx].leave_index ==
        table->entries[entry_idx - 1].leave_index) {
      inference_carryover_table_destroy(table);
      return NULL;
    }
  }
  return table;
}

struct InferenceCarryoverChain {
  InferenceCarryoverTable *table;
  int length;
};

InferenceCarryoverChain *inference_carryover_chain_create(void) {
  return calloc_or_die(1, sizeof(InferenceCarryoverChain));
}

void inference_carryover_chain_destroy(InferenceCarryoverChain *chain) {
  if (chain == NULL) {
    return;
  }
  inference_carryover_table_destroy(chain->table);
  free(chain);
}

void inference_carryover_chain_reset(InferenceCarryoverChain *chain) {
  inference_carryover_table_destroy(chain->table);
  chain->table = NULL;
  chain->length = 0;
}

const InferenceCarryoverTable *
inference_carryover_chain_get_table(const InferenceCarryoverChain *chain) {
  return chain->table;
}

int inference_carryover_chain_get_length(const InferenceCarryoverChain *chain) {
  return chain->length;
}

void inference_carryover_chain_advance(InferenceCarryoverChain *chain,
                                       InferenceCarryoverTable *table) {
  if (table == NULL) {
    inference_carryover_chain_reset(chain);
    return;
  }
  inference_carryover_table_destroy(chain->table);
  chain->table = table;
  chain->length++;
}
