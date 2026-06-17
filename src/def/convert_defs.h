#ifndef CONVERT_DEFS_H
#define CONVERT_DEFS_H

typedef enum {
  CONVERT_TEXT2DAWG,
  CONVERT_TEXT2GADDAG,
  CONVERT_TEXT2KWG,
  // Like CONVERT_TEXT2KWG but uses wolges-style tail merging for a smaller
  // (but slightly slower to traverse) node array.
  CONVERT_TEXT2KWG_TAIL_MERGE,
  // DAWG-only output that additionally reorders each node's child list to
  // maximize tail merging (smallest node array). NOT valid for the Alpha
  // cross-set path; for linear-scan readers / export only.
  CONVERT_TEXT2DAWG_TAIL_REORDER,
  CONVERT_DAWG2TEXT,
  CONVERT_GADDAG2TEXT,
  CONVERT_CSV2KLV,
  CONVERT_KLV2CSV,
  CONVERT_TEXT2WORDMAP,
  CONVERT_DAWG2WORDMAP,
  CONVERT_KLVWMP2RIT,
  CONVERT_UNKNOWN,
} conversion_type_t;

#endif
