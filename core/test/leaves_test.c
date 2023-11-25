#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../src/log.h"
#include "../src/string_util.h"
#include "../src/util.h"

#include "test_util.h"
#include "testconfig.h"

void test_leaves(TestConfig *testconfig) {
  const Config *config = get_csw_config(testconfig);
  const KLV *klv = players_data_get_klv(config->players_data, 0);
  const LetterDistribution *letter_distribution = config->letter_distribution;
  Rack *rack = create_rack(config->letter_distribution->size);

  const char *leaves_csv_filename = "./data/lexica/CSW21.csv";
  FILE *file = fopen(leaves_csv_filename, "r");
  if (!file) {
    log_fatal("Error opening file: %s\n", leaves_csv_filename);
  }

  char line[100];
  while (fgets(line, sizeof(line), file)) {
    StringSplitter *leave_and_value = split_string(line, ',', true);
    set_rack_to_string(letter_distribution, rack,
                       string_splitter_get_item(leave_and_value, 0));
    double klv_leave_value = get_leave_value(klv, rack);
    assert(within_epsilon(
        klv_leave_value,
        string_to_double(string_splitter_get_item(leave_and_value, 1))));

    destroy_string_splitter(leave_and_value);
  }

  destroy_rack(rack);
  fclose(file);
}