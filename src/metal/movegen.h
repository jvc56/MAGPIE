#ifndef METAL_MOVEGEN_H
#define METAL_MOVEGEN_H

// C-callable interface to the Metal WMPG (WMP-on-GPU) movegen kernels. The
// implementation lives in movegen_impl.m and is only linked on Darwin (the
// Makefile gates it on uname=Darwin). Callers that may run on other
// platforms should test for `gpu_matcher_is_available()` before using the
// rest of the API.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GpuMatcher GpuMatcher;

bool gpu_matcher_is_available(void);

// Create a matcher backed by the compiled metallib at `metallib_path`. The
// caller must subsequently call gpu_matcher_load_wmpg() with a flattened
// WMPG buffer (built via wmpg_build) before dispatching any kernels.
// Returns NULL on failure.
GpuMatcher *gpu_matcher_create(const char *metallib_path);

void gpu_matcher_destroy(GpuMatcher *m);

// Upload a flattened WMPG buffer (from wmpg_build) to the GPU. Must be called
// once after gpu_matcher_create. The matcher takes its own copy; the caller
// can free wmpg_bytes after this returns. Returns false on parse error.
bool gpu_matcher_load_wmpg(GpuMatcher *m, const uint8_t *wmpg_bytes,
                           size_t wmpg_size);

// Counts-only WMPG dispatch: per-rack thread enumerates subracks and hashes
// each into the per-length WMP buckets. Output: B uint32s, one count per
// rack (overwritten, not accumulated). Currently does NOT use the blank-1 /
// blank-2 sub-tables (counts-with-blanks would need an extension parallel
// to the top-1 kernels). Returns wall-clock seconds.
double
gpu_matcher_count_wmpg(GpuMatcher *m, uint32_t word_length,
                       uint32_t target_used_size, const uint8_t *racks_data,
                       uint32_t B, const uint64_t *cross_sets,
                       const uint8_t *fixed_letters, uint64_t fixed_bitrack_low,
                       uint64_t fixed_bitrack_high, uint32_t *out_counts);

// WMPG-based (count, equity_sum) per rack. Output: 2*B uint32s.
// out[2i]=count, out[2i+1]=equity_sum (raw millipoints stored as uint32 via
// atomic_fetch_add — addition wraps consistently in two's complement).
double gpu_matcher_equity_wmpg(
    GpuMatcher *m, uint32_t word_length, uint32_t target_used_size,
    const uint8_t *racks_data, uint32_t B, const uint64_t *cross_sets,
    const uint8_t *fixed_letters, uint64_t fixed_bitrack_low,
    uint64_t fixed_bitrack_high, const int32_t *letter_scores,
    const int32_t *position_multipliers, int32_t base_score,
    int32_t bingo_to_add, const uint8_t *leave_used,
    const int32_t *leave_values, uint32_t n_leaves,
    uint32_t *out_count_equity_pairs);

// Top-1 (best move per rack by equity), full millipoint precision. Two-pass
// design: pass 1 atomic_max on int32 millipoints accumulating across all
// slot dispatches; pass 2 (separate kernel) re-runs match+equity and updates
// a packed locator for each match whose equity_mp equals the per-rack max
// from pass 1. Two tiebreak modes available — see GpuTop1Tiebreak.
//
// best_loc[i] packing (decode same way regardless of mode):
//   bits 31..28: row (4)
//   bits 27..24: col (4)
//   bit 23:      dir (0 = horizontal)
//   bits 22..19: length (4)
//   bits 18..0:  word_idx within length slice (19) — for WMPG this is a
//                per-thread anagram counter rather than a flat-lex index,
//                so canonical tiebreaks within a slot at exact equity ties
//                may pick a different (still-correct) word than MAGPIE's
//                compare_moves tiebreak chain.
// Sentinel 0xFFFFFFFF = no match for that rack.
//
// best_eq_mp[i]: int32 millipoints; INT32_MIN if no match.
//
// Blank racks (1 or 2 blanks) trigger blank-1 / blank-2 sub-table lookups
// inside the kernel; the blank tile is placed at the X-position with the
// smallest position_multipliers[p] to maximize equity (since blank scores 0
// at p, this minimizes the discount letter_scores[X] *
// position_multipliers[p]).
typedef enum {
  // Speedy: CAS first-wins. One atomic op per match, non-deterministic across
  // ties (whichever thread schedules first wins). Slightly faster but may
  // disagree with MAGPIE on rare exact equity ties.
  GPU_TOP1_TIEBREAK_SPEEDY = 0,
  // Canonical: atomic_fetch_min on packed locator. Smaller (row, col, dir,
  // length, word_idx) wins. Deterministic.
  GPU_TOP1_TIEBREAK_CANONICAL = 1,
} GpuTop1Tiebreak;

void gpu_matcher_top1_reset(GpuMatcher *m, uint32_t B);

// `leave_stride` (in entries): 0 = shared leaves (all racks read leave_used
// at offset 0..n_leaves-1). > 0 = per-rack leaves; rack i reads leaves at
// [i*stride, i*stride+n_leaves). Caller pre-pads each rack's leave block to
// `stride` entries with sentinel-unmatching bitracks (e.g. all-0xFF) and
// pads leave_values correspondingly. Buffers must be sized B*stride entries.
// Use this to dispatch B distinct racks (each with its own leave table) in
// a single batched kernel call.
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

void gpu_matcher_top1_read(GpuMatcher *m, uint32_t B, int32_t *out_best_eq_mp,
                           uint32_t *out_best_loc);

// Profiling: each dispatch records its GPU execution time (from
// MTLCommandBuffer.GPUStartTime/GPUEndTime) into matcher state. Read after a
// dispatch returns to attribute wall time to (dispatch overhead + GPU exec).
// Returns 0 if no GPU timing is available.
double gpu_matcher_get_last_gpu_us(const GpuMatcher *m);

#ifdef __cplusplus
}
#endif

#endif // METAL_MOVEGEN_H
