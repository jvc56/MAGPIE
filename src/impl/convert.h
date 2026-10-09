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
  // klvwmp2rit only: the names to load the KLV and the wordmap under, when
  // they are not the output's.
  //
  // A rack info table is built from a .klv2 and a .wmp and stores precomputed
  // leave values, so it belongs to that (lexicon, leaves) pair rather than to
  // the lexicon: CSW24 played with CSW_quackle_leaves and CSW24 played with
  // its own leaves need different tables. Loading both inputs under the
  // output's name, as this did, forces the table to be named after one of
  // them, and birdtest then has no name it can give the file that says which
  // pair it is for. NULL for either means "the output's name", which is the
  // CLI's `convert klvwmp2rit CSW24` unchanged.
  const char *klv_name;
  const char *wmp_name;
  int num_threads;
} ConversionArgs;

void convert(const ConversionArgs *args, ConversionResults *conversion_results,
             ErrorStack *error_stack);
#endif