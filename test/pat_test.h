#ifndef PAT_TEST_H
#define PAT_TEST_H

void test_pat(void);
// WMP recording against exhaustive recording with PAT, under every speed
// table the lexicons have; also part of test_pat.
void test_pat_wmp_parity(void);
void pat_run_through_table_check(const char *lexicon);

#endif
