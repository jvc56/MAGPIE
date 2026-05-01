#ifndef METAL_MOVEGEN_H
#define METAL_MOVEGEN_H

// C-callable interface to the Metal subset-match kernels. The implementation
// lives in movegen_impl.m and is only linked on Darwin (the Makefile gates
// it on uname=Darwin). Callers that may run on other platforms should test
// for `gpu_matcher_is_available()` before using the rest of the API.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GpuMatcher GpuMatcher;

bool gpu_matcher_is_available(void);

// Create a matcher that owns Metal-side copies of the lexicon's bitracks
// (total_words * 16 bytes) and optionally the letters block (variable bytes,
// concatenated length-major matching the bitracks order). Pass letters_data
// = NULL and total_letters_bytes = 0 if only subset-only kernels are needed.
// `metallib_path` must point to the compiled movegen.metallib. Returns NULL
// on failure. The caller-supplied buffers can be freed after this.
GpuMatcher *gpu_matcher_create(const char *metallib_path,
                               const uint8_t *bitracks_data,
                               uint32_t total_words,
                               const uint8_t *letters_data,
                               size_t total_letters_bytes);

void gpu_matcher_destroy(GpuMatcher *m);

// Dispatch count_kernel against words [first_word, first_word + n_words) of
// the loaded lexicon, for B racks. `racks_data` is B*16 bytes of packed
// BitRacks. `out_counts` (B uint32s) receives the number of subset-matches
// per rack (overwritten, not accumulated). Returns wall-clock seconds for
// commit + waitUntilCompleted.
double gpu_matcher_count(GpuMatcher *m, uint32_t first_word, uint32_t n_words,
                         const uint8_t *racks_data, uint32_t B,
                         uint32_t *out_counts);

// Cross-check-filtered count. For each (rack, word) pair in word range
// [first_word, first_word + n_words) of the loaded lexicon (assumed all of
// length `word_length`), counts a match iff:
//   1. word.bitrack ⊆ rack.bitrack
//   2. for each i in 0..word_length-1, bit (1ULL << word.letters[i]) is set
//      in cross_sets[i].
// `letters_byte_offset` is the byte offset into the loaded letters block
// where word `first_word`'s letters begin (= sum_{L<word_length}(count[L]*L)
// in flat-lex layout).
// `cross_sets` is `word_length` uint64s; the caller owns the storage.
// `out_counts` is overwritten (not accumulated). Returns wall time.
double gpu_matcher_count_with_cross(
    GpuMatcher *m, uint32_t first_word, uint32_t n_words,
    size_t letters_byte_offset, uint32_t word_length, const uint8_t *racks_data,
    uint32_t B, const uint64_t *cross_sets, uint32_t *out_counts);

// Playthrough-aware count. Like count_with_cross but additionally takes:
//   fixed_letters[word_length] — 0 = empty, else the playthrough tile's
//     unblanked machine letter at that slot position
//   fixed_bitrack — 16 bytes; BitRack of the playthrough tiles
// At empty positions (fixed_letters[i]==0) the cross-set must allow the
// word's letter; at fixed positions the word's letter must equal exactly.
// Subset test runs against rack+fixed_bitrack (the "effective rack").
double gpu_matcher_count_with_playthrough(
    GpuMatcher *m, uint32_t first_word, uint32_t n_words,
    size_t letters_byte_offset, uint32_t word_length, const uint8_t *racks_data,
    uint32_t B, const uint64_t *cross_sets, const uint8_t *fixed_letters,
    const uint8_t *fixed_bitrack, uint32_t *out_counts);

// Score-and-count. Same matching semantics as count_with_playthrough plus
// computes the placement score per match and accumulates per rack. Host
// pre-computes per-slot constants:
//   letter_scores[32] — per machine letter (0 for blank)
//   position_multipliers[word_length] — 0 at fixed positions; else
//       letter_mult[i] * (prod_word_mult + is_cross_word[i] *
//       this_word_mult[i])
//   base_score — (sum of fixed letter scores) * prod_word_mult +
//   hooked_cross_total bingo_to_add — bingo_bonus if all positions placed AND
//   placed_count==RACK_SIZE
// Output: 2*B uint32s; out[2i]=count for rack i, out[2i+1]=score_sum for rack
// i.
double
gpu_matcher_score(GpuMatcher *m, uint32_t first_word, uint32_t n_words,
                  size_t letters_byte_offset, uint32_t word_length,
                  const uint8_t *racks_data, uint32_t B,
                  const uint64_t *cross_sets, const uint8_t *fixed_letters,
                  const uint8_t *fixed_bitrack, const int32_t *letter_scores,
                  const int32_t *position_multipliers, int32_t base_score,
                  int32_t bingo_to_add, uint32_t *out_count_score_pairs);

