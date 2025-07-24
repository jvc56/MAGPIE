#include <assert.h>
#include <stdint.h>

#include "../../src/ent/win_pct.h"
#include "../../src/impl/config.h"

#include "test_util.h"

void assert_win_pct_get(const float actual, const double expected) {
  assert(within_epsilon(actual, (float)expected));
}

void test_win_pct(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 score -s2 score -r1 all -r2 all -numplays 1");
  const WinPct *win_pct = config_get_win_pcts(config);
  // Test "corners"
  assert_win_pct_get(win_pct_get(win_pct, -600, 1), 0.0);
  assert_win_pct_get(win_pct_get(win_pct, -500, 1), 0.0);
  assert_win_pct_get(win_pct_get(win_pct, -600, 93),
                     9644 / (double)((uint64_t)2932802774 * 2));
  assert_win_pct_get(win_pct_get(win_pct, -500, 93),
                     9644 / (double)((uint64_t)2932802774 * 2));
  assert_win_pct_get(win_pct_get(win_pct, 600, 1), 1.0);
  assert_win_pct_get(win_pct_get(win_pct, 500, 1), 1.0);
  assert_win_pct_get(win_pct_get(win_pct, 600, 93),
                     5865602560 / (double)((uint64_t)2932802774 * 2));
  assert_win_pct_get(win_pct_get(win_pct, 500, 93),
                     5865602560 / (double)((uint64_t)2932802774 * 2));
  // Test various other cases
  assert_win_pct_get(win_pct_get(win_pct, -490, 78),
                     1504 / (double)((uint64_t)526840707 * 2));
  assert_win_pct_get(win_pct_get(win_pct, -490, 91),
                     198 / (double)((uint64_t)88159945 * 2));
  assert_win_pct_get(win_pct_get(win_pct, -490, 92),
                     198 / (double)((uint64_t)88159945 * 2));
  assert_win_pct_get(win_pct_get(win_pct, 0, 93),
                     3267384562 / (double)((uint64_t)2932802774 * 2));
  assert_win_pct_get(win_pct_get(win_pct, -200, 93),
                     180395057 / (double)((uint64_t)2932802774 * 2));
  assert_win_pct_get(win_pct_get(win_pct, 250, 93),
                     5842108920 / (double)((uint64_t)2932802774 * 2));
  config_destroy(config);
}
