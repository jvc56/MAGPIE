#ifndef PAT_COST_RATIO_TEST_H
#define PAT_COST_RATIO_TEST_H

// Part 1: measures the real per-iteration wall-clock cost of a rollout's
// forward-play plies with PAT active vs PAT forced off (see
// player_set_rollout_disable_pat in player.h), on the SAME frozen
// candidate lists, same seeds, same iteration counts as
// pat_rollout_value_test.c's own outer round-robin comparison. Prints the
// cost ratio r = seconds-per-iteration(PAT on) / seconds-per-iteration(PAT
// off).
void test_pat_cost_ratio_measure(void);

// Same idea, but timing three rollout conditions instead of two: full PAT,
// TWS-only (PAT_CLASS_MASK_ALL & ~PAT_CLASS_MASK_TWS_ONLY via
// player_set_rollout_pat_disabled_classes_mask -- pat.h's "a fast mode e.g.
// TWS only, for rollouts"), and PAT off. Reports all three pairwise
// ratios, in particular tws_only/off: what a TWS-only rollout costs
// relative to no PAT at all.
void test_pat_cost_ratio_tws_only_measure(void);

// Part 2 (pilot). For each position, compares PAT-on at
// PAT_COST_RATIO_ITERATIONS_PER_CANDIDATE iterations/candidate against
// PAT-off at the SAME iteration count (the naive, equal-sample-count
// comparison pat_rollout_value_test.c's phase 1 already makes) AND against
// PAT-off at round(PAT_COST_RATIO_ITERATIONS_PER_CANDIDATE * r)
// iterations/candidate (the equal-wall-clock comparison), where r is
// test_pat_cost_ratio_measure's own printed ratio, passed in by the
// caller (run_test's "patcostratiopilot:<r>" spec) since a pilot run
// should use a freshly measured ratio rather than a hard-coded one.
// Reports how often PAT-on disagrees with PAT-off under each comparison,
// and how often the two PAT-off conditions (equal-count vs equal-time)
// disagree with each other.
void pat_cost_ratio_crossover_pilot(double cost_ratio_on_over_off);
// "<r>" -- run_test's "patcostratiopilot:<r>" hook; r is
// test_pat_cost_ratio_measure's own printed ratio (a fresh pilot should
// use a freshly measured ratio, not a hard-coded one).
void pat_cost_ratio_run_pilot_spec(const char *spec);

#endif
