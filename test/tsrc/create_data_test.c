#include <assert.h>

#include "../../src/impl/config.h"

#include "test_util.h"

void test_create_data(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  const char *klv_filename = "CSW21_small_create_test";
  char *create_data_command =
      get_formatted_string("createdata klv %s english_small", klv_filename);
  char *set_klv_command = get_formatted_string(
      "set -k1 %s -k2 %s -ld english_small", klv_filename, klv_filename);

  load_and_exec_config_or_die(config, "set -path testdata");
  load_and_exec_config_or_die(config, create_data_command);
  load_and_exec_config_or_die(config, set_klv_command);

  const KLV *klv = players_data_get_klv(config_get_players_data(config), 0);
  assert(klv_get_number_of_leaves(klv) == 11);
  const LetterDistribution *ld = config_get_ld(config);
  Rack *leave = rack_create(ld_get_size(ld));
  rack_set_to_string(ld, leave, "AAB");
  assert(klv_get_leave_value(klv, leave) == EQUITY_ZERO_VALUE);

  free(create_data_command);
  free(set_klv_command);
  rack_destroy(leave);
  config_destroy(config);
}