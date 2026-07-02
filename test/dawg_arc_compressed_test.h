#ifndef DAWG_ARC_COMPRESSED_TEST_H
#define DAWG_ARC_COMPRESSED_TEST_H

void test_dawg_arc_compressed(void);

// On-demand (acdawgstats): size breakdown + what-if sizing of candidate
// re-encodings, computed analytically from the arc distributions.
void test_dawg_arc_compressed_stats(void);

// On-demand (acdawgbench): word-lookup + full-enumeration timings vs the
// packed DAWG on the same corpus, to check size changes for speed regressions.
void test_dawg_arc_compressed_bench(void);

#endif
