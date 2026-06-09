#ifndef PEG_GREEDY_BENCH_TEST_H
#define PEG_GREEDY_BENCH_TEST_H

// Deep-rational PEG reference evaluator ("greedy bench"): full scenario
// enumeration with a configurable-depth rational walker on non-emptier
// scenarios (RAT_WALK + OPP_RANK_BY_PLAYOUT + perception/walk strides) and
// exact endgames on emptier ones, emitting per-candidate TSV results. This is
// the research evaluator the production cascade (src/impl/peg.c) was distilled
// from; it remains the only in-repo evaluator with depth > 0 on non-emptier
// scenarios, so it serves as the deep-rational cross-check until production
// grows nested inner depth.
//
// Positions come from a CGP file (PASSPEG_GREEDY_PATH, default
// /tmp/peg_positions.txt). Key env knobs (PASSPEG_GREEDY_* unless noted):
// ONLY (cand filter), DEPTH, TOP_K, BUDGET, THREADS, RATIONAL, RAT_WALK,
// WALK_K_FIRST_OPP / WALK_K_LATER_OPP / WALK_K_BY_BAG, RESULT_FILE,
// PASSPEG_SCENARIO_STRIDE, PASSPEG_PERCEPTION_STRIDE, PASSPEG_WALK_STRIDE,
// PASSPEG_OPP_RANK_BY_PLAYOUT, PASSPEG_OPP_RANK_POOL, PASSPEG_INCLUDE_PASS.
void test_pass_peg_greedy_bench(void);

#endif // PEG_GREEDY_BENCH_TEST_H
