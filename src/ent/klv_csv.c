#include "../ent/dictionary_word.h"
#include "../ent/klv.h"
#include "../ent/letter_distribution.h"
#include "../ent/rack.h"

#include "../impl/kwg_maker.h"

#include "../str/rack_string.h"

#define LEAVES_CSV_MAX_LINE_LENGTH 256

typedef void (*leave_iter_func_t)(void *, uint32_t);

typedef struct LeaveIter {
  leave_iter_func_t leave_iter_func;
  void *data;
} LeaveIter;

typedef struct KLVWriteData {
  const KLV *klv;
  const LetterDistribution *ld;
  const Rack *leave;
  const uint64_t *leave_counts;
  StringBuilder *string_builder;
} KLVWriteData;

typedef struct KLVCreateData {
  const Rack *leave;
  DictionaryWordList *dwl;
} KLVCreateData;

void klv_write_row(void *data, uint32_t word_index) {
  KLVWriteData *klv_write_data = (KLVWriteData *)data;
  string_builder_add_rack(klv_write_data->string_builder, klv_write_data->leave,
                          klv_write_data->ld, true);
  if (klv_write_data->leave_counts) {
    const uint64_t leave_count = klv_write_data->leave_counts[word_index];
    string_builder_add_formatted_string(klv_write_data->string_builder,
                                        ",%lu\n", leave_count);
  } else {
    const double value = equity_to_double(
        klv_get_indexed_leave_value(klv_write_data->klv, word_index));
    string_builder_add_formatted_string(klv_write_data->string_builder, ",%f\n",
                                        value);
  }
}

void klv_add_leave_to_word_list(void *data,
                                uint32_t __attribute__((unused)) word_index) {
  KLVCreateData *klv_data = (KLVCreateData *)data;
  uint8_t word[(RACK_SIZE)-1];
  int letter_index = 0;
  const int dist_size = rack_get_dist_size(klv_data->leave);
  for (int i = 0; i < dist_size; i++) {
    for (int j = 0; j < rack_get_letter(klv_data->leave, i); j++) {
      word[letter_index++] = i;
    }
  }
  dictionary_word_list_add_word(klv_data->dwl, word, letter_index);
}

// To record in alphabetical order for all lengths, use length = -1
void klv_iter_for_length_recur(LeaveIter *leave_iter, KLV *klv, int length,
                               Rack *bag_as_rack, Rack *leave, uint8_t ml) {
  const int dist_size = rack_get_dist_size(leave);
  if (ml == dist_size) {
    return;
  }

  const int total_letters_on_rack = rack_get_total_letters(leave);

  // If length is -1 then we are recording for all lengths
  // and just need the leave to not be empty. If length isn't -1
  // then we are recording for a specific length.
  if ((length == -1 && total_letters_on_rack > 0) ||
      total_letters_on_rack == length) {
    leave_iter->leave_iter_func(leave_iter->data,
                                klv_get_word_index(klv, leave));
    // If we are only recording the leaves for a certain length,
    // then we have already reached the specified length and
    // must return so we do not exceed that length.
    if (length != -1 || total_letters_on_rack == (RACK_SIZE)-1) {
      return;
    }
  }

  for (int i = ml; i < dist_size; i++) {
    if (rack_get_letter(bag_as_rack, i) > 0) {
      rack_take_letter(bag_as_rack, i);
      rack_add_letter(leave, i);
      klv_iter_for_length_recur(leave_iter, klv, length, bag_as_rack, leave, i);
      rack_add_letter(bag_as_rack, i);
      rack_take_letter(leave, i);
    }
  }
}

// To record in alphabetical order for all lengths, use length = -1
void klv_iter_for_length(LeaveIter *leave_iter, KLV *klv, Rack *bag_as_rack,
                         Rack *leave, int length) {
  klv_iter_for_length_recur(leave_iter, klv, length, bag_as_rack, leave, 0);
}

