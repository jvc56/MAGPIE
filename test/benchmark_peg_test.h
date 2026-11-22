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
void test_peg_stage_stability(void);
void test_peg_strength_ab(void);
void test_gen_peg_fresh(void);
void test_peg_strength_curve(void);

void peg_switch_run_spec(const char *spec);
void peg_close_run_spec(const char *spec);
void peg_arms_run_spec(const char *spec);
void peg_mid_run_spec(const char *spec);
void peg_built_run_spec(const char *spec);
void peg_diag_run_spec(const char *spec);
void peg_tie_run_spec(const char *spec);
void peg_tie_loss_run_spec(const char *spec);
void peg_rejudge_run_spec(const char *spec);
void peg_soft_run_spec(const char *spec);
void peg_judge_run_spec(const char *spec);
void peg_nest_run_spec(const char *spec);

#endif
