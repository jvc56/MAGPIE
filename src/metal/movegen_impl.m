// Metal implementation of GpuMatcher. C API exposed via movegen.h.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "movegen.h"

#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BITRACK_BYTES 16

struct GpuMatcher {
  void *device;                              // id<MTLDevice>
  void *queue;                               // id<MTLCommandQueue>
  void *count_pipeline;                      // id<MTLComputePipelineState>
  void *count_with_cross_pipeline;           // id<MTLComputePipelineState>
  void *count_with_playthrough_pipeline;     // id<MTLComputePipelineState>
  void *score_with_playthrough_pipeline;     // id<MTLComputePipelineState>
  void *equity_with_playthrough_pipeline;    // id<MTLComputePipelineState>
  void *top1_eqmp_pipeline;                  // id<MTLComputePipelineState>
  void *top1_loc_pipeline;                   // id<MTLComputePipelineState>
  void *best_eq_mp_buf;                      // id<MTLBuffer>, B int32s
  void *best_loc_buf;                        // id<MTLBuffer>, B uint32s
  uint32_t top1_cap_b;
  double last_gpu_us;                        // GPU exec time of last dispatch
  void *bitracks_buf;                        // id<MTLBuffer> lexicon bitracks
  void *letters_buf;                         // id<MTLBuffer> letters or nil
  uint32_t total_words;
  size_t total_letters_bytes;
  void *racks_buf;                           // id<MTLBuffer>, grow-on-demand
  void *counts_buf;                          // id<MTLBuffer>, grow-on-demand
  uint32_t cap_b;
  // Managed leave buffers for batched per-rack leave tables (used when
  // leave_stride > 0; setBytes can't fit B * stride entries past ~4 KB).
  // Sized to hold B * stride entries; grow-on-demand.
  void *leave_used_buf;                      // id<MTLBuffer>
  void *leave_values_buf;                    // id<MTLBuffer>
  size_t leave_used_cap_bytes;
  size_t leave_values_cap_bytes;
  // WMPG (WMP-on-GPU) state
  void *count_wmpg_pipeline;                 // id<MTLComputePipelineState>
  void *equity_wmpg_pipeline;                // id<MTLComputePipelineState>
  void *top1_eqmp_wmpg_pipeline;             // id<MTLComputePipelineState>
  void *top1_loc_wmpg_pipeline;              // id<MTLComputePipelineState>
  void *wmpg_buf;                            // id<MTLBuffer>, full WMPG file
  uint32_t wmpg_max_len_plus_one;
  // Word-table (blank-0) per-length offsets (existing).
  uint32_t wmpg_num_buckets[16];
  uint32_t wmpg_bucket_starts_byte_offset[16];
  uint32_t wmpg_entries_byte_offset[16];
  uint32_t wmpg_uninlined_letters_byte_offset[16];
  // Blank-1 (single-blank) table per-length offsets (v2).
  uint32_t wmpg_b1_num_buckets[16];
  uint32_t wmpg_b1_bucket_starts_byte_offset[16];
  uint32_t wmpg_b1_entries_byte_offset[16];
  // Blank-2 (double-blank) table per-length offsets (v2).
  uint32_t wmpg_b2_num_buckets[16];
  uint32_t wmpg_b2_bucket_starts_byte_offset[16];
  uint32_t wmpg_b2_entries_byte_offset[16];
};

static double mach_seconds(uint64_t ticks) {
  static mach_timebase_info_data_t tb = {0, 0};
  if (tb.denom == 0) {
    mach_timebase_info(&tb);
  }
  return (double)ticks * (double)tb.numer / (double)tb.denom / 1e9;
}

bool gpu_matcher_is_available(void) {
  @autoreleasepool {
    return MTLCreateSystemDefaultDevice() != nil;
  }
}

GpuMatcher *gpu_matcher_create(const char *metallib_path,
                               const uint8_t *bitracks_data,
                               uint32_t total_words,
                               const uint8_t *letters_data,
                               size_t total_letters_bytes) {
  if (metallib_path == NULL || bitracks_data == NULL || total_words == 0) {
    return NULL;
  }
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil) {
      fprintf(stderr, "gpu_matcher: no Metal device\n");
      return NULL;
    }
    NSString *path = [NSString stringWithUTF8String:metallib_path];
    NSError *err = nil;
    id<MTLLibrary> lib =
        [device newLibraryWithURL:[NSURL fileURLWithPath:path] error:&err];
    if (lib == nil) {
      fprintf(stderr, "gpu_matcher: failed to load %s: %s\n", metallib_path,
              err ? [[err localizedDescription] UTF8String] : "(no error)");
      return NULL;
    }
    id<MTLFunction> countFn = [lib newFunctionWithName:@"count_kernel"];
    id<MTLFunction> countCrossFn =
        [lib newFunctionWithName:@"count_with_cross_kernel"];
    id<MTLFunction> countPlaythroughFn =
        [lib newFunctionWithName:@"count_with_playthrough_kernel"];
    id<MTLFunction> scoreFn =
        [lib newFunctionWithName:@"score_with_playthrough_kernel"];
    id<MTLFunction> equityFn =
        [lib newFunctionWithName:@"equity_with_playthrough_kernel"];
    id<MTLFunction> top1EqmpFn =
        [lib newFunctionWithName:@"top1_eqmp_kernel"];
    id<MTLFunction> top1LocFn =
        [lib newFunctionWithName:@"top1_loc_kernel"];
    id<MTLFunction> countWmpgFn =
        [lib newFunctionWithName:@"count_wmpg_kernel"];
    id<MTLFunction> equityWmpgFn =
        [lib newFunctionWithName:@"equity_wmpg_kernel"];
    id<MTLFunction> top1EqmpWmpgFn =
        [lib newFunctionWithName:@"top1_eqmp_wmpg_kernel"];
    id<MTLFunction> top1LocWmpgFn =
        [lib newFunctionWithName:@"top1_loc_wmpg_kernel"];
    if (countFn == nil || countCrossFn == nil ||
        countPlaythroughFn == nil || scoreFn == nil || equityFn == nil ||
        top1EqmpFn == nil || top1LocFn == nil || countWmpgFn == nil ||
        equityWmpgFn == nil || top1EqmpWmpgFn == nil ||
        top1LocWmpgFn == nil) {
      fprintf(stderr, "gpu_matcher: kernel not found in metallib\n");
      return NULL;
    }
    id<MTLComputePipelineState> ps =
        [device newComputePipelineStateWithFunction:countFn error:&err];
    id<MTLComputePipelineState> psCross =
        [device newComputePipelineStateWithFunction:countCrossFn error:&err];
    id<MTLComputePipelineState> psPlaythrough = [device
        newComputePipelineStateWithFunction:countPlaythroughFn error:&err];
    id<MTLComputePipelineState> psScore =
        [device newComputePipelineStateWithFunction:scoreFn error:&err];
    id<MTLComputePipelineState> psEquity =
        [device newComputePipelineStateWithFunction:equityFn error:&err];
    id<MTLComputePipelineState> psTop1Eqmp =
        [device newComputePipelineStateWithFunction:top1EqmpFn error:&err];
    id<MTLComputePipelineState> psTop1Loc =
        [device newComputePipelineStateWithFunction:top1LocFn error:&err];
    id<MTLComputePipelineState> psCountWmpg =
        [device newComputePipelineStateWithFunction:countWmpgFn error:&err];
    id<MTLComputePipelineState> psEquityWmpg =
        [device newComputePipelineStateWithFunction:equityWmpgFn error:&err];
    id<MTLComputePipelineState> psTop1EqmpWmpg =
        [device newComputePipelineStateWithFunction:top1EqmpWmpgFn error:&err];
    id<MTLComputePipelineState> psTop1LocWmpg =
        [device newComputePipelineStateWithFunction:top1LocWmpgFn error:&err];
    if (ps == nil || psCross == nil || psPlaythrough == nil ||
        psScore == nil || psEquity == nil || psTop1Eqmp == nil ||
        psTop1Loc == nil || psCountWmpg == nil || psEquityWmpg == nil ||
        psTop1EqmpWmpg == nil || psTop1LocWmpg == nil) {
      fprintf(stderr, "gpu_matcher: pipeline create failed: %s\n",
              err ? [[err localizedDescription] UTF8String] : "(no error)");
      return NULL;
    }
    id<MTLCommandQueue> queue = [device newCommandQueue];

    const size_t lex_bytes = (size_t)total_words * BITRACK_BYTES;
    id<MTLBuffer> bitracks =
        [device newBufferWithBytes:bitracks_data
                            length:lex_bytes
                           options:MTLResourceStorageModeShared];
    id<MTLBuffer> letters = nil;
    if (letters_data != NULL && total_letters_bytes > 0) {
      letters = [device newBufferWithBytes:letters_data
                                    length:total_letters_bytes
                                   options:MTLResourceStorageModeShared];
    }

    GpuMatcher *m = (GpuMatcher *)calloc(1, sizeof(GpuMatcher));
    m->device = (__bridge_retained void *)device;
    m->queue = (__bridge_retained void *)queue;
    m->count_pipeline = (__bridge_retained void *)ps;
    m->count_with_cross_pipeline = (__bridge_retained void *)psCross;
    m->count_with_playthrough_pipeline =
        (__bridge_retained void *)psPlaythrough;
    m->score_with_playthrough_pipeline = (__bridge_retained void *)psScore;
    m->equity_with_playthrough_pipeline = (__bridge_retained void *)psEquity;
    m->top1_eqmp_pipeline = (__bridge_retained void *)psTop1Eqmp;
    m->top1_loc_pipeline = (__bridge_retained void *)psTop1Loc;
    m->count_wmpg_pipeline = (__bridge_retained void *)psCountWmpg;
    m->equity_wmpg_pipeline = (__bridge_retained void *)psEquityWmpg;
    m->top1_eqmp_wmpg_pipeline = (__bridge_retained void *)psTop1EqmpWmpg;
    m->top1_loc_wmpg_pipeline = (__bridge_retained void *)psTop1LocWmpg;
    m->wmpg_buf = NULL;
    m->wmpg_max_len_plus_one = 0;
    m->best_eq_mp_buf = NULL;
    m->best_loc_buf = NULL;
    m->top1_cap_b = 0;
    m->last_gpu_us = 0;
    m->bitracks_buf = (__bridge_retained void *)bitracks;
    m->letters_buf =
        (letters != nil) ? (__bridge_retained void *)letters : NULL;
    m->total_words = total_words;
    m->total_letters_bytes = total_letters_bytes;
    m->racks_buf = NULL;
    m->counts_buf = NULL;
    m->cap_b = 0;
    m->leave_used_buf = NULL;
    m->leave_values_buf = NULL;
    m->leave_used_cap_bytes = 0;
    m->leave_values_cap_bytes = 0;
    return m;
  }
}

