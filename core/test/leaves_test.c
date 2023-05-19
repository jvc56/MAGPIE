#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

#include "../src/alphabet.h"

#include "test_util.h"
#include "superconfig.h"

void test_leaves(SuperConfig * superconfig, const char* leaves_csv_filename) {
    Config * config = get_csw_config(superconfig);
    KLV * klv = config->player_1_strategy_params->klv;
    Alphabet * alphabet = config->kwg->alphabet;
    Rack * rack = create_rack(config->letter_distribution->size);


    FILE* file = fopen(leaves_csv_filename, "r");
    if (file == NULL) {
        printf("Error opening file: %s\n", leaves_csv_filename);
        abort();
    }

    char line[100];
    while (fgets(line, sizeof(line), file)) {
        char* token;
        // Leave token
        token = strtok(line, ",");
        char* leave = strdup(token);

        // Value token
        token = strtok(NULL, ",");
        float actual_value = strtof(token, NULL);

        set_rack_to_string(rack, leave, alphabet);
        float klv_leave_value = leave_value(klv, rack);
        assert(within_epsilon_float(klv_leave_value, actual_value));
    }

    fclose(file);
}