// Equity (score + leave_value) per match. Same matching/scoring as score
// kernel, then per-match used_bitrack = word.bitrack - fixed_bitrack is
// looked up in a per-rack leave table (`leave_used[n_leaves*16 bytes]` +
// `leave_values[n_leaves]`) via linear scan; equity = score + leave_value.
// Leave table is host-precomputed by enumerating all subracks of the rack
// (multisets ⊆ rack) and calling klv_get_leave_value for each.
// Output: 2*B uint32s; out[2i]=count, out[2i+1]=equity_sum (in whatever
// units letter_scores and leave_values are passed in — typically Equity
// millipoints).
double
gpu_matcher_equity(GpuMatcher *m, uint32_t first_word, uint32_t n_words,
                   size_t letters_byte_offset, uint32_t word_length,
                   const uint8_t *racks_data, uint32_t B,
                   const uint64_t *cross_sets, const uint8_t *fixed_letters,
                   const uint8_t *fixed_bitrack, const int32_t *letter_scores,
                   const int32_t *position_multipliers, int32_t base_score,
                   int32_t bingo_to_add, const uint8_t *leave_used,
                   const int32_t *leave_values, uint32_t n_leaves,
                   uint32_t *out_count_equity_pairs);

// Top-1 (best move per rack by equity), full millipoint precision.
// Two-pass design: pass 1 atomic_max on int32 millipoints accumulating across
// all slot dispatches; pass 2 (separate kernel) re-runs match+equity and
// updates a packed locator for each match whose equity_mp equals the per-rack
// max from pass 1. Two tiebreak modes available — see GpuTop1Tiebreak.
//
// best_loc[i] packing (decode same way regardless of mode):
//   bits 31..28: row (4)
//   bits 27..24: col (4)
//   bit 23:      dir (0 = horizontal)
//   bits 22..19: length (4)
//   bits 18..0:  word_idx within length slice (19)
// Sentinel 0xFFFFFFFF = no match for that rack.
//
// best_eq_mp[i]: int32 millipoints; INT32_MIN if no match.
typedef enum {
  // Speedy: CAS first-wins. One atomic op per match, non-deterministic across
  // ties (whichever thread schedules first wins). Slightly faster but may
  // disagree with MAGPIE on rare exact equity ties.
  GPU_TOP1_TIEBREAK_SPEEDY = 0,
  // Canonical: atomic_fetch_min on packed locator. Smaller (row, col, dir,
  // length, word_idx) wins, which exactly matches MAGPIE's compare_moves
  // tiebreak chain (modulo the score-vs-leave decomposition that's identical
  // when equities tie at a single slot). Deterministic.
  GPU_TOP1_TIEBREAK_CANONICAL = 1,
} GpuTop1Tiebreak;

void gpu_matcher_top1_reset(GpuMatcher *m, uint32_t B);
// `leave_stride` (in entries): 0 = shared leaves (all racks read leave_used
// at offset 0..n_leaves-1, legacy behavior). > 0 = per-rack leaves; rack i
// reads leaves at [i*stride, i*stride+n_leaves). Caller pre-pads each rack's
// leave block to `stride` entries with sentinel-unmatching bitracks (e.g.
// all-0xFF) and pads leave_values correspondingly. Buffers must be sized
// B*stride entries. Use this to dispatch B distinct racks (each with its own
// leave table) in a single batched kernel call.
double gpu_matcher_top1_pass1(
    GpuMatcher *m, uint32_t first_word, uint32_t n_words,
    size_t letters_byte_offset, uint32_t word_length, const uint8_t *racks_data,
    uint32_t B, const uint64_t *cross_sets, const uint8_t *fixed_letters,
    const uint8_t *fixed_bitrack, const int32_t *letter_scores,
    const int32_t *position_multipliers, int32_t base_score,
    int32_t bingo_to_add, const uint8_t *leave_used,
    const int32_t *leave_values, uint32_t n_leaves, uint32_t leave_stride);
double gpu_matcher_top1_pass2(
    GpuMatcher *m, uint32_t first_word, uint32_t n_words,
    size_t letters_byte_offset, uint32_t word_length, const uint8_t *racks_data,
    uint32_t B, const uint64_t *cross_sets, const uint8_t *fixed_letters,
    const uint8_t *fixed_bitrack, const int32_t *letter_scores,
    const int32_t *position_multipliers, int32_t base_score,
    int32_t bingo_to_add, const uint8_t *leave_used,
    const int32_t *leave_values, uint32_t n_leaves, uint32_t leave_stride,
    uint32_t row, uint32_t col, uint32_t dir, GpuTop1Tiebreak mode);
void gpu_matcher_top1_read(GpuMatcher *m, uint32_t B, int32_t *out_best_eq_mp,
                           uint32_t *out_best_loc);

