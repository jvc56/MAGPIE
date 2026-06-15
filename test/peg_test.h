#ifndef PEG_TEST_H
#define PEG_TEST_H

// Fast, self-contained smoke tests for the pre-endgame (PEG) solver that run
// in the main suite (the heavier reference/benchmark coverage stays on-demand
// in peg_pess_test / benchmark_peg_test). Each drives the public peg_solve API
// on a known position via PegArgs.only_moves so the cost is one candidate.
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

// Pessimistic (worst-case opponent) variants of the 3-/4-bag anchors
// (test.c keys peg3pahpess / peg4pondpess), bounded by inner_top_k (opponent
// replies weighed) and scenario_stride (bag orderings sampled) so they stay
// tractable. Pin production's bounded-pessimistic win%/spread for the move.
void test_peg_3bag_pah_pessimistic(void);
void test_peg_4bag_pond_pessimistic(void);

#endif
