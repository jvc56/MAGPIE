#ifndef WINPCT_TEST_H
#define WINPCT_TEST_H

#include "../src/config.h"
#include "testconfig.h"

void test_sim(TestConfig *testconfig);

void perf_test_sim(Config *config, ThreadControl *thread_control);
void perf_test_multithread_sim(Config *config, ThreadControl *thread_control);
void perf_test_multithread_blocking_sim(Config *config,
                                        ThreadControl *thread_control);

#endif