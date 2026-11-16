#include "rack_value_table_test.h"

#include "../src/compat/cfile.h"
#include "../src/def/letter_distribution_defs.h"
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
  RVT_TEST_RACKS = 5000,
  RVT_TEST_KLV_HASH_LOW = 0x89abcdef,
  RVT_TEST_KLV_HASH_HIGH = 0x01234567,
  // where the fields of the file's header start
  RVT_TEST_VERSION_OFFSET = 0,
  RVT_TEST_RACK_SIZE_OFFSET = 1,
  RVT_TEST_NUM_BUCKETS_OFFSET = 4,
  RVT_TEST_NUM_ENTRIES_OFFSET = 8,
  RVT_TEST_BUCKET_STARTS_OFFSET = 20,
};

// A different BitRack for each index below 4^8: the index's base-4 digits are
// the counts of the first eight letters after the blank. The table does not
// look at how many tiles a key holds.
static BitRack rvt_test_rack(uint32_t index) {
  BitRack bit_rack = bit_rack_create_empty();
  for (int ml = 1; ml <= 8; ml++) {
    for (uint32_t copies = index % 4; copies > 0; copies--) {
      bit_rack_add_letter(&bit_rack, (MachineLetter)ml);
    }
    index /= 4;
  }
  assert(index == 0);
  return bit_rack;
}

