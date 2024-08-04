#include "convert.h"

#include "../def/cross_set_defs.h"

#include "../ent/conversion_results.h"
#include "../ent/dictionary_word.h"
#include "../ent/kwg.h"

#include "klv_csv.h"
#include "kwg_maker.h"

#include "../util/log.h"
#include "../util/string_util.h"
#include "../util/util.h"

conversion_status_t convert_with_filenames(
    const LetterDistribution *ld, conversion_type_t conversion_type,
    const char *data_paths, const char *input_filename,
    const char *output_filename, ConversionResults *conversion_results) {
  DictionaryWordList *strings = dictionary_word_list_create();
  char line[BOARD_DIM + 2]; // +1 for \n, +1 for \0
  uint8_t mls[BOARD_DIM];
  if ((conversion_type == CONVERT_TEXT2DAWG) ||
      (conversion_type == CONVERT_TEXT2GADDAG) ||
      (conversion_type == CONVERT_TEXT2KWG)) {
    FILE *input_file = fopen(input_filename, "r");
    if (!input_file) {
      return CONVERT_STATUS_INPUT_FILE_ERROR;
    }
    while (fgets(line, BOARD_DIM + 2, input_file)) {
      const int word_length = string_length(line) - 1;
      line[word_length] = '\0';
      if (word_length > BOARD_DIM) {
        return CONVERT_STATUS_TEXT_CONTAINS_WORD_TOO_LONG;
      }
      int mls_length = ld_str_to_mls(ld, line, false, mls, word_length);
      if (mls_length < 0) {
        return CONVERT_STATUS_TEXT_CONTAINS_INVALID_LETTER;
      }
      if (mls_length < 2) {
        return CONVERT_STATUS_TEXT_CONTAINS_WORD_TOO_SHORT;
      }
      dictionary_word_list_add_word(strings, mls, mls_length);
    }

    kwg_maker_output_t output_type = KWG_MAKER_OUTPUT_DAWG_AND_GADDAG;
    if (conversion_type == CONVERT_TEXT2DAWG) {
      output_type = KWG_MAKER_OUTPUT_DAWG;
    } else if (conversion_type == CONVERT_TEXT2GADDAG) {
      output_type = KWG_MAKER_OUTPUT_GADDAG;
    }
    KWG *kwg = make_kwg_from_words(strings, output_type, KWG_MAKER_MERGE_EXACT);
    if (!kwg_write_to_file(kwg, output_filename)) {
      printf("failed to write output file: %s\n", output_filename);
      return CONVERT_STATUS_OUTPUT_FILE_NOT_WRITABLE;
    }
    conversion_results_set_number_of_strings(
        conversion_results, dictionary_word_list_get_count(strings));
  } else if (conversion_type == CONVERT_CSV2KLV) {
    KLV *klv = klv_read_from_csv(ld, data_paths, input_filename);
    klv_write(klv, NULL, output_filename);
    klv_destroy(klv);
  } else if (conversion_type == CONVERT_KLV2CSV) {
    KLV *klv = klv_create(data_paths, input_filename);
    klv_write_to_csv(klv, ld, NULL, output_filename);
    klv_destroy(klv);
  } else {
    return CONVERT_STATUS_UNIMPLEMENTED_CONVERSION_TYPE;
  }

  return CONVERT_STATUS_SUCCESS;
}

data_filepath_t
get_input_filepath_type_from_conv_type(conversion_type_t conversion_type) {
  data_filepath_t filepath_type;
  switch (conversion_type) {
  case CONVERT_TEXT2DAWG:
    filepath_type = DATA_FILEPATH_TYPE_LEXICON;
    break;
  case CONVERT_TEXT2GADDAG:
    filepath_type = DATA_FILEPATH_TYPE_LEXICON;
    break;
  case CONVERT_TEXT2KWG:
    filepath_type = DATA_FILEPATH_TYPE_LEXICON;
    break;
  case CONVERT_DAWG2TEXT:
    filepath_type = DATA_FILEPATH_TYPE_KWG;
    break;
  case CONVERT_GADDAG2TEXT:
    filepath_type = DATA_FILEPATH_TYPE_KWG;
    break;
  case CONVERT_CSV2KLV:
    filepath_type = DATA_FILEPATH_TYPE_LEAVES;
    break;
  case CONVERT_KLV2CSV:
    filepath_type = DATA_FILEPATH_TYPE_KLV;
    break;
  default:
    log_fatal("cannot get input filepath type for unknown conversion type");
    break;
  }
  return filepath_type;
}

