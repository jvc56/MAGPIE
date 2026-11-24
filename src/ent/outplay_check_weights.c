#include "outplay_check_weights.h"

#include "../def/letter_distribution_defs.h"
#include "../def/rack_defs.h"
#include "../util/fileproxy.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "data_filepaths.h"
#include "equity.h"
#include "letter_distribution.h"
#include "rack.h"
#include "static_eval.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

enum {
  OUTPLAY_CHECK_NUMBER_OF_TAGS = 2,
  OUTPLAY_CHECK_TAG_NO = 0,
  OUTPLAY_CHECK_TAG_OO = 1,
  // A letter term names one letter or a pair; one more slot catches a term
  // with too many.
  OUTPLAY_CHECK_TERM_LETTER_SLOTS = 3,
};

struct OutplayCheckWeights {
  char *name;
  char *ld_name;
  double constant[OUTPLAY_CHECK_NUMBER_OF_TAGS];
  double opponent_best_out;
  double opponent_best;
  double face[OUTPLAY_CHECK_NUMBER_OF_TAGS];
  double rack_size[OUTPLAY_CHECK_NUMBER_OF_TAGS][RACK_SIZE + 1];
  double single[OUTPLAY_CHECK_NUMBER_OF_TAGS][MAX_ALPHABET_SIZE];
  double pair[OUTPLAY_CHECK_NUMBER_OF_TAGS][MAX_ALPHABET_SIZE]
             [MAX_ALPHABET_SIZE];
  double own[OUTPLAY_CHECK_NUMBER_OF_TAGS];
  double own_best[OUTPLAY_CHECK_NUMBER_OF_TAGS];
  double own_two_lanes[OUTPLAY_CHECK_NUMBER_OF_TAGS];
  double own_three_lanes[OUTPLAY_CHECK_NUMBER_OF_TAGS];
  double own_face[OUTPLAY_CHECK_NUMBER_OF_TAGS];
  double own_size[OUTPLAY_CHECK_NUMBER_OF_TAGS][RACK_SIZE + 1];
  double center[RACK_SIZE + 1][RACK_SIZE + 1];
};

void outplay_check_weights_destroy(OutplayCheckWeights *weights) {
  if (!weights) {
    return;
  }
  free(weights->name);
  free(weights->ld_name);
  free(weights);
}

const char *outplay_check_weights_get_name(const OutplayCheckWeights *weights) {
  return weights->name;
}

const char *
outplay_check_weights_get_ld_name(const OutplayCheckWeights *weights) {
  return weights->ld_name;
}

// The file being read: its name for errors, the row being read, and the
// letter distribution named by its first row.
typedef struct WeightsReader {
  const char *data_paths;
  const char *filename;
  int row_number;
  LetterDistribution *ld;
  OutplayCheckWeights *weights;
  ErrorStack *error_stack;
} WeightsReader;

static void push_row_error(const WeightsReader *reader, const char *reason) {
  error_stack_push(reader->error_stack, ERROR_STATUS_OUTPLAY_CHECK_INVALID_ROW,
                   get_formatted_string("out-play check file %s, row %d: %s",
                                        reader->filename, reader->row_number,
                                        reason));
}

// The tag after a term's '|': "no" or "oo". Returns -1, after pushing an
// error, for anything else.
static int parse_tag(const WeightsReader *reader, const char *tag) {
  if (strings_equal(tag, "no")) {
    return OUTPLAY_CHECK_TAG_NO;
  }
  if (strings_equal(tag, "oo")) {
    return OUTPLAY_CHECK_TAG_OO;
  }
  push_row_error(reader, "a tag is no or oo");
  return -1;
}

// A count of 0 to RACK_SIZE in a term (n<k>, ownn<k> or a center's bag or
// leave). Returns -1, after pushing an error, for anything else.
static int parse_count(const WeightsReader *reader, const char *field) {
  if (field[0] < '0' || field[0] > '9' || field[1] != '\0' ||
      field[0] - '0' > RACK_SIZE) {
    push_row_error(reader, "expected a count of tiles from 0 to the rack size");
    return -1;
  }
  return field[0] - '0';
}

// center|<bag>|<leave size>.
static void add_center(WeightsReader *reader, const StringSplitter *parts,
                       double value) {
  if (string_splitter_get_number_of_items(parts) != 3) {
    push_row_error(reader, "a center is center|<bag>|<leave size>");
    return;
  }
  const int bag_size = parse_count(reader, string_splitter_get_item(parts, 1));
  if (bag_size < 0) {
    return;
  }
  const int leave_size =
      parse_count(reader, string_splitter_get_item(parts, 2));
  if (leave_size < 0) {
    return;
  }
  reader->weights->center[bag_size][leave_size] += value;
}

