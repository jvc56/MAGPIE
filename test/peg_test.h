#ifndef PEG_TEST_H
#define PEG_TEST_H

// Fast, self-contained smoke tests for the pre-endgame (PEG) solver that run
// in the main suite (the heavier reference/benchmark coverage stays on-demand
// in peg_pess_test / peg_greedy_bench_test / benchmark_peg_test). Each drives
// the public peg_solve API on a known position via PegArgs.only_moves so the
// cost is one candidate.
void test_peg(void);

// On-demand deep anchors (test.c keys peg1pb / peg1onyx / peg2axe / peg2acid /
// peg3pah / peg4pond): full-position production peg_solve runs on studied
// boards at strong deterministic settings, pinning the best move and its
// win%/spread as regression anchors (macondo / GillesB exact values where
// published).
void test_peg_1bag_pass_best(void);
void test_peg_1bag_onyx(void);
void test_peg_2bag_axe(void);
void test_peg_2bag_acidotic(void);
void test_peg_3bag_pah(void);
void test_peg_4bag_pond(void);

#endif
