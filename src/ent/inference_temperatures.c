#include "inference_temperatures.h"

#include "../def/inference_defs.h"
#include "../util/fileproxy.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "data_filepaths.h"
#include "equity.h"
#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>

struct InferenceTemperatures {
  char *name;
  int number_of_knots;
  int bags[INFERENCE_TEMPERATURES_MAX_KNOTS];
  double temperatures[INFERENCE_TEMPERATURES_MAX_KNOTS];
  bool has_exchange;
  double exchange_temperature;
};

static bool temperature_is_valid(double temperature) {
  return isfinite(temperature) && temperature > 0 &&
         temperature <= INFERENCE_TEMPERATURES_MAX_VALUE;
}

InferenceTemperatures *inference_temperatures_create_from_knots(
    const char *name, const int *bags, const double *temperatures,
    int number_of_knots, bool has_exchange, double exchange_temperature) {
  if (number_of_knots < 1 ||
      number_of_knots > INFERENCE_TEMPERATURES_MAX_KNOTS) {
    log_fatal("an inference temperature table has 1 to %d knots, not %d",
              INFERENCE_TEMPERATURES_MAX_KNOTS, number_of_knots);
  }
  if (has_exchange && !temperature_is_valid(exchange_temperature)) {
    log_fatal("invalid inference exchange temperature %g",
              exchange_temperature);
  }
  InferenceTemperatures *table = malloc_or_die(sizeof(InferenceTemperatures));
  table->name = string_duplicate(name);
  table->number_of_knots = number_of_knots;
  for (int knot_idx = 0; knot_idx < number_of_knots; knot_idx++) {
    if (bags[knot_idx] < 0 ||
        (knot_idx > 0 && bags[knot_idx] <= bags[knot_idx - 1]) ||
        !temperature_is_valid(temperatures[knot_idx])) {
      log_fatal("invalid inference temperature knot %d: bag %d, %g points",
                knot_idx, bags[knot_idx], temperatures[knot_idx]);
    }
    table->bags[knot_idx] = bags[knot_idx];
    table->temperatures[knot_idx] = temperatures[knot_idx];
  }
  table->has_exchange = has_exchange;
  table->exchange_temperature = has_exchange ? exchange_temperature : 0;
  return table;
}

void inference_temperatures_destroy(InferenceTemperatures *table) {
  if (!table) {
    return;
  }
  free(table->name);
  free(table);
}

static void push_row_error(ErrorStack *error_stack, const char *filename,
                           int row_number, const char *reason) {
  error_stack_push(
      error_stack, ERROR_STATUS_INFERENCE_TEMPERATURES_INVALID_ROW,
      get_formatted_string("inference temperature file %s, row %d: %s",
                           filename, row_number, reason));
}

// Whether field holds nothing but whitespace, which the number parsers would
// read as 0.
static bool field_is_blank(const char *field) {
  for (; *field != '\0'; field++) {
    if (!isspace((unsigned char)*field)) {
      return false;
    }
  }
  return true;
}

// The rows read so far from a file.
typedef struct TemperatureRows {
  int number_of_knots;
  int bags[INFERENCE_TEMPERATURES_MAX_KNOTS];
  double temperatures[INFERENCE_TEMPERATURES_MAX_KNOTS];
  bool has_exchange;
  double exchange_temperature;
} TemperatureRows;

// Reads the temperature of a row, pushing an error if it is not one.
static double parse_temperature(const char *field, const char *filename,
                                int row_number, ErrorStack *error_stack) {
  const double temperature = string_to_double(field, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    push_row_error(error_stack, filename, row_number, "invalid temperature");
    return 0;
  }
  if (!temperature_is_valid(temperature)) {
    char *reason = get_formatted_string(
        "temperature must be a number of points above 0 and at most %g",
        INFERENCE_TEMPERATURES_MAX_VALUE);
    push_row_error(error_stack, filename, row_number, reason);
    free(reason);
    return 0;
  }
  return temperature;
}