void gpu_matcher_destroy(GpuMatcher *m) {
  if (m == NULL) {
    return;
  }
  if (m->device) {
    CFRelease(m->device);
  }
  if (m->queue) {
    CFRelease(m->queue);
  }
  if (m->count_pipeline) {
    CFRelease(m->count_pipeline);
  }
  if (m->count_with_cross_pipeline) {
    CFRelease(m->count_with_cross_pipeline);
  }
  if (m->count_with_playthrough_pipeline) {
    CFRelease(m->count_with_playthrough_pipeline);
  }
  if (m->score_with_playthrough_pipeline) {
    CFRelease(m->score_with_playthrough_pipeline);
  }
  if (m->equity_with_playthrough_pipeline) {
    CFRelease(m->equity_with_playthrough_pipeline);
  }
  if (m->top1_eqmp_pipeline) {
    CFRelease(m->top1_eqmp_pipeline);
  }
  if (m->top1_loc_pipeline) {
    CFRelease(m->top1_loc_pipeline);
  }
  if (m->count_wmpg_pipeline) {
    CFRelease(m->count_wmpg_pipeline);
  }
  if (m->equity_wmpg_pipeline) {
    CFRelease(m->equity_wmpg_pipeline);
  }
  if (m->top1_eqmp_wmpg_pipeline) {
    CFRelease(m->top1_eqmp_wmpg_pipeline);
  }
  if (m->top1_loc_wmpg_pipeline) {
    CFRelease(m->top1_loc_wmpg_pipeline);
  }
  if (m->wmpg_buf) {
    CFRelease(m->wmpg_buf);
  }
  if (m->best_eq_mp_buf) {
    CFRelease(m->best_eq_mp_buf);
  }
  if (m->best_loc_buf) {
    CFRelease(m->best_loc_buf);
  }
  if (m->bitracks_buf) {
    CFRelease(m->bitracks_buf);
  }
  if (m->letters_buf) {
    CFRelease(m->letters_buf);
  }
  if (m->racks_buf) {
    CFRelease(m->racks_buf);
  }
  if (m->counts_buf) {
    CFRelease(m->counts_buf);
  }
  if (m->leave_used_buf) {
    CFRelease(m->leave_used_buf);
  }
  if (m->leave_values_buf) {
    CFRelease(m->leave_values_buf);
  }
  free(m);
}

// Ensure leave_used_buf and leave_values_buf are sized for `B * stride`
// entries. Grows on demand. Caller will copy data in immediately after.
static void ensure_leave_buffers(GpuMatcher *m, size_t entries_needed) {
  const size_t lu_bytes = entries_needed * BITRACK_BYTES;
  const size_t lv_bytes = entries_needed * sizeof(int32_t);
  id<MTLDevice> device = (__bridge id<MTLDevice>)m->device;
  if (lu_bytes > m->leave_used_cap_bytes) {
    if (m->leave_used_buf) {
      CFRelease(m->leave_used_buf);
      m->leave_used_buf = NULL;
    }
    id<MTLBuffer> b = [device newBufferWithLength:lu_bytes
                                          options:MTLResourceStorageModeShared];
    m->leave_used_buf = (__bridge_retained void *)b;
    m->leave_used_cap_bytes = lu_bytes;
  }
  if (lv_bytes > m->leave_values_cap_bytes) {
    if (m->leave_values_buf) {
      CFRelease(m->leave_values_buf);
      m->leave_values_buf = NULL;
    }
    id<MTLBuffer> b = [device newBufferWithLength:lv_bytes
                                          options:MTLResourceStorageModeShared];
    m->leave_values_buf = (__bridge_retained void *)b;
    m->leave_values_cap_bytes = lv_bytes;
  }
}

static void ensure_batch_buffers(GpuMatcher *m, uint32_t B) {
  if (B <= m->cap_b) {
    return;
  }
  if (m->racks_buf) {
    CFRelease(m->racks_buf);
    m->racks_buf = NULL;
  }
  if (m->counts_buf) {
    CFRelease(m->counts_buf);
    m->counts_buf = NULL;
  }
  id<MTLDevice> device = (__bridge id<MTLDevice>)m->device;
  id<MTLBuffer> racks = [device
      newBufferWithLength:(NSUInteger)((size_t)B * BITRACK_BYTES)
                  options:MTLResourceStorageModeShared];
  // 2*uint32 per rack so we can share this buffer with score_kernel which
  // writes (count, score_sum) pairs. count_kernel only writes the first half
  // (stride 1 word), so we just allocate the larger size and use it for both.
  id<MTLBuffer> counts = [device
      newBufferWithLength:(NSUInteger)((size_t)B * 2 * sizeof(uint32_t))
                  options:MTLResourceStorageModeShared];
  m->racks_buf = (__bridge_retained void *)racks;
  m->counts_buf = (__bridge_retained void *)counts;
  m->cap_b = B;
}

double gpu_matcher_count(GpuMatcher *m, uint32_t first_word, uint32_t n_words,
                         const uint8_t *racks_data, uint32_t B,
                         uint32_t *out_counts) {
  if (m == NULL || racks_data == NULL || B == 0 || n_words == 0) {
    return 0.0;
  }
  if ((uint64_t)first_word + n_words > m->total_words) {
    return 0.0;
  }
  @autoreleasepool {
    ensure_batch_buffers(m, B);
    id<MTLBuffer> racks = (__bridge id<MTLBuffer>)m->racks_buf;
    id<MTLBuffer> counts = (__bridge id<MTLBuffer>)m->counts_buf;
    id<MTLBuffer> bitracks = (__bridge id<MTLBuffer>)m->bitracks_buf;
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)m->queue;
    id<MTLComputePipelineState> ps =
        (__bridge id<MTLComputePipelineState>)m->count_pipeline;

    memcpy(racks.contents, racks_data, (size_t)B * BITRACK_BYTES);
    memset(counts.contents, 0, (size_t)B * sizeof(uint32_t));

    const uint64_t t0 = mach_absolute_time();
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:ps];
    [enc setBuffer:bitracks
            offset:(NSUInteger)((size_t)first_word * BITRACK_BYTES)
           atIndex:0];
    [enc setBuffer:racks offset:0 atIndex:1];
    [enc setBytes:&n_words length:sizeof(uint32_t) atIndex:2];
    [enc setBuffer:counts offset:0 atIndex:3];
    [enc dispatchThreads:MTLSizeMake(n_words, B, 1)
        threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    m->last_gpu_us = (cb.GPUEndTime - cb.GPUStartTime) * 1e6;
    const double dt = mach_seconds(mach_absolute_time() - t0);

    if (out_counts != NULL) {
      memcpy(out_counts, counts.contents, (size_t)B * sizeof(uint32_t));
    }
    return dt;
  }
}

