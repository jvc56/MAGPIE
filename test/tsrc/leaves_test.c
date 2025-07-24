#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../src/def/equity_defs.h"
#include "../../src/ent/data_filepaths.h"
#include "../../src/ent/equity.h"
#include "../../src/ent/klv.h"
#include "../../src/ent/letter_distribution.h"
#include "../../src/ent/players_data.h"
#include "../../src/ent/rack.h"

#include "../../src/impl/config.h"

#include "../../src/util/io_util.h"
#include "../../src/util/string_util.h"

#include "test_util.h"

void test_leaves(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  const KLV *klv = players_data_get_klv(config_get_players_data(config), 0);
  const LetterDistribution *ld = config_get_ld(config);
  Rack *rack = rack_create(ld_get_size(ld));
  ErrorStack *error_stack = error_stack_create();

  char *leaves_csv_filename = data_filepaths_get_readable_filename(
      config_get_data_paths(config), "CSW21", DATA_FILEPATH_TYPE_LEAVES,
      error_stack);

  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("failed to get leaves csv filepath for test\n");
  }

  FILE *file = fopen_or_die(leaves_csv_filename, "r");
  free(leaves_csv_filename);
  char line[100];
  while (fgets(line, sizeof(line), file)) {
    StringSplitter *leave_and_value = split_string(line, ',', true);
    rack_set_to_string(ld, rack, string_splitter_get_item(leave_and_value, 0));
    const Equity klv_leave_value = klv_get_leave_value(klv, rack);
    const double csv_leave_value_double = string_to_double(
        string_splitter_get_item(leave_and_value, 1), error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_print_and_reset(error_stack);
      abort();
    }
    if (fabs(equity_to_double(klv_leave_value) - csv_leave_value_double) >
        (double)EQUITY_RESOLUTION) {
      log_fatal(
          "leave value mismatch for rack '%s': abs(%0.20f, %.20f) > %0.20f\n",
          string_splitter_get_item(leave_and_value, 0),
          equity_to_double(klv_leave_value), csv_leave_value_double,
          (double)EQUITY_RESOLUTION);
    }
    string_splitter_destroy(leave_and_value);
  }

  fclose_or_die(file);

  error_stack_destroy(error_stack);
  rack_destroy(rack);
  config_destroy(config);
}