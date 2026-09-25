#include "win_pct_test.h"

#include "../src/ent/win_pct.h"
#include "../src/impl/config.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void assert_win_pct_get(const float actual, const double expected) {
  assert(within_epsilon(actual, expected));
}

void test_win_pct(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 score -s2 score -r1 all -r2 all -numplays 1 "
      "-winpct winpct");
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

// Writes testdata/strategy/<name>.csv: the rows of testdata's winpct.csv
// with its last row repeated until the table has num_rows rows.
static void write_padded_win_pct(const char *name, int num_rows) {
  FILE *in = fopen("./testdata/strategy/winpct.csv", "r");
  assert(in);
  char *path = get_formatted_string("./testdata/strategy/%s.csv", name);
  FILE *out = fopen(path, "w");
  assert(out);
  free(path);
  static char line[1 << 16];
  static char last[1 << 16];
  int rows = 0;
  while (fgets(line, sizeof(line), in)) {
    if (line[0] == '\n' || line[0] == '\0') {
      continue;
    }
    fputs(line, out);
    if (line[strlen(line) - 1] != '\n') {
      fputc('\n', out);
    }
    snprintf(last, sizeof(last), "%s", line);
    rows++;
  }
  fclose(in);
  for (; rows < num_rows; rows++) {
    fputs(last, out);
    if (last[strlen(last) - 1] != '\n') {
      fputc('\n', out);
    }
  }
  fclose(out);
}

void test_win_pct_coverage(void) {
  // An explicitly chosen table is kept across a lexicon change, and a
  // 102-tile bag (95 unseen at most) outgrows the 93-row English table.
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 score -s2 score -r1 all -r2 all -numplays 1 "
      "-winpct winpct");
  assert_config_exec_status(config, "set -lex FRA20",
                            ERROR_STATUS_CONFIG_WIN_PCT_TOO_SMALL);
  config_destroy(config);

  // Without -winpct the table stays lazy at set time and is checked when
  // it is first needed.
  config = config_create_or_die(
      "set -lex FRA20 -s1 score -s2 score -r1 all -r2 all -numplays 1");
  ErrorStack *error_stack = error_stack_create();
  config_load_win_pcts(config, error_stack);
  assert(error_stack_top(error_stack) == ERROR_STATUS_CONFIG_WIN_PCT_TOO_SMALL);
  error_stack_destroy(error_stack);
  config_destroy(config);

  // A table named for the distribution is the default when it exists, and
  // one with enough rows passes.
  write_padded_win_pct("winpct_french", 95);
  config = config_create_or_die(
      "set -lex FRA20 -s1 score -s2 score -r1 all -r2 all -numplays 1");
  error_stack = error_stack_create();
  config_load_win_pcts(config, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(strings_equal(win_pct_get_name(config_get_win_pcts(config)),
                       "winpct_french"));
  assert(win_pct_get_max_tiles_unseen(config_get_win_pcts(config)) == 95);
  error_stack_destroy(error_stack);
  config_destroy(config);
  remove("./testdata/strategy/winpct_french.csv");
}
