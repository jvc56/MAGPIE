#ifndef BENCHMARK_PEG_TEST_H
#define BENCHMARK_PEG_TEST_H

void test_benchmark_peg_3(void);
void test_benchmark_peg_4(void);

// On-demand: regenerate the notes/peg_positions/random_{2,3,4}peg.txt fixtures
// (random_{3,4}peg drive the benchmarks above).
void test_generate_peg_cgps(void);

#endif
