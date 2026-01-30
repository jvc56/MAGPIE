#include "convert.h"

#include "../def/board_defs.h"
#include "../def/convert_defs.h"
#include "../def/kwg_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../ent/conversion_results.h"
#include "../ent/data_filepaths.h"
#include "../ent/dictionary_word.h"
#include "../ent/klv.h"
#include "../ent/klv_csv.h"
#include "../ent/kwg.h"
#include "../ent/letter_distribution.h"
#include "../ent/wmp.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "kwg_maker.h"
#include "wmp_maker.h"
#include <stdio.h>
#include <stdlib.h>

void convert_from_text_with_dwl(const LetterDistribution *ld,
                                conversion_type_t conversion_type,
                                const char *data_paths, const char *input_name,
                                const char *output_name,
                                DictionaryWordList *strings,
                                ConversionResults *conversion_results,
                                int num_threads, ErrorStack *error_stack) {

  char *input_filename = data_filepaths_get_readable_filename(
      data_paths, input_name, DATA_FILEPATH_TYPE_LEXICON, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  FILE *input_file = fopen_safe(input_filename, "r", error_stack);
  free(input_filename);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  char *line = NULL;
  size_t len = 0;
  ssize_t read;
  while ((read = getline_ignore_carriage_return(&line, &len, input_file)) !=
         -1) {
    if (read > 0 && line[read - 1] == '\n') {
      line[read - 1] = '\0';
    }
    const size_t line_length = string_length(line);
    MachineLetter *mls = malloc_or_die(line_length);
    const int mls_length = ld_str_to_mls(ld, line, false, mls, line_length);
    if (mls_length > BOARD_DIM) {
      error_stack_push(
          error_stack, ERROR_STATUS_CONVERT_TEXT_CONTAINS_WORD_TOO_LONG,
          get_formatted_string(
              "could not convert word '%s' with a length greater than %d", line,
              mls_length));
      free(mls);
      break;
    }
    if (mls_length < 0) {
      error_stack_push(
          error_stack, ERROR_STATUS_CONVERT_TEXT_CONTAINS_INVALID_LETTER,
          get_formatted_string(
              "could not convert word '%s' with invalid letter", line));
      free(mls);
      break;
    }
    if (!unblank_machine_letters(mls, mls_length)) {
      error_stack_push(
          error_stack, ERROR_STATUS_CONVERT_TEXT_CONTAINS_INVALID_LETTER,
          get_formatted_string(
              "could not convert word '%s' with invalid letter", line));
      free(mls);
      break;
    }
    if (mls_length < 2) {
      error_stack_push(
          error_stack, ERROR_STATUS_CONVERT_TEXT_CONTAINS_WORD_TOO_SHORT,
          get_formatted_string("could not convert word less than length 2: %s",
                               line));
      free(mls);
      break;
    }
    dictionary_word_list_add_word(strings, mls, mls_length);
    free(mls);
  }
  if (line != NULL) {
    free(line);
  }
  if (!error_stack_is_empty(error_stack)) {
    fclose_or_die(input_file);
    return;
  }

  if (conversion_type == CONVERT_TEXT2WORDMAP) {
    char *wmp_output_filename = data_filepaths_get_writable_filename(
        data_paths, output_name, DATA_FILEPATH_TYPE_WORDMAP, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
    WMP *wmp = make_wmp_from_words(strings, ld, num_threads);
    wmp_write_to_file(wmp, wmp_output_filename, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(
          error_stack, ERROR_STATUS_CONVERT_OUTPUT_FILE_NOT_WRITABLE,
          get_formatted_string("could not write wordmap to output file: %s",
                               wmp_output_filename));
    }
    wmp_destroy(wmp);
    free(wmp_output_filename);
  } else {
    char *kwg_output_filename = data_filepaths_get_writable_filename(
        data_paths, output_name, DATA_FILEPATH_TYPE_KWG, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
    kwg_maker_output_t output_type = KWG_MAKER_OUTPUT_DAWG_AND_GADDAG;
    if (conversion_type == CONVERT_TEXT2DAWG) {
      output_type = KWG_MAKER_OUTPUT_DAWG;
    } else if (conversion_type == CONVERT_TEXT2GADDAG) {
      output_type = KWG_MAKER_OUTPUT_GADDAG;
    }
    KWG *kwg = make_kwg_from_words(strings, output_type, KWG_MAKER_MERGE_EXACT);
    kwg_write_to_file(kwg, kwg_output_filename, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(
          error_stack, ERROR_STATUS_CONVERT_OUTPUT_FILE_NOT_WRITABLE,
          get_formatted_string("could not write kwg to output file: %s",
                               kwg_output_filename));
    } else {
      conversion_results_set_number_of_strings(
          conversion_results, dictionary_word_list_get_count(strings));
    }
    kwg_destroy(kwg);
    free(kwg_output_filename);
  }
}

void convert_with_names(const LetterDistribution *ld,
                        conversion_type_t conversion_type,
                        const char *data_paths, const char *input_name,
                        const char *output_name,
                        ConversionResults *conversion_results, int num_threads,
                        ErrorStack *error_stack) {
  if ((conversion_type == CONVERT_TEXT2DAWG) ||
      (conversion_type == CONVERT_TEXT2GADDAG) ||
      (conversion_type == CONVERT_TEXT2KWG) ||
      (conversion_type == CONVERT_TEXT2WORDMAP)) {
    DictionaryWordList *strings = dictionary_word_list_create();
    convert_from_text_with_dwl(ld, conversion_type, data_paths, input_name,
                               output_name, strings, conversion_results,
                               num_threads, error_stack);
    dictionary_word_list_destroy(strings);
  } else if (conversion_type == CONVERT_DAWG2TEXT) {
    KWG *kwg = kwg_create(data_paths, input_name, error_stack);
    if (error_stack_is_empty(error_stack)) {
      DictionaryWordList *words = dictionary_word_list_create();
      kwg_write_words(kwg, kwg_get_dawg_root_node_index(kwg), words, NULL);
      dictionary_word_list_write_to_file(words, ld, data_paths, output_name,
                                         error_stack);
      dictionary_word_list_destroy(words);
    }
    kwg_destroy(kwg);
  } else if (conversion_type == CONVERT_DAWG2WORDMAP) {
    KWG *kwg = kwg_create(data_paths, input_name, error_stack);
    if (error_stack_is_empty(error_stack)) {
      char *wmp_output_filename = data_filepaths_get_writable_filename(
          data_paths, output_name, DATA_FILEPATH_TYPE_WORDMAP, error_stack);
      if (error_stack_is_empty(error_stack)) {
        DictionaryWordList *words = dictionary_word_list_create();
        kwg_write_words(kwg, kwg_get_dawg_root_node_index(kwg), words, NULL);
        WMP *wmp = make_wmp_from_words(words, ld, num_threads);
        wmp_write_to_file(wmp, wmp_output_filename, error_stack);
        if (!error_stack_is_empty(error_stack)) {
          error_stack_push(
              error_stack, ERROR_STATUS_CONVERT_OUTPUT_FILE_NOT_WRITABLE,
              get_formatted_string("could not write wordmap to output file: %s",
                                   wmp_output_filename));
        } else {
          conversion_results_set_number_of_strings(
              conversion_results, dictionary_word_list_get_count(words));
        }
        wmp_destroy(wmp);
        dictionary_word_list_destroy(words);
        free(wmp_output_filename);
      }
    }
    kwg_destroy(kwg);
  } else if (conversion_type == CONVERT_CSV2KLV) {
    KLV *klv = klv_read_from_csv(ld, data_paths, input_name, error_stack);
    if (error_stack_is_empty(error_stack)) {
      klv_write(klv, data_paths, output_name, error_stack);
    }
    klv_destroy(klv);
  } else if (conversion_type == CONVERT_KLV2CSV) {
    KLV *klv = klv_create(data_paths, input_name, error_stack);
    if (error_stack_is_empty(error_stack)) {
      klv_write_to_csv(klv, ld, data_paths, output_name, NULL, error_stack);
    }
    klv_destroy(klv);
  } else {
    error_stack_push(error_stack,
                     ERROR_STATUS_CONVERT_UNIMPLEMENTED_CONVERSION_TYPE,
                     string_duplicate("unimplemented conversion type"));
  }
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
  } else if (strings_equal(conversion_type_string, "text2wordmap")) {
    conversion_type = CONVERT_TEXT2WORDMAP;
  } else if (strings_equal(conversion_type_string, "dawg2wordmap")) {
    conversion_type = CONVERT_DAWG2WORDMAP;
  }
  return conversion_type;
}

void convert(const ConversionArgs *args, ConversionResults *conversion_results,
             ErrorStack *error_stack) {
  const char *conversion_type_string = args->conversion_type_string;
  conversion_type_t conversion_type =
      get_conversion_type_from_string(conversion_type_string);

  if (conversion_type == CONVERT_UNKNOWN) {
    error_stack_push(error_stack,
                     ERROR_STATUS_CONVERT_UNRECOGNIZED_CONVERSION_TYPE,
                     get_formatted_string("unrecognized conversion type: %s",
                                          conversion_type_string));
    return;
  }

  if (args->input_and_output_name == NULL) {
    error_stack_push(error_stack, ERROR_STATUS_CONVERT_INPUT_FILE_ERROR,
                     get_formatted_string("input file name is missing"));
    return;
  }

  char *ld_name = NULL;
  if (args->ld_name != NULL) {
    ld_name = string_duplicate(args->ld_name);
  } else {
    ld_name = ld_get_default_name_from_lexicon_name(args->input_and_output_name,
                                                    error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }

  LetterDistribution *ld = ld_create(args->data_paths, ld_name, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  free(ld_name);

  convert_with_names(ld, conversion_type, args->data_paths,
                     args->input_and_output_name, args->input_and_output_name,
                     conversion_results, args->num_threads, error_stack);
  ld_destroy(ld);
}
