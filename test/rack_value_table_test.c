#include "rack_value_table_test.h"

#include "../src/compat/cfile.h"
#include "../src/def/bit_rack_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/bit_rack.h"
#include "../src/ent/equity.h"
#include "../src/ent/rack_value_table.h"
#include "../src/util/io_util.h"
#include "test_platform.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  RVT_TEST_KLV_HASH_LOW = 0x89abcdef,
  RVT_TEST_KLV_HASH_HIGH = 0x01234567,
  // where the fields of the file's header start
  RVT_TEST_VERSION_OFFSET = 0,
  RVT_TEST_RACK_SIZE_OFFSET = 1,
  RVT_TEST_NUM_LETTERS_OFFSET = 2,
  RVT_TEST_NUM_RACKS_OFFSET = 4,
  RVT_TEST_LETTER_COUNTS_OFFSET = 16,
  RVT_TEST_VALUE_BYTES = 3,
  // one rack in this many is left unvalued
  RVT_TEST_UNVALUED_PERIOD = 5,
  RVT_TEST_ENGLISH_LETTERS = 27,
  RVT_TEST_ENGLISH_RACKS = 3199724,
};

#define RVT_TEST_KLV_HASH                                                      \
  (((uint64_t)RVT_TEST_KLV_HASH_HIGH << 32) | RVT_TEST_KLV_HASH_LOW)

static Equity rvt_test_value(uint32_t index) {
  return (Equity)((int)(index * 7919U % 200001U) - 100000);
}

static bool rvt_test_is_valued(uint32_t index) {
  return index % RVT_TEST_UNVALUED_PERIOD != RVT_TEST_UNVALUED_PERIOD - 1;
}

static uint8_t *rvt_test_read_file(const char *filename, size_t *size) {
  FILE *stream = fopen_or_die(filename, "rb");
  const int seek_end_result = fseek(stream, 0, SEEK_END);
  assert(seek_end_result == 0);
  const long length = ftell(stream);
  assert(length >= 0);
  const int seek_start_result = fseek(stream, 0, SEEK_SET);
  assert(seek_start_result == 0);
  uint8_t *data = malloc_or_die((size_t)length + 1);
  const size_t bytes_read = fread(data, 1, (size_t)length, stream);
  assert(bytes_read == (size_t)length);
  fclose_or_die(stream);
  *size = (size_t)length;
  return data;
}

static void rvt_test_write_file(const char *filename, const uint8_t *data,
                                size_t size) {
  FILE *stream = fopen_or_die(filename, "wb");
  const size_t bytes_written = fwrite(data, 1, size, stream);
  assert(bytes_written == size);
  fclose_or_die(stream);
}

typedef enum {
  RVT_TEST_VISIT_SET,
  RVT_TEST_VISIT_CHECK,
} rvt_test_visit_t;

// Walks every rack of RACK_SIZE tiles that letter_counts can deal, fewest of
// the first letter first: the order the table numbers them in, found here
// without the table's arithmetic.
typedef struct RvtTestWalk {
  const uint8_t *letter_counts;
  int num_letters;
  RackValueTable *rvt;
  rvt_test_visit_t visit;
  BitRack bit_rack;
  uint32_t racks_seen;
} RvtTestWalk;

static void rvt_test_visit_rack(RvtTestWalk *walk) {
  const uint32_t index = walk->racks_seen++;
  uint32_t table_index = UINT32_MAX;
  const bool has_index =
      rack_value_table_get_rack_index(walk->rvt, &walk->bit_rack, &table_index);
  assert(has_index);
  assert(table_index == index);
  if (walk->visit == RVT_TEST_VISIT_SET) {
    if (rvt_test_is_valued(index)) {
      const bool was_set = rack_value_table_set(walk->rvt, &walk->bit_rack,
                                                rvt_test_value(index));
      assert(was_set);
    }
    return;
  }
  Equity value = 12345;
  const bool found =
      rack_value_table_lookup(walk->rvt, &walk->bit_rack, &value);
  assert(found == rvt_test_is_valued(index));
  assert(value == (found ? rvt_test_value(index) : 12345));
}

static void rvt_test_walk_from(RvtTestWalk *walk, int ml, int tiles) {
  if (ml == walk->num_letters) {
    if (tiles == 0) {
      rvt_test_visit_rack(walk);
    }
    return;
  }
  int copies = 0;
  while (true) {
    rvt_test_walk_from(walk, ml + 1, tiles - copies);
    if (copies == tiles || copies == walk->letter_counts[ml]) {
      break;
    }
    bit_rack_add_letter(&walk->bit_rack, (MachineLetter)ml);
    copies++;
  }
  for (; copies > 0; copies--) {
    bit_rack_take_letter(&walk->bit_rack, (MachineLetter)ml);
  }
}

