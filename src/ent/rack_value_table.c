#include "rack_value_table.h"

#include "../compat/cmap.h"
#include "../compat/endian_conv.h"
#include "../def/bit_rack_defs.h"
#include "../def/rack_defs.h"
#include "../util/fileproxy.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "data_filepaths.h"
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  // The bytes a stream is read by, when the table is not mapped.
  RACK_VALUE_TABLE_READ_CHUNK_BYTES = 1 << 20,
};

// Fills the table's rank offsets from its letter counts and returns the
// number of racks the distribution can deal, or UINT64_MAX if that does not
// fit the file's 32-bit count.
static uint64_t rvt_fill_rank_offsets(RackValueTable *rvt) {
  // ways[tiles]: the racks of tiles tiles that the letters after the current
  // one can deal. After the last letter, only the empty rack.
  uint64_t ways[(RACK_SIZE) + 1] = {0};
  ways[0] = 1;
  for (int ml = BIT_RACK_MAX_ALPHABET_SIZE - 1; ml >= 0; ml--) {
    uint64_t ways_with_letter[(RACK_SIZE) + 1] = {0};
    for (int tiles = 0; tiles <= (RACK_SIZE); tiles++) {
      uint64_t racks_before = 0;
      for (int count = 0; count <= (RACK_SIZE); count++) {
        const bool possible = count <= tiles && count <= rvt->letter_counts[ml];
        rvt->rank_offsets[ml][tiles][count] =
            possible ? (uint32_t)racks_before : UINT32_MAX;
        if (possible) {
          racks_before += ways[tiles - count];
        }
      }
      if (racks_before >= UINT32_MAX) {
        return UINT64_MAX;
      }
      ways_with_letter[tiles] = racks_before;
    }
    memcpy(ways, ways_with_letter, sizeof(ways));
  }
  return ways[(RACK_SIZE)];
}

RackValueTable *rack_value_table_create_unvalued(const uint8_t *letter_counts,
                                                 int num_letters,
                                                 uint64_t klv_hash) {
  if (num_letters < 1 || num_letters > BIT_RACK_MAX_ALPHABET_SIZE) {
    return NULL;
  }
  RackValueTable *rvt = calloc_or_die(1, sizeof(RackValueTable));
  rvt->version = RACK_VALUE_TABLE_VERSION;
  rvt->klv_hash = klv_hash;
  rvt->num_letters = (uint8_t)num_letters;
  memcpy(rvt->letter_counts, letter_counts, (size_t)num_letters);
  // The alphabet and rack size limits keep the count far inside 32 bits.
  rvt->num_racks = (uint32_t)rvt_fill_rank_offsets(rvt);
  const size_t capacity = rvt->num_racks > 0 ? rvt->num_racks : 1;
  rvt->values = malloc_or_die(capacity * RACK_VALUE_TABLE_VALUE_BYTES);
  for (uint32_t rack_idx = 0; rack_idx < rvt->num_racks; rack_idx++) {
    rack_value_table_write_stored(rvt, rack_idx, RACK_VALUE_TABLE_UNVALUED);
  }
  return rvt;
}

uint32_t
rack_value_table_get_number_of_valued_racks(const RackValueTable *rvt) {
  uint32_t number_of_valued_racks = 0;
  for (uint32_t rack_idx = 0; rack_idx < rvt->num_racks; rack_idx++) {
    if (rack_value_table_read_stored(rvt, rack_idx) !=
        RACK_VALUE_TABLE_UNVALUED) {
      number_of_valued_racks++;
    }
  }
  return number_of_valued_racks;
}

void rack_value_table_destroy(RackValueTable *rvt) {
  if (!rvt) {
    return;
  }
  free(rvt->name);
  if (rvt->is_mmapped) {
    cmap_close(&rvt->map);
  } else {
    free(rvt->values);
  }
  free(rvt);
}

static bool rvt_fwrite(const void *ptr, size_t size, size_t nmemb,
                       FILE *stream) {
  return fwrite(ptr, size, nmemb, stream) == nmemb;
}

static bool rvt_write_uint32(uint32_t value, FILE *stream) {
  const uint32_t le = htole32(value);
  return rvt_fwrite(&le, sizeof(le), 1, stream);
}

