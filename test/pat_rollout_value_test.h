#ifndef PAT_ROLLOUT_VALUE_TEST_H
#define PAT_ROLLOUT_VALUE_TEST_H

// Isolates whether PAT adds value by steering forward-play *during* a Monte
// Carlo rollout, beyond whatever it already contributed to picking the
// candidates a round-robin simulation is handed. See pat_rollout_value_test.c
// for the full methodology.
void test_pat_rollout_value(void);
// Continuous accumulation mode: see the file comment above
// test_pat_rollout_value_accumulate in the .c file.
void test_pat_rollout_value_accumulate(void);

#endif
