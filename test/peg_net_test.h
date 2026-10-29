#ifndef PEG_NET_TEST_H
#define PEG_NET_TEST_H

// pegnet:<mode>:<fields>: runs one PEG-net harness mode (training data in
// peg_net_data_test.h, evaluation in peg_net_eval_test.h, the judge in
// peg_judge.h).
void peg_net_test_run_spec(const char *spec);

#endif