double gpu_matcher_count_with_cross(GpuMatcher *m, uint32_t first_word,
                                    uint32_t n_words,
                                    size_t letters_byte_offset,
                                    uint32_t word_length,
                                    const uint8_t *racks_data, uint32_t B,
                                    const uint64_t *cross_sets,
                                    uint32_t *out_counts) {
  if (m == NULL || racks_data == NULL || B == 0 || n_words == 0 ||
      cross_sets == NULL || word_length == 0 || m->letters_buf == NULL) {
    return 0.0;
  }
  if ((uint64_t)first_word + n_words > m->total_words) {
    return 0.0;
  }
  if (letters_byte_offset + (size_t)n_words * word_length >
      m->total_letters_bytes) {
    return 0.0;
  }
  @autoreleasepool {
    ensure_batch_buffers(m, B);
    id<MTLBuffer> racks = (__bridge id<MTLBuffer>)m->racks_buf;
    id<MTLBuffer> counts = (__bridge id<MTLBuffer>)m->counts_buf;
    id<MTLBuffer> bitracks = (__bridge id<MTLBuffer>)m->bitracks_buf;
    id<MTLBuffer> letters = (__bridge id<MTLBuffer>)m->letters_buf;
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)m->queue;
    id<MTLComputePipelineState> ps =
        (__bridge id<MTLComputePipelineState>)m->count_with_cross_pipeline;

    memcpy(racks.contents, racks_data, (size_t)B * BITRACK_BYTES);
    memset(counts.contents, 0, (size_t)B * sizeof(uint32_t));

    const uint64_t t0 = mach_absolute_time();
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:ps];
    [enc setBuffer:bitracks
            offset:(NSUInteger)((size_t)first_word * BITRACK_BYTES)
           atIndex:0];
    [enc setBuffer:letters offset:(NSUInteger)letters_byte_offset atIndex:1];
    [enc setBuffer:racks offset:0 atIndex:2];
    [enc setBytes:&n_words length:sizeof(uint32_t) atIndex:3];
    [enc setBytes:&word_length length:sizeof(uint32_t) atIndex:4];
    [enc setBytes:cross_sets
           length:(NSUInteger)((size_t)word_length * sizeof(uint64_t))
          atIndex:5];
    [enc setBuffer:counts offset:0 atIndex:6];
    [enc dispatchThreads:MTLSizeMake(n_words, B, 1)
        threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    m->last_gpu_us = (cb.GPUEndTime - cb.GPUStartTime) * 1e6;
    const double dt = mach_seconds(mach_absolute_time() - t0);

    if (out_counts != NULL) {
      memcpy(out_counts, counts.contents, (size_t)B * sizeof(uint32_t));
    }
    return dt;
  }
}

double gpu_matcher_count_with_playthrough(
    GpuMatcher *m, uint32_t first_word, uint32_t n_words,
    size_t letters_byte_offset, uint32_t word_length, const uint8_t *racks_data,
    uint32_t B, const uint64_t *cross_sets, const uint8_t *fixed_letters,
    const uint8_t *fixed_bitrack, uint32_t *out_counts) {
  if (m == NULL || racks_data == NULL || B == 0 || n_words == 0 ||
      cross_sets == NULL || fixed_letters == NULL || fixed_bitrack == NULL ||
      word_length == 0 || m->letters_buf == NULL) {
    return 0.0;
  }
  if ((uint64_t)first_word + n_words > m->total_words) {
    return 0.0;
  }
  if (letters_byte_offset + (size_t)n_words * word_length >
      m->total_letters_bytes) {
    return 0.0;
  }
  @autoreleasepool {
    ensure_batch_buffers(m, B);
    id<MTLBuffer> racks = (__bridge id<MTLBuffer>)m->racks_buf;
    id<MTLBuffer> counts = (__bridge id<MTLBuffer>)m->counts_buf;
    id<MTLBuffer> bitracks = (__bridge id<MTLBuffer>)m->bitracks_buf;
    id<MTLBuffer> letters = (__bridge id<MTLBuffer>)m->letters_buf;
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)m->queue;
    id<MTLComputePipelineState> ps =
        (__bridge id<MTLComputePipelineState>)m->count_with_playthrough_pipeline;

    memcpy(racks.contents, racks_data, (size_t)B * BITRACK_BYTES);
    memset(counts.contents, 0, (size_t)B * sizeof(uint32_t));

    const uint64_t t0 = mach_absolute_time();
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:ps];
    [enc setBuffer:bitracks
            offset:(NSUInteger)((size_t)first_word * BITRACK_BYTES)
           atIndex:0];
    [enc setBuffer:letters offset:(NSUInteger)letters_byte_offset atIndex:1];
    [enc setBuffer:racks offset:0 atIndex:2];
    [enc setBytes:&n_words length:sizeof(uint32_t) atIndex:3];
    [enc setBytes:&word_length length:sizeof(uint32_t) atIndex:4];
    [enc setBytes:cross_sets
           length:(NSUInteger)((size_t)word_length * sizeof(uint64_t))
          atIndex:5];
    [enc setBytes:fixed_letters length:(NSUInteger)word_length atIndex:6];
    [enc setBytes:fixed_bitrack length:BITRACK_BYTES atIndex:7];
    [enc setBuffer:counts offset:0 atIndex:8];
    [enc dispatchThreads:MTLSizeMake(n_words, B, 1)
        threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    m->last_gpu_us = (cb.GPUEndTime - cb.GPUStartTime) * 1e6;
    const double dt = mach_seconds(mach_absolute_time() - t0);

    if (out_counts != NULL) {
      memcpy(out_counts, counts.contents, (size_t)B * sizeof(uint32_t));
    }
    return dt;
  }
}

double gpu_matcher_score(GpuMatcher *m, uint32_t first_word, uint32_t n_words,
                         size_t letters_byte_offset, uint32_t word_length,
                         const uint8_t *racks_data, uint32_t B,
                         const uint64_t *cross_sets,
                         const uint8_t *fixed_letters,
                         const uint8_t *fixed_bitrack,
                         const int32_t *letter_scores,
                         const int32_t *position_multipliers,
                         int32_t base_score, int32_t bingo_to_add,
                         uint32_t *out_count_score_pairs) {
  if (m == NULL || racks_data == NULL || B == 0 || n_words == 0 ||
      cross_sets == NULL || fixed_letters == NULL || fixed_bitrack == NULL ||
      letter_scores == NULL || position_multipliers == NULL ||
      word_length == 0 || m->letters_buf == NULL) {
    return 0.0;
  }
  if ((uint64_t)first_word + n_words > m->total_words) {
    return 0.0;
  }
  if (letters_byte_offset + (size_t)n_words * word_length >
      m->total_letters_bytes) {
    return 0.0;
  }
  @autoreleasepool {
    ensure_batch_buffers(m, B);
    id<MTLBuffer> racks = (__bridge id<MTLBuffer>)m->racks_buf;
    id<MTLBuffer> output = (__bridge id<MTLBuffer>)m->counts_buf;
    id<MTLBuffer> bitracks = (__bridge id<MTLBuffer>)m->bitracks_buf;
    id<MTLBuffer> letters = (__bridge id<MTLBuffer>)m->letters_buf;
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)m->queue;
    id<MTLComputePipelineState> ps =
        (__bridge id<MTLComputePipelineState>)m->score_with_playthrough_pipeline;

    memcpy(racks.contents, racks_data, (size_t)B * BITRACK_BYTES);
    memset(output.contents, 0, (size_t)B * 2 * sizeof(uint32_t));

    const uint64_t t0 = mach_absolute_time();
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:ps];
    [enc setBuffer:bitracks
            offset:(NSUInteger)((size_t)first_word * BITRACK_BYTES)
           atIndex:0];
    [enc setBuffer:letters offset:(NSUInteger)letters_byte_offset atIndex:1];
    [enc setBuffer:racks offset:0 atIndex:2];
    [enc setBytes:&n_words length:sizeof(uint32_t) atIndex:3];
    [enc setBytes:&word_length length:sizeof(uint32_t) atIndex:4];
    [enc setBytes:cross_sets
           length:(NSUInteger)((size_t)word_length * sizeof(uint64_t))
          atIndex:5];
    [enc setBytes:fixed_letters length:(NSUInteger)word_length atIndex:6];
    [enc setBytes:fixed_bitrack length:BITRACK_BYTES atIndex:7];
    [enc setBytes:letter_scores length:32 * sizeof(int32_t) atIndex:8];
    [enc setBytes:position_multipliers
           length:(NSUInteger)((size_t)word_length * sizeof(int32_t))
          atIndex:9];
    [enc setBytes:&base_score length:sizeof(int32_t) atIndex:10];
    [enc setBytes:&bingo_to_add length:sizeof(int32_t) atIndex:11];
    [enc setBuffer:output offset:0 atIndex:12];
    [enc dispatchThreads:MTLSizeMake(n_words, B, 1)
        threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    m->last_gpu_us = (cb.GPUEndTime - cb.GPUStartTime) * 1e6;
    const double dt = mach_seconds(mach_absolute_time() - t0);

    if (out_count_score_pairs != NULL) {
      memcpy(out_count_score_pairs, output.contents,
             (size_t)B * 2 * sizeof(uint32_t));
    }
    return dt;
  }
}

