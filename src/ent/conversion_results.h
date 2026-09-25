#ifndef CONVERSION_RESULTS_H
#define CONVERSION_RESULTS_H

typedef struct ConversionResults ConversionResults;

ConversionResults *conversion_results_create(void);
void conversion_results_destroy(ConversionResults *results);

int conversion_results_get_number_of_nodes(const ConversionResults *results);

void conversion_results_set_number_of_strings(ConversionResults *results,
                                              int number_of_strings);
void conversion_results_set_number_of_nodes(ConversionResults *results,
                                            int number_of_nodes);
// A report for the user, or NULL. Takes ownership of report.
const char *conversion_results_get_report(const ConversionResults *results);
void conversion_results_set_report(ConversionResults *results, char *report);

#endif