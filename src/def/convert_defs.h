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
  // Like CONVERT_TEXT2DAWG_TAIL_REORDER, but additionally re-encodes the
  // reorder DAWG into a "packed DAWG": each node uses only the bits it needs
  // (minimal arc width + minimal tile width) rather than a full 32-bit word.
  // Niche, opt-in output for fitting a word list onto small/retro hardware.
  CONVERT_TEXT2DAWG_PACKED,
  // Like CONVERT_TEXT2DAWG_PACKED, but arc-compresses the reorder DAWG: the
  // first-child arc is encoded as a popular-table index, a local signed gap, or
  // a rank-located escape, shrinking the resident footprint further than the
  // packed DAWG. Same niche, opt-in retro-hardware use; same Alpha caveat.
  CONVERT_TEXT2DAWG_ARC_COMPRESSED,
  // Same arc-compressed format, but built in BALANCED mode: a lower escape rate
  // (a little more RAM, still smaller than the packed DAWG) for materially
  // faster traversal. See dawg_arc_compressed_mode_t.
  CONVERT_TEXT2DAWG_ARC_COMPRESSED_BALANCED,
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
