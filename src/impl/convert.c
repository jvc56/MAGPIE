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

conversion_status_t convert_from_text_with_dwl(
    const LetterDistribution *ld, conversion_type_t conversion_type,
    const char *input_filename, const char *output_filename,
    DictionaryWordList *strings, ConversionResults *conversion_results) {
  FILE *input_file = fopen(input_filename, "r");
  if (!input_file) {
    return CONVERT_STATUS_INPUT_FILE_ERROR;
  }
  char line[BOARD_DIM + 2]; // +1 for \n, +1 for \0
  uint8_t mls[BOARD_DIM];
  while (fgets(line, BOARD_DIM + 2, input_file)) {
    int word_length_with_newline = string_length(line);
    if (word_length_with_newline == BOARD_DIM + 1 &&
        line[word_length_with_newline - 1] != '\n') {
      return CONVERT_STATUS_TEXT_CONTAINS_WORD_TOO_LONG;
    }
    if (line[word_length_with_newline - 1] == '\n') {
      line[word_length_with_newline - 1] = '\0';
    }
    int word_length = string_length(line);
    int mls_length = ld_str_to_mls(ld, line, false, mls, word_length);
    if (mls_length < 0) {
      return CONVERT_STATUS_TEXT_CONTAINS_INVALID_LETTER;
    }
    if (!unblank_machine_letters(mls, mls_length)) {
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
  conversion_status_t status = CONVERT_STATUS_SUCCESS;
  KWG *kwg = make_kwg_from_words(strings, output_type, KWG_MAKER_MERGE_EXACT);
  if (!kwg_write_to_file(kwg, output_filename)) {
    status = CONVERT_STATUS_OUTPUT_FILE_NOT_WRITABLE;
  } else {
    conversion_results_set_number_of_strings(
        conversion_results, dictionary_word_list_get_count(strings));
  }
  kwg_destroy(kwg);
  return status;
}

conversion_status_t convert_with_filenames(
    const LetterDistribution *ld, conversion_type_t conversion_type,
    const char *data_paths, const char *input_filename,
    const char *output_filename, ConversionResults *conversion_results) {
  conversion_status_t status = CONVERT_STATUS_SUCCESS;
  if ((conversion_type == CONVERT_TEXT2DAWG) ||
      (conversion_type == CONVERT_TEXT2GADDAG) ||
      (conversion_type == CONVERT_TEXT2KWG)) {
    DictionaryWordList *strings = dictionary_word_list_create();
    status = convert_from_text_with_dwl(ld, conversion_type, input_filename,
                                        output_filename, strings,
                                        conversion_results);
    dictionary_word_list_destroy(strings);
  } else if (conversion_type == CONVERT_CSV2KLV) {
    KLV *klv = klv_read_from_csv(ld, data_paths, input_filename);
    klv_write(klv, NULL, output_filename);
    klv_destroy(klv);
  } else if (conversion_type == CONVERT_KLV2CSV) {
    KLV *klv = klv_create(data_paths, input_filename);
    klv_write_to_csv(klv, ld, NULL, output_filename);
    klv_destroy(klv);
  } else {
    status = CONVERT_STATUS_UNIMPLEMENTED_CONVERSION_TYPE;
  }
  return status;
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