double gpu_matcher_equity(GpuMatcher *m, uint32_t first_word, uint32_t n_words,
                          size_t letters_byte_offset, uint32_t word_length,
                          const uint8_t *racks_data, uint32_t B,
                          const uint64_t *cross_sets,
                          const uint8_t *fixed_letters,
                          const uint8_t *fixed_bitrack,
                          const int32_t *letter_scores,
                          const int32_t *position_multipliers,
                          int32_t base_score, int32_t bingo_to_add,
                          const uint8_t *leave_used,
                          const int32_t *leave_values, uint32_t n_leaves,
                          uint32_t *out_count_equity_pairs) {
  if (m == NULL || racks_data == NULL || B == 0 || n_words == 0 ||
      cross_sets == NULL || fixed_letters == NULL || fixed_bitrack == NULL ||
      letter_scores == NULL || position_multipliers == NULL ||
      leave_used == NULL || leave_values == NULL || n_leaves == 0 ||
      word_length == 0 || m->letters_buf == NULL) {
    return 0.0;
  }
  if ((uint64_t)first_word + n_words > m->total_words) {
    return 0.0;
  }
  if (letters_byte_offset + (size_t)n_words * word_length >
      m->total_letters_bytes) {
    return 0.0;
  }
  @autoreleasepool {
    ensure_batch_buffers(m, B);
    id<MTLBuffer> racks = (__bridge id<MTLBuffer>)m->racks_buf;
    id<MTLBuffer> output = (__bridge id<MTLBuffer>)m->counts_buf;
    id<MTLBuffer> bitracks = (__bridge id<MTLBuffer>)m->bitracks_buf;
    id<MTLBuffer> letters = (__bridge id<MTLBuffer>)m->letters_buf;
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)m->queue;
    id<MTLComputePipelineState> ps =
        (__bridge id<MTLComputePipelineState>)m->equity_with_playthrough_pipeline;

    memcpy(racks.contents, racks_data, (size_t)B * BITRACK_BYTES);
    memset(output.contents, 0, (size_t)B * 2 * sizeof(uint32_t));

    const uint64_t t0 = mach_absolute_time();
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:ps];
    [enc setBuffer:bitracks
            offset:(NSUInteger)((size_t)first_word * BITRACK_BYTES)
           atIndex:0];
    [enc setBuffer:letters offset:(NSUInteger)letters_byte_offset atIndex:1];
    [enc setBuffer:racks offset:0 atIndex:2];
    [enc setBytes:&n_words length:sizeof(uint32_t) atIndex:3];
    [enc setBytes:&word_length length:sizeof(uint32_t) atIndex:4];
    [enc setBytes:cross_sets
           length:(NSUInteger)((size_t)word_length * sizeof(uint64_t))
          atIndex:5];
    [enc setBytes:fixed_letters length:(NSUInteger)word_length atIndex:6];
    [enc setBytes:fixed_bitrack length:BITRACK_BYTES atIndex:7];
    [enc setBytes:letter_scores length:32 * sizeof(int32_t) atIndex:8];
    [enc setBytes:position_multipliers
           length:(NSUInteger)((size_t)word_length * sizeof(int32_t))
          atIndex:9];
    [enc setBytes:&base_score length:sizeof(int32_t) atIndex:10];
    [enc setBytes:&bingo_to_add length:sizeof(int32_t) atIndex:11];
    [enc setBytes:leave_used
           length:(NSUInteger)((size_t)n_leaves * BITRACK_BYTES)
          atIndex:12];
    [enc setBytes:leave_values
           length:(NSUInteger)((size_t)n_leaves * sizeof(int32_t))
          atIndex:13];
    [enc setBytes:&n_leaves length:sizeof(uint32_t) atIndex:14];
    [enc setBuffer:output offset:0 atIndex:15];
    [enc dispatchThreads:MTLSizeMake(n_words, B, 1)
        threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    m->last_gpu_us = (cb.GPUEndTime - cb.GPUStartTime) * 1e6;
    const double dt = mach_seconds(mach_absolute_time() - t0);

    if (out_count_equity_pairs != NULL) {
      memcpy(out_count_equity_pairs, output.contents,
             (size_t)B * 2 * sizeof(uint32_t));
    }
    return dt;
  }
}

static void ensure_top1_buffers(GpuMatcher *m, uint32_t B) {
  if (B <= m->top1_cap_b) {
    return;
  }
  if (m->best_eq_mp_buf) {
    CFRelease(m->best_eq_mp_buf);
    m->best_eq_mp_buf = NULL;
  }
  if (m->best_loc_buf) {
    CFRelease(m->best_loc_buf);
    m->best_loc_buf = NULL;
  }
  id<MTLDevice> device = (__bridge id<MTLDevice>)m->device;
  id<MTLBuffer> eq = [device
      newBufferWithLength:(NSUInteger)((size_t)B * sizeof(int32_t))
                  options:MTLResourceStorageModeShared];
  id<MTLBuffer> loc = [device
      newBufferWithLength:(NSUInteger)((size_t)B * sizeof(uint32_t))
                  options:MTLResourceStorageModeShared];
  m->best_eq_mp_buf = (__bridge_retained void *)eq;
  m->best_loc_buf = (__bridge_retained void *)loc;
  m->top1_cap_b = B;
}

void gpu_matcher_top1_reset(GpuMatcher *m, uint32_t B) {
  if (m == NULL || B == 0) {
    return;
  }
  @autoreleasepool {
    ensure_top1_buffers(m, B);
    ensure_batch_buffers(m, B);
    id<MTLBuffer> eq = (__bridge id<MTLBuffer>)m->best_eq_mp_buf;
    id<MTLBuffer> loc = (__bridge id<MTLBuffer>)m->best_loc_buf;
    int32_t *eq_ptr = (int32_t *)eq.contents;
    for (uint32_t i = 0; i < B; i++) {
      eq_ptr[i] = INT32_MIN;
    }
    memset(loc.contents, 0xFF, (size_t)B * sizeof(uint32_t));
  }
}