static uint32_t rvt_test_walk(RackValueTable *rvt, const uint8_t *letter_counts,
                              int num_letters, rvt_test_visit_t visit) {
  RvtTestWalk walk = {.letter_counts = letter_counts,
                      .num_letters = num_letters,
                      .rvt = rvt,
                      .visit = visit,
                      .bit_rack = bit_rack_create_empty(),
                      .racks_seen = 0};
  rvt_test_walk_from(&walk, 0, RACK_SIZE);
  return walk.racks_seen;
}

// BitRacks that letter_counts cannot deal have no index.
static void assert_undealt_racks_missing(const RackValueTable *rvt,
                                         const uint8_t *letter_counts,
                                         int num_letters) {
  Equity value = 12345;
  uint32_t rack_index = 54321;
  // too few tiles and too many, of a letter with RACK_SIZE or more if any
  for (int ml = 0; ml < num_letters; ml++) {
    BitRack bit_rack = bit_rack_create_empty();
    for (int copies = 0; copies <= RACK_SIZE + 1; copies++) {
      const bool dealt = copies == RACK_SIZE && letter_counts[ml] >= RACK_SIZE;
      const bool has_index =
          rack_value_table_get_rack_index(rvt, &bit_rack, &rack_index);
      assert(has_index == dealt);
      if (!dealt) {
        const bool found = rack_value_table_lookup(rvt, &bit_rack, &value);
        assert(!found);
        assert(value == 12345);
        assert(rack_index == 54321);
      }
      rack_index = 54321;
      bit_rack_add_letter(&bit_rack, (MachineLetter)ml);
    }
  }
  // a letter past the distribution's last
  if (num_letters < BIT_RACK_MAX_ALPHABET_SIZE) {
    BitRack bit_rack = bit_rack_create_empty();
    bit_rack_add_letter(&bit_rack, (MachineLetter)num_letters);
    for (int ml = 0; ml < num_letters; ml++) {
      for (int copies = 0; copies < letter_counts[ml] &&
                           bit_rack_num_letters(&bit_rack) < RACK_SIZE;
           copies++) {
        bit_rack_add_letter(&bit_rack, (MachineLetter)ml);
      }
    }
    const bool has_index =
        rack_value_table_get_rack_index(rvt, &bit_rack, &rack_index);
    assert(!has_index);
  }
}

// Builds the table of a distribution, values its racks and reads it back from
// its file both ways. Returns the number of racks.
static uint32_t test_round_trip(const char *data_paths,
                                const uint8_t *letter_counts, int num_letters,
                                const char *name) {
  RackValueTable *built = rack_value_table_create_unvalued(
      letter_counts, num_letters, RVT_TEST_KLV_HASH);
  assert(built);
  const uint32_t number_of_racks = rack_value_table_get_number_of_racks(built);
  // unvalued until set
  BitRack first_rack = bit_rack_create_empty();
  for (int ml = 0; ml < num_letters; ml++) {
    for (int copies = 0; copies < letter_counts[ml] &&
                         bit_rack_num_letters(&first_rack) < RACK_SIZE;
         copies++) {
      bit_rack_add_letter(&first_rack, (MachineLetter)ml);
    }
  }
  Equity unset_value = 12345;
  const bool found_unset =
      rack_value_table_lookup(built, &first_rack, &unset_value);
  assert(!found_unset);
  assert(unset_value == 12345);

  const uint32_t racks_set =
      rvt_test_walk(built, letter_counts, num_letters, RVT_TEST_VISIT_SET);
  assert(racks_set == number_of_racks);
  rvt_test_walk(built, letter_counts, num_letters, RVT_TEST_VISIT_CHECK);
  // all but one in each full period
  assert(rack_value_table_get_number_of_valued_racks(built) ==
         number_of_racks - number_of_racks / RVT_TEST_UNVALUED_PERIOD);
  assert_undealt_racks_missing(built, letter_counts, num_letters);

  ErrorStack *error_stack = error_stack_create();
  rack_value_table_write(built, data_paths, name, error_stack);
  assert(error_stack_is_empty(error_stack));
  rack_value_table_destroy(built);
  for (int use_mmap = 0; use_mmap < 2; use_mmap++) {
    RackValueTable *loaded =
        rack_value_table_create(data_paths, name, use_mmap == 1, error_stack);
    assert(error_stack_is_empty(error_stack));
    assert_strings_equal(rack_value_table_get_name(loaded), name);
    assert(rack_value_table_get_number_of_racks(loaded) == number_of_racks);
    assert(rack_value_table_get_klv_hash(loaded) == RVT_TEST_KLV_HASH);
    assert(rack_value_table_get_number_of_valued_racks(loaded) ==
           number_of_racks - number_of_racks / RVT_TEST_UNVALUED_PERIOD);
    const uint32_t racks_checked =
        rvt_test_walk(loaded, letter_counts, num_letters, RVT_TEST_VISIT_CHECK);
    assert(racks_checked == number_of_racks);
    assert_undealt_racks_missing(loaded, letter_counts, num_letters);
    if (use_mmap == 1 && number_of_racks > 0) {
      // a mapped table is read-only
      const bool was_set = rack_value_table_set(loaded, &first_rack, 1);
      assert(!was_set);
    }
    rack_value_table_destroy(loaded);
  }
  error_stack_destroy(error_stack);
  return number_of_racks;
}

