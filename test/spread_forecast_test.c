#include "spread_forecast_test.h"

#include "../src/compat/endian_conv.h"
#include "../src/ent/equity.h"
#include "../src/ent/spread_forecast.h"
#include "../src/impl/config.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

enum {
  TEST_MAX_BAG = 2,
  TEST_MAX_RACK = 7,
  TEST_RACK_STATES = TEST_MAX_RACK + 1,
  TEST_CELL_COUNT = (TEST_MAX_BAG + 1) * TEST_RACK_STATES * TEST_RACK_STATES,
};

static const char *const TEST_MODEL_NAME = "test_spread_forecast";
static const char *const TEST_MODEL_PATH =
    "data/strategy/test_spread_forecast.sfc";

static int test_cell_index(int bag, int on_turn_rack, int off_turn_rack) {
  return (bag * TEST_RACK_STATES + on_turn_rack) * TEST_RACK_STATES +
         off_turn_rack;
}

static void write_test_spread_forecast(void) {
  FILE *stream = fopen(TEST_MODEL_PATH, "wbe");
  assert(stream != NULL);
  const uint8_t magic[8] = {'M', 'A', 'G', 'S', 'P', 'R', 'D', '\0'};
  const size_t magic_written = fwrite(magic, sizeof(magic), 1, stream);
  assert(magic_written == 1);
  const uint32_t header[4] = {
      convert_uint32_to_le(1),
      convert_uint32_to_le(TEST_MAX_BAG),
      convert_uint32_to_le(TEST_MAX_RACK),
      convert_uint32_to_le(TEST_CELL_COUNT),
  };
  const size_t header_written = fwrite(header, sizeof(uint32_t), 4, stream);
  assert(header_written == 4);
  for (int i = 0; i < TEST_CELL_COUNT; i++) {
    float spread = 0.0F;
    float on_turn_turns = 0.0F;
    float off_turn_turns = 0.0F;
    uint32_t samples = 0;
    if (i == test_cell_index(2, 7, 7)) {
      spread = 12.5F;
      on_turn_turns = 3.25F;
      off_turn_turns = 2.75F;
      samples = 123;
    } else if (i == test_cell_index(0, 4, 7)) {
      spread = -8.0F;
      on_turn_turns = 2.0F;
      off_turn_turns = 3.0F;
      samples = 456;
    }
    const float values[3] = {
        convert_float_to_le(spread),
        convert_float_to_le(on_turn_turns),
        convert_float_to_le(off_turn_turns),
    };
    const uint32_t le_samples = convert_uint32_to_le(samples);
    const size_t values_written = fwrite(values, sizeof(float), 3, stream);
    assert(values_written == 3);
    const size_t samples_written =
        fwrite(&le_samples, sizeof(uint32_t), 1, stream);
    assert(samples_written == 1);
  }
  fclose_or_die(stream);
}

void test_spread_forecast(void) {
  write_test_spread_forecast();
  ErrorStack *error_stack = error_stack_create();
  SpreadForecast *forecast =
      spread_forecast_create("data", TEST_MODEL_NAME, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(forecast != NULL);
  assert(strings_equal(spread_forecast_get_name(forecast), TEST_MODEL_NAME));

  const Equity projected =
      spread_forecast_get_on_turn(forecast, double_to_equity(10.0), 2, 7, 7);
  assert(within_epsilon(equity_to_double(projected), 22.5));
  // Oversized values clamp to the model's trained edge.
  assert(spread_forecast_get_on_turn(forecast, double_to_equity(10.0), 99, 99,
                                     99) == projected);
  assert(within_epsilon(
      spread_forecast_get_expected_turns_for_counts(forecast, 2, 7, 7, true),
      3.25));
  assert(within_epsilon(
      spread_forecast_get_expected_turns_for_counts(forecast, 2, 7, 7, false),
      2.75));

  const Equity post_bag =
      spread_forecast_get_on_turn(forecast, double_to_equity(20.0), 0, 4, 7);
  assert(within_epsilon(equity_to_double(post_bag), 12.0));

  spread_forecast_destroy(forecast);

  Config *config = config_create_default_test();
  config_load_command(config, "set -spreadforecast test_spread_forecast",
                      error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(config_get_spread_forecast(config) != NULL);
  assert(strings_equal(
      spread_forecast_get_name(config_get_spread_forecast(config)),
      TEST_MODEL_NAME));
  config_load_command(config, "set -spreadforecast none", error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(config_get_spread_forecast(config) == NULL);
  config_destroy(config);

  error_stack_destroy(error_stack);
  const int remove_result = remove(TEST_MODEL_PATH);
  assert(remove_result == 0);
}