// Common bind-and-dispatch for both passes. is_pass2 selects locator-output
// kernel and binds the additional pass-2 args; otherwise runs pass-1 (eq_mp
// only). `leave_stride` (in entries): 0 means shared leaves; > 0 means
// per-rack tables of `n_leaves` entries each at stride spacing — the kernel
// reads rack i's leaves at [i*stride, i*stride+n_leaves) and the caller
// must size leave_used/leave_values accordingly (B * stride entries).
static double dispatch_top1_common(
    GpuMatcher *m, void *pipeline_ptr, uint32_t first_word, uint32_t n_words,
    size_t letters_byte_offset, uint32_t word_length,
    const uint8_t *racks_data, uint32_t B, const uint64_t *cross_sets,
    const uint8_t *fixed_letters, const uint8_t *fixed_bitrack,
    const int32_t *letter_scores, const int32_t *position_multipliers,
    int32_t base_score, int32_t bingo_to_add, const uint8_t *leave_used,
    const int32_t *leave_values, uint32_t n_leaves, uint32_t leave_stride,
    bool is_pass2, uint32_t row, uint32_t col, uint32_t dir, uint32_t mode) {
  @autoreleasepool {
    id<MTLBuffer> racks = (__bridge id<MTLBuffer>)m->racks_buf;
    id<MTLBuffer> bitracks = (__bridge id<MTLBuffer>)m->bitracks_buf;
    id<MTLBuffer> letters = (__bridge id<MTLBuffer>)m->letters_buf;
    id<MTLBuffer> eq = (__bridge id<MTLBuffer>)m->best_eq_mp_buf;
    id<MTLBuffer> loc = (__bridge id<MTLBuffer>)m->best_loc_buf;
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)m->queue;
    id<MTLComputePipelineState> ps =
        (__bridge id<MTLComputePipelineState>)pipeline_ptr;

    memcpy(racks.contents, racks_data, (size_t)B * BITRACK_BYTES);

    const uint64_t t0 = mach_absolute_time();
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:ps];
    [enc setBuffer:bitracks
            offset:(NSUInteger)((size_t)first_word * BITRACK_BYTES)
           atIndex:0];
    [enc setBuffer:letters offset:(NSUInteger)letters_byte_offset atIndex:1];
    [enc setBuffer:racks offset:0 atIndex:2];
    [enc setBytes:&n_words length:sizeof(uint32_t) atIndex:3];
    [enc setBytes:&word_length length:sizeof(uint32_t) atIndex:4];
    [enc setBytes:cross_sets
           length:(NSUInteger)((size_t)word_length * sizeof(uint64_t))
          atIndex:5];
    [enc setBytes:fixed_letters length:(NSUInteger)word_length atIndex:6];
    [enc setBytes:fixed_bitrack length:BITRACK_BYTES atIndex:7];
    [enc setBytes:letter_scores length:32 * sizeof(int32_t) atIndex:8];
    [enc setBytes:position_multipliers
           length:(NSUInteger)((size_t)word_length * sizeof(int32_t))
          atIndex:9];
    [enc setBytes:&base_score length:sizeof(int32_t) atIndex:10];
    [enc setBytes:&bingo_to_add length:sizeof(int32_t) atIndex:11];
    // Leave buffers: stride==0 → shared (n_leaves entries via setBytes when
    // small enough); stride>0 → per-rack (B*stride entries via managed
    // MTLBuffer since setBytes can't carry MB-scale payloads).
    const size_t lu_total =
        (leave_stride == 0) ? (size_t)n_leaves : ((size_t)B * leave_stride);
    if (leave_stride > 0) {
      ensure_leave_buffers(m, lu_total);
      id<MTLBuffer> lu_b = (__bridge id<MTLBuffer>)m->leave_used_buf;
      id<MTLBuffer> lv_b = (__bridge id<MTLBuffer>)m->leave_values_buf;
      memcpy(lu_b.contents, leave_used, lu_total * BITRACK_BYTES);
      memcpy(lv_b.contents, leave_values, lu_total * sizeof(int32_t));
      [enc setBuffer:lu_b offset:0 atIndex:12];
      [enc setBuffer:lv_b offset:0 atIndex:13];
    } else {
      [enc setBytes:leave_used
             length:(NSUInteger)(lu_total * BITRACK_BYTES)
            atIndex:12];
      [enc setBytes:leave_values
             length:(NSUInteger)(lu_total * sizeof(int32_t))
            atIndex:13];
    }
    [enc setBytes:&n_leaves length:sizeof(uint32_t) atIndex:14];
    if (!is_pass2) {
      [enc setBuffer:eq offset:0 atIndex:15];
      [enc setBytes:&leave_stride length:sizeof(uint32_t) atIndex:16];
    } else {
      [enc setBuffer:eq offset:0 atIndex:15];
      [enc setBytes:&row length:sizeof(uint32_t) atIndex:16];
      [enc setBytes:&col length:sizeof(uint32_t) atIndex:17];
      [enc setBytes:&dir length:sizeof(uint32_t) atIndex:18];
      [enc setBytes:&mode length:sizeof(uint32_t) atIndex:19];
      [enc setBuffer:loc offset:0 atIndex:20];
      [enc setBytes:&leave_stride length:sizeof(uint32_t) atIndex:21];
    }
    [enc dispatchThreads:MTLSizeMake(n_words, B, 1)
        threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    m->last_gpu_us = (cb.GPUEndTime - cb.GPUStartTime) * 1e6;
    return mach_seconds(mach_absolute_time() - t0);
  }
}

double gpu_matcher_top1_pass1(GpuMatcher *m, uint32_t first_word,
                              uint32_t n_words, size_t letters_byte_offset,
                              uint32_t word_length, const uint8_t *racks_data,
                              uint32_t B, const uint64_t *cross_sets,
                              const uint8_t *fixed_letters,
                              const uint8_t *fixed_bitrack,
                              const int32_t *letter_scores,
                              const int32_t *position_multipliers,
                              int32_t base_score, int32_t bingo_to_add,
                              const uint8_t *leave_used,
                              const int32_t *leave_values, uint32_t n_leaves,
                              uint32_t leave_stride) {
  if (m == NULL || racks_data == NULL || B == 0 || n_words == 0 ||
      cross_sets == NULL || fixed_letters == NULL || fixed_bitrack == NULL ||
      letter_scores == NULL || position_multipliers == NULL ||
      leave_used == NULL || leave_values == NULL || n_leaves == 0 ||
      word_length == 0 || m->letters_buf == NULL ||
      m->best_eq_mp_buf == NULL) {
    return 0.0;
  }
  if ((uint64_t)first_word + n_words > m->total_words) {
    return 0.0;
  }
  if (letters_byte_offset + (size_t)n_words * word_length >
      m->total_letters_bytes) {
    return 0.0;
  }
  return dispatch_top1_common(m, m->top1_eqmp_pipeline, first_word, n_words,
                              letters_byte_offset, word_length, racks_data, B,
                              cross_sets, fixed_letters, fixed_bitrack,
                              letter_scores, position_multipliers, base_score,
                              bingo_to_add, leave_used, leave_values, n_leaves,
                              leave_stride, false, 0, 0, 0, 0);
}

double gpu_matcher_top1_pass2(
    GpuMatcher *m, uint32_t first_word, uint32_t n_words,
    size_t letters_byte_offset, uint32_t word_length,
    const uint8_t *racks_data, uint32_t B, const uint64_t *cross_sets,
    const uint8_t *fixed_letters, const uint8_t *fixed_bitrack,
    const int32_t *letter_scores, const int32_t *position_multipliers,
    int32_t base_score, int32_t bingo_to_add, const uint8_t *leave_used,
    const int32_t *leave_values, uint32_t n_leaves, uint32_t leave_stride,
    uint32_t row, uint32_t col, uint32_t dir, GpuTop1Tiebreak mode) {
  if (m == NULL || racks_data == NULL || B == 0 || n_words == 0 ||
      cross_sets == NULL || fixed_letters == NULL || fixed_bitrack == NULL ||
      letter_scores == NULL || position_multipliers == NULL ||
      leave_used == NULL || leave_values == NULL || n_leaves == 0 ||
      word_length == 0 || m->letters_buf == NULL ||
      m->best_eq_mp_buf == NULL || m->best_loc_buf == NULL) {
    return 0.0;
  }
  if ((uint64_t)first_word + n_words > m->total_words) {
    return 0.0;
  }
  if (letters_byte_offset + (size_t)n_words * word_length >
      m->total_letters_bytes) {
    return 0.0;
  }
  return dispatch_top1_common(m, m->top1_loc_pipeline, first_word, n_words,
                              letters_byte_offset, word_length, racks_data, B,
                              cross_sets, fixed_letters, fixed_bitrack,
                              letter_scores, position_multipliers, base_score,
                              bingo_to_add, leave_used, leave_values, n_leaves,
                              leave_stride, true, row, col, dir,
                              (uint32_t)mode);
}

void gpu_matcher_top1_read(GpuMatcher *m, uint32_t B, int32_t *out_best_eq_mp,
                           uint32_t *out_best_loc) {
  if (m == NULL || B == 0 || m->best_eq_mp_buf == NULL ||
      m->best_loc_buf == NULL) {
    return;
  }
  @autoreleasepool {
    id<MTLBuffer> eq = (__bridge id<MTLBuffer>)m->best_eq_mp_buf;
    id<MTLBuffer> loc = (__bridge id<MTLBuffer>)m->best_loc_buf;
    if (out_best_eq_mp != NULL) {
      memcpy(out_best_eq_mp, eq.contents, (size_t)B * sizeof(int32_t));
    }
    if (out_best_loc != NULL) {
      memcpy(out_best_loc, loc.contents, (size_t)B * sizeof(uint32_t));
    }
  }
}

