#ifndef KWG_DEFS_H
#define KWG_DEFS_H

#include "board_defs.h"

#define KWG_FILEPATH "data/lexica/"
#define KWG_FILE_EXTENSION ".kwg"

#define KWG_NODE_IS_END_FLAG 0x400000
#define KWG_NODE_ACCEPTS_FLAG 0x800000
#define KWG_ARC_INDEX_MASK 0x3FFFFF
#define KWG_TILE_BIT_OFFSET 24
#define MAX_KWG_STRING_LENGTH (BOARD_DIM)+1

typedef enum {
    KWG_MAKER_OUTPUT_DAWG,
    KWG_MAKER_OUTPUT_GADDAG,
    KWG_MAKER_OUTPUT_DAWG_AND_GADDAG,
} kwg_maker_output_t;

typedef enum {
    KWG_MAKER_MERGE_NONE,
    KWG_MAKER_MERGE_EXACT,
    KWG_MAKER_MERGE_ORDERED_SUBLIST,
    KWG_MAKER_MERGE_UNORDERED_SUBLIST,
} kwg_maker_merge_t;

#endif
