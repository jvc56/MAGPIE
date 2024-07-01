#include "conversion_results.h"

#include "../util/util.h"

struct ConversionResults {
  int number_of_strings;
  int number_of_nodes;
};

ConversionResults *conversion_results_create(void) {
  ConversionResults *conversion_results =
      malloc_or_die(sizeof(ConversionResults));
  conversion_results->number_of_strings = 0;
  conversion_results->number_of_nodes = 0;
  return conversion_results;
}

void conversion_results_destroy(ConversionResults *conversion_results) {
  if (!conversion_results) {
    return;
  }
  free(conversion_results);
}

int conversion_results_get_number_of_strings(
    const ConversionResults *conversion_results) {
  return conversion_results->number_of_strings;
}

int conversion_results_get_number_of_nodes(
    const ConversionResults *conversion_results) {
  return conversion_results->number_of_nodes;
}

void conversion_results_set_number_of_strings(
    ConversionResults *conversion_results, int number_of_strings) {
  conversion_results->number_of_strings = number_of_strings;
}

void conversion_results_set_number_of_nodes(
    ConversionResults *conversion_results, int number_of_nodes) {
  conversion_results->number_of_nodes = number_of_nodes;
}