#ifndef BENCHMARK_PEG_TEST_H
#define BENCHMARK_PEG_TEST_H

void test_benchmark_peg_1(void);
void test_benchmark_peg_2(void);
void test_benchmark_peg_3(void);
void test_benchmark_peg_4(void);

// On-demand: regenerate the notes/peg_positions/random_{1,2,3,4}peg.txt
// fixtures that drive the benchmarks above.
void test_generate_peg_cgps(void);

// CI test: -pegtopk all (no candidate cap = exhaustive single stage) keeps the
// whole field. Fast: a canned near-decided 2-in-bag position whose exhaustive
// solve completes in ~0.03s.
void test_peg_pegtopk_all(void);

void test_peg_bench_fixture(void);

void test_peg_nested_gap(void);

void test_gen_peg_more(void);

#endif
