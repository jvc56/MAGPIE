#include <assert.h>
#include <stdio.h>

#include "../src/winpct.h"

#include "superconfig.h"
#include "test_util.h"

void test_win_pct(SuperConfig *superconfig) {
  Config *config = get_csw_config(superconfig);
  assert(within_epsilon(win_pct(config->win_pcts, 118, 90), 0.844430));
}

void test_sim(SuperConfig *superconfig) {
  test_win_pct(superconfig);
}