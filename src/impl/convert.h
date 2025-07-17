#ifndef CONVERT_H
#define CONVERT_H

#include "../def/convert_defs.h"

#include "../ent/conversion_results.h"
#include "../ent/letter_distribution.h"

#include "../util/io_util.h"

typedef struct ConversionArgs {
  const char *conversion_type_string;
  const char *data_paths;
  const char *input_and_output_name;
  const char *ld_name;
} ConversionArgs;

void convert(const ConversionArgs *args, ConversionResults *conversion_results,
             ErrorStack *error_stack);
#endif