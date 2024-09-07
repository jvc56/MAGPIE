#ifndef LEAVE_COUNT_HASHMAP_H
#define LEAVE_COUNT_HASHMAP_H

#include <stdint.h>

#define UNSET_KEY_OR_VALUE 0xFFFFFFFFFFFFFFFF

typedef struct LeaveCountHashMap LeaveCountHashMap;

LeaveCountHashMap *leave_count_hashmap_create(uint64_t num_buckets);
void leave_count_hashmap_destroy(LeaveCountHashMap *hm);
void leave_count_hashmap_reset(LeaveCountHashMap *hm);
uint64_t leave_count_hashmap_get_num_entries(LeaveCountHashMap *hm);

uint64_t leave_count_hashmap_get(LeaveCountHashMap *hm, uint64_t key);
void leave_count_hashmap_delete(LeaveCountHashMap *hm, uint64_t key);
void leave_count_hashmap_set(LeaveCountHashMap *hm, uint64_t key,
                             uint64_t value);

#endif