data_filepath_t
get_output_filepath_type_from_conv_type(conversion_type_t conversion_type) {
  data_filepath_t filepath_type;
  switch (conversion_type) {
  case CONVERT_TEXT2DAWG:
    filepath_type = DATA_FILEPATH_TYPE_KWG;
    break;
  case CONVERT_TEXT2GADDAG:
    filepath_type = DATA_FILEPATH_TYPE_KWG;
    break;
  case CONVERT_TEXT2KWG:
    filepath_type = DATA_FILEPATH_TYPE_KWG;
    break;
  case CONVERT_DAWG2TEXT:
    filepath_type = DATA_FILEPATH_TYPE_LEXICON;
    break;
  case CONVERT_GADDAG2TEXT:
    filepath_type = DATA_FILEPATH_TYPE_LEXICON;
    break;
  case CONVERT_CSV2KLV:
    filepath_type = DATA_FILEPATH_TYPE_KLV;
    break;
  case CONVERT_KLV2CSV:
    filepath_type = DATA_FILEPATH_TYPE_LEAVES;
    break;
  default:
    log_fatal("cannot get output filepath type for unknown conversion type");
    break;
  }
  return filepath_type;
}

conversion_type_t
get_conversion_type_from_string(const char *conversion_type_string) {
  conversion_type_t conversion_type = CONVERT_UNKNOWN;
  if (strings_equal(conversion_type_string, "text2dawg")) {
    conversion_type = CONVERT_TEXT2DAWG;
  } else if (strings_equal(conversion_type_string, "text2gaddag")) {
    conversion_type = CONVERT_TEXT2GADDAG;
  } else if (strings_equal(conversion_type_string, "text2kwg")) {
    conversion_type = CONVERT_TEXT2KWG;
  } else if (strings_equal(conversion_type_string, "dawg2text")) {
    conversion_type = CONVERT_DAWG2TEXT;
  } else if (strings_equal(conversion_type_string, "gaddag2text")) {
    conversion_type = CONVERT_GADDAG2TEXT;
  } else if (strings_equal(conversion_type_string, "csv2klv")) {
    conversion_type = CONVERT_CSV2KLV;
  } else if (strings_equal(conversion_type_string, "klv2csv")) {
    conversion_type = CONVERT_KLV2CSV;
  }
  return conversion_type;
}

conversion_status_t convert(ConversionArgs *args,
                            ConversionResults *conversion_results) {
  const char *conversion_type_string = args->conversion_type_string;
  conversion_type_t conversion_type =
      get_conversion_type_from_string(conversion_type_string);

  if (conversion_type == CONVERT_UNKNOWN) {
    return CONVERT_STATUS_UNRECOGNIZED_CONVERSION_TYPE;
  }

  if (args->input_name == NULL) {
    return CONVERT_STATUS_INPUT_FILE_ERROR;
  }
  if (args->output_name == NULL) {
    return CONVERT_STATUS_OUTPUT_FILE_NOT_WRITABLE;
  }

  char *input_filename = data_filepaths_get_readable_filename(
      args->data_paths, args->input_name,
      get_input_filepath_type_from_conv_type(conversion_type));
  char *output_filename = data_filepaths_get_writable_filename(
      args->data_paths, args->output_name,
      get_output_filepath_type_from_conv_type(conversion_type));

  conversion_status_t status = convert_with_filenames(
      args->ld, conversion_type, args->data_paths, input_filename,
      output_filename, conversion_results);

  free(input_filename);
  free(output_filename);

  return status;
}