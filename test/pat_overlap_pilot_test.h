#ifndef PAT_OVERLAP_PILOT_TEST_H
#define PAT_OVERLAP_PILOT_TEST_H

#include "../src/ent/game.h"

void test_pat_overlap_pilot(void);
int pat_overlap_narrow_measure(const Game *game, int mover_index);
int pat_lm_aggregate(const Game *game, int mover_index);
void test_pat_premium_combo_pilot(void);

#endif