double gpu_matcher_get_last_gpu_us(const GpuMatcher *m) {
  return m ? m->last_gpu_us : 0.0;
}

bool gpu_matcher_load_wmpg(GpuMatcher *m, const uint8_t *wmpg_bytes,
                           size_t wmpg_size) {
  if (m == NULL || wmpg_bytes == NULL || wmpg_size < 32) {
    return false;
  }
  if (wmpg_bytes[0] != 'W' || wmpg_bytes[1] != 'M' || wmpg_bytes[2] != 'P' ||
      wmpg_bytes[3] != 'G' || wmpg_bytes[4] != 2) {
    return false;
  }
  const int max_len_plus_one = wmpg_bytes[6];
  if (max_len_plus_one > 16) {
    return false;
  }
  // v2 per-length meta is 56 bytes — three sub-blocks (word @ 0..23,
  // blank-1 @ 24..39, blank-2 @ 40..55). See wmpg_maker.h.
  const uint32_t sections_offset =
      (uint32_t)wmpg_bytes[20] | ((uint32_t)wmpg_bytes[21] << 8) |
      ((uint32_t)wmpg_bytes[22] << 16) | ((uint32_t)wmpg_bytes[23] << 24);
  const uint32_t total_bucket_starts =
      (uint32_t)wmpg_bytes[8] | ((uint32_t)wmpg_bytes[9] << 8) |
      ((uint32_t)wmpg_bytes[10] << 16) | ((uint32_t)wmpg_bytes[11] << 24);
  const uint32_t total_entries =
      (uint32_t)wmpg_bytes[12] | ((uint32_t)wmpg_bytes[13] << 8) |
      ((uint32_t)wmpg_bytes[14] << 16) | ((uint32_t)wmpg_bytes[15] << 24);

  const uint32_t buckets_section_offset = sections_offset;
  const uint32_t entries_section_offset =
      sections_offset + total_bucket_starts * (uint32_t)sizeof(uint32_t);
  const uint32_t letters_section_offset =
      entries_section_offset + total_entries * 32u;

  m->wmpg_max_len_plus_one = (uint32_t)max_len_plus_one;
  for (int len = 0; len < max_len_plus_one; len++) {
    const uint8_t *meta = wmpg_bytes + 32 + (size_t)len * 56;
    // Word (blank-0) sub-block.
    const uint32_t w_nb = (uint32_t)meta[0] | ((uint32_t)meta[1] << 8) |
                          ((uint32_t)meta[2] << 16) |
                          ((uint32_t)meta[3] << 24);
    const uint32_t w_bo = (uint32_t)meta[4] | ((uint32_t)meta[5] << 8) |
                          ((uint32_t)meta[6] << 16) |
                          ((uint32_t)meta[7] << 24);
    const uint32_t w_eo = (uint32_t)meta[12] | ((uint32_t)meta[13] << 8) |
                          ((uint32_t)meta[14] << 16) |
                          ((uint32_t)meta[15] << 24);
    const uint32_t w_lo = (uint32_t)meta[20] | ((uint32_t)meta[21] << 8) |
                          ((uint32_t)meta[22] << 16) |
                          ((uint32_t)meta[23] << 24);
    m->wmpg_num_buckets[len] = w_nb;
    m->wmpg_bucket_starts_byte_offset[len] = buckets_section_offset + w_bo;
    m->wmpg_entries_byte_offset[len] = entries_section_offset + w_eo;
    m->wmpg_uninlined_letters_byte_offset[len] = letters_section_offset + w_lo;
    // Blank-1 sub-block.
    const uint32_t b1_nb = (uint32_t)meta[24] | ((uint32_t)meta[25] << 8) |
                           ((uint32_t)meta[26] << 16) |
                           ((uint32_t)meta[27] << 24);
    const uint32_t b1_bo = (uint32_t)meta[28] | ((uint32_t)meta[29] << 8) |
                           ((uint32_t)meta[30] << 16) |
                           ((uint32_t)meta[31] << 24);
    const uint32_t b1_eo = (uint32_t)meta[36] | ((uint32_t)meta[37] << 8) |
                           ((uint32_t)meta[38] << 16) |
                           ((uint32_t)meta[39] << 24);
    m->wmpg_b1_num_buckets[len] = b1_nb;
    m->wmpg_b1_bucket_starts_byte_offset[len] = buckets_section_offset + b1_bo;
    m->wmpg_b1_entries_byte_offset[len] = entries_section_offset + b1_eo;
    // Blank-2 sub-block.
    const uint32_t b2_nb = (uint32_t)meta[40] | ((uint32_t)meta[41] << 8) |
                           ((uint32_t)meta[42] << 16) |
                           ((uint32_t)meta[43] << 24);
    const uint32_t b2_bo = (uint32_t)meta[44] | ((uint32_t)meta[45] << 8) |
                           ((uint32_t)meta[46] << 16) |
                           ((uint32_t)meta[47] << 24);
    const uint32_t b2_eo = (uint32_t)meta[52] | ((uint32_t)meta[53] << 8) |
                           ((uint32_t)meta[54] << 16) |
                           ((uint32_t)meta[55] << 24);
    m->wmpg_b2_num_buckets[len] = b2_nb;
    m->wmpg_b2_bucket_starts_byte_offset[len] = buckets_section_offset + b2_bo;
    m->wmpg_b2_entries_byte_offset[len] = entries_section_offset + b2_eo;
  }

  @autoreleasepool {
    if (m->wmpg_buf) {
      CFRelease(m->wmpg_buf);
      m->wmpg_buf = NULL;
    }
    id<MTLDevice> device = (__bridge id<MTLDevice>)m->device;
    id<MTLBuffer> buf = [device newBufferWithBytes:wmpg_bytes
                                            length:wmpg_size
                                           options:MTLResourceStorageModeShared];
    m->wmpg_buf = (__bridge_retained void *)buf;
  }
  return true;
}

double gpu_matcher_count_wmpg(GpuMatcher *m, uint32_t word_length,
                              uint32_t target_used_size,
                              const uint8_t *racks_data, uint32_t B,
                              const uint64_t *cross_sets,
                              const uint8_t *fixed_letters,
                              uint64_t fixed_bitrack_low,
                              uint64_t fixed_bitrack_high,
                              uint32_t *out_counts) {
  if (m == NULL || racks_data == NULL || B == 0 || word_length == 0 ||
      cross_sets == NULL || fixed_letters == NULL ||
      m->wmpg_buf == NULL || word_length >= m->wmpg_max_len_plus_one) {
    return 0.0;
  }
  if (m->wmpg_num_buckets[word_length] == 0) {
    if (out_counts != NULL) {
      memset(out_counts, 0, (size_t)B * sizeof(uint32_t));
    }
    return 0.0;
  }
  @autoreleasepool {
    ensure_batch_buffers(m, B);
    id<MTLBuffer> racks = (__bridge id<MTLBuffer>)m->racks_buf;
    id<MTLBuffer> counts = (__bridge id<MTLBuffer>)m->counts_buf;
    id<MTLBuffer> wmpg = (__bridge id<MTLBuffer>)m->wmpg_buf;
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)m->queue;
    id<MTLComputePipelineState> ps =
        (__bridge id<MTLComputePipelineState>)m->count_wmpg_pipeline;

    memcpy(racks.contents, racks_data, (size_t)B * BITRACK_BYTES);
    memset(counts.contents, 0, (size_t)B * sizeof(uint32_t));

    const uint32_t num_buckets = m->wmpg_num_buckets[word_length];
    const NSUInteger bs_off =
        (NSUInteger)m->wmpg_bucket_starts_byte_offset[word_length];
    const NSUInteger en_off =
        (NSUInteger)m->wmpg_entries_byte_offset[word_length];
    const NSUInteger ul_off =
        (NSUInteger)m->wmpg_uninlined_letters_byte_offset[word_length];

    const uint64_t t0 = mach_absolute_time();
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:ps];
    [enc setBuffer:wmpg offset:bs_off atIndex:0];
    [enc setBuffer:wmpg offset:en_off atIndex:1];
    [enc setBuffer:wmpg offset:ul_off atIndex:2];
    [enc setBuffer:racks offset:0 atIndex:3];
    [enc setBytes:&num_buckets length:sizeof(uint32_t) atIndex:4];
    [enc setBytes:&word_length length:sizeof(uint32_t) atIndex:5];
    [enc setBytes:&target_used_size length:sizeof(uint32_t) atIndex:6];
    [enc setBytes:cross_sets
           length:(NSUInteger)((size_t)word_length * sizeof(uint64_t))
          atIndex:7];
    [enc setBytes:fixed_letters length:(NSUInteger)word_length atIndex:8];
    [enc setBytes:&fixed_bitrack_low length:sizeof(uint64_t) atIndex:9];
    [enc setBytes:&fixed_bitrack_high length:sizeof(uint64_t) atIndex:10];
    [enc setBuffer:counts offset:0 atIndex:11];
    // One thread per rack (per-rack subrack enumeration done in-thread).
    [enc dispatchThreads:MTLSizeMake(B, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    m->last_gpu_us = (cb.GPUEndTime - cb.GPUStartTime) * 1e6;
    const double dt = mach_seconds(mach_absolute_time() - t0);

    if (out_counts != NULL) {
      memcpy(out_counts, counts.contents, (size_t)B * sizeof(uint32_t));
    }
    return dt;
  }
}

