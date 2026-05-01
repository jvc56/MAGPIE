#ifndef WMPG_MAKER_H
#define WMPG_MAKER_H

// WMP-on-GPU: flatten an in-memory WMP into a single contiguous buffer
// suitable for upload to a Metal MTLBuffer. Preserves WMP's bucket+entry
// layout exactly so the GPU kernel mirrors `wfl_get_word_entry`,
// `wfl_get_blank_entry`, and `wfl_get_double_blank_entry`.
//
// File format v2 (little-endian throughout):
//
//   Header (32 bytes):
//     [0..3]   magic "WMPG"
//     [4]      version major (2 — adds blank-1 + blank-2 tables vs v1)
//     [5]      alphabet_size
//     [6]      max_word_length_plus_one (= BOARD_DIM + 1)
//     [7]      bitrack_bytes (16)
//     [8..11]  total_bucket_starts_count (sum across all 3 tables, all lengths)
//     [12..15] total_entries (sum across all 3 tables, all lengths)
//     [16..19] total_uninlined_letter_bytes (word table only)
//     [20..23] sections_offset (= 32 + max_len_plus_one * 56)
//     [24..27] meta_bytes_per_length (= 56 in v2)
//     [28..31] reserved
//
//   Per-length metadata: max_word_length_plus_one * 56 bytes.
//
//     For each L, three sub-blocks:
//
//     Word (blank-0) table — 24 bytes:
//       [ 0.. 3] word_num_buckets         (power of 2; 0 if no entries)
//       [ 4.. 7] word_bucket_starts_off   (offset within buckets section)
//       [ 8..11] word_num_entries
//       [12..15] word_entries_off         (offset within entries section)
//       [16..19] word_num_uninlined_words
//       [20..23] word_uninlined_letters_off  (offset within letters section)
//
//     Blank-1 (single-blank) table — 16 bytes:
//       [24..27] b1_num_buckets
//       [28..31] b1_bucket_starts_off
//       [32..35] b1_num_entries
//       [36..39] b1_entries_off
//
//     Blank-2 (double-blank) table — 16 bytes:
//       [40..43] b2_num_buckets
//       [44..47] b2_bucket_starts_off
//       [48..51] b2_num_entries
//       [52..55] b2_entries_off
//
//   Buckets section: total_bucket_starts_count * 4 bytes (uint32 each)
//     Concatenated bucket_starts arrays. For a given (table, L), the array
//     contains (num_buckets + 1) entries; last is sentinel == num_entries.
//     Order: for L = 0..max-1, [word_starts(L), b1_starts(L), b2_starts(L)].
//
//   Entries section: total_entries * 32 bytes
//     Concatenated entries. Order: word, b1, b2 per length.
//     Each entry preserves WMP's WMPEntry layout exactly:
//       [0]       nonzero_if_inlined (or first inline letter)
//       [1..7]    inline padding (or inline letter bytes)
//       [8..11]   word_start (b0) / blank_letters bitvector (b1) /
//                 first_blank_letters bitvector (b2)
//       [12..15]  num_words (b0) / unused (b1, b2)
//       [16..23]  bitrack low (uint64 little-endian)
//       [24..31]  bitrack high
//
//   Letters section: total_uninlined_letter_bytes
//     Concatenated word-table uninlined_letters. Blank-1 and blank-2 entries
//     have no letters (only bitvectors of fillable letters).
//
// Lookup:
//   - 0 blanks: word table; bucket = mix64(target) & (word_num_buckets-1);
//     scan for matching bitrack; decode inlined / non-inlined like v1.
//   - 1 blank:  blank-1 table; entry's blank_letters bitvector tells which
//     letters could fill the blank with at least one solution. For each set
//     bit i, do a follow-up word-table lookup with bitrack having blank
//     replaced by i to enumerate the actual anagrams.
//   - 2 blanks: blank-2 table; entry's first_blank_letters bitvector. For
//     each set bit i, do a follow-up blank-1 lookup with one blank replaced
//     by i (and blank count reduced to 1) to enumerate the second-blank
//     letters; then word-table lookup as in the 1-blank case.

#include "../ent/wmp.h"
#include "../util/io_util.h"
#include <stddef.h>
#include <stdint.h>

// Allocates and fills *out_bytes (caller frees). Reads from `wmp` (as loaded
// by the standard MAGPIE config flow) and emits the flat buffer.
void wmpg_build(const WMP *wmp, uint8_t **out_bytes, size_t *out_size,
                ErrorStack *error_stack);

void wmpg_make(const WMP *wmp, const char *output_path,
               ErrorStack *error_stack);

#endif // WMPG_MAKER_H