static size_t rvt_padded_letter_bytes(size_t num_letters) {
  return (num_letters + 3) / 4 * 4;
}

static bool rvt_write_body(const RackValueTable *rvt, FILE *stream) {
  const uint8_t leading[4] = {rvt->version, (uint8_t)(RACK_SIZE),
                              rvt->num_letters, 0};
  // zero past num_letters, so the padding is written with the counts
  bool written =
      rvt_fwrite(leading, sizeof(uint8_t), sizeof(leading), stream) &&
      rvt_write_uint32(rvt->num_racks, stream) &&
      rvt_write_uint32((uint32_t)rvt->klv_hash, stream) &&
      rvt_write_uint32((uint32_t)(rvt->klv_hash >> 32), stream) &&
      rvt_fwrite(rvt->letter_counts, sizeof(uint8_t),
                 rvt_padded_letter_bytes(rvt->num_letters), stream);
  return written && rvt_fwrite(rvt->values, RACK_VALUE_TABLE_VALUE_BYTES,
                               rvt->num_racks, stream);
}

void rack_value_table_write(const RackValueTable *rvt, const char *data_paths,
                            const char *name, ErrorStack *error_stack) {
  char *filename = data_filepaths_get_writable_filename(
      data_paths, name, DATA_FILEPATH_TYPE_RACK_VALUE_TABLE, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    free(filename);
    return;
  }
  // Written beside the final name and renamed into place, so that no reader
  // sees part of a table under its name.
  char *temporary = temporary_sibling(filename);
  FILE *stream = fopen_exclusive_safe(temporary, "wb", error_stack);
  if (!error_stack_is_empty(error_stack)) {
    free(temporary);
    free(filename);
    return;
  }
  errno = 0;
  bool written = rvt_write_body(rvt, stream);
  int error_number = written ? 0 : errno;
  // A full disk often shows only here, when the buffered tail is flushed.
  if (fclose(stream) != 0) {
    if (written) {
      error_number = errno;
    }
    written = false;
  }
  if (written) {
    rename_into_place(temporary, filename, "rack value table", error_stack);
  } else {
    (void)remove(temporary);
    error_stack_push(error_stack, ERROR_STATUS_RW_WRITE_ERROR,
                     get_formatted_string(
                         "could not write rack value table %s: %s", filename,
                         error_number != 0 ? strerror(error_number)
                                           : "unknown I/O failure"));
  }
  free(temporary);
  free(filename);
}

static uint32_t rvt_read_uint32(const uint8_t *data) {
  uint32_t value;
  memcpy(&value, data, sizeof(value));
  return le32toh(value);
}

// Reads the table from the size bytes at data. With map_in_place, the values
// are left where they lie in data, which must outlive the table; otherwise
// they are copied out of it.
static void rvt_parse(RackValueTable *rvt, const uint8_t *data, size_t size,
                      bool map_in_place, const char *filename,
                      ErrorStack *error_stack) {
  if (size < RACK_VALUE_TABLE_HEADER_BYTES) {
    error_stack_push(
        error_stack, ERROR_STATUS_RW_READ_ERROR,
        get_formatted_string("rack value table too small for a header: %s",
                             filename));
    return;
  }
  if (data[0] != RACK_VALUE_TABLE_VERSION) {
    error_stack_push(
        error_stack, ERROR_STATUS_WMP_UNSUPPORTED_VERSION,
        get_formatted_string(
            "detected rack value table version %d but only %d is supported: "
            "%s",
            data[0], RACK_VALUE_TABLE_VERSION, filename));
    return;
  }
  if (data[1] != (RACK_SIZE)) {
    error_stack_push(
        error_stack, ERROR_STATUS_WMP_INCOMPATIBLE_BOARD_DIM,
        get_formatted_string("rack value table rack size %d does not match "
                             "build rack size %d: %s",
                             data[1], (RACK_SIZE), filename));
    return;
  }
  rvt->version = data[0];
  rvt->num_letters = data[2];
  rvt->num_racks = rvt_read_uint32(data + 4);
  rvt->klv_hash = (uint64_t)rvt_read_uint32(data + 8) |
                  ((uint64_t)rvt_read_uint32(data + 12) << 32);

  // The file must be exactly its header, its letter counts and one value for
  // each rack those counts can deal, or a lookup would read past the values.
  const size_t letter_bytes = rvt_padded_letter_bytes(rvt->num_letters);
  bool valid = rvt->num_letters >= 1 &&
               rvt->num_letters <= BIT_RACK_MAX_ALPHABET_SIZE &&
               size - RACK_VALUE_TABLE_HEADER_BYTES >= letter_bytes;
  if (valid) {
    memcpy(rvt->letter_counts, data + RACK_VALUE_TABLE_HEADER_BYTES,
           rvt->num_letters);
    const size_t values_bytes =
        size - RACK_VALUE_TABLE_HEADER_BYTES - letter_bytes;
    valid = rvt_fill_rank_offsets(rvt) == rvt->num_racks &&
            values_bytes % RACK_VALUE_TABLE_VALUE_BYTES == 0 &&
            values_bytes / RACK_VALUE_TABLE_VALUE_BYTES == rvt->num_racks;
  }
  if (!valid) {
    error_stack_push(
        error_stack, ERROR_STATUS_RW_READ_ERROR,
        get_formatted_string("rack value table truncated or corrupt: %s",
                             filename));
    return;
  }
  const uint8_t *values_data =
      data + RACK_VALUE_TABLE_HEADER_BYTES + letter_bytes;
  if (map_in_place) {
    // The values are read a byte at a time, so they need no alignment and
    // no byte order. A mapped table is never written to.
    rvt->values = (uint8_t *)values_data;
    return;
  }
  const size_t values_size =
      (size_t)rvt->num_racks * RACK_VALUE_TABLE_VALUE_BYTES;
  rvt->values = malloc_or_die(values_size > 0 ? values_size : 1);
  memcpy(rvt->values, values_data, values_size);
}

