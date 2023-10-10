#include <assert.h>
#include <stddef.h>

#include "../src/config.h"

#include "config_test.h"

#define TEST_MOVE_LIST_CAPACITY 1000000

void test_config_empty_string_klv2() {
  Config *config = create_config(
      "./data/letterdistributions/english.csv", "", "./data/lexica/CSW21.kwg",
      "./data/lexica/CSW21.klv2", MOVE_SORT_EQUITY, MOVE_RECORDER_ALL, "", "",
      -1, -1, 0, 3, 0, 0, NULL, 0, 0, 0, 0, 1, "", TEST_MOVE_LIST_CAPACITY);

  assert(config->klv_is_shared);
  assert(config->kwg_is_shared);
  assert(!config->use_game_pairs);
  assert(config->number_of_games_or_pairs == 3);
  config->player_1_strategy_params->klv->word_counts[0] = 3000;
  config->player_2_strategy_params->klv->word_counts[0] = 4000;
  assert(config->player_1_strategy_params->klv->word_counts[0] == 4000);

  destroy_config(config);
}

void test_config_identical_klv2() {
  Config *config = create_config(
      "./data/letterdistributions/english.csv", "", "./data/lexica/CSW21.kwg",
      "./data/lexica/CSW21.klv2", MOVE_SORT_EQUITY, MOVE_RECORDER_ALL,
      "./data/lexica/CSW21.kwg", "./data/lexica/CSW21.klv2", -1, -1, 0, 10000,
      0, 0, NULL, 0, 0, 0, 0, 1, "", TEST_MOVE_LIST_CAPACITY);

  assert(config->klv_is_shared);
  assert(config->kwg_is_shared);
  assert(!config->use_game_pairs);
  config->player_1_strategy_params->klv->word_counts[0] = 3000;
  config->player_2_strategy_params->klv->word_counts[0] = 4000;
  assert(config->player_1_strategy_params->klv->word_counts[0] == 4000);

  destroy_config(config);
}

void test_config_different_klv2() {
  Config *config = create_config(
      "./data/letterdistributions/english.csv", "", "./data/lexica/CSW21.kwg",
      "./data/lexica/CSW21.klv2", MOVE_SORT_EQUITY, MOVE_RECORDER_ALL,
      "./data/lexica/NWL20.kwg", "./data/lexica/english.klv2", -1, -1, 1, 10000,
      0, 0, NULL, 0, 0, 0, 0, 1, "", TEST_MOVE_LIST_CAPACITY);

  assert(!config->klv_is_shared);
  assert(!config->kwg_is_shared);
  assert(config->use_game_pairs);
  config->player_1_strategy_params->klv->word_counts[0] = 3000;
  config->player_2_strategy_params->klv->word_counts[0] = 4000;
  assert(config->player_1_strategy_params->klv->word_counts[0] == 3000);

  destroy_config(config);
}

void test_config() {
  test_config_empty_string_klv2();
  test_config_identical_klv2();
  test_config_different_klv2();
}