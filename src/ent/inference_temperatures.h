#ifndef INFERENCE_TEMPERATURES_H
#define INFERENCE_TEMPERATURES_H

#include "../util/io_util.h"
#include "equity.h"
#include <stdbool.h>

// Inference temperatures by bag: the softmax temperature inference uses for
// an observed play, set by how many tiles the real bag held before it (see
// game_get_real_bag_size). The table has knots, each a bag and a
// temperature in points. A bag at or below the first knot's takes the first
// temperature and one above the last knot's the last; between two knots the
// temperature is interpolated linearly in the bag on the log of the
// temperature. An exchange takes the table's exchange temperature when it
// has one, and the bag's otherwise.
typedef struct InferenceTemperatures InferenceTemperatures;

// Reads <data path>/strategy/<name>.csv. Each row is "bag,temperature", the
// bags in strictly ascending order from 0, or "exchange,temperature", at
// most once. A temperature is a number of points above 0 and at most
// INFERENCE_TEMPERATURES_MAX_VALUE. There is at least one bag row and at most
// INFERENCE_TEMPERATURES_MAX_KNOTS. Blank rows and rows starting with '#' are
// skipped.
InferenceTemperatures *inference_temperatures_create(const char *data_paths,
                                                     const char *name,
                                                     ErrorStack *error_stack);
// A table of number_of_knots knots (1 to INFERENCE_TEMPERATURES_MAX_KNOTS,
// bags strictly ascending, temperatures in points within range) and, if
// has_exchange, an exchange temperature, named name.
InferenceTemperatures *inference_temperatures_create_from_knots(
    const char *name, const int *bags, const double *temperatures,
    int number_of_knots, bool has_exchange, double exchange_temperature);
void inference_temperatures_destroy(
    InferenceTemperatures *inference_temperatures);

const char *
inference_temperatures_get_name(const InferenceTemperatures *temperatures);
int inference_temperatures_get_number_of_knots(
    const InferenceTemperatures *temperatures);
int inference_temperatures_get_knot_bag(
    const InferenceTemperatures *temperatures, int knot_index);
double inference_temperatures_get_knot_temperature(
    const InferenceTemperatures *temperatures, int knot_index);
bool inference_temperatures_has_exchange(
    const InferenceTemperatures *temperatures);

// The temperature, in points, for a play from a real bag of
// number_of_tiles_in_bag tiles; is_exchange for an exchange.
double
inference_temperatures_get_points(const InferenceTemperatures *temperatures,
                                  int number_of_tiles_in_bag, bool is_exchange);
// The same as an Equity.
Equity inference_temperatures_get(const InferenceTemperatures *temperatures,
                                  int number_of_tiles_in_bag, bool is_exchange);

#endif
