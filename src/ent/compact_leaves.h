#ifndef COMPACT_LEAVES_H
#define COMPACT_LEAVES_H

#include "../def/board_defs.h"
#include "../ent/equity.h"
#include "../ent/rack.h"
#include "../util/io_util.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A "compact leaves" file (.clv) is a small parametric APPROXIMATION of a KLV,
// for systems that cannot store the exhaustive leave table (CSW21's .klv2 is
// ~3.67 MB / 914,624 leaves; a .clv targets a few KB). It is opt-in and
// orthogonal to mainline MAGPIE; the regular KLV remains the default.
//
// The value of a leave L (tile multiset, counts n[ml], ml 0 = blank) is
//   f(L) = base
//        + sum_ml v[ml] * n[ml]                 (per-tile linear)
//        + sum_ml d[ml] * max(n[ml]-1, 0)        (duplicate penalty; optional)
//        + g(nv, nc)                             (vowel/consonant adjustment)
//        + sum_{t in synergies} s_t * count(t in L)
// where nv/nc are the leave's vowel/consonant counts (the vowel set is stored
// in the file, so lookup needs no LetterDistribution). Everything is
// FIXED-POINT: each coefficient is a signed integer of "ticks", and value =
// (sum ticks) * radix_millipoints. Default radix = EQUITY_RESOLUTION/8 = 125 mp
// = 1/8 point (divides 1000 evenly -> lossless), configurable in the header.
// See COMPACT_LEAVES.md for the model, the frequency-weighted fit, and the
// format.

enum {
  COMPACT_LEAVES_VERSION = 1,
  // 4 magic + 1 version + 1 radix_code + 1 dist_size + 1 flags + 4 num_synergy.
  COMPACT_LEAVES_HEADER_BYTES = 12,
  // flags bits.
  COMPACT_LEAVES_FLAG_HAS_DUP = 0x1,  // dup_ticks[] present
  COMPACT_LEAVES_FLAG_VC_TABLE = 0x2, // V/C is the full 2-D table (else poly)
  // All coefficients ("ticks") and synergy fields are sub-byte bit-packed
  // (zigzag fixed-width). Lets more synergies fit a byte budget. When unset the
  // body is byte-granular (the simpler, more portable default).
  COMPACT_LEAVES_FLAG_BITPACKED = 0x4,
  // Vowel/consonant adjustment: clamped 2-D table over (min(nv,K), min(nc,K)).
  COMPACT_LEAVES_VC_CLAMP = 4,       // counts clamped to 0..4
  COMPACT_LEAVES_VC_DIM = 5,         // (CLAMP + 1)
  COMPACT_LEAVES_VC_TABLE_SIZE = 25, // DIM * DIM
  COMPACT_LEAVES_VC_POLY_SIZE = 2,   // a1*i + a2*i^2 fallback (i = nv-nc)
  // A synergy term is a small tile multiset; a leave holds <= RACK_SIZE-1
  // tiles.
  COMPACT_LEAVES_MAX_SYNERGY_TILES = 6,
};

#define COMPACT_LEAVES_MAGIC "CLV1"

// radix_code -> millipoints per tick (kept small/enumerable; all divide
// EQUITY_RESOLUTION evenly so decode is lossless).
typedef enum {
  COMPACT_LEAVES_RADIX_EIGHTH = 0,  // 125 mp = 1/8 point (default)
  COMPACT_LEAVES_RADIX_QUARTER = 1, // 250 mp = 1/4 point
  COMPACT_LEAVES_RADIX_HALF = 2,    // 500 mp = 1/2 point
  COMPACT_LEAVES_RADIX_WHOLE = 3,   // 1000 mp = 1 point
} compact_leaves_radix_t;

// One selected interaction term: a sorted tile multiset and its coefficient.
typedef struct CompactLeavesSynergy {
  uint8_t num_tiles; // 2..COMPACT_LEAVES_MAX_SYNERGY_TILES
  MachineLetter tiles[COMPACT_LEAVES_MAX_SYNERGY_TILES]; // sorted, ml order
  int16_t value_ticks;
} CompactLeavesSynergy;

typedef struct CompactLeaves {
  uint8_t dist_size;         // machine letters incl blank (~27 for English)
  uint8_t radix_code;        // compact_leaves_radix_t (stored in the header)
  int32_t radix_millipoints; // derived: millipoints per tick (125 = 1/8 point)
  uint8_t flags;
  // Bit width of each zigzag-encoded coefficient when FLAG_BITPACKED is set
  // (stored in the file just after vowel_bits). Unused in byte mode.
  uint8_t coef_bits;
  // Which machine letters are vowels (bit ml set). Stored so a lookup is
  // self-contained -- no LetterDistribution needed at query time.
  uint64_t vowel_bits;
  int16_t base_ticks;
  int16_t *tile_ticks; // [dist_size] per-tile v[ml]
  int16_t *dup_ticks;  // [dist_size] d[ml], or NULL if !HAS_DUP
  int16_t vc_ticks[COMPACT_LEAVES_VC_TABLE_SIZE]; // table or [0..1] for poly
  CompactLeavesSynergy *synergies;                // [num_synergies]
  uint32_t num_synergies;
} CompactLeaves;

// Radix code <-> millipoints.
static inline int32_t compact_leaves_radix_millipoints(uint8_t radix_code) {
  switch ((compact_leaves_radix_t)radix_code) {
  case COMPACT_LEAVES_RADIX_QUARTER:
    return EQUITY_RESOLUTION / 4;
  case COMPACT_LEAVES_RADIX_HALF:
    return EQUITY_RESOLUTION / 2;
  case COMPACT_LEAVES_RADIX_WHOLE:
    return EQUITY_RESOLUTION;
  case COMPACT_LEAVES_RADIX_EIGHTH:
  default:
    return EQUITY_RESOLUTION / 8;
  }
}

void compact_leaves_destroy(CompactLeaves *cl);

void compact_leaves_write_to_file(const CompactLeaves *cl, const char *filename,
                                  ErrorStack *error_stack);

CompactLeaves *compact_leaves_read_from_file(const char *filename,
                                             ErrorStack *error_stack);

// Approximate leave value as an Equity (millipoints), accumulated in tick space
// and scaled by the radix once. Drops into the existing additive use site
// (static_eval) with the same contract as klv_get_leave_value. Empty leave ->
// 0.
Equity compact_leaves_get_leave_value(const CompactLeaves *cl,
                                      const Rack *leave);

#endif