// WMPG (WMP-on-GPU): the matcher can also hold a flattened WMP buffer for
// hash-table-based lookup, replacing the brute-force "scan every word"
// approach. Workflow:
//   1. Build WMPG bytes via wmpg_build/wmpg_make (CPU-side).
//   2. Call gpu_matcher_load_wmpg() once to upload to GPU.
//   3. Per slot, call gpu_matcher_count_wmpg() with the slot's length and
//      constants. The matcher uses the WMPG length-meta table internally to
//      compute the right buffer offsets.
//
// The kernel matches MAGPIE's wfl_get_word_entry semantics line-for-line:
// hash with bit_rack_mix_to_64, walk bucket [start, end), compare bitrack,
// decode inlined or non-inlined entry, iterate anagrams, validate
// cross-checks + playthrough letter matches, accumulate count.
bool gpu_matcher_load_wmpg(GpuMatcher *m, const uint8_t *wmpg_bytes,
                           size_t wmpg_size);

double
gpu_matcher_count_wmpg(GpuMatcher *m, uint32_t word_length,
                       uint32_t target_used_size, const uint8_t *racks_data,
                       uint32_t B, const uint64_t *cross_sets,
                       const uint8_t *fixed_letters, uint64_t fixed_bitrack_low,
                       uint64_t fixed_bitrack_high, uint32_t *out_counts);

// WMPG-based equity. Same matching+scoring+leave logic as
// gpu_matcher_equity, but per-rack thread does subrack enumeration + hash
// lookup instead of per-(rack,word) brute-force scan. Output: 2*B uint32s
// (count, equity_sum) per rack.
double gpu_matcher_equity_wmpg(
    GpuMatcher *m, uint32_t word_length, uint32_t target_used_size,
    const uint8_t *racks_data, uint32_t B, const uint64_t *cross_sets,
    const uint8_t *fixed_letters, uint64_t fixed_bitrack_low,
    uint64_t fixed_bitrack_high, const int32_t *letter_scores,
    const int32_t *position_multipliers, int32_t base_score,
    int32_t bingo_to_add, const uint8_t *leave_used,
    const int32_t *leave_values, uint32_t n_leaves,
    uint32_t *out_count_equity_pairs);

// WMPG-based top-1 (best move per rack by equity), two-pass design analogous
// to gpu_matcher_top1_pass1/pass2 but using WMPG hash lookup instead of
// brute-force scan. Caller must call gpu_matcher_top1_reset(m, B) before the
// first pass-1 dispatch in a sequence.
//
// Pass 1: per slot, atomic_fetch_max on best_eq_mp[i] (int32 millipoints)
//   accumulating across all slot dispatches.
// Pass 2: per slot, re-runs match+equity; for each match whose equity_mp
//   equals best_eq_mp[i], updates best_loc[i] using the chosen tiebreak mode
//   (speedy CAS-first-wins, or canonical atomic_min on packed locator).
//
// Locator packing matches the brute-force top1_loc_kernel:
//   bits 31..28 row | 27..24 col | 23 dir | 22..19 length | 18..0 word_idx
// CAVEAT (WMPG only): WMPG has no flat-lex word_idx, so the 19-bit field is a
// per-thread anagram counter (validated-anagram order seen by the rack thread
// in this dispatch). Within (row,col,dir,length) the canonical tiebreak picks
// the smallest WMPG counter, which may differ from MAGPIE's flat-lex tiebreak
// when multiple words at the same slot tie at the exact same equity. The
// chosen move is still a correct best-equity move.
double gpu_matcher_top1_pass1_wmpg(
    GpuMatcher *m, uint32_t word_length, uint32_t target_used_size,
    const uint8_t *racks_data, uint32_t B, const uint64_t *cross_sets,
    const uint8_t *fixed_letters, uint64_t fixed_bitrack_low,
    uint64_t fixed_bitrack_high, const int32_t *letter_scores,
    const int32_t *position_multipliers, int32_t base_score,
    int32_t bingo_to_add, const uint8_t *leave_used,
    const int32_t *leave_values, uint32_t n_leaves, uint32_t leave_stride);

double gpu_matcher_top1_pass2_wmpg(
    GpuMatcher *m, uint32_t word_length, uint32_t target_used_size,
    const uint8_t *racks_data, uint32_t B, const uint64_t *cross_sets,
    const uint8_t *fixed_letters, uint64_t fixed_bitrack_low,
    uint64_t fixed_bitrack_high, const int32_t *letter_scores,
    const int32_t *position_multipliers, int32_t base_score,
    int32_t bingo_to_add, const uint8_t *leave_used,
    const int32_t *leave_values, uint32_t n_leaves, uint32_t leave_stride,
    uint32_t row, uint32_t col, uint32_t dir, GpuTop1Tiebreak mode);

// Profiling: each dispatch records its GPU execution time (from
// MTLCommandBuffer.GPUStartTime/GPUEndTime) into matcher state. Read after a
// dispatch returns to attribute wall time to (dispatch overhead + GPU exec).
// Returns 0 if no GPU timing is available (e.g., on platforms that don't
// populate the timestamps).
double gpu_matcher_get_last_gpu_us(const GpuMatcher *m);

#ifdef __cplusplus
}
#endif

#endif // METAL_MOVEGEN_H
