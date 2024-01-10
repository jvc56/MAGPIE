#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "../../src/ent/config.h"
#include "../../src/ent/klv.h"
#include "../../src/ent/letter_distribution.h"
#include "../../src/ent/players_data.h"
#include "../../src/ent/rack.h"

#include "../../src/util/log.h"
#include "../../src/util/string_util.h"
#include "../../src/util/util.h"

#include "test_util.h"

void test_leaves() {
  Config *config = create_config_or_die(
      "setoptions lex CSW21 s1 equity s2 equity r1 all r2 all numplays 1");
  const KLV *klv = players_data_get_klv(config_get_players_data(config), 0);
  const LetterDistribution *ld = config_get_ld(config);
  Rack *rack = rack_create(ld_get_size(ld));

  const char *leaves_csv_filename = "./data/lexica/CSW21.csv";
  FILE *file = fopen(leaves_csv_filename, "r");
  if (!file) {
    log_fatal("Error opening file: %s\n", leaves_csv_filename);
  }

  char line[100];
  while (fgets(line, sizeof(line), file)) {
    StringSplitter *leave_and_value = split_string(line, ',', true);
    rack_set_to_string(ld, rack, string_splitter_get_item(leave_and_value, 0));
    double klv_leave_value = klv_get_leave_value(klv, rack);
    assert(within_epsilon(
        klv_leave_value,
        string_to_double(string_splitter_get_item(leave_and_value, 1))));

    string_splitter_destroy(leave_and_value);
  }

  fclose(file);

  rack_destroy(rack);
  config_destroy(config);
}