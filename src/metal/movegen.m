// metal_movegen — phase-3 standalone CLI driver. Loads a flat lexicon, picks
// one length, and benches the Metal subset-match kernel via GpuMatcher (the
// C API in movegen.h) at varying batch sizes. Validates against a CPU
// brute-force scan.
//
// Usage:
//   ./bin/metal_movegen <flatlex_path> [rack=AABDELT] [length=7] [bench_iters=200]
//
// Blanks ("?") in the rack are recorded as letter 0; the kernel does not yet
// treat them as wildcards.

#include "movegen.h"

#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BITRACK_BYTES 16
#define MAX_LEN_PLUS_ONE 32

static double mach_seconds(uint64_t ticks) {
  static mach_timebase_info_data_t tb = {0, 0};
  if (tb.denom == 0) {
    mach_timebase_info(&tb);
  }
  return (double)ticks * (double)tb.numer / (double)tb.denom / 1e9;
}

typedef struct {
  uint8_t *data;
  size_t bytes;
  int max_len_plus_one;
  uint32_t total_words;
  uint32_t counts[MAX_LEN_PLUS_ONE];
  size_t bitracks_offset[MAX_LEN_PLUS_ONE];
  size_t letters_offset[MAX_LEN_PLUS_ONE];
} FlatLex;

static int flat_lex_load(const char *path, FlatLex *fl) {
  memset(fl, 0, sizeof(*fl));
  FILE *fp = fopen(path, "rb");
  if (fp == NULL) {
    fprintf(stderr, "open %s failed\n", path);
    return 1;
  }
  fseek(fp, 0, SEEK_END);
  long bytes = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  if (bytes < 16) {
    fclose(fp);
    return 1;
  }
  uint8_t *data = (uint8_t *)malloc((size_t)bytes);
  size_t got = fread(data, 1, (size_t)bytes, fp);
  fclose(fp);
  if (got != (size_t)bytes) {
    free(data);
    return 1;
  }
  if (data[0] != 'F' || data[1] != 'L' || data[2] != 'E' || data[3] != 'X' ||
      data[4] != 1) {
    free(data);
    fprintf(stderr, "bad header\n");
    return 1;
  }
  fl->data = data;
  fl->bytes = (size_t)bytes;
  fl->max_len_plus_one = data[6];
  if (fl->max_len_plus_one > MAX_LEN_PLUS_ONE) {
    free(data);
    return 1;
  }
  fl->total_words = (uint32_t)data[8] | ((uint32_t)data[9] << 8) |
                    ((uint32_t)data[10] << 16) | ((uint32_t)data[11] << 24);
  size_t off = 16;
  for (int i = 0; i < fl->max_len_plus_one; i++) {
    fl->counts[i] = (uint32_t)data[off] | ((uint32_t)data[off + 1] << 8) |
                    ((uint32_t)data[off + 2] << 16) |
                    ((uint32_t)data[off + 3] << 24);
    off += 4;
  }
  const size_t bitracks_start = 16 + (size_t)fl->max_len_plus_one * 4;
  const size_t letters_start =
      bitracks_start + (size_t)fl->total_words * BITRACK_BYTES;
  size_t br_cum = 0, lt_cum = 0;
  for (int i = 0; i < fl->max_len_plus_one; i++) {
    fl->bitracks_offset[i] = bitracks_start + br_cum * BITRACK_BYTES;
    fl->letters_offset[i] = letters_start + lt_cum;
    br_cum += fl->counts[i];
    lt_cum += (size_t)fl->counts[i] * (size_t)i;
  }
  return 0;
}

static int rack_to_bitrack(const char *rack_str, uint8_t *out16) {
  uint64_t lo = 0, hi = 0;
  for (const char *p = rack_str; *p != '\0'; p++) {
    int ml;
    if (*p == '?' || *p == '_') {
      ml = 0;
    } else if (*p >= 'A' && *p <= 'Z') {
      ml = (*p - 'A') + 1;
    } else if (*p >= 'a' && *p <= 'z') {
      ml = (*p - 'a') + 1;
    } else {
      fprintf(stderr, "bad rack char '%c'\n", *p);
      return 1;
    }
    const int shift = ml * 4;
    if (shift < 64) {
      lo += (uint64_t)1 << shift;
    } else {
      hi += (uint64_t)1 << (shift - 64);
    }
  }
  memcpy(out16, &lo, 8);
  memcpy(out16 + 8, &hi, 8);
  return 0;
}

static uint32_t cpu_count_matches(const uint8_t *bitracks, uint32_t n_words,
                                  const uint8_t *rack) {
  uint32_t count = 0;
  for (uint32_t i = 0; i < n_words; i++) {
    const uint8_t *w = bitracks + (size_t)i * BITRACK_BYTES;
    int ok = 1;
    for (int b = 0; b < BITRACK_BYTES && ok; b++) {
      if ((w[b] & 0x0F) > (rack[b] & 0x0F) ||
          ((w[b] >> 4) & 0x0F) > ((rack[b] >> 4) & 0x0F)) {
        ok = 0;
      }
    }
    if (ok) {
      count++;
    }
  }
  return count;
}

