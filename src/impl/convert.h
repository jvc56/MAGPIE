#ifndef CONVERT_H
#define CONVERT_H

#include "../def/convert_defs.h"

#include "../ent/conversion_results.h"
#include "../ent/letter_distribution.h"
#include "../util/error_stack.h"

typedef struct ConversionArgs {
  const char *conversion_type_string;
  const char *data_paths;
  const char *input_name;
  const char *output_name;
  const LetterDistribution *ld;
} ConversionArgs;

void convert(ConversionArgs *args, ConversionResults *conversion_results,
             ErrorStack *error_stack);
#endif