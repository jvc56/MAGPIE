#include "convert.h"

#include "../def/board_defs.h"
#include "../def/convert_defs.h"
#include "../def/kwg_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../ent/compact_leaves.h"
#include "../ent/conversion_results.h"
#include "../ent/data_filepaths.h"
#include "../ent/dawg_arc_compressed.h"
#include "../ent/dawg_packed.h"
#include "../ent/dictionary_word.h"
#include "../ent/klv.h"
#include "../ent/klv_csv.h"
#include "../ent/kwg.h"
#include "../ent/letter_distribution.h"
#include "../ent/rack_info_table.h"
#include "../ent/wmp.h"
#include "../util/fileproxy.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "compact_leaves_maker.h"
#include "kwg_maker.h"
#include "rack_info_table_maker.h"
#include "wmp_maker.h"
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
  } else if (conversion_type == CONVERT_TEXT2DAWG_ARC_COMPRESSED ||
             conversion_type == CONVERT_TEXT2DAWG_ARC_COMPRESSED_BALANCED) {
    char *arc_compressed_output_filename = data_filepaths_get_writable_filename(
        data_paths, output_name, DATA_FILEPATH_TYPE_DAWG_ARC_COMPRESSED,
        error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
    // Build the reorder DAWG, then arc-compress it (popular table + local gaps
    // + rank-located escapes) for a smaller resident footprint than the packed
    // DAWG. Niche, opt-in output for fitting a word list onto retro hardware.
    // BALANCED trades a little of that RAM win for materially faster traversal.
    const dawg_arc_compressed_mode_t mode =
        conversion_type == CONVERT_TEXT2DAWG_ARC_COMPRESSED_BALANCED
            ? DAWG_ARC_COMPRESSED_MODE_BALANCED
            : DAWG_ARC_COMPRESSED_MODE_MIN_RAM;
    KWG *kwg = make_kwg_from_words(strings, KWG_MAKER_OUTPUT_DAWG,
                                   KWG_MAKER_MERGE_TAIL_REORDER);
    DawgArcCompressed *dp = dawg_arc_compressed_create_from_kwg(kwg, mode);
    dawg_arc_compressed_write_to_file(dp, arc_compressed_output_filename,
                                      error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(
          error_stack, ERROR_STATUS_CONVERT_OUTPUT_FILE_NOT_WRITABLE,
          get_formatted_string(
              "could not write arc-compressed dawg to output file: %s",
              arc_compressed_output_filename));
    } else {
      conversion_results_set_number_of_strings(
          conversion_results, dictionary_word_list_get_count(strings));
    }
    dawg_arc_compressed_destroy(dp);
    kwg_destroy(kwg);
    free(arc_compressed_output_filename);
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

void convert_with_names(const LetterDistribution *ld,
                        conversion_type_t conversion_type,
                        const char *data_paths, const char *input_name,
                        const char *output_name,
                        ConversionResults *conversion_results, int num_threads,
                        int clv_target_bytes, ErrorStack *error_stack) {
  if ((conversion_type == CONVERT_TEXT2DAWG) ||
      (conversion_type == CONVERT_TEXT2GADDAG) ||
      (conversion_type == CONVERT_TEXT2KWG) ||
      (conversion_type == CONVERT_TEXT2KWG_TAIL_MERGE) ||
      (conversion_type == CONVERT_TEXT2DAWG_TAIL_REORDER) ||
      (conversion_type == CONVERT_TEXT2DAWG_PACKED) ||
      (conversion_type == CONVERT_TEXT2DAWG_ARC_COMPRESSED) ||
      (conversion_type == CONVERT_TEXT2DAWG_ARC_COMPRESSED_BALANCED) ||
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
  } else if (conversion_type == CONVERT_KLV2CLV) {
    KLV *klv = klv_create(data_paths, input_name, error_stack);
    if (error_stack_is_empty(error_stack)) {
      char *clv_output_filename = data_filepaths_get_writable_filename(
          data_paths, output_name, DATA_FILEPATH_TYPE_COMPACT_LEAVES,
          error_stack);
      if (error_stack_is_empty(error_stack)) {
        // Uniform weighting, default (eighth) radix, bit-packed body for the
        // smallest fit. Frequency-weighted fitting (compact_leaves_read_weights
        // _csv) and radix selection are available in the maker for callers that
        // want them; the convert command ships the common case.
        const size_t target_bytes = (size_t)clv_target_bytes;
        CompactLeaves *cl = compact_leaves_create_from_klv(
            klv, ld, NULL, target_bytes, COMPACT_LEAVES_RADIX_EIGHTH, true);
        compact_leaves_write_to_file(cl, clv_output_filename, error_stack);
        if (!error_stack_is_empty(error_stack)) {
          error_stack_push(
              error_stack, ERROR_STATUS_CONVERT_OUTPUT_FILE_NOT_WRITABLE,
              get_formatted_string(
                  "could not write compact leaves to output file: %s",
                  clv_output_filename));
        }
        compact_leaves_destroy(cl);
      }
      free(clv_output_filename);
    }
    klv_destroy(klv);
  } else if (conversion_type == CONVERT_KLVWMP2RIT) {
    KLV *klv = klv_create(data_paths, input_name, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
    WMP *wmp = wmp_create(data_paths, input_name, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      klv_destroy(klv);
      return;
    }
    char *rit_output_filename = data_filepaths_get_writable_filename(
        data_paths, output_name, DATA_FILEPATH_TYPE_RACK_INFO_TABLE,
        error_stack);
    if (error_stack_is_empty(error_stack)) {
      // Default coverage is the full interval [1, RACK_SIZE]. The rit_sweep
      // on-demand test (test/rack_info_table_test.c) showed that widening
      // coverage from played_size == RACK_SIZE down to played_size == 1
      // monotonically improves CSW24 movegen user time by ~3% while the
      // on-disk file stays flat at ~1.69 GB (entry size is fixed at 560 B
      // regardless of min because playthrough_union is a fixed-size
      // leave_size-indexed array, not variable-length per-slot storage).
      // So there's no lighter variant worth shipping.
      const uint8_t playthrough_min_played_size = 1;
      RackInfoTable *rit = make_rack_info_table(klv, wmp, ld, num_threads,
                                                playthrough_min_played_size);
      rack_info_table_write_to_file(rit, rit_output_filename, error_stack);
      if (!error_stack_is_empty(error_stack)) {
        error_stack_push(
            error_stack, ERROR_STATUS_CONVERT_OUTPUT_FILE_NOT_WRITABLE,
            get_formatted_string(
                "could not write rack info table to output file: %s",
                rit_output_filename));
      }
      rack_info_table_destroy(rit);
    }
    free(rit_output_filename);
    wmp_destroy(wmp);
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
  } else if (strings_equal(conversion_type_string, "text2kwgtailmerge")) {
    conversion_type = CONVERT_TEXT2KWG_TAIL_MERGE;
  } else if (strings_equal(conversion_type_string, "text2dawgtailreorder")) {
    conversion_type = CONVERT_TEXT2DAWG_TAIL_REORDER;
  } else if (strings_equal(conversion_type_string, "text2dawgpacked")) {
    conversion_type = CONVERT_TEXT2DAWG_PACKED;
  } else if (strings_equal(conversion_type_string, "text2acdawg")) {
    conversion_type = CONVERT_TEXT2DAWG_ARC_COMPRESSED;
  } else if (strings_equal(conversion_type_string, "text2acdawgbalanced")) {
    conversion_type = CONVERT_TEXT2DAWG_ARC_COMPRESSED_BALANCED;
  } else if (strings_equal(conversion_type_string, "dawg2text")) {
    conversion_type = CONVERT_DAWG2TEXT;
  } else if (strings_equal(conversion_type_string, "gaddag2text")) {
    conversion_type = CONVERT_GADDAG2TEXT;
  } else if (strings_equal(conversion_type_string, "csv2klv")) {
    conversion_type = CONVERT_CSV2KLV;
  } else if (strings_equal(conversion_type_string, "klv2csv")) {
    conversion_type = CONVERT_KLV2CSV;
  } else if (strings_equal(conversion_type_string, "klv2clv")) {
    conversion_type = CONVERT_KLV2CLV;
  } else if (strings_equal(conversion_type_string, "text2wordmap")) {
    conversion_type = CONVERT_TEXT2WORDMAP;
  } else if (strings_equal(conversion_type_string, "dawg2wordmap")) {
    conversion_type = CONVERT_DAWG2WORDMAP;
  } else if (strings_equal(conversion_type_string, "klvwmp2rit")) {
    conversion_type = CONVERT_KLVWMP2RIT;
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
                     conversion_results, args->num_threads,
                     args->clv_target_bytes, error_stack);
  ld_destroy(ld);
}
