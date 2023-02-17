#include <assert.h>

#include "../src/config.h"

#include "config_test.h"

void test_config_empty_string_laddag() {
    Config * config = create_config(
      "./data/lexica/CSW21.gaddag",
      "./data/lexica/CSW21.alph",
      "./data/letterdistributions/english.dist",
      "",
      "./data/lexica/CSW21.laddag",
      SORT_BY_EQUITY,
      PLAY_RECORDER_TYPE_ALL,
      "",
      -1,
      -1
    );

    assert(config->laddag_is_shared);
    config->player_1_strategy_params->laddag->edges[0] = 3000;
    config->player_2_strategy_params->laddag->edges[0] = 4000;
    assert(config->player_1_strategy_params->laddag->edges[0] == 4000);

    destroy_config(config);
}

void test_config_identical_laddag() {
    Config * config = create_config(
      "./data/lexica/CSW21.gaddag",
      "./data/lexica/CSW21.alph",
      "./data/letterdistributions/english.dist",
      "",
      "./data/lexica/CSW21.laddag",
      SORT_BY_EQUITY,
      PLAY_RECORDER_TYPE_ALL,
      "./data/lexica/CSW21.laddag",
      -1,
      -1
    );

    assert(config->laddag_is_shared);
    config->player_1_strategy_params->laddag->edges[0] = 3000;
    config->player_2_strategy_params->laddag->edges[0] = 4000;
    assert(config->player_1_strategy_params->laddag->edges[0] == 4000);

    destroy_config(config);
}

void test_config_different_laddag() {
    Config * config = create_config(
      "./data/lexica/CSW21.gaddag",
      "./data/lexica/CSW21.alph",
      "./data/letterdistributions/english.dist",
      "",
      "./data/lexica/CSW21.laddag",
      SORT_BY_EQUITY,
      PLAY_RECORDER_TYPE_ALL,
      "./data/lexica/America.laddag",
      -1,
      -1
    );

    assert(!config->laddag_is_shared);
    config->player_1_strategy_params->laddag->edges[0] = 3000;
    config->player_2_strategy_params->laddag->edges[0] = 4000;
    assert(config->player_1_strategy_params->laddag->edges[0] == 3000);

    destroy_config(config);
}


void test_config() {
    test_config_empty_string_laddag();
    test_config_identical_laddag();
    test_config_different_laddag();
}