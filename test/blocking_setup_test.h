#ifndef BLOCKING_SETUP_TEST_H
#define BLOCKING_SETUP_TEST_H

void test_blocking_setup(void);
void blocking_setup_replay_run_spec(const char *spec);
void blocking_setup_bench_run_spec(const char *spec);
void blocking_setup_race_run_spec(const char *spec);

#endif
