#include "spread_forecast.h"

#include "../compat/endian_conv.h"
#include "../def/equity_defs.h"
#include "../def/rack_defs.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "bag.h"
#include "data_filepaths.h"
#include "equity.h"
#include "game.h"
#include "player.h"
#include "rack.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  SPREAD_FORECAST_VERSION = 1,
  SPREAD_FORECAST_HEADER_U32S = 4,
};

static const uint8_t SPREAD_FORECAST_MAGIC[8] = {
    'M', 'A', 'G', 'S', 'P', 'R', 'D', '\0',
};

typedef struct SpreadForecastCell {
  Equity expected_spread_swing;
  float expected_on_turn_turns;
  float expected_off_turn_turns;
  uint32_t sample_count;
} SpreadForecastCell;

struct SpreadForecast {
  char *name;
  uint32_t max_bag_tiles;
  uint32_t max_rack_tiles;
  uint32_t rack_states;
  SpreadForecastCell *cells;
};

static size_t spread_forecast_cell_index(const SpreadForecast *forecast,
                                         uint32_t bag_tiles,
                                         uint32_t on_turn_rack_tiles,
                                         uint32_t off_turn_rack_tiles) {
  if (bag_tiles > forecast->max_bag_tiles) {
    bag_tiles = forecast->max_bag_tiles;
  }
  if (on_turn_rack_tiles > forecast->max_rack_tiles) {
    on_turn_rack_tiles = forecast->max_rack_tiles;
  }
  if (off_turn_rack_tiles > forecast->max_rack_tiles) {
    off_turn_rack_tiles = forecast->max_rack_tiles;
  }
  return ((size_t)bag_tiles * forecast->rack_states + on_turn_rack_tiles) *
             forecast->rack_states +
         off_turn_rack_tiles;
}

static const SpreadForecastCell *
spread_forecast_get_game_cell(const SpreadForecast *forecast,
                              const Game *game) {
  const int on_turn = game_get_player_on_turn_index(game);
  const uint32_t bag_tiles = (uint32_t)bag_get_letters(game_get_bag(game));
  const uint32_t on_turn_rack_tiles = (uint32_t)rack_get_total_letters(
      player_get_rack(game_get_player(game, on_turn)));
  const uint32_t off_turn_rack_tiles = (uint32_t)rack_get_total_letters(
      player_get_rack(game_get_player(game, 1 - on_turn)));
  return &forecast->cells[spread_forecast_cell_index(
      forecast, bag_tiles, on_turn_rack_tiles, off_turn_rack_tiles)];
}

SpreadForecast *spread_forecast_create(const char *data_paths, const char *name,
                                       ErrorStack *error_stack) {
  char *filename = data_filepaths_get_readable_filename(
      data_paths, name, DATA_FILEPATH_TYPE_SPREAD_FORECAST, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    free(filename);
    return NULL;
  }

  FILE *stream = fopen(filename, "rbe");
  if (stream == NULL) {
    error_stack_push(error_stack, ERROR_STATUS_FILEPATH_FILE_NOT_FOUND,
                     get_formatted_string(
                         "could not open spread forecast file: %s", filename));
    free(filename);
    return NULL;
  }

  uint8_t magic[sizeof(SPREAD_FORECAST_MAGIC)];
  uint32_t header[SPREAD_FORECAST_HEADER_U32S];
  if (fread(magic, sizeof(magic), 1, stream) != 1 ||
      memcmp(magic, SPREAD_FORECAST_MAGIC, sizeof(magic)) != 0 ||
      fread(header, sizeof(uint32_t), SPREAD_FORECAST_HEADER_U32S, stream) !=
          SPREAD_FORECAST_HEADER_U32S) {
    error_stack_push(
        error_stack, ERROR_STATUS_FILEPATH_FILE_NOT_FOUND,
        get_formatted_string("invalid spread forecast header: %s", filename));
    fclose_or_die(stream);
    free(filename);
    return NULL;
  }

  const uint32_t version = le32toh(header[0]);
  const uint32_t max_bag_tiles = le32toh(header[1]);
  const uint32_t max_rack_tiles = le32toh(header[2]);
  const uint32_t cell_count = le32toh(header[3]);
  const uint64_t expected_cell_count = ((uint64_t)max_bag_tiles + 1) *
                                       ((uint64_t)max_rack_tiles + 1) *
                                       ((uint64_t)max_rack_tiles + 1);
  if (version != SPREAD_FORECAST_VERSION || max_bag_tiles > MAX_BAG_SIZE ||
      max_rack_tiles > RACK_SIZE || cell_count != expected_cell_count) {
    error_stack_push(
        error_stack, ERROR_STATUS_FILEPATH_FILE_NOT_FOUND,
        get_formatted_string("unsupported spread forecast dimensions: %s",
                             filename));
    fclose_or_die(stream);
    free(filename);
    return NULL;
  }

  SpreadForecast *forecast = calloc_or_die(1, sizeof(SpreadForecast));
  forecast->name = string_duplicate(name);
  forecast->max_bag_tiles = max_bag_tiles;
  forecast->max_rack_tiles = max_rack_tiles;
  forecast->rack_states = max_rack_tiles + 1;
  forecast->cells =
      calloc_or_die((size_t)cell_count, sizeof(SpreadForecastCell));

  for (uint32_t i = 0; i < cell_count; i++) {
    float values[3];
    uint32_t sample_count;
    if (fread(values, sizeof(float), 3, stream) != 3 ||
        fread(&sample_count, sizeof(uint32_t), 1, stream) != 1) {
      error_stack_push(
          error_stack, ERROR_STATUS_FILEPATH_FILE_NOT_FOUND,
          get_formatted_string("truncated spread forecast data: %s", filename));
      spread_forecast_destroy(forecast);
      fclose_or_die(stream);
      free(filename);
      return NULL;
    }
    const double spread_swing = (double)convert_float_to_le(values[0]);
    const float on_turn_turns = convert_float_to_le(values[1]);
    const float off_turn_turns = convert_float_to_le(values[2]);
    if (!isfinite(spread_swing) || !isfinite(on_turn_turns) ||
        !isfinite(off_turn_turns) || on_turn_turns < 0.0F ||
        off_turn_turns < 0.0F) {
      error_stack_push(
          error_stack, ERROR_STATUS_FILEPATH_FILE_NOT_FOUND,
          get_formatted_string("invalid spread forecast value: %s", filename));
      spread_forecast_destroy(forecast);
      fclose_or_die(stream);
      free(filename);
      return NULL;
    }
    forecast->cells[i] = (SpreadForecastCell){
        .expected_spread_swing = double_to_equity(spread_swing),
        .expected_on_turn_turns = on_turn_turns,
        .expected_off_turn_turns = off_turn_turns,
        .sample_count = le32toh(sample_count),
    };
  }

  const int trailing_byte = fgetc(stream);
  if (trailing_byte != EOF) {
    error_stack_push(error_stack, ERROR_STATUS_FILEPATH_FILE_NOT_FOUND,
                     get_formatted_string(
                         "spread forecast has trailing data: %s", filename));
    spread_forecast_destroy(forecast);
    forecast = NULL;
  }
  fclose_or_die(stream);
  free(filename);
  return forecast;
}