static void parse_exchange_row(const char *temperature_field,
                               const char *filename, int row_number,
                               TemperatureRows *rows, ErrorStack *error_stack) {
  if (rows->has_exchange) {
    push_row_error(error_stack, filename, row_number,
                   "exchange row given twice");
    return;
  }
  const double temperature =
      parse_temperature(temperature_field, filename, row_number, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  rows->has_exchange = true;
  rows->exchange_temperature = temperature;
}

static void parse_bag_row(const char *bag_field, const char *temperature_field,
                          const char *filename, int row_number,
                          TemperatureRows *rows, ErrorStack *error_stack) {
  const int bag = string_to_int(bag_field, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    push_row_error(error_stack, filename, row_number, "invalid bag");
    return;
  }
  if (bag < 0) {
    push_row_error(error_stack, filename, row_number, "bag must be 0 or more");
    return;
  }
  if (rows->number_of_knots > 0 &&
      bag <= rows->bags[rows->number_of_knots - 1]) {
    push_row_error(error_stack, filename, row_number,
                   "bags must be in strictly ascending order");
    return;
  }
  if (rows->number_of_knots >= INFERENCE_TEMPERATURES_MAX_KNOTS) {
    char *reason = get_formatted_string("at most %d bag rows",
                                        INFERENCE_TEMPERATURES_MAX_KNOTS);
    push_row_error(error_stack, filename, row_number, reason);
    free(reason);
    return;
  }
  const double temperature =
      parse_temperature(temperature_field, filename, row_number, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  rows->bags[rows->number_of_knots] = bag;
  rows->temperatures[rows->number_of_knots] = temperature;
  rows->number_of_knots++;
}

static void parse_row(const char *row, const char *filename, int row_number,
                      TemperatureRows *rows, ErrorStack *error_stack) {
  StringSplitter *fields = split_string(row, ',', false);
  if (string_splitter_get_number_of_items(fields) != 2 ||
      field_is_blank(string_splitter_get_item(fields, 0)) ||
      field_is_blank(string_splitter_get_item(fields, 1))) {
    push_row_error(error_stack, filename, row_number,
                   "expected two fields, a bag (or exchange) and a "
                   "temperature");
    string_splitter_destroy(fields);
    return;
  }
  char *key = string_duplicate(string_splitter_get_item(fields, 0));
  trim_whitespace(key);
  if (strings_iequal(key, "exchange")) {
    parse_exchange_row(string_splitter_get_item(fields, 1), filename,
                       row_number, rows, error_stack);
  } else {
    parse_bag_row(key, string_splitter_get_item(fields, 1), filename,
                  row_number, rows, error_stack);
  }
  free(key);
  string_splitter_destroy(fields);
}

InferenceTemperatures *inference_temperatures_create(const char *data_paths,
                                                     const char *name,
                                                     ErrorStack *error_stack) {
  char *filename = data_filepaths_get_readable_filename(
      data_paths, name, DATA_FILEPATH_TYPE_INFERENCE_TEMPERATURES, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    free(filename);
    return NULL;
  }
  char *contents = fileproxy_get_string_from_filename(filename, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    free(contents);
    free(filename);
    return NULL;
  }
  TemperatureRows rows = {0};
  StringSplitter *lines = split_string_by_newline(contents, false);
  const int number_of_lines = string_splitter_get_number_of_items(lines);
  for (int line_idx = 0;
       line_idx < number_of_lines && error_stack_is_empty(error_stack);
       line_idx++) {
    char *row = string_duplicate(string_splitter_get_item(lines, line_idx));
    trim_whitespace(row);
    if (row[0] != '\0' && row[0] != '#') {
      parse_row(row, filename, line_idx + 1, &rows, error_stack);
    }
    free(row);
  }
  string_splitter_destroy(lines);
  free(contents);
  if (error_stack_is_empty(error_stack) && rows.number_of_knots == 0) {
    push_row_error(error_stack, filename, number_of_lines,
                   "expected at least one bag row");
  }
  free(filename);
  if (!error_stack_is_empty(error_stack)) {
    return NULL;
  }
  return inference_temperatures_create_from_knots(
      name, rows.bags, rows.temperatures, rows.number_of_knots,
      rows.has_exchange, rows.exchange_temperature);
}

const char *
inference_temperatures_get_name(const InferenceTemperatures *temperatures) {
  return temperatures->name;
}

int inference_temperatures_get_number_of_knots(
    const InferenceTemperatures *temperatures) {
  return temperatures->number_of_knots;
}

int inference_temperatures_get_knot_bag(
    const InferenceTemperatures *temperatures, int knot_index) {
  return temperatures->bags[knot_index];
}

double inference_temperatures_get_knot_temperature(
    const InferenceTemperatures *temperatures, int knot_index) {
  return temperatures->temperatures[knot_index];
}

bool inference_temperatures_has_exchange(
    const InferenceTemperatures *temperatures) {
  return temperatures->has_exchange;
}

double
inference_temperatures_get_points(const InferenceTemperatures *temperatures,
                                  int number_of_tiles_in_bag,
                                  bool is_exchange) {
  if (is_exchange && temperatures->has_exchange) {
    return temperatures->exchange_temperature;
  }
  const int *bags = temperatures->bags;
  const double *values = temperatures->temperatures;
  const int last = temperatures->number_of_knots - 1;
  if (number_of_tiles_in_bag <= bags[0]) {
    return values[0];
  }
  if (number_of_tiles_in_bag >= bags[last]) {
    return values[last];
  }
  int knot_idx = 1;
  while (number_of_tiles_in_bag > bags[knot_idx]) {
    knot_idx++;
  }
  const double weight = (double)(number_of_tiles_in_bag - bags[knot_idx - 1]) /
                        (double)(bags[knot_idx] - bags[knot_idx - 1]);
  return exp(((1 - weight) * log(values[knot_idx - 1])) +
             (weight * log(values[knot_idx])));
}

Equity inference_temperatures_get(const InferenceTemperatures *temperatures,
                                  int number_of_tiles_in_bag,
                                  bool is_exchange) {
  return double_to_equity(inference_temperatures_get_points(
      temperatures, number_of_tiles_in_bag, is_exchange));
}
