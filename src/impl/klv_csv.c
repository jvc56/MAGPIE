#include "../ent/dictionary_word.h"
#include "../ent/klv.h"
#include "../ent/leave_list.h"
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
  StringBuilder *string_builder;
} KLVWriteData;

typedef struct KLVCreateData {
  const Rack *leave;
  DictionaryWordList *dwl;
} KLVCreateData;

void klv_write_row(void *data, uint32_t word_index) {
  KLVWriteData *lasb = (KLVWriteData *)data;
  string_builder_add_rack(lasb->string_builder, lasb->leave, lasb->ld, true);
  string_builder_add_formatted_string(
      lasb->string_builder, ",%f\n",
      klv_get_indexed_leave_value(lasb->klv, word_index));
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
void klv_write_to_csv(KLV *klv, const LetterDistribution *ld,
                      const char *csv_filename) {
  const int dist_size = ld_get_size(ld);
  Rack *leave = rack_create(dist_size);
  Rack *bag_as_rack = get_new_bag_as_rack(ld);
  StringBuilder *klv_builder = string_builder_create();

  KLVWriteData klv_write_data;
  klv_write_data.klv = klv;
  klv_write_data.ld = ld;
  klv_write_data.leave = leave;
  klv_write_data.string_builder = klv_builder;

  LeaveIter leave_iter;
  leave_iter.data = (void *)&klv_write_data;
  leave_iter.leave_iter_func = klv_write_row;

  for (int i = 1; i < (RACK_SIZE); i++) {
    klv_iter_for_length(&leave_iter, klv, bag_as_rack, leave, i);
  }

  rack_destroy(leave);
  rack_destroy(bag_as_rack);

  write_string_to_file(csv_filename, "w", string_builder_peek(klv_builder));
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
KLV *klv_read_from_csv(const LetterDistribution *ld, const char *data_paths,
                       const char *leaves_name) {
  char *leaves_filename = data_filepaths_get_readable_filename(
      data_paths, leaves_name, DATA_FILEPATH_TYPE_LEAVES);

  FILE *stream = stream_from_filename(leaves_filename);
  if (!stream) {
    log_fatal("failed to open stream from filename: %s\n", leaves_filename);
  }
  free(leaves_filename);

  KLV *klv = klv_create_empty(ld, leaves_name);

  int number_of_leaves = klv_get_number_of_leaves(klv);

  bool *leave_was_set = (bool *)malloc_or_die(sizeof(bool) * number_of_leaves);

  for (int i = 0; i < number_of_leaves; i++) {
    leave_was_set[i] = false;
  }

  Rack *leave_rack = rack_create(ld_get_size(ld));
  bool str_to_double_success = false;
  char line[LEAVES_CSV_MAX_LINE_LENGTH];
  while (fgets(line, sizeof(line), stream)) {
    if (strchr(line, '\n') == NULL && !feof(stream)) {
      log_fatal("line exceeds max length: %s\n", line);
    }
    char *leave_str = strtok(line, ",");
    char *value_str = strtok(NULL, "\n");
    if (leave_str && value_str) {
      rack_set_to_string(ld, leave_rack, leave_str);
      int leave_index = klv_get_word_index(klv, leave_rack);
      if (leave_was_set[leave_index]) {
        log_fatal("duplicate leave: %s\n", leave_str);
      }
      double value =
          string_to_double_or_set_error(value_str, &str_to_double_success);
      if (!str_to_double_success) {
        log_fatal("malformed leave value in klv: %s\n", line);
      }
      klv_set_indexed_leave_value(klv, leave_index, value);
      leave_was_set[leave_index] = true;
    } else {
      log_fatal("malformed row in klv: %s\n", line);
    }
  }

  rack_destroy(leave_rack);
  free(leave_was_set);

  fclose(stream);

  return klv;
}