// A letter term: one letter or a pair, for one tag (tag_idx >= 0) or both.
static void add_letter_term(WeightsReader *reader, const char *letters,
                            int tag_idx, double value) {
  MachineLetter mls[OUTPLAY_CHECK_TERM_LETTER_SLOTS];
  const int number_of_letters = ld_str_to_mls(reader->ld, letters, false, mls,
                                              OUTPLAY_CHECK_TERM_LETTER_SLOTS);
  if (number_of_letters < 1 || number_of_letters > 2) {
    push_row_error(reader, "unknown term");
    return;
  }
  for (int letter_idx = 0; letter_idx < number_of_letters; letter_idx++) {
    if (mls[letter_idx] != BLANK_MACHINE_LETTER &&
        get_is_blanked(mls[letter_idx])) {
      push_row_error(reader, "a letter term is written in capitals and ?");
      return;
    }
  }
  if (number_of_letters == 2 && mls[1] < mls[0]) {
    push_row_error(reader, "a pair's letters are in the distribution's order");
    return;
  }
  OutplayCheckWeights *weights = reader->weights;
  for (int each_tag = 0; each_tag < OUTPLAY_CHECK_NUMBER_OF_TAGS; each_tag++) {
    if (tag_idx >= 0 && each_tag != tag_idx) {
      continue;
    }
    if (number_of_letters == 1) {
      weights->single[each_tag][mls[0]] += value;
    } else {
      weights->pair[each_tag][mls[0]][mls[1]] += value;
    }
  }
}

// A tagged term other than a letter term. Returns false if base is not one.
static bool add_tagged_term(OutplayCheckWeights *weights, const char *base,
                            int tag_idx, double value,
                            const WeightsReader *reader) {
  if (strings_equal(base, "const")) {
    weights->constant[tag_idx] += value;
  } else if (strings_equal(base, "face")) {
    weights->face[tag_idx] += value;
  } else if (strings_equal(base, "own")) {
    weights->own[tag_idx] += value;
  } else if (strings_equal(base, "ownb")) {
    weights->own_best[tag_idx] += value;
  } else if (strings_equal(base, "ownl2")) {
    weights->own_two_lanes[tag_idx] += value;
  } else if (strings_equal(base, "ownl3")) {
    weights->own_three_lanes[tag_idx] += value;
  } else if (strings_equal(base, "ownface")) {
    weights->own_face[tag_idx] += value;
  } else if (has_prefix("ownn", base)) {
    const int size = parse_count(reader, base + 4);
    if (size >= 0) {
      weights->own_size[tag_idx][size] += value;
    }
  } else if (base[0] == 'n' && isdigit((unsigned char)base[1])) {
    const int size = parse_count(reader, base + 1);
    if (size >= 0) {
      weights->rack_size[tag_idx][size] += value;
    }
  } else {
    return false;
  }
  return true;
}

static void add_term(WeightsReader *reader, const char *term, double value) {
  StringSplitter *parts = split_string(term, '|', false);
  const int number_of_parts = string_splitter_get_number_of_items(parts);
  const char *base = string_splitter_get_item(parts, 0);
  OutplayCheckWeights *weights = reader->weights;
  if (strings_equal(base, "center")) {
    add_center(reader, parts, value);
  } else if (number_of_parts > 2) {
    push_row_error(reader, "a term has at most one tag");
  } else if (strings_equal(base, "obo") || strings_equal(base, "obs")) {
    if (number_of_parts != 1) {
      push_row_error(reader, "obo and obs take no tag");
    } else if (strings_equal(base, "obo")) {
      weights->opponent_best_out += value;
    } else {
      weights->opponent_best += value;
    }
  } else {
    int tag_idx = -1;
    if (number_of_parts == 2) {
      tag_idx = parse_tag(reader, string_splitter_get_item(parts, 1));
    }
    if (error_stack_is_empty(reader->error_stack)) {
      if (tag_idx >= 0 &&
          add_tagged_term(weights, base, tag_idx, value, reader)) {
        // Added, or an error pushed.
      } else if (error_stack_is_empty(reader->error_stack)) {
        add_letter_term(reader, base, tag_idx, value);
      }
    }
  }
  string_splitter_destroy(parts);
}

static void read_row(WeightsReader *reader, const char *row) {
  StringSplitter *fields = split_string(row, ',', false);
  if (string_splitter_get_number_of_items(fields) != 2) {
    push_row_error(reader, "expected two fields, a term and a value");
    string_splitter_destroy(fields);
    return;
  }
  char *term = string_duplicate(string_splitter_get_item(fields, 0));
  trim_whitespace(term);
  char *value_field = string_duplicate(string_splitter_get_item(fields, 1));
  trim_whitespace(value_field);
  string_splitter_destroy(fields);
  if (reader->ld == NULL) {
    if (!strings_equal(term, "letterdistribution") || value_field[0] == '\0') {
      push_row_error(reader, "the first row is letterdistribution,<name>");
    } else {
      reader->ld =
          ld_create(reader->data_paths, value_field, reader->error_stack);
      if (reader->ld != NULL) {
        reader->weights->ld_name = string_duplicate(ld_get_name(reader->ld));
      }
    }
  } else if (strings_equal(term, "letterdistribution")) {
    push_row_error(reader, "the letter distribution is named once");
  } else {
    const double value = string_to_double(value_field, reader->error_stack);
    if (!error_stack_is_empty(reader->error_stack) || value_field[0] == '\0') {
      push_row_error(reader, "invalid value");
    } else {
      add_term(reader, term, value);
    }
  }
  free(value_field);
  free(term);
}

