#include "leave_count_hashmap.h"

#include <stdint.h>

#include "../util/log.h"
#include "../util/util.h"

typedef struct HashEntry HashEntry;

struct HashEntry {
  uint64_t key;
  uint64_t value;
  uint64_t index;
  HashEntry *next;
};

HashEntry *hashentry_create(uint64_t index) {
  HashEntry *hashentry = malloc_or_die(sizeof(HashEntry));
  hashentry->key = UNSET_KEY_OR_VALUE;
  hashentry->index = index;
  hashentry->next = NULL;
  return hashentry;
}

void hashentry_destroy(HashEntry *hashentry) { free(hashentry); }

struct LeaveCountHashMap {
  uint64_t capacity;
  uint64_t next_available_entry_index;
  // The entries field contains an ordered list of all
  // of the malloc'd hash entries. Once created, the
  // entries field is never modified. It is only used
  // to store the actual hash entries.
  HashEntry **entries;
  // The buckets contain the linked list of hash entries
  // that make up the hash map. The buckets are modified
  // as new entries in the hm are created and deleted.
  HashEntry **buckets;
};

void leave_count_hashmap_reset(LeaveCountHashMap *hm) {
  for (uint64_t i = 0; i < hm->capacity; i++) {
    hm->buckets[i] = NULL;
    hm->entries[i]->key = UNSET_KEY_OR_VALUE;
    hm->entries[i]->next = NULL;
  }
  hm->next_available_entry_index = 0;
}

LeaveCountHashMap *leave_count_hashmap_create(uint64_t capacity) {
  if (capacity == 0) {
    log_fatal("leave count hashmap capacity must be greater than 0\n");
  }
  LeaveCountHashMap *hm = malloc_or_die(sizeof(LeaveCountHashMap));
  hm->capacity = capacity;
  hm->next_available_entry_index = 0;
  hm->entries = malloc_or_die(sizeof(HashEntry *) * capacity);
  hm->buckets = malloc_or_die(sizeof(HashEntry *) * capacity);
  for (uint64_t i = 0; i < capacity; i++) {
    hm->buckets[i] = NULL;
    hm->entries[i] = hashentry_create(i);
  }
  return hm;
}

void leave_count_hashmap_destroy(LeaveCountHashMap *hm) {
  if (!hm) {
    return;
  }
  for (uint64_t i = 0; i < hm->capacity; i++) {
    hashentry_destroy(hm->entries[i]);
  }
  free(hm->entries);
  free(hm->buckets);
  free(hm);
}

uint64_t leave_count_hashmap_get_num_entries(LeaveCountHashMap *hm) {
  return hm->next_available_entry_index;
}

uint64_t leave_count_hashmap_hash(LeaveCountHashMap *hm, uint64_t key) {
  return key % hm->capacity;
}

uint64_t leave_count_hashmap_get(LeaveCountHashMap *hm, uint64_t key) {
  uint64_t hashkey = leave_count_hashmap_hash(hm, key);
  HashEntry *entry = hm->buckets[hashkey];
  while (entry) {
    if (entry->key == key) {
      return entry->value;
    }
    entry = entry->next;
  }
  return UNSET_KEY_OR_VALUE;
}

HashEntry *leave_count_hashmap_get_next_available_entry(LeaveCountHashMap *hm,
                                                        uint64_t key,
                                                        uint64_t value) {
  if (hm->next_available_entry_index >= hm->capacity) {
    log_fatal("leave count hashmap is full\n");
  }
  HashEntry *entry = hm->entries[hm->next_available_entry_index];
  entry->key = key;
  entry->value = value;
  entry->next = NULL;
  hm->next_available_entry_index++;
  return entry;
}

void leave_count_hashmap_return_deleted_entry(LeaveCountHashMap *hm,
                                              HashEntry *entry) {
  uint64_t deleted_index = entry->index;
  hm->next_available_entry_index--;
  hm->entries[deleted_index] = hm->entries[hm->next_available_entry_index];
  hm->entries[deleted_index]->index = deleted_index;
  hm->entries[hm->next_available_entry_index] = entry;
  hm->entries[hm->next_available_entry_index]->index =
      hm->next_available_entry_index;
}

void leave_count_hashmap_delete(LeaveCountHashMap *hm, uint64_t key) {
  uint64_t hashkey = leave_count_hashmap_hash(hm, key);
  HashEntry *entry = hm->buckets[hashkey];
  if (!entry) {
    return;
  }
  HashEntry *prev = NULL;
  while (entry) {
    if (entry->key == key) {
      if (prev) {
        prev->next = entry->next;
      } else {
        hm->buckets[hashkey] = entry->next;
      }
      leave_count_hashmap_return_deleted_entry(hm, entry);
      return;
    }
    prev = entry;
    entry = entry->next;
  }
}

void leave_count_hashmap_set(LeaveCountHashMap *hm, uint64_t key,
                             uint64_t value) {
  leave_count_hashmap_delete(hm, key);
  uint64_t hashkey = leave_count_hashmap_hash(hm, key);
  if (!hm->buckets[hashkey]) {
    hm->buckets[hashkey] =
        leave_count_hashmap_get_next_available_entry(hm, key, value);
    return;
  }
  HashEntry *entry = hm->buckets[hashkey];
  while (entry->next) {
    entry = entry->next;
  }
  entry->next = leave_count_hashmap_get_next_available_entry(hm, key, value);
}