// Writes data as the table named name and checks that neither way of loading
// it accepts it, each failing with expected_status.
static void assert_load_fails(const char *data_paths, const char *filename,
                              const char *name, const uint8_t *data,
                              size_t size, error_code_t expected_status) {
  rvt_test_write_file(filename, data, size);
  for (int use_mmap = 0; use_mmap < 2; use_mmap++) {
    ErrorStack *error_stack = error_stack_create();
    const RackValueTable *rvt =
        rack_value_table_create(data_paths, name, use_mmap == 1, error_stack);
    assert(!rvt);
    assert(error_stack_top(error_stack) == expected_status);
    error_stack_destroy(error_stack);
  }
}

static void rvt_test_set_uint32(uint8_t *data, size_t offset, uint32_t value) {
  for (int byte_idx = 0; byte_idx < 4; byte_idx++) {
    data[offset + (size_t)byte_idx] = (uint8_t)(value >> (8 * byte_idx));
  }
}

static uint32_t rvt_test_get_uint32(const uint8_t *data, size_t offset) {
  uint32_t value = 0;
  for (int byte_idx = 0; byte_idx < 4; byte_idx++) {
    value |= (uint32_t)data[offset + (size_t)byte_idx] << (8 * byte_idx);
  }
  return value;
}

// The values at and past the ends of what the file holds, through a file.
static void test_value_range(const char *data_paths) {
  enum { RANGE_TEST_VALUES = 9 };
  // the largest value the file's 24 bits hold, in thousandths of a point
  const Equity max_value = 8388607;
  // {given, expected}
  const Equity cases[RANGE_TEST_VALUES][2] = {
      {0, 0},
      {-1, -1},
      {39214, 39214},
      {max_value, max_value},
      {-max_value, -max_value},
      {max_value + 1, max_value},
      // the number that marks an unvalued rack is not a value
      {-max_value - 1, -max_value},
      {EQUITY_MAX_VALUE, max_value},
      {EQUITY_MIN_VALUE, -max_value},
  };
  // RACK_SIZE + 1 racks: 0 to RACK_SIZE of the first letter, the rest of the
  // second
  const uint8_t letter_counts[2] = {RANGE_TEST_VALUES, RACK_SIZE};
  RackValueTable *built =
      rack_value_table_create_unvalued(letter_counts, 2, RVT_TEST_KLV_HASH);
  assert(rack_value_table_get_number_of_racks(built) == RACK_SIZE + 1);
  ErrorStack *error_stack = error_stack_create();
  for (int case_idx = 0; case_idx < RANGE_TEST_VALUES; case_idx++) {
    BitRack bit_rack = bit_rack_create_empty();
    const int first_letter_copies = case_idx % (RACK_SIZE + 1);
    for (int tile_idx = 0; tile_idx < RACK_SIZE; tile_idx++) {
      bit_rack_add_letter(
          &bit_rack, (MachineLetter)(tile_idx < first_letter_copies ? 0 : 1));
    }
    const bool was_set =
        rack_value_table_set(built, &bit_rack, cases[case_idx][0]);
    assert(was_set);
    rack_value_table_write(built, data_paths, "range", error_stack);
    assert(error_stack_is_empty(error_stack));
    for (int use_mmap = 0; use_mmap < 2; use_mmap++) {
      RackValueTable *loaded = rack_value_table_create(
          data_paths, "range", use_mmap == 1, error_stack);
      assert(error_stack_is_empty(error_stack));
      Equity value = 12345;
      const bool found = rack_value_table_lookup(loaded, &bit_rack, &value);
      assert(found);
      assert(value == cases[case_idx][1]);
      rack_value_table_destroy(loaded);
    }
  }
  error_stack_destroy(error_stack);
  rack_value_table_destroy(built);
}