static Equity rvt_test_value(uint32_t index) {
  return (Equity)((int)(index * 7919U % 200001U) - 100000);
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

static void assert_table_holds_racks(const RackValueTable *rvt,
                                     uint32_t number_of_racks) {
  assert(rack_value_table_get_number_of_racks(rvt) == number_of_racks);
  assert(rack_value_table_get_klv_hash(rvt) ==
         (((uint64_t)RVT_TEST_KLV_HASH_HIGH << 32) | RVT_TEST_KLV_HASH_LOW));
  for (uint32_t rack_idx = 0; rack_idx < number_of_racks; rack_idx++) {
    const BitRack bit_rack = rvt_test_rack(rack_idx);
    Equity value = 0;
    const bool found = rack_value_table_lookup(rvt, &bit_rack, &value);
    assert(found);
    assert(value == rvt_test_value(rack_idx));
  }
  // racks the table was not given
  for (uint32_t rack_idx = number_of_racks; rack_idx < number_of_racks + 1000;
       rack_idx++) {
    const BitRack bit_rack = rvt_test_rack(rack_idx);
    Equity value = 12345;
    const bool found = rack_value_table_lookup(rvt, &bit_rack, &value);
    assert(!found);
    assert(value == 12345);
  }
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

static void test_round_trip(const char *data_paths, uint32_t number_of_racks,
                            const char *name) {
  BitRack *bit_racks = malloc_or_die(sizeof(BitRack) * (number_of_racks + 1));
  Equity *values = malloc_or_die(sizeof(Equity) * (number_of_racks + 1));
  for (uint32_t rack_idx = 0; rack_idx < number_of_racks; rack_idx++) {
    bit_racks[rack_idx] = rvt_test_rack(rack_idx);
    values[rack_idx] = rvt_test_value(rack_idx);
  }
  RackValueTable *built = rack_value_table_create_from_values(
      bit_racks, values, number_of_racks,
      ((uint64_t)RVT_TEST_KLV_HASH_HIGH << 32) | RVT_TEST_KLV_HASH_LOW);
  assert_table_holds_racks(built, number_of_racks);
  ErrorStack *error_stack = error_stack_create();
  rack_value_table_write(built, data_paths, name, error_stack);
  assert(error_stack_is_empty(error_stack));
  rack_value_table_destroy(built);
  for (int use_mmap = 0; use_mmap < 2; use_mmap++) {
    RackValueTable *loaded =
        rack_value_table_create(data_paths, name, use_mmap == 1, error_stack);
    assert(error_stack_is_empty(error_stack));
    assert_strings_equal(rack_value_table_get_name(loaded), name);
    assert_table_holds_racks(loaded, number_of_racks);
    rack_value_table_destroy(loaded);
  }
  error_stack_destroy(error_stack);
  free(values);
  free(bit_racks);
}

void test_rack_value_table(void) {
  char *tmp_dir = test_platform_make_temporary_directory("magpie_rvt_");
  char *lexica_dir = get_formatted_string("%s/lexica", tmp_dir);
  assert(cfile_mkdir(lexica_dir) == 0);

  test_round_trip(tmp_dir, RVT_TEST_RACKS, "many");
  test_round_trip(tmp_dir, 1, "one");
  test_round_trip(tmp_dir, 0, "none");

  ErrorStack *error_stack = error_stack_create();
  const RackValueTable *missing =
      rack_value_table_create(tmp_dir, "missing", false, error_stack);
  assert(!missing);
  assert(!error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);

  // Files that must not load.
  char *good_filename = get_formatted_string("%s/many.rvt", lexica_dir);
  char *bad_filename = get_formatted_string("%s/bad.rvt", lexica_dir);
  size_t size = 0;
  uint8_t *good = rvt_test_read_file(good_filename, &size);
  uint8_t *bad = malloc_or_die(size);
  const uint32_t num_buckets =
      rvt_test_get_uint32(good, RVT_TEST_NUM_BUCKETS_OFFSET);
  assert(size == RVT_TEST_BUCKET_STARTS_OFFSET + ((size_t)num_buckets + 1) * 4 +
                     (size_t)RVT_TEST_RACKS * 20);

  // the file as written, under the other name: the control for the rest
  rvt_test_write_file(bad_filename, good, size);
  error_stack = error_stack_create();
  RackValueTable *copy =
      rack_value_table_create(tmp_dir, "bad", false, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert_table_holds_racks(copy, RVT_TEST_RACKS);
  rack_value_table_destroy(copy);
  error_stack_destroy(error_stack);

  memcpy(bad, good, size);
  bad[RVT_TEST_VERSION_OFFSET]++;
  assert_load_fails(tmp_dir, bad_filename, "bad", bad, size,
                    ERROR_STATUS_WMP_UNSUPPORTED_VERSION);

  memcpy(bad, good, size);
  bad[RVT_TEST_RACK_SIZE_OFFSET]++;
  assert_load_fails(tmp_dir, bad_filename, "bad", bad, size,
                    ERROR_STATUS_WMP_INCOMPATIBLE_BOARD_DIM);

  // cut short: inside the header, the bucket starts, and the last entry
  assert_load_fails(tmp_dir, bad_filename, "bad", good, 0,
                    ERROR_STATUS_RW_READ_ERROR);
  assert_load_fails(tmp_dir, bad_filename, "bad", good,
                    RVT_TEST_BUCKET_STARTS_OFFSET - 1,
                    ERROR_STATUS_RW_READ_ERROR);
  assert_load_fails(tmp_dir, bad_filename, "bad", good,
                    RVT_TEST_BUCKET_STARTS_OFFSET + 10,
                    ERROR_STATUS_RW_READ_ERROR);
  assert_load_fails(tmp_dir, bad_filename, "bad", good, size - 1,
                    ERROR_STATUS_RW_READ_ERROR);

  // a bucket count that is not a power of 2, and one of zero
  memcpy(bad, good, size);
  rvt_test_set_uint32(bad, RVT_TEST_NUM_BUCKETS_OFFSET, num_buckets - 1);
  assert_load_fails(tmp_dir, bad_filename, "bad", bad, size,
                    ERROR_STATUS_RW_READ_ERROR);
  rvt_test_set_uint32(bad, RVT_TEST_NUM_BUCKETS_OFFSET, 0);
  assert_load_fails(tmp_dir, bad_filename, "bad", bad, size,
                    ERROR_STATUS_RW_READ_ERROR);

  // counts too large for the file
  memcpy(bad, good, size);
  rvt_test_set_uint32(bad, RVT_TEST_NUM_BUCKETS_OFFSET, 0x80000000U);
  assert_load_fails(tmp_dir, bad_filename, "bad", bad, size,
                    ERROR_STATUS_RW_READ_ERROR);
  memcpy(bad, good, size);
  rvt_test_set_uint32(bad, RVT_TEST_NUM_ENTRIES_OFFSET, 0xffffffffU);
  assert_load_fails(tmp_dir, bad_filename, "bad", bad, size,
                    ERROR_STATUS_RW_READ_ERROR);

  // bucket starts that go down, and that run past the entries
  memcpy(bad, good, size);
  rvt_test_set_uint32(bad, RVT_TEST_BUCKET_STARTS_OFFSET + 4 * 10,
                      RVT_TEST_RACKS);
  assert_load_fails(tmp_dir, bad_filename, "bad", bad, size,
                    ERROR_STATUS_RW_READ_ERROR);
  memcpy(bad, good, size);
  rvt_test_set_uint32(bad,
                      RVT_TEST_BUCKET_STARTS_OFFSET + 4 * (size_t)num_buckets,
                      RVT_TEST_RACKS + 1);
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