static char *metallib_path_from_argv(const char *argv0) {
  static char buf[1024];
  const char *slash = strrchr(argv0, '/');
  if (slash == NULL) {
    snprintf(buf, sizeof(buf), "movegen.metallib");
  } else {
    const size_t dir_len = (size_t)(slash - argv0);
    memcpy(buf, argv0, dir_len);
    snprintf(buf + dir_len, sizeof(buf) - dir_len, "/movegen.metallib");
  }
  return buf;
}

int main(int argc, const char **argv) {
  if (argc < 2) {
    fprintf(stderr,
            "usage: %s <flatlex_path> [rack=AABDELT] [length=7] [bench_iters=200]\n",
            argv[0]);
    return 1;
  }
  const char *flatlex_path = argv[1];
  const char *rack_str = (argc > 2) ? argv[2] : "AABDELT";
  const int length = (argc > 3) ? atoi(argv[3]) : 7;
  const int bench_iters = (argc > 4) ? atoi(argv[4]) : 200;

  FlatLex fl;
  if (flat_lex_load(flatlex_path, &fl) != 0) {
    return 1;
  }
  if (length < 0 || length >= fl.max_len_plus_one) {
    fprintf(stderr, "length %d out of range\n", length);
    return 1;
  }
  const uint32_t n_words = fl.counts[length];
  if (n_words == 0) {
    fprintf(stderr, "no words of length %d\n", length);
    return 1;
  }
  const uint8_t *bitracks_ptr = fl.data + fl.bitracks_offset[length];

  uint8_t rack_br[BITRACK_BYTES];
  if (rack_to_bitrack(rack_str, rack_br) != 0) {
    return 1;
  }

  printf("flatlex: total=%u, length=%d has %u words\n", fl.total_words, length,
         n_words);
  printf("rack: %s\n", rack_str);

  const uint64_t cpu_t0 = mach_absolute_time();
  uint32_t cpu_count = 0;
  for (int it = 0; it < bench_iters; it++) {
    cpu_count = cpu_count_matches(bitracks_ptr, n_words, rack_br);
  }
  const double cpu_dt = mach_seconds(mach_absolute_time() - cpu_t0);
  printf("CPU brute-force: %u matches/rack, %d iters in %.3f s = %.1f us/rack\n",
         cpu_count, bench_iters, cpu_dt, 1e6 * cpu_dt / bench_iters);

  if (!gpu_matcher_is_available()) {
    fprintf(stderr, "no Metal device, skipping GPU\n");
    free(fl.data);
    return 0;
  }
  // Load the entire lex (all lengths) into the matcher; dispatch over the
  // length-L slice with first_word/n_words. CLI bench doesn't exercise the
  // cross-check kernel, so we skip uploading letters here.
  const uint8_t *all_bitracks = fl.data + fl.bitracks_offset[0];
  GpuMatcher *m = gpu_matcher_create(metallib_path_from_argv(argv[0]),
                                     all_bitracks, fl.total_words, NULL, 0);
  if (m == NULL) {
    free(fl.data);
    return 1;
  }
  uint32_t first_word_for_length = 0;
  for (int L = 0; L < length; L++) {
    first_word_for_length += fl.counts[L];
  }

  const int B_MAX = 16384;
  uint8_t *batch_racks = (uint8_t *)malloc((size_t)B_MAX * BITRACK_BYTES);
  for (int i = 0; i < B_MAX; i++) {
    memcpy(batch_racks + (size_t)i * BITRACK_BYTES, rack_br, BITRACK_BYTES);
  }
  uint32_t *out_counts = (uint32_t *)malloc((size_t)B_MAX * sizeof(uint32_t));

  printf("\nGPU count_kernel timing (rack: %s, length %d, n_words=%u):\n",
         rack_str, length, n_words);
  printf("  %-7s %-9s %-10s %-12s %-12s\n", "B", "iters", "wall ms",
         "us/dispatch", "us/rack");
  const int Bs[] = {1, 64, 1024, 16384};
  const int n_Bs = sizeof(Bs) / sizeof(Bs[0]);
  for (int bi = 0; bi < n_Bs; bi++) {
    const int B = Bs[bi];
    for (int i = 0; i < 5; i++) {
      gpu_matcher_count(m, first_word_for_length, n_words, batch_racks,
                        (uint32_t)B, out_counts);
    }
    const int sane = ((int)out_counts[0] == (int)cpu_count);
    const uint64_t t0 = mach_absolute_time();
    for (int it = 0; it < bench_iters; it++) {
      gpu_matcher_count(m, first_word_for_length, n_words, batch_racks,
                        (uint32_t)B, out_counts);
    }
    const double dt = mach_seconds(mach_absolute_time() - t0);
    const double per_dispatch_us = 1e6 * dt / bench_iters;
    printf("  %-7d %-9d %-10.3f %-12.1f %-12.3f  %s\n", B, bench_iters,
           1e3 * dt, per_dispatch_us, per_dispatch_us / B,
           sane ? "" : "(SANITY FAIL)");
  }

  free(out_counts);
  free(batch_racks);
  gpu_matcher_destroy(m);
  free(fl.data);
  return 0;
}