// Common bind-and-dispatch for both WMPG top-1 passes. is_pass2 selects the
// locator-output kernel and binds the additional pass-2 args (row, col, dir,
// mode, best_loc); otherwise runs pass-1 (best_eq_mp only).
static double dispatch_top1_wmpg_common(
    GpuMatcher *m, void *pipeline_ptr, uint32_t word_length,
    uint32_t target_used_size, const uint8_t *racks_data, uint32_t B,
    const uint64_t *cross_sets, const uint8_t *fixed_letters,
    uint64_t fixed_bitrack_low, uint64_t fixed_bitrack_high,
    const int32_t *letter_scores, const int32_t *position_multipliers,
    int32_t base_score, int32_t bingo_to_add, const uint8_t *leave_used,
    const int32_t *leave_values, uint32_t n_leaves, uint32_t leave_stride,
    bool is_pass2, uint32_t row, uint32_t col, uint32_t dir, uint32_t mode) {
  @autoreleasepool {
    id<MTLBuffer> racks = (__bridge id<MTLBuffer>)m->racks_buf;
    id<MTLBuffer> wmpg = (__bridge id<MTLBuffer>)m->wmpg_buf;
    id<MTLBuffer> eq = (__bridge id<MTLBuffer>)m->best_eq_mp_buf;
    id<MTLBuffer> loc = (__bridge id<MTLBuffer>)m->best_loc_buf;
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)m->queue;
    id<MTLComputePipelineState> ps =
        (__bridge id<MTLComputePipelineState>)pipeline_ptr;

    memcpy(racks.contents, racks_data, (size_t)B * BITRACK_BYTES);

    const uint32_t num_buckets = m->wmpg_num_buckets[word_length];
    const NSUInteger bs_off =
        (NSUInteger)m->wmpg_bucket_starts_byte_offset[word_length];
    const NSUInteger en_off =
        (NSUInteger)m->wmpg_entries_byte_offset[word_length];
    const NSUInteger ul_off =
        (NSUInteger)m->wmpg_uninlined_letters_byte_offset[word_length];

    const uint64_t t0 = mach_absolute_time();
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:ps];
    [enc setBuffer:wmpg offset:bs_off atIndex:0];
    [enc setBuffer:wmpg offset:en_off atIndex:1];
    [enc setBuffer:wmpg offset:ul_off atIndex:2];
    [enc setBuffer:racks offset:0 atIndex:3];
    [enc setBytes:&num_buckets length:sizeof(uint32_t) atIndex:4];
    [enc setBytes:&word_length length:sizeof(uint32_t) atIndex:5];
    [enc setBytes:&target_used_size length:sizeof(uint32_t) atIndex:6];
    [enc setBytes:cross_sets
           length:(NSUInteger)((size_t)word_length * sizeof(uint64_t))
          atIndex:7];
    [enc setBytes:fixed_letters length:(NSUInteger)word_length atIndex:8];
    [enc setBytes:&fixed_bitrack_low length:sizeof(uint64_t) atIndex:9];
    [enc setBytes:&fixed_bitrack_high length:sizeof(uint64_t) atIndex:10];
    [enc setBytes:letter_scores length:32 * sizeof(int32_t) atIndex:11];
    [enc setBytes:position_multipliers
           length:(NSUInteger)((size_t)word_length * sizeof(int32_t))
          atIndex:12];
    [enc setBytes:&base_score length:sizeof(int32_t) atIndex:13];
    [enc setBytes:&bingo_to_add length:sizeof(int32_t) atIndex:14];
    const size_t lu_total =
        (leave_stride == 0) ? (size_t)n_leaves : ((size_t)B * leave_stride);
    if (leave_stride > 0) {
      ensure_leave_buffers(m, lu_total);
      id<MTLBuffer> lu_b = (__bridge id<MTLBuffer>)m->leave_used_buf;
      id<MTLBuffer> lv_b = (__bridge id<MTLBuffer>)m->leave_values_buf;
      memcpy(lu_b.contents, leave_used, lu_total * BITRACK_BYTES);
      memcpy(lv_b.contents, leave_values, lu_total * sizeof(int32_t));
      [enc setBuffer:lu_b offset:0 atIndex:15];
      [enc setBuffer:lv_b offset:0 atIndex:16];
    } else {
      [enc setBytes:leave_used
             length:(NSUInteger)(lu_total * BITRACK_BYTES)
            atIndex:15];
      [enc setBytes:leave_values
             length:(NSUInteger)(lu_total * sizeof(int32_t))
            atIndex:16];
    }
    [enc setBytes:&n_leaves length:sizeof(uint32_t) atIndex:17];
    [enc setBuffer:eq offset:0 atIndex:18];
    // Blank-1 / blank-2 sub-table offsets (for blank-rack support) — bound
    // to the same wmpg_buf at the per-length offsets we recorded at load.
    const uint32_t b1_num_buckets = m->wmpg_b1_num_buckets[word_length];
    const NSUInteger b1_bs_off =
        (NSUInteger)m->wmpg_b1_bucket_starts_byte_offset[word_length];
    const NSUInteger b1_en_off =
        (NSUInteger)m->wmpg_b1_entries_byte_offset[word_length];
    const uint32_t b2_num_buckets = m->wmpg_b2_num_buckets[word_length];
    const NSUInteger b2_bs_off =
        (NSUInteger)m->wmpg_b2_bucket_starts_byte_offset[word_length];
    const NSUInteger b2_en_off =
        (NSUInteger)m->wmpg_b2_entries_byte_offset[word_length];
    if (!is_pass2) {
      // top1_eqmp_wmpg_kernel: leave_stride at 19, then b1/b2 at 20..25.
      [enc setBytes:&leave_stride length:sizeof(uint32_t) atIndex:19];
      [enc setBuffer:wmpg offset:b1_bs_off atIndex:20];
      [enc setBuffer:wmpg offset:b1_en_off atIndex:21];
      [enc setBytes:&b1_num_buckets length:sizeof(uint32_t) atIndex:22];
      [enc setBuffer:wmpg offset:b2_bs_off atIndex:23];
      [enc setBuffer:wmpg offset:b2_en_off atIndex:24];
      [enc setBytes:&b2_num_buckets length:sizeof(uint32_t) atIndex:25];
    } else {
      // top1_loc_wmpg_kernel: row/col/dir/mode/loc at 19..23,
      // leave_stride at 24, b1/b2 at 25..30.
      [enc setBytes:&row length:sizeof(uint32_t) atIndex:19];
      [enc setBytes:&col length:sizeof(uint32_t) atIndex:20];
      [enc setBytes:&dir length:sizeof(uint32_t) atIndex:21];
      [enc setBytes:&mode length:sizeof(uint32_t) atIndex:22];
      [enc setBuffer:loc offset:0 atIndex:23];
      [enc setBytes:&leave_stride length:sizeof(uint32_t) atIndex:24];
      [enc setBuffer:wmpg offset:b1_bs_off atIndex:25];
      [enc setBuffer:wmpg offset:b1_en_off atIndex:26];
      [enc setBytes:&b1_num_buckets length:sizeof(uint32_t) atIndex:27];
      [enc setBuffer:wmpg offset:b2_bs_off atIndex:28];
      [enc setBuffer:wmpg offset:b2_en_off atIndex:29];
      [enc setBytes:&b2_num_buckets length:sizeof(uint32_t) atIndex:30];
    }
    [enc dispatchThreads:MTLSizeMake(B, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    m->last_gpu_us = (cb.GPUEndTime - cb.GPUStartTime) * 1e6;
    return mach_seconds(mach_absolute_time() - t0);
  }
}

