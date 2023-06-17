#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "superconfig.h"
#include "test_util.h"

void test_leaves(SuperConfig *superconfig, const char *leaves_csv_filename) {
  Config *config = get_csw_config(superconfig);
  KLV *klv = config->player_1_strategy_params->klv;
  LetterDistribution *letter_distribution = config->letter_distribution;
  Rack *rack = create_rack(config->letter_distribution->size);

  FILE *file = fopen(leaves_csv_filename, "r");
  if (file == NULL) {
    printf("Error opening file: %s\n", leaves_csv_filename);
    abort();
  }

  char line[100];
  while (fgets(line, sizeof(line), file)) {
    char *token;
    // Leave token
    token = strtok(line, ",");
    char *leave = strdup(token);

    // Value token
    token = strtok(NULL, ",");
    double actual_value = strtof(token, NULL);

    set_rack_to_string(rack, leave, letter_distribution);
    double klv_leave_value = get_leave_value(klv, rack);
    assert(within_epsilon(klv_leave_value, actual_value));

    free(leave);
  }

  destroy_rack(rack);
  fclose(file);
}