void test_rack_value_table(void) {
  char *tmp_dir = test_platform_make_temporary_directory("magpie_rvt_");
  char *lexica_dir = get_formatted_string("%s/lexica", tmp_dir);
  const int mkdir_result = cfile_mkdir(lexica_dir);
  assert(mkdir_result == 0);

  // ten letters, some scarcer than a rack and some not
  const uint8_t ten_counts[10] = {2, 9, 2, 4, 12, 1, 3, 7, 1, 2};
  const uint32_t ten_racks = test_round_trip(tmp_dir, ten_counts, 10, "ten");
  assert(ten_racks > 1000);
  // the English tiles: as many racks as a census of them values
  const uint8_t english_counts[RVT_TEST_ENGLISH_LETTERS] = {
      2, 9, 2, 2, 4, 12, 2, 3, 2, 9, 1, 1, 4, 2,
      6, 8, 2, 1, 6, 4,  6, 4, 2, 2, 1, 2, 1};
  const uint32_t english_racks = test_round_trip(
      tmp_dir, english_counts, RVT_TEST_ENGLISH_LETTERS, "english");
  assert(english_racks == RVT_TEST_ENGLISH_RACKS);
  // one rack, and none: fewer tiles than a rack
  const uint8_t one_rack_counts[2] = {RACK_SIZE - 3, 3};
  const uint32_t one_rack = test_round_trip(tmp_dir, one_rack_counts, 2, "one");
  assert(one_rack == 1);
  const uint8_t no_rack_counts[2] = {3, RACK_SIZE - 4};
  const uint32_t no_racks = test_round_trip(tmp_dir, no_rack_counts, 2, "none");
  assert(no_racks == 0);
  // as many letters as a BitRack holds
  uint8_t most_counts[BIT_RACK_MAX_ALPHABET_SIZE + 1];
  memset(most_counts, 1, sizeof(most_counts));
  most_counts[BIT_RACK_MAX_ALPHABET_SIZE - 1] = 2;
  const uint32_t most_racks =
      test_round_trip(tmp_dir, most_counts, BIT_RACK_MAX_ALPHABET_SIZE, "most");
  assert(most_racks > 0);
  const RackValueTable *too_many_letters = rack_value_table_create_unvalued(
      most_counts, BIT_RACK_MAX_ALPHABET_SIZE + 1, RVT_TEST_KLV_HASH);
  assert(!too_many_letters);
  const RackValueTable *no_letters =
      rack_value_table_create_unvalued(most_counts, 0, RVT_TEST_KLV_HASH);
  assert(!no_letters);

  test_value_range(tmp_dir);

  ErrorStack *error_stack = error_stack_create();
  const RackValueTable *missing =
      rack_value_table_create(tmp_dir, "missing", false, error_stack);
  assert(!missing);
  assert(!error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);

  // Files that must not load.
  char *good_filename = get_formatted_string("%s/ten.rvt", lexica_dir);
  char *bad_filename = get_formatted_string("%s/bad.rvt", lexica_dir);
  size_t size = 0;
  uint8_t *good = rvt_test_read_file(good_filename, &size);
  uint8_t *bad = malloc_or_die(size + 4);
  assert(rvt_test_get_uint32(good, RVT_TEST_NUM_RACKS_OFFSET) == ten_racks);
  // ten letter counts, padded to twelve bytes
  const size_t values_offset = RVT_TEST_LETTER_COUNTS_OFFSET + 12;
  assert(size == values_offset + (size_t)ten_racks * RVT_TEST_VALUE_BYTES);

  // the file as written, under the other name: the control for the rest
  rvt_test_write_file(bad_filename, good, size);
  error_stack = error_stack_create();
  RackValueTable *copy =
      rack_value_table_create(tmp_dir, "bad", false, error_stack);
  assert(error_stack_is_empty(error_stack));
  const uint32_t copy_racks =
      rvt_test_walk(copy, ten_counts, 10, RVT_TEST_VISIT_CHECK);
  assert(copy_racks == ten_racks);
  rack_value_table_destroy(copy);
  error_stack_destroy(error_stack);

  memcpy(bad, good, size);
  bad[RVT_TEST_VERSION_OFFSET]++;
  assert_load_fails(tmp_dir, bad_filename, "bad", bad, size,
                    ERROR_STATUS_WMP_UNSUPPORTED_VERSION);
  // the layout before this one
  bad[RVT_TEST_VERSION_OFFSET] = 1;
  assert_load_fails(tmp_dir, bad_filename, "bad", bad, size,
                    ERROR_STATUS_WMP_UNSUPPORTED_VERSION);

  memcpy(bad, good, size);
  bad[RVT_TEST_RACK_SIZE_OFFSET]++;
  assert_load_fails(tmp_dir, bad_filename, "bad", bad, size,
                    ERROR_STATUS_WMP_INCOMPATIBLE_BOARD_DIM);

  // cut short: inside the header, the letter counts, and the last value
  assert_load_fails(tmp_dir, bad_filename, "bad", good, 0,
                    ERROR_STATUS_RW_READ_ERROR);
  assert_load_fails(tmp_dir, bad_filename, "bad", good,
                    RVT_TEST_LETTER_COUNTS_OFFSET - 1,
                    ERROR_STATUS_RW_READ_ERROR);
  assert_load_fails(tmp_dir, bad_filename, "bad", good,
                    RVT_TEST_LETTER_COUNTS_OFFSET + 5,
                    ERROR_STATUS_RW_READ_ERROR);
  assert_load_fails(tmp_dir, bad_filename, "bad", good, values_offset,
                    ERROR_STATUS_RW_READ_ERROR);
  assert_load_fails(tmp_dir, bad_filename, "bad", good, size - 1,
                    ERROR_STATUS_RW_READ_ERROR);
  assert_load_fails(tmp_dir, bad_filename, "bad", good,
                    size - RVT_TEST_VALUE_BYTES, ERROR_STATUS_RW_READ_ERROR);

  // longer than its racks
  memcpy(bad, good, size);
  memset(bad + size, 0, 4);
  assert_load_fails(tmp_dir, bad_filename, "bad", bad, size + 1,
                    ERROR_STATUS_RW_READ_ERROR);
  assert_load_fails(tmp_dir, bad_filename, "bad", bad,
                    size + RVT_TEST_VALUE_BYTES, ERROR_STATUS_RW_READ_ERROR);

  // no letters, and more than a BitRack holds
  memcpy(bad, good, size);
  bad[RVT_TEST_NUM_LETTERS_OFFSET] = 0;
  assert_load_fails(tmp_dir, bad_filename, "bad", bad, size,
                    ERROR_STATUS_RW_READ_ERROR);
  bad[RVT_TEST_NUM_LETTERS_OFFSET] = BIT_RACK_MAX_ALPHABET_SIZE + 1;
  assert_load_fails(tmp_dir, bad_filename, "bad", bad, size,
                    ERROR_STATUS_RW_READ_ERROR);
  bad[RVT_TEST_NUM_LETTERS_OFFSET] = 0xff;
  assert_load_fails(tmp_dir, bad_filename, "bad", bad, size,
                    ERROR_STATUS_RW_READ_ERROR);

  // a rack count that is not the letter counts'
  memcpy(bad, good, size);
  rvt_test_set_uint32(bad, RVT_TEST_NUM_RACKS_OFFSET, ten_racks - 1);
  assert_load_fails(tmp_dir, bad_filename, "bad", bad, size,
                    ERROR_STATUS_RW_READ_ERROR);
  rvt_test_set_uint32(bad, RVT_TEST_NUM_RACKS_OFFSET, 0xffffffffU);
  assert_load_fails(tmp_dir, bad_filename, "bad", bad, size,
                    ERROR_STATUS_RW_READ_ERROR);

  // letter counts that deal other racks than the file has values for
  memcpy(bad, good, size);
  bad[RVT_TEST_LETTER_COUNTS_OFFSET]++;
  assert_load_fails(tmp_dir, bad_filename, "bad", bad, size,
                    ERROR_STATUS_RW_READ_ERROR);
  memcpy(bad, good, size);
  bad[RVT_TEST_LETTER_COUNTS_OFFSET + 9] = 0;
  assert_load_fails(tmp_dir, bad_filename, "bad", bad, size,
                    ERROR_STATUS_RW_READ_ERROR);

  free(bad);
  free(good);
  free(bad_filename);
  free(good_filename);
  free(lexica_dir);
  test_platform_remove_directory_tree(tmp_dir);
  free(tmp_dir);
}
