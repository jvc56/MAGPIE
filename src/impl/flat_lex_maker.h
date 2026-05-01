#ifndef FLAT_LEX_MAKER_H
#define FLAT_LEX_MAKER_H

// Flat lexicon: a GPU-friendly word file.
//
// Each word is stored twice — once as a 16-byte BitRack (per-letter count
// vector, the canonical multiset fingerprint) and once as its raw
// MachineLetter sequence. Words are grouped by length so a kernel can scan
// only words of a given length, and within each length the BitRacks form a
// tight contiguous array suitable for SIMD-coalesced reads.
//
// File format (little-endian throughout):
//
//   Header (16 bytes):
//     [0..3]   magic "FLEX"
//     [4]      version major (currently 1)
//     [5]      alphabet_size (e.g. 27 for English)
//     [6]      max_word_length_plus_one (typically BOARD_DIM+1)
//     [7]      bitrack_bytes (always 16)
//     [8..11]  total_word_count
//     [12..15] reserved (zero)
//
//   Count table: max_word_length_plus_one * 4 bytes
//     count[L] = number of words of length L
//
//   BitRacks block: total_word_count * 16 bytes
//     Words ordered by length (L=0,1,...,max), within each length in lex
//     order of their MachineLetter sequence. Each entry is a BitRack stored
//     little-endian.
//
//   Letters block: sum over L of count[L]*L bytes
//     Same ordering as BitRacks. Word_i's letters span L_i contiguous bytes.
//
// At load time a reader can compute per-length offsets from the count table:
//   bitracks_for_len[L]_offset = sum_{i<L}(count[i]) * 16
//   letters_for_len[L]_offset  = sum_{i<L}(count[i] * i)

#include "../ent/kwg.h"
#include "../ent/letter_distribution.h"
#include "../util/io_util.h"

// Build the flat-lex bytes in memory. On success *out_bytes is a malloc'd
// buffer of *out_size bytes (caller frees). On failure *out_bytes is NULL
// and the error stack is populated.
void flat_lex_build(const KWG *kwg, const LetterDistribution *ld,
                    uint8_t **out_bytes, size_t *out_size,
                    ErrorStack *error_stack);

// Build the flat-lex bytes and write them to a file.
void flat_lex_make(const KWG *kwg, const LetterDistribution *ld,
                   const char *output_path, ErrorStack *error_stack);

#endif // FLAT_LEX_MAKER_H
