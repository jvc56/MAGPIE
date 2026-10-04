#ifndef SIMINF_ORACLE_TEST_H
#define SIMINF_ORACLE_TEST_H

// Oracle-eval pilot for simmed inference: at many positions, let SIMINF and
// NOINF each pick a move (test/simmedinf_benchmark.c's play_sim_turn), keep
// only positions where they disagree, then score both candidate moves with a
// much stronger/slower oracle (best-equity playout to bag-empty, then a real
// endgame solve), sharing the resampled opponent rack across the pair so
// per-position noise cancels rather than accumulating. See
// logs/inference-bugs/RESULTS.md and the "oracle-eval" plan for the full
// methodology.

// Generates the position corpus (notes/siminf_positions/oracle_pilot.txt):
// self-play to a random bag depth, then a NOINF-vs-SIMINF disagreement check
// at each candidate position.
void test_generate_siminf_oracle_positions(void);

// Reads the corpus and runs the paired oracle scoring, logging per-position
// results (notes/siminf_positions/oracle_pilot_results.csv) and printing the
// aggregate paired-difference statistics plus a sample-size recommendation.
void test_siminf_oracle_eval(void);

#endif
