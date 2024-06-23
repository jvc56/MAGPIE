#ifndef CONVERSION_RESULTS_H
#define CONVERSION_RESULTS_H

typedef struct ConversionResults ConversionResults;

ConversionResults *conversion_results_create(void);
void conversion_results_destroy(ConversionResults *results);

int conversion_results_get_number_of_strings(const ConversionResults *results);
int conversion_results_get_number_of_nodes(const ConversionResults *results);

void conversion_results_set_number_of_strings(ConversionResults *results,
                                              int number_of_strings);
void conversion_results_set_number_of_nodes(ConversionResults *results,
                                            int number_of_nodes);

#endif