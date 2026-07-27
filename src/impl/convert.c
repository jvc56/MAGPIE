#include "convert.h"

#include "../def/board_defs.h"
#include "../def/convert_defs.h"
#include "../def/kwg_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../ent/conversion_results.h"
#include "../ent/data_filepaths.h"
#include "../ent/dawg_packed.h"
#include "../ent/dictionary_word.h"
#include "../ent/klv.h"
#include "../ent/klv_csv.h"
#include "../ent/kwg.h"
#include "../ent/letter_distribution.h"
#include "../ent/rack_info_table.h"
#include "../ent/wmp.h"
#include "../ent/word_info_table.h"
#include "../util/fileproxy.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "kwg_maker.h"
#include "rack_info_table_maker.h"
#include "wmp_maker.h"
#include "word_info_table_maker.h"
#include <stdint.h>
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

  FILE *input_file = stream_from_filename(input_filename, error_stack);
  free(input_filename);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  // Initialize fast converter once for O(1) ASCII lookups
  FastStringConverter fc;
  fast_converter_init(&fc, ld);

  char *line = NULL;
  size_t len = 0;
  ssize_t read;
  // Use BOARD_DIM + 2 so we can distinguish "too long" (mls_length ==
  // BOARD_DIM + 1) from "invalid letter" (mls_length == -1). Multi-byte
  // or multichar tiles (e.g. UTF-8, [QU]) mean raw byte length can exceed
  // BOARD_DIM even when the word has <= BOARD_DIM tiles.
  MachineLetter mls[BOARD_DIM + 2];
  while ((read = getline_ignore_carriage_return(&line, &len, input_file)) !=
         -1) {
    if (read > 0 && line[read - 1] == '\n') {
      line[read - 1] = '\0';
      read--;
    }
    const int mls_length =
        fast_str_to_mls(&fc, line, false, mls, BOARD_DIM + 2);
    if (mls_length < 0) {
      error_stack_push(
          error_stack, ERROR_STATUS_CONVERT_TEXT_CONTAINS_INVALID_LETTER,
          get_formatted_string(
              "could not convert word '%s' with invalid letter", line));
      break;
    }
    if (mls_length > BOARD_DIM) {
      error_stack_push(
          error_stack, ERROR_STATUS_CONVERT_TEXT_CONTAINS_WORD_TOO_LONG,
          get_formatted_string(
              "could not convert word '%s' with a length greater than %d", line,
              BOARD_DIM));
      break;
    }
    if (!unblank_machine_letters(mls, mls_length)) {
      error_stack_push(
          error_stack, ERROR_STATUS_CONVERT_TEXT_CONTAINS_INVALID_LETTER,
          get_formatted_string(
              "could not convert word '%s' with invalid letter", line));
      break;
    }
    if (mls_length < 2) {
      error_stack_push(
          error_stack, ERROR_STATUS_CONVERT_TEXT_CONTAINS_WORD_TOO_SHORT,
          get_formatted_string("could not convert word less than length 2: %s",
                               line));
      break;
    }
    dictionary_word_list_add_word(strings, mls, mls_length);
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
  } else if (conversion_type == CONVERT_TEXT2DAWG_PACKED) {
    char *packed_output_filename = data_filepaths_get_writable_filename(
        data_paths, output_name, DATA_FILEPATH_TYPE_DAWG_PACKED, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
    // Build the reorder DAWG, then re-encode it into minimal-width nodes. The
    // CLI default is bit-packed (smallest); the byte-aligned strategy is for
    // callers who decode on hardware that pays for cross-byte shifts.
    KWG *kwg = make_kwg_from_words(strings, KWG_MAKER_OUTPUT_DAWG,
                                   KWG_MAKER_MERGE_TAIL_REORDER);
    DawgPacked *dp = dawg_packed_create_from_kwg(kwg, false);
    dawg_packed_write_to_file(dp, packed_output_filename, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(
          error_stack, ERROR_STATUS_CONVERT_OUTPUT_FILE_NOT_WRITABLE,
          get_formatted_string("could not write packed dawg to output file: %s",
                               packed_output_filename));
    } else {
      conversion_results_set_number_of_strings(
          conversion_results, dictionary_word_list_get_count(strings));
    }
    dawg_packed_destroy(dp);
    kwg_destroy(kwg);
    free(packed_output_filename);
  } else {
    char *kwg_output_filename = data_filepaths_get_writable_filename(
        data_paths, output_name, DATA_FILEPATH_TYPE_KWG, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
    kwg_maker_output_t output_type = KWG_MAKER_OUTPUT_DAWG_AND_GADDAG;
    if (conversion_type == CONVERT_TEXT2DAWG ||
        conversion_type == CONVERT_TEXT2DAWG_TAIL_REORDER) {
      output_type = KWG_MAKER_OUTPUT_DAWG;
    } else if (conversion_type == CONVERT_TEXT2GADDAG) {
      output_type = KWG_MAKER_OUTPUT_GADDAG;
    }
    kwg_maker_merge_t merge_type = KWG_MAKER_MERGE_EXACT;
    if (conversion_type == CONVERT_TEXT2KWG_TAIL_MERGE) {
      merge_type = KWG_MAKER_MERGE_TAIL;
    } else if (conversion_type == CONVERT_TEXT2DAWG_TAIL_REORDER) {
      merge_type = KWG_MAKER_MERGE_TAIL_REORDER;
    }
    KWG *kwg = make_kwg_from_words(strings, output_type, merge_type);
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

static void convert_klv_wmp_to_rit(const LetterDistribution *ld,
                                   const char *data_paths,
                                   const char *klv_name, const char *wmp_name,
                                   const char *base_rit_name,
                                   const char *output_name, int num_threads,
                                   ErrorStack *error_stack) {
  KLV *klv = klv_create(data_paths, klv_name, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  WMP *wmp = wmp_create(data_paths, wmp_name, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    klv_destroy(klv);
    return;
  }
  char *rit_output_filename = data_filepaths_get_writable_filename(
      data_paths, output_name, DATA_FILEPATH_TYPE_RACK_INFO_TABLE, error_stack);
  if (error_stack_is_empty(error_stack)) {
    // Default coverage is the full interval [1, RACK_SIZE]. The rit_sweep
    // on-demand test showed that widening coverage down to played_size == 1
    // monotonically improves movegen while the base entry size stays fixed.
    const uint8_t playthrough_min_played_size = 1;
    RackInfoTable *rit = make_rack_info_table(
        klv, wmp, ld, num_threads, playthrough_min_played_size);
    if (base_rit_name == NULL) {
      rack_info_table_write_to_file(rit, rit_output_filename, error_stack);
    } else {
      RackInfoTable *base_rit = rack_info_table_create(
          data_paths, base_rit_name, true, error_stack);
      char *base_rit_filename = NULL;
      if (error_stack_is_empty(error_stack)) {
        base_rit_filename = data_filepaths_get_readable_filename(
            data_paths, base_rit_name, DATA_FILEPATH_TYPE_RACK_INFO_TABLE,
            error_stack);
      }
      if (error_stack_is_empty(error_stack)) {
        rack_info_table_write_contextual_clone(
            rit, base_rit, base_rit_filename, rit_output_filename,
            error_stack);
      }
      free(base_rit_filename);
      rack_info_table_destroy(base_rit);
    }
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(
          error_stack, ERROR_STATUS_CONVERT_OUTPUT_FILE_NOT_WRITABLE,
          get_formatted_string("could not write rack info table to output "
                               "file: %s",
                               rit_output_filename));
    }
    rack_info_table_destroy(rit);
  }
  free(rit_output_filename);
  wmp_destroy(wmp);
  klv_destroy(klv);
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
      (conversion_type == CONVERT_TEXT2KWG_TAIL_MERGE) ||
      (conversion_type == CONVERT_TEXT2DAWG_TAIL_REORDER) ||
      (conversion_type == CONVERT_TEXT2DAWG_PACKED) ||
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
        WMP *wmp = make_wmp_from_kwg(kwg, ld, num_threads);
        wmp_write_to_file(wmp, wmp_output_filename, error_stack);
        if (!error_stack_is_empty(error_stack)) {
          error_stack_push(
              error_stack, ERROR_STATUS_CONVERT_OUTPUT_FILE_NOT_WRITABLE,
              get_formatted_string("could not write wordmap to output file: %s",
                                   wmp_output_filename));
        }
        wmp_destroy(wmp);
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
  } else if (conversion_type == CONVERT_KLVWMP2RIT) {
    convert_klv_wmp_to_rit(ld, data_paths, input_name, input_name, NULL,
                           output_name, num_threads, error_stack);
  } else if (conversion_type == CONVERT_RIT2WORDRIT) {
    RackInfoTable *rit =
        rack_info_table_create(data_paths, input_name, true, error_stack);
    char *output_filename = NULL;
    if (error_stack_is_empty(error_stack)) {
      output_filename = data_filepaths_get_writable_filename(
          data_paths, output_name, DATA_FILEPATH_TYPE_RACK_INFO_TABLE,
          error_stack);
    }
    if (error_stack_is_empty(error_stack)) {
      rack_info_table_write_word_only_copy(rit, output_filename, error_stack);
    }
    free(output_filename);
    rack_info_table_destroy(rit);
  } else if (conversion_type == CONVERT_KWG2WIT) {
    KWG *kwg = kwg_create(data_paths, input_name, error_stack);
    if (error_stack_is_empty(error_stack)) {
      char *wit_output_filename = data_filepaths_get_writable_filename(
          data_paths, output_name, DATA_FILEPATH_TYPE_WORD_INFO_TABLE,
          error_stack);
      if (error_stack_is_empty(error_stack)) {
        WordInfoTable *wit = make_word_info_table_from_kwg(kwg);
        word_info_table_write_to_file(wit, wit_output_filename, error_stack);
        if (!error_stack_is_empty(error_stack)) {
          error_stack_push(
              error_stack, ERROR_STATUS_CONVERT_OUTPUT_FILE_NOT_WRITABLE,
              get_formatted_string(
                  "could not write word info table to output file: %s",
                  wit_output_filename));
        }
        word_info_table_destroy(wit);
      }
      free(wit_output_filename);
    }
    kwg_destroy(kwg);
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
  } else if (strings_equal(conversion_type_string, "text2kwgtailmerge")) {
    conversion_type = CONVERT_TEXT2KWG_TAIL_MERGE;
  } else if (strings_equal(conversion_type_string, "text2dawgtailreorder")) {
    conversion_type = CONVERT_TEXT2DAWG_TAIL_REORDER;
  } else if (strings_equal(conversion_type_string, "text2dawgpacked")) {
    conversion_type = CONVERT_TEXT2DAWG_PACKED;
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
  } else if (strings_equal(conversion_type_string, "klvwmp2rit")) {
    conversion_type = CONVERT_KLVWMP2RIT;
  } else if (strings_equal(conversion_type_string, "rit2wordrit")) {
    conversion_type = CONVERT_RIT2WORDRIT;
  } else if (strings_equal(conversion_type_string, "kwg2wit")) {
    conversion_type = CONVERT_KWG2WIT;
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

  const char *input_name = args->input_and_output_name;
  const char *output_name = input_name;
  const char *wmp_name = input_name;
  const char *base_rit_name = NULL;
  StringSplitter *rit_names = NULL;
  if (conversion_type == CONVERT_KLVWMP2RIT) {
    rit_names = split_string(args->input_and_output_name, ',', false);
    const int name_count = string_splitter_get_number_of_items(rit_names);
    if (name_count == 3) {
      input_name = string_splitter_get_item(rit_names, 0);
      wmp_name = string_splitter_get_item(rit_names, 1);
      output_name = string_splitter_get_item(rit_names, 2);
    } else if (name_count == 4) {
      input_name = string_splitter_get_item(rit_names, 0);
      wmp_name = string_splitter_get_item(rit_names, 1);
      base_rit_name = string_splitter_get_item(rit_names, 2);
      output_name = string_splitter_get_item(rit_names, 3);
    } else if (name_count != 1) {
      error_stack_push(
          error_stack, ERROR_STATUS_CONVERT_INPUT_FILE_ERROR,
          string_duplicate("klvwmp2rit expects NAME, "
                           "KLV_NAME,WMP_NAME,OUTPUT_NAME, or "
                           "KLV_NAME,WMP_NAME,BASE_RIT_NAME,OUTPUT_NAME"));
      string_splitter_destroy(rit_names);
      return;
    }
  } else if (conversion_type == CONVERT_RIT2WORDRIT) {
    rit_names = split_string(args->input_and_output_name, ',', false);
    const int name_count = string_splitter_get_number_of_items(rit_names);
    if (name_count != 2) {
      error_stack_push(
          error_stack, ERROR_STATUS_CONVERT_INPUT_FILE_ERROR,
          string_duplicate(
              "rit2wordrit expects RIT_NAME,OUTPUT_NAME"));
      string_splitter_destroy(rit_names);
      return;
    }
    input_name = string_splitter_get_item(rit_names, 0);
    wmp_name = input_name;
    output_name = string_splitter_get_item(rit_names, 1);
  }

  char *ld_name = NULL;
  if (args->ld_name != NULL) {
    ld_name = string_duplicate(args->ld_name);
  } else {
    ld_name = ld_get_default_name_from_lexicon_name(wmp_name, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      string_splitter_destroy(rit_names);
      return;
    }
  }

  LetterDistribution *ld = ld_create(args->data_paths, ld_name, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    string_splitter_destroy(rit_names);
    return;
  }
  free(ld_name);

  if (conversion_type == CONVERT_KLVWMP2RIT &&
      (base_rit_name != NULL || !strings_equal(input_name, wmp_name))) {
    convert_klv_wmp_to_rit(
        ld, args->data_paths, input_name, wmp_name, base_rit_name, output_name,
        args->num_threads, error_stack);
  } else {
    convert_with_names(ld, conversion_type, args->data_paths, input_name,
                       output_name, conversion_results, args->num_threads,
                       error_stack);
  }
  ld_destroy(ld);
  string_splitter_destroy(rit_names);
}