OutplayCheckWeights *outplay_check_weights_create(const char *data_paths,
                                                  const char *name,
                                                  ErrorStack *error_stack) {
  char *filename = data_filepaths_get_readable_filename(
      data_paths, name, DATA_FILEPATH_TYPE_OUTPLAY_CHECK, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    free(filename);
    return NULL;
  }
  char *contents = fileproxy_get_string_from_filename(filename, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    free(contents);
    free(filename);
    return NULL;
  }
  OutplayCheckWeights *weights = calloc_or_die(1, sizeof(OutplayCheckWeights));
  weights->name = string_duplicate(name);
  WeightsReader reader = {.data_paths = data_paths,
                          .filename = filename,
                          .row_number = 0,
                          .ld = NULL,
                          .weights = weights,
                          .error_stack = error_stack};
  StringSplitter *lines = split_string_by_newline(contents, false);
  const int number_of_lines = string_splitter_get_number_of_items(lines);
  for (int line_idx = 0;
       line_idx < number_of_lines && error_stack_is_empty(error_stack);
       line_idx++) {
    char *row = string_duplicate(string_splitter_get_item(lines, line_idx));
    trim_whitespace(row);
    reader.row_number = line_idx + 1;
    if (row[0] != '\0' && row[0] != '#') {
      read_row(&reader, row);
    }
    free(row);
  }
  string_splitter_destroy(lines);
  free(contents);
  if (error_stack_is_empty(error_stack) && reader.ld == NULL) {
    reader.row_number = number_of_lines;
    push_row_error(&reader, "expected a letterdistribution row");
  }
  ld_destroy(reader.ld);
  free(filename);
  if (!error_stack_is_empty(error_stack)) {
    outplay_check_weights_destroy(weights);
    return NULL;
  }
  return weights;
}

// The letter and pair terms of the rack held, and its size and face terms.
static double rack_terms(const OutplayCheckWeights *weights,
                         const LetterDistribution *ld, const Rack *held,
                         int tag) {
  const int held_tiles = rack_get_total_letters(held);
  double value = 0.0;
  if (held_tiles <= RACK_SIZE) {
    value += weights->rack_size[tag][held_tiles];
  }
  value += weights->face[tag] * equity_to_double(rack_get_score(ld, held));
  const int ld_size = ld_get_size(ld);
  for (int first = 0; first < ld_size; first++) {
    const int count = rack_get_letter(held, (MachineLetter)first);
    if (count == 0) {
      continue;
    }
    value += weights->single[tag][first];
    if (count >= 2) {
      value += weights->pair[tag][first][first];
    }
    for (int second = first + 1; second < ld_size; second++) {
      if (rack_get_letter(held, (MachineLetter)second) > 0) {
        value += weights->pair[tag][first][second];
      }
    }
  }
  return value;
}

double outplay_check_weights_value(const OutplayCheckWeights *weights,
                                   const LetterDistribution *ld,
                                   const Rack *held,
                                   const OutplayCheckFeatures *features) {
  const int tag = features->opponent_can_go_out ? OUTPLAY_CHECK_TAG_OO
                                                : OUTPLAY_CHECK_TAG_NO;
  const Equity held_face = rack_get_score(ld, held);
  double value = equity_to_double(endgame_nonoutplay_adjustment(held_face));
  if (features->opponent_can_go_out) {
    value -= features->opponent_best_out_score;
  }
  value += weights->constant[tag];
  if (features->opponent_can_go_out) {
    value += weights->opponent_best_out * features->opponent_best_out_score;
  } else {
    value += weights->opponent_best * features->opponent_best_score;
  }
  value += rack_terms(weights, ld, held, tag);
  if (features->own_out_plays > 0) {
    const int held_tiles = rack_get_total_letters(held);
    value +=
        weights->own[tag] +
        weights->own_best[tag] * features->own_best_out_score +
        (features->own_out_lanes >= 2 ? weights->own_two_lanes[tag] : 0.0) +
        (features->own_out_lanes >= 3 ? weights->own_three_lanes[tag] : 0.0) +
        weights->own_face[tag] * equity_to_double(held_face) +
        (held_tiles <= RACK_SIZE ? weights->own_size[tag][held_tiles] : 0.0);
  }
  return value;
}

double outplay_check_weights_get_center(const OutplayCheckWeights *weights,
                                        int bag_size, int leave_size) {
  if (bag_size < 0 || bag_size > RACK_SIZE || leave_size < 0 ||
      leave_size > RACK_SIZE) {
    return 0.0;
  }
  return weights->center[bag_size][leave_size];
}
