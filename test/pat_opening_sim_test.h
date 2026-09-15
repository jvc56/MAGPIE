#ifndef PAT_OPENING_SIM_TEST_H
#define PAT_OPENING_SIM_TEST_H

void test_pat_opening_sim(void);
void pat_opening_sim_run(const char *lexicon, const char *pat_name,
                         int max_racks, const char *leaves);
void pat_opening_sim_run_spec(const char *spec);

#endif