double gpu_matcher_top1_pass1_wmpg(
    GpuMatcher *m, uint32_t word_length, uint32_t target_used_size,
    const uint8_t *racks_data, uint32_t B, const uint64_t *cross_sets,
    const uint8_t *fixed_letters, uint64_t fixed_bitrack_low,
    uint64_t fixed_bitrack_high, const int32_t *letter_scores,
    const int32_t *position_multipliers, int32_t base_score,
    int32_t bingo_to_add, const uint8_t *leave_used,
    const int32_t *leave_values, uint32_t n_leaves, uint32_t leave_stride) {
  if (m == NULL || racks_data == NULL || B == 0 || word_length == 0 ||
      cross_sets == NULL || fixed_letters == NULL ||
      letter_scores == NULL || position_multipliers == NULL ||
      leave_used == NULL || leave_values == NULL || n_leaves == 0 ||
      m->wmpg_buf == NULL || word_length >= m->wmpg_max_len_plus_one ||
      m->best_eq_mp_buf == NULL) {
    return 0.0;
  }
  if (m->wmpg_num_buckets[word_length] == 0) {
    return 0.0;
  }
  ensure_batch_buffers(m, B);
  return dispatch_top1_wmpg_common(
      m, m->top1_eqmp_wmpg_pipeline, word_length, target_used_size, racks_data,
      B, cross_sets, fixed_letters, fixed_bitrack_low, fixed_bitrack_high,
      letter_scores, position_multipliers, base_score, bingo_to_add, leave_used,
      leave_values, n_leaves, leave_stride, false, 0, 0, 0, 0);
}

double gpu_matcher_top1_pass2_wmpg(
    GpuMatcher *m, uint32_t word_length, uint32_t target_used_size,
    const uint8_t *racks_data, uint32_t B, const uint64_t *cross_sets,
    const uint8_t *fixed_letters, uint64_t fixed_bitrack_low,
    uint64_t fixed_bitrack_high, const int32_t *letter_scores,
    const int32_t *position_multipliers, int32_t base_score,
    int32_t bingo_to_add, const uint8_t *leave_used,
    const int32_t *leave_values, uint32_t n_leaves, uint32_t leave_stride,
    uint32_t row, uint32_t col, uint32_t dir, GpuTop1Tiebreak mode) {
  if (m == NULL || racks_data == NULL || B == 0 || word_length == 0 ||
      cross_sets == NULL || fixed_letters == NULL ||
      letter_scores == NULL || position_multipliers == NULL ||
      leave_used == NULL || leave_values == NULL || n_leaves == 0 ||
      m->wmpg_buf == NULL || word_length >= m->wmpg_max_len_plus_one ||
      m->best_eq_mp_buf == NULL || m->best_loc_buf == NULL) {
    return 0.0;
  }
  if (m->wmpg_num_buckets[word_length] == 0) {
    return 0.0;
  }
  ensure_batch_buffers(m, B);
  return dispatch_top1_wmpg_common(
      m, m->top1_loc_wmpg_pipeline, word_length, target_used_size, racks_data,
      B, cross_sets, fixed_letters, fixed_bitrack_low, fixed_bitrack_high,
      letter_scores, position_multipliers, base_score, bingo_to_add, leave_used,
      leave_values, n_leaves, leave_stride, true, row, col, dir,
      (uint32_t)mode);
}

double gpu_matcher_equity_wmpg(GpuMatcher *m, uint32_t word_length,
                               uint32_t target_used_size,
                               const uint8_t *racks_data, uint32_t B,
                               const uint64_t *cross_sets,
                               const uint8_t *fixed_letters,
                               uint64_t fixed_bitrack_low,
                               uint64_t fixed_bitrack_high,
                               const int32_t *letter_scores,
                               const int32_t *position_multipliers,
                               int32_t base_score, int32_t bingo_to_add,
                               const uint8_t *leave_used,
                               const int32_t *leave_values, uint32_t n_leaves,
                               uint32_t *out_count_equity_pairs) {
  if (m == NULL || racks_data == NULL || B == 0 || word_length == 0 ||
      cross_sets == NULL || fixed_letters == NULL ||
      letter_scores == NULL || position_multipliers == NULL ||
      leave_used == NULL || leave_values == NULL || n_leaves == 0 ||
      m->wmpg_buf == NULL || word_length >= m->wmpg_max_len_plus_one) {
    return 0.0;
  }
  if (m->wmpg_num_buckets[word_length] == 0) {
    if (out_count_equity_pairs != NULL) {
      memset(out_count_equity_pairs, 0, (size_t)B * 2 * sizeof(uint32_t));
    }
    return 0.0;
  }
  @autoreleasepool {
    ensure_batch_buffers(m, B);
    id<MTLBuffer> racks = (__bridge id<MTLBuffer>)m->racks_buf;
    id<MTLBuffer> output = (__bridge id<MTLBuffer>)m->counts_buf;
    id<MTLBuffer> wmpg = (__bridge id<MTLBuffer>)m->wmpg_buf;
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)m->queue;
    id<MTLComputePipelineState> ps =
        (__bridge id<MTLComputePipelineState>)m->equity_wmpg_pipeline;

    memcpy(racks.contents, racks_data, (size_t)B * BITRACK_BYTES);
    memset(output.contents, 0, (size_t)B * 2 * sizeof(uint32_t));

    const uint32_t num_buckets = m->wmpg_num_buckets[word_length];
    const NSUInteger bs_off =
        (NSUInteger)m->wmpg_bucket_starts_byte_offset[word_length];
    const NSUInteger en_off =
        (NSUInteger)m->wmpg_entries_byte_offset[word_length];
    const NSUInteger ul_off =
        (NSUInteger)m->wmpg_uninlined_letters_byte_offset[word_length];

    const uint64_t t0 = mach_absolute_time();
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:ps];
    [enc setBuffer:wmpg offset:bs_off atIndex:0];
    [enc setBuffer:wmpg offset:en_off atIndex:1];
    [enc setBuffer:wmpg offset:ul_off atIndex:2];
    [enc setBuffer:racks offset:0 atIndex:3];
    [enc setBytes:&num_buckets length:sizeof(uint32_t) atIndex:4];
    [enc setBytes:&word_length length:sizeof(uint32_t) atIndex:5];
    [enc setBytes:&target_used_size length:sizeof(uint32_t) atIndex:6];
    [enc setBytes:cross_sets
           length:(NSUInteger)((size_t)word_length * sizeof(uint64_t))
          atIndex:7];
    [enc setBytes:fixed_letters length:(NSUInteger)word_length atIndex:8];
    [enc setBytes:&fixed_bitrack_low length:sizeof(uint64_t) atIndex:9];
    [enc setBytes:&fixed_bitrack_high length:sizeof(uint64_t) atIndex:10];
    [enc setBytes:letter_scores length:32 * sizeof(int32_t) atIndex:11];
    [enc setBytes:position_multipliers
           length:(NSUInteger)((size_t)word_length * sizeof(int32_t))
          atIndex:12];
    [enc setBytes:&base_score length:sizeof(int32_t) atIndex:13];
    [enc setBytes:&bingo_to_add length:sizeof(int32_t) atIndex:14];
    [enc setBytes:leave_used
           length:(NSUInteger)((size_t)n_leaves * BITRACK_BYTES)
          atIndex:15];
    [enc setBytes:leave_values
           length:(NSUInteger)((size_t)n_leaves * sizeof(int32_t))
          atIndex:16];
    [enc setBytes:&n_leaves length:sizeof(uint32_t) atIndex:17];
    [enc setBuffer:output offset:0 atIndex:18];
    // One thread per rack (per-rack subrack enumeration done in-thread).
    [enc dispatchThreads:MTLSizeMake(B, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    m->last_gpu_us = (cb.GPUEndTime - cb.GPUStartTime) * 1e6;
    const double dt = mach_seconds(mach_absolute_time() - t0);

    if (out_count_equity_pairs != NULL) {
      memcpy(out_count_equity_pairs, output.contents,
             (size_t)B * 2 * sizeof(uint32_t));
    }
    return dt;
  }
}