void spread_forecast_destroy(SpreadForecast *forecast) {
  if (forecast == NULL) {
    return;
  }
  free(forecast->name);
  free(forecast->cells);
  free(forecast);
}

const char *spread_forecast_get_name(const SpreadForecast *forecast) {
  return forecast->name;
}

Equity spread_forecast_get_on_turn(const SpreadForecast *forecast,
                                   Equity current_on_turn_spread,
                                   unsigned int bag_tiles,
                                   unsigned int on_turn_rack_tiles,
                                   unsigned int off_turn_rack_tiles) {
  const SpreadForecastCell *cell = &forecast->cells[spread_forecast_cell_index(
      forecast, bag_tiles, on_turn_rack_tiles, off_turn_rack_tiles)];
  const int64_t projected =
      (int64_t)current_on_turn_spread + cell->expected_spread_swing;
  if (projected > EQUITY_MAX_VALUE || projected < EQUITY_MIN_VALUE) {
    log_fatal("spread forecast out of Equity range");
  }
  return (Equity)projected;
}

double spread_forecast_get_expected_turns_for_counts(
    const SpreadForecast *forecast, unsigned int bag_tiles,
    unsigned int on_turn_rack_tiles, unsigned int off_turn_rack_tiles,
    bool for_player_on_turn) {
  const SpreadForecastCell *cell = &forecast->cells[spread_forecast_cell_index(
      forecast, bag_tiles, on_turn_rack_tiles, off_turn_rack_tiles)];
  return for_player_on_turn ? (double)cell->expected_on_turn_turns
                            : (double)cell->expected_off_turn_turns;
}

Equity spread_forecast_get(const SpreadForecast *forecast, const Game *game,
                           int pov_player_index) {
  const int on_turn = game_get_player_on_turn_index(game);
  const Player *on_turn_player = game_get_player(game, on_turn);
  const Player *off_turn_player = game_get_player(game, 1 - on_turn);
  const Equity current_on_turn_spread =
      player_get_score(on_turn_player) - player_get_score(off_turn_player);
  const Equity on_turn_forecast = spread_forecast_get_on_turn(
      forecast, current_on_turn_spread,
      (unsigned int)bag_get_letters(game_get_bag(game)),
      (unsigned int)rack_get_total_letters(player_get_rack(on_turn_player)),
      (unsigned int)rack_get_total_letters(player_get_rack(off_turn_player)));
  return pov_player_index == on_turn ? on_turn_forecast
                                     : equity_negate(on_turn_forecast);
}

double spread_forecast_get_expected_turns(const SpreadForecast *forecast,
                                          const Game *game, int player_index) {
  const int on_turn = game_get_player_on_turn_index(game);
  const SpreadForecastCell *cell =
      spread_forecast_get_game_cell(forecast, game);
  return player_index == on_turn ? (double)cell->expected_on_turn_turns
                                 : (double)cell->expected_off_turn_turns;
}

uint32_t spread_forecast_get_sample_count(const SpreadForecast *forecast,
                                          const Game *game) {
  return spread_forecast_get_game_cell(forecast, game)->sample_count;
}
