#ifndef PEG_TEST_H
#define PEG_TEST_H

// Fast, self-contained smoke tests for the pre-endgame (PEG) solver that run
// in the main suite (the heavier oracle/benchmark coverage stays on-demand in
// pass_peg_search_test / benchmark_peg_test). Each drives the public peg_solve
// API on a known position via PegArgs.only_moves so the cost is one candidate.
void test_peg(void);

#endif
