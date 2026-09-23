#ifndef LEAVE_ROLLOUT_VALUE_TEST_H
#define LEAVE_ROLLOUT_VALUE_TEST_H

// Isolates whether KLV leave value adds anything by steering a Monte Carlo
// rollout's own forward-play plies, analogous to
// pat_rollout_value_test.c's PAT question but for leave instead of PAT and
// with a cheaper (shallower) oracle -- see leave_rollout_value_test.c for
// the full methodology. Deliberately separate test code/log files from the
// PAT experiment: different question (leave, not PAT), different oracle
// depth, no shared state.
void test_leave_rollout_value_accumulate(void);

#endif