// Writes a CSV file of leave,value for the leaves in the KLV.
// If leave_counts is NULL, then the value is the equity of the leave as a
// double. If leave_counts is not NULL, then the value is
// leave_counts[leave_index] as a uint64_t.
void klv_write_to_csv(KLV *klv, const LetterDistribution *ld,
                      const char *data_paths, const char *leaves_name,
                      const uint64_t *leave_counts, ErrorStack *error_stack) {
  char *leaves_filename = data_filepaths_get_writable_filename(
      data_paths, leaves_name, DATA_FILEPATH_TYPE_LEAVES, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  const int dist_size = ld_get_size(ld);
  Rack *leave = rack_create(dist_size);
  Rack *bag_as_rack = get_new_bag_as_rack(ld);
  StringBuilder *klv_builder = string_builder_create();

  KLVWriteData klv_write_data;
  klv_write_data.klv = klv;
  klv_write_data.ld = ld;
  klv_write_data.leave = leave;
  klv_write_data.leave_counts = leave_counts;
  klv_write_data.string_builder = klv_builder;

  LeaveIter leave_iter;
  leave_iter.data = (void *)&klv_write_data;
  leave_iter.leave_iter_func = klv_write_row;

  for (int i = 1; i < (RACK_SIZE); i++) {
    klv_iter_for_length(&leave_iter, klv, bag_as_rack, leave, i);
  }

  rack_destroy(leave);
  rack_destroy(bag_as_rack);

  write_string_to_file(leaves_filename, "w", string_builder_peek(klv_builder),
                       error_stack);
  free(leaves_filename);
  string_builder_destroy(klv_builder);
}

KLV *klv_create_empty(const LetterDistribution *ld, const char *name) {
  const int dist_size = ld_get_size(ld);
  Rack *leave = rack_create(dist_size);
  Rack *bag_as_rack = get_new_bag_as_rack(ld);
  DictionaryWordList *dwl = dictionary_word_list_create();

  KLVCreateData klv_create_data;
  klv_create_data.dwl = dwl;
  klv_create_data.leave = leave;

  LeaveIter leave_iter;
  leave_iter.data = (void *)&klv_create_data;
  leave_iter.leave_iter_func = klv_add_leave_to_word_list;

  klv_iter_for_length(&leave_iter, NULL, bag_as_rack, leave, -1);

  rack_destroy(leave);
  rack_destroy(bag_as_rack);

  int number_of_words = dictionary_word_list_get_count(klv_create_data.dwl);
  KWG *kwg = make_kwg_from_words(klv_create_data.dwl, KWG_MAKER_OUTPUT_DAWG,
                                 KWG_MAKER_MERGE_EXACT);
  dictionary_word_list_destroy(dwl);

  return klv_create_zeroed_from_kwg(kwg, number_of_words, name);
}

// Reads a CSV file of leave,value and returns a KLV.
void klv_read_from_csv_internal(const LetterDistribution *ld,
                                FILE *leaves_stream, KLV *klv,
                                bool *leave_was_set,
                                const char *leaves_filename,
                                ErrorStack *error_stack) {
  Rack leave_rack;
  rack_set_dist_size(&leave_rack, ld_get_size(ld));
  char line[LEAVES_CSV_MAX_LINE_LENGTH];
  while (fgets(line, sizeof(line), leaves_stream)) {
    if (strchr(line, '\n') == NULL && !feof(leaves_stream)) {
      error_stack_push(
          error_stack, ERROR_STATUS_KLV_LINE_EXCEEDS_MAX_LENGTH,
          get_formatted_string(
              "line in klv csv file '%s' exceeds max length of %d: %s",
              leaves_filename, LEAVES_CSV_MAX_LINE_LENGTH, line));
      return;
    }
    char *leave_str = strtok(line, ",");
    trim_whitespace(leave_str);
    char *value_str = strtok(NULL, "\n");
    trim_whitespace(value_str);
    if (leave_str && value_str) {
      rack_set_to_string(ld, &leave_rack, leave_str);
      const int leave_index = klv_get_word_index(klv, &leave_rack);
      if (leave_was_set[leave_index]) {
        error_stack_push(
            error_stack, ERROR_STATUS_KLV_DUPLICATE_LEAVE,
            get_formatted_string("duplicate leave found in klv csv file %s: %s",
                                 leaves_filename, leave_str));
        return;
      }
      const double value = string_to_double(value_str, error_stack);
      if (!error_stack_is_empty(error_stack)) {
        error_stack_push(
            error_stack, ERROR_STATUS_KLV_INVALID_LEAVE,
            get_formatted_string("invalid leave found in klv csv file %s: %s",
                                 leaves_filename, line));
        return;
      }
      klv_set_indexed_leave_value(klv, leave_index, double_to_equity(value));
      leave_was_set[leave_index] = true;
    } else {
      error_stack_push(
          error_stack, ERROR_STATUS_KLV_INVALID_ROW,
          get_formatted_string("invalid row found in klv csv file %s: %s",
                               leaves_filename, line));
      return;
    }
  }
}

KLV *klv_read_from_csv(const LetterDistribution *ld, const char *data_paths,
                       const char *leaves_name, ErrorStack *error_stack) {
  char *leaves_filename = data_filepaths_get_readable_filename(
      data_paths, leaves_name, DATA_FILEPATH_TYPE_LEAVES, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return NULL;
  }

  KLV *klv = NULL;
  FILE *stream = stream_from_filename(leaves_filename, error_stack);
  if (error_stack_is_empty(error_stack)) {
    klv = klv_create_empty(ld, leaves_name);
    int number_of_leaves = klv_get_number_of_leaves(klv);
    bool *leave_was_set = (bool *)calloc_or_die(number_of_leaves, sizeof(bool));
    klv_read_from_csv_internal(ld, stream, klv, leave_was_set, leaves_filename,
                               error_stack);
    if (!error_stack_is_empty(error_stack)) {
      klv_destroy(klv);
      klv = NULL;
    }
    free(leave_was_set);
    fclose_or_die(stream);
  }
  free(leaves_filename);
  return klv;
}