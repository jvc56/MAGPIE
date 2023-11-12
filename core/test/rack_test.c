#include <assert.h>
#include <stdio.h>

#include "../src/config.h"
#include "../src/constants.h"
#include "../src/letter_distribution.h"
#include "../src/rack.h"

#include "rack_test.h"
#include "test_util.h"
#include "testconfig.h"

void test_rack_main(TestConfig *testconfig) {
  const Config *config = get_nwl_config(testconfig);
  Rack *rack = create_rack(config->letter_distribution->size);
  Rack *expected_rack = create_rack(config->letter_distribution->size);

  // Test score on rack
  set_rack_to_string(rack, "ABCDEFG", config->letter_distribution);
  assert(score_on_rack(config->letter_distribution, rack) == 16);
  set_rack_to_string(rack, "XYZ", config->letter_distribution);
  assert(score_on_rack(config->letter_distribution, rack) == 22);
  set_rack_to_string(rack, "??", config->letter_distribution);
  assert(score_on_rack(config->letter_distribution, rack) == 0);
  set_rack_to_string(rack, "?QWERTY", config->letter_distribution);
  assert(score_on_rack(config->letter_distribution, rack) == 21);
  set_rack_to_string(rack, "RETINAO", config->letter_distribution);
  assert(score_on_rack(config->letter_distribution, rack) == 7);
  set_rack_to_string(rack, "AABBEWW", config->letter_distribution);
  assert(score_on_rack(config->letter_distribution, rack) == 17);

  set_rack_to_string(rack, "AENPPSW", config->letter_distribution);

  for (int i = 0; i < (expected_rack->array_size); i++) {
    expected_rack->array[i] = 0;
  }
  expected_rack->array[1] = 1;
  expected_rack->array[5] = 1;
  expected_rack->array[14] = 1;
  expected_rack->array[16] = 2;
  expected_rack->array[19] = 1;
  expected_rack->array[23] = 1;
  expected_rack->empty = 0;
  expected_rack->number_of_letters = 7;

  assert(equal_rack(expected_rack, rack));

  take_letter_from_rack(rack, 16);
  expected_rack->array[16] = 1;
  expected_rack->number_of_letters = 6;
  assert(equal_rack(expected_rack, rack));

  take_letter_from_rack(rack, 14);
  expected_rack->array[14] = 0;
  expected_rack->number_of_letters = 5;
  assert(equal_rack(expected_rack, rack));

  take_letter_from_rack(rack, 1);
  assert(!rack->empty);
  take_letter_from_rack(rack, 5);
  assert(!rack->empty);
  take_letter_from_rack(rack, 16);
  assert(!rack->empty);
  take_letter_from_rack(rack, 19);
  assert(!rack->empty);
  take_letter_from_rack(rack, 23);
  assert(rack->empty);

  for (int i = 0; i < (expected_rack->array_size); i++) {
    expected_rack->array[i] = 0;
  }
  expected_rack->empty = 1;
  expected_rack->number_of_letters = 0;

  assert(equal_rack(expected_rack, rack));

  add_letter_to_rack(rack, 13);
  take_letter_from_rack(rack, 13);
  add_letter_to_rack(rack, 13);
  add_letter_to_rack(rack, 13);

  expected_rack->array[13] = 2;
  expected_rack->empty = 0;
  expected_rack->number_of_letters = 2;

  assert(equal_rack(expected_rack, rack));

  destroy_rack(rack);
  destroy_rack(expected_rack);
}

void test_rack(TestConfig *testconfig) { test_rack_main(testconfig); }
