#ifndef KWG_DEFS_H
#define KWG_DEFS_H

#include "board_defs.h"

#define KWG_FILEPATH "data/lexica/"
#define KWG_FILE_EXTENSION ".kwg"

#define KWG_NODE_IS_END_FLAG 0x400000
#define KWG_NODE_ACCEPTS_FLAG 0x800000
#define KWG_ARC_INDEX_MASK 0x3FFFFF
#define KWG_TILE_BIT_OFFSET 24

#define KWG_HASH_COMBINING_PRIME_1 0xb492b66fbe98f273ULL;
#define KWG_HASH_COMBINING_PRIME_2 0x9ae16a3b2f90404fULL;
#define KWG_HASH_NUMBER_OF_BUCKETS 1300021
#define ENGLISH_ALPHABET_BITS_USED 5

#define KWG_NODE_INDEX_LIST_INITIAL_CAPACITY 3
// These were picked to fit CSW21 without resizing
#define KWG_MUTABLE_NODE_LIST_INITIAL_CAPACITY 7300000
#define KWG_ORDERED_POINTER_LIST_INITIAL_CAPACITY 1250000
#define KWG_HASH_BUCKET_ITEMS_CAPACITY 1

typedef enum {
  KWG_MAKER_OUTPUT_DAWG,
  KWG_MAKER_OUTPUT_GADDAG,
  KWG_MAKER_OUTPUT_DAWG_AND_GADDAG,
} kwg_maker_output_t;

typedef enum {
  KWG_MAKER_MERGE_NONE,
  KWG_MAKER_MERGE_EXACT,
} kwg_maker_merge_t;

#endif