static void rvt_load_mapped(RackValueTable *rvt, const char *filename,
                            ErrorStack *error_stack) {
  CMap map;
  // Lookups land anywhere in the values, so readahead would only waste I/O.
  if (!cmap_open(&map, filename, true)) {
    error_stack_push(
        error_stack,
        errno == ENOENT ? ERROR_STATUS_FILEPATH_FILE_NOT_FOUND
                        : ERROR_STATUS_RW_READ_ERROR,
        get_formatted_string("could not map rack value table %s: %s", filename,
                             strerror(errno)));
    return;
  }
  rvt_parse(rvt, (const uint8_t *)map.data, map.size, true, filename,
            error_stack);
  if (!error_stack_is_empty(error_stack)) {
    cmap_close(&map);
    return;
  }
  rvt->is_mmapped = true;
  rvt->map = map;
}

static void rvt_load_read(RackValueTable *rvt, const char *filename,
                          ErrorStack *error_stack) {
  FILE *stream = stream_from_filename(filename, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  size_t capacity = RACK_VALUE_TABLE_READ_CHUNK_BYTES;
  size_t size = 0;
  uint8_t *data = malloc_or_die(capacity);
  while (true) {
    const size_t bytes_read = fread(data + size, 1, capacity - size, stream);
    size += bytes_read;
    if (size < capacity) {
      break;
    }
    capacity *= 2;
    data = realloc_or_die(data, capacity);
  }
  const bool read_failed = ferror(stream) != 0;
  fclose_or_die(stream);
  if (read_failed) {
    error_stack_push(
        error_stack, ERROR_STATUS_RW_READ_ERROR,
        get_formatted_string("could not read rack value table: %s", filename));
  } else {
    rvt_parse(rvt, data, size, false, filename, error_stack);
  }
  free(data);
}

RackValueTable *rack_value_table_create(const char *data_paths,
                                        const char *name, bool use_mmap,
                                        ErrorStack *error_stack) {
  char *filename = data_filepaths_get_readable_filename(
      data_paths, name, DATA_FILEPATH_TYPE_RACK_VALUE_TABLE, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    free(filename);
    return NULL;
  }
  RackValueTable *rvt = calloc_or_die(1, sizeof(RackValueTable));
  if (use_mmap) {
    rvt_load_mapped(rvt, filename, error_stack);
  } else {
    rvt_load_read(rvt, filename, error_stack);
  }
  free(filename);
  if (!error_stack_is_empty(error_stack)) {
    // a failed load leaves the table holding nothing
    free(rvt);
    return NULL;
  }
  rvt->name = string_duplicate(name);
  return rvt;
}
