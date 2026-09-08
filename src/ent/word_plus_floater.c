#include "word_plus_floater.h"

#include "../compat/endian_conv.h"
#include "../def/board_defs.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "word_info_table.h"
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool wpf_write_uint32s(const uint32_t *values, size_t count,
                              FILE *stream) {
  if (count == 0) {
    return true;
  }
#if IS_LITTLE_ENDIAN
  return fwrite(values, sizeof(uint32_t), count, stream) == count;
#else
  for (size_t index = 0; index < count; index++) {
    const uint32_t value = htole32(values[index]);
    if (fwrite(&value, sizeof(value), 1, stream) != 1) {
      return false;
    }
  }
  return true;
#endif
}

static bool wpf_validate_rows(const WordInfoTable *wit, int length) {
  const uint32_t num_rows = wit->tries[length].num_values;
  const size_t stride = word_plus_floater_cells_per_key(length);
  return num_rows <= INT32_MAX && num_rows <= SIZE_MAX / stride &&
         (size_t)num_rows * stride <= UINT32_MAX / sizeof(uint32_t) &&
         (num_rows == 0 || wit->word_plus_floater[length] != NULL);
}

static bool wpf_write_stream(const WordInfoTable *wit, FILE *stream) {
  const unsigned char magic[8] = {'W', 'P', 'F', 'M', '1', 'L', 'E', 0};
  const uint32_t header[4] = {BOARD_DIM, WPF_MIN_BLOCK_LENGTH,
                              WPF_MAX_BLOCK_LENGTH, 0};
  const uint64_t layout_hash = word_plus_floater_layout_hash(wit);
  const uint32_t fingerprints[4] = {
      (uint32_t)wit->kwg_hash, (uint32_t)(wit->kwg_hash >> 32),
      (uint32_t)layout_hash, (uint32_t)(layout_hash >> 32)};
  if (fwrite(magic, sizeof(magic), 1, stream) != 1 ||
      !wpf_write_uint32s(header, 4, stream) ||
      !wpf_write_uint32s(fingerprints, 4, stream)) {
    return false;
  }
  for (int length = WPF_MIN_BLOCK_LENGTH; length <= WPF_MAX_BLOCK_LENGTH;
       length++) {
    const uint32_t num_values = wit->tries[length].num_values;
    const size_t stride = word_plus_floater_cells_per_key(length);
    const uint32_t section[4] = {(uint32_t)length, num_values, (uint32_t)stride,
                                 0};
    if (!wpf_write_uint32s(section, 4, stream)) {
      return false;
    }
    if (!wpf_write_uint32s(wit->word_plus_floater[length],
                           (size_t)num_values * stride, stream)) {
      return false;
    }
  }
  return true;
}

void word_plus_floater_write_to_file(const WordInfoTable *wit,
                                     const char *filename,
                                     ErrorStack *error_stack) {
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  bool valid = wit->kwg_hash != 0;
  for (int length = WPF_MIN_BLOCK_LENGTH;
       valid && length <= WPF_MAX_BLOCK_LENGTH; length++) {
    valid = wpf_validate_rows(wit, length);
  }
  if (!valid) {
    error_stack_push(
        error_stack, ERROR_STATUS_RW_WRITE_ERROR,
        string_duplicate("cannot write invalid WordPlusFloater rows"));
    return;
  }
  char *temporary_filename = get_formatted_string("%s.tmp.XXXXXX", filename);
  const int descriptor = mkstemp(temporary_filename);
  if (descriptor == -1) {
    error_stack_push(
        error_stack, ERROR_STATUS_RW_FAILED_TO_OPEN_STREAM,
        get_formatted_string(
            "could not create WordPlusFloater output beside %s: %s", filename,
            strerror(errno)));
    free(temporary_filename);
    return;
  }
  FILE *stream = fdopen(descriptor, "wb");
  if (stream == NULL) {
    const int error_number = errno;
    (void)close(descriptor);
    (void)remove(temporary_filename);
    error_stack_push(
        error_stack, ERROR_STATUS_RW_FAILED_TO_OPEN_STREAM,
        get_formatted_string("could not open WordPlusFloater output for %s: %s",
                             filename, strerror(error_number)));
    free(temporary_filename);
    return;
  }
  bool written = wpf_write_stream(wit, stream);
  int error_number = written ? 0 : errno;
  if (fclose(stream) != 0) {
    if (written) {
      error_number = errno;
    }
    written = false;
  }
  if (written && rename(temporary_filename, filename) != 0) {
    error_number = errno;
    written = false;
  }
  if (!written) {
    (void)remove(temporary_filename);
    error_stack_push(
        error_stack, ERROR_STATUS_RW_WRITE_ERROR,
        get_formatted_string("could not write WordPlusFloater output %s: %s",
                             filename,
                             error_number != 0 ? strerror(error_number)
                                               : "unknown I/O failure"));
  }
  free(temporary_filename);
}
