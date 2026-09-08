#include "word_info_table.h"

#include "../compat/endian_conv.h"
#include "../def/board_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../util/fileproxy.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "data_filepaths.h"
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// v3: version, dimension, two zero bytes, KWG hash, ordinary trie sections.
// v4: byte 2 is flags (bit 0: positional masks), byte 3 remains zero. The
// ordinary sections are unchanged. If flagged, append every length-2..4
// positional row in terminal-ID order, with sizes derived from those tries.
// All integers are little-endian; there is no second identity or key layout.
static bool wit_write_uint32s(const uint32_t *values, size_t count,
                              FILE *stream) {
  if (count == 0) {
    return true;
  }
#if IS_LITTLE_ENDIAN
  return fwrite(values, sizeof(uint32_t), count, stream) == count;
#else
  for (size_t index = 0; index < count; index++) {
    const uint32_t value = values[index];
    const uint8_t bytes[4] = {(uint8_t)value, (uint8_t)(value >> 8),
                              (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
    if (fwrite(bytes, sizeof(bytes), 1, stream) != 1) {
      return false;
    }
  }
  return true;
#endif
}

static bool wit_read_uint32s(uint32_t *values, size_t count, FILE *stream) {
  if (count != 0 && fread(values, sizeof(uint32_t), count, stream) != count) {
    return false;
  }
#if !IS_LITTLE_ENDIAN
  for (size_t index = 0; index < count; index++) {
    const uint32_t value = values[index];
    values[index] = (value >> 24) | ((value >> 8) & 0xff00U) |
                    ((value << 8) & 0xff0000U) | (value << 24);
  }
#endif
  return true;
}

static void wit_clear(WordInfoTable *wit) {
  for (int length = 0; length <= BOARD_DIM; length++) {
    WitTrie *trie = &wit->tries[length];
    free(trie->node_tile);
    free(trie->node_last);
    free(trie->node_child);
    free(trie->node_value);
    free(trie->values);
    free(wit->word_plus_floater[length]);
  }
  free(wit->name);
  memset(wit, 0, sizeof(*wit));
}

static bool wit_validate_nodes(const WitTrie *trie, uint32_t first_node,
                               int depth, int length, uint8_t *seen_nodes,
                               uint8_t *seen_values) {
  MachineLetter previous_tile = 0;
  for (uint32_t node = first_node; node != 0; node++) {
    if (node >= trie->num_nodes || seen_nodes[node] || depth > length) {
      return false;
    }
    seen_nodes[node] = 1;
    const MachineLetter tile = trie->node_tile[node];
    if (tile <= previous_tile || tile >= MAX_ALPHABET_SIZE ||
        trie->node_last[node] > 1) {
      return false;
    }
    previous_tile = tile;
    const int32_t value = trie->node_value[node];
    if (value < -1 || (value >= 0 && (depth != length ||
                                      (uint32_t)value >= trie->num_values ||
                                      seen_values[value]))) {
      return false;
    }
    if (value >= 0) {
      seen_values[value] = 1;
    }
    if (trie->node_child[node] != 0 &&
        !wit_validate_nodes(trie, trie->node_child[node], depth + 1, length,
                            seen_nodes, seen_values)) {
      return false;
    }
    if (trie->node_last[node]) {
      return true;
    }
  }
  return true;
}

static bool wit_validate_trie(const WitTrie *trie, int length) {
  if (trie->num_nodes == 0 || trie->root >= trie->num_nodes ||
      trie->num_values > INT32_MAX || trie->num_values >= trie->num_nodes ||
      trie->node_tile == NULL || trie->node_last == NULL ||
      trie->node_child == NULL || trie->node_value == NULL ||
      (trie->num_values != 0 && trie->values == NULL)) {
    return false;
  }
  uint8_t *seen_nodes = calloc_or_die(trie->num_nodes, 1);
  uint8_t *seen_values =
      trie->num_values != 0 ? calloc_or_die(trie->num_values, 1) : NULL;
  bool valid =
      wit_validate_nodes(trie, trie->root, 1, length, seen_nodes, seen_values);
  for (uint32_t value = 0; valid && value < trie->num_values; value++) {
    valid = seen_values[value] != 0;
  }
  for (uint32_t node = 1; valid && node < trie->num_nodes; node++) {
    valid = seen_nodes[node] != 0;
  }
  free(seen_values);
  free(seen_nodes);
  return valid;
}

static bool wit_positional_alphabet_supported(const WordInfoTable *wit) {
  for (int length = 1; length <= BOARD_DIM; length++) {
    const WitTrie *trie = &wit->tries[length];
    for (uint32_t node = 1; node < trie->num_nodes; node++) {
      if (trie->node_tile[node] > WPF_ALPHABET_SIZE) {
        return false;
      }
    }
  }
  return true;
}

static bool wit_write_stream(const WordInfoTable *wit, bool positional,
                             FILE *stream) {
  const uint8_t header[4] = {WIT_VERSION, BOARD_DIM,
                             positional ? WIT_FLAG_WORD_PLUS_FLOATER : 0, 0};
  const uint32_t fingerprint[2] = {(uint32_t)wit->kwg_hash,
                                   (uint32_t)(wit->kwg_hash >> 32)};
  if (fwrite(header, sizeof(header), 1, stream) != 1 ||
      !wit_write_uint32s(fingerprint, 2, stream)) {
    return false;
  }
  for (int length = 1; length <= BOARD_DIM; length++) {
    const WitTrie *trie = &wit->tries[length];
    const uint32_t section[3] = {trie->num_nodes, trie->root, trie->num_values};
    if (!wit_write_uint32s(section, 3, stream) ||
        fwrite(trie->node_tile, 1, trie->num_nodes, stream) !=
            trie->num_nodes ||
        fwrite(trie->node_last, 1, trie->num_nodes, stream) !=
            trie->num_nodes ||
        !wit_write_uint32s(trie->node_child, trie->num_nodes, stream) ||
        !wit_write_uint32s((const uint32_t *)trie->node_value, trie->num_nodes,
                           stream) ||
        !wit_write_uint32s(
            trie->values, (size_t)trie->num_values * wit_stride_for_len(length),
            stream)) {
      return false;
    }
  }
  for (int length = WPF_MIN_BLOCK_LENGTH;
       positional && length <= WPF_MAX_BLOCK_LENGTH; length++) {
    if (!wit_write_uint32s(wit->word_plus_floater[length],
                           wit->tries[length].num_values *
                               word_plus_floater_cells_per_key(length),
                           stream)) {
      return false;
    }
  }
  return true;
}

void word_info_table_write_to_file(const WordInfoTable *wit,
                                   const char *filename,
                                   ErrorStack *error_stack) {
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  bool positional = false;
  for (int length = WPF_MIN_BLOCK_LENGTH; length <= WPF_MAX_BLOCK_LENGTH;
       length++) {
    positional |= wit->word_plus_floater[length] != NULL;
  }
  bool valid = true;
  for (int length = 1; valid && length <= BOARD_DIM; length++) {
    valid = wit_validate_trie(&wit->tries[length], length);
  }
  valid = valid && (!positional || (wit->kwg_hash != 0 &&
                                    wit_positional_alphabet_supported(wit)));
  for (int length = WPF_MIN_BLOCK_LENGTH;
       valid && positional && length <= WPF_MAX_BLOCK_LENGTH; length++) {
    const uint32_t rows = wit->tries[length].num_values;
    const size_t stride = word_plus_floater_cells_per_key(length);
    valid = rows <= SIZE_MAX / stride &&
            ((size_t)rows * stride) <= SIZE_MAX / sizeof(uint32_t) &&
            (rows == 0 || wit->word_plus_floater[length] != NULL);
  }
  if (!valid) {
    error_stack_push(error_stack, ERROR_STATUS_RW_WRITE_ERROR,
                     string_duplicate("cannot write invalid word info table"));
    return;
  }
  char *temporary_filename = get_formatted_string("%s.tmp.XXXXXX", filename);
  const int descriptor = mkstemp(temporary_filename);
  if (descriptor == -1) {
    error_stack_push(
        error_stack, ERROR_STATUS_RW_FAILED_TO_OPEN_STREAM,
        get_formatted_string("could not create word info table beside %s: %s",
                             filename, strerror(errno)));
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
        get_formatted_string("could not open word info table output %s: %s",
                             filename, strerror(error_number)));
    free(temporary_filename);
    return;
  }
  bool written = wit_write_stream(wit, positional, stream);
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
        get_formatted_string("could not write word info table %s: %s", filename,
                             error_number != 0 ? strerror(error_number)
                                               : "unknown I/O failure"));
  }
  free(temporary_filename);
}

static bool wit_read_trie(WitTrie *trie, int length, size_t *remaining,
                          FILE *stream) {
  uint32_t section[3];
  if (*remaining < sizeof(section) || !wit_read_uint32s(section, 3, stream)) {
    return false;
  }
  *remaining -= sizeof(section);
  const uint32_t nodes = section[0];
  const uint32_t values = section[2];
  const uint64_t size = ((uint64_t)nodes * 10) +
                        ((uint64_t)values * wit_stride_for_len(length) * 4);
  if (nodes == 0 || section[1] >= nodes || values >= nodes ||
      values > INT32_MAX || size > *remaining) {
    return false;
  }
  *remaining -= (size_t)size;
  trie->num_nodes = nodes;
  trie->root = section[1];
  trie->num_values = values;
  trie->node_tile = malloc_or_die(nodes);
  trie->node_last = malloc_or_die(nodes);
  trie->node_child = malloc_or_die((size_t)nodes * sizeof(uint32_t));
  trie->node_value = malloc_or_die((size_t)nodes * sizeof(int32_t));
  const size_t count = (size_t)values * wit_stride_for_len(length);
  trie->values = count != 0 ? malloc_or_die(count * sizeof(uint32_t)) : NULL;
  return fread(trie->node_tile, 1, nodes, stream) == nodes &&
         fread(trie->node_last, 1, nodes, stream) == nodes &&
         wit_read_uint32s(trie->node_child, nodes, stream) &&
         wit_read_uint32s((uint32_t *)trie->node_value, nodes, stream) &&
         wit_read_uint32s(trie->values, count, stream) &&
         wit_validate_trie(trie, length);
}

void word_info_table_load(WordInfoTable *wit, const char *name,
                          const char *filename, ErrorStack *error_stack) {
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  wit_clear(wit);
  FILE *stream = stream_from_filename(filename, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  const char *message = "invalid or truncated word info table";
  error_code_t status = ERROR_STATUS_RW_READ_ERROR;
  if (fseek(stream, 0, SEEK_END) != 0) {
    goto invalid;
  }
  const long file_size = ftell(stream);
  if (file_size < 12 || fseek(stream, 0, SEEK_SET) != 0) {
    goto invalid;
  }
  uint8_t header[4];
  uint32_t fingerprint[2];
  if (fread(header, sizeof(header), 1, stream) != 1 ||
      !wit_read_uint32s(fingerprint, 2, stream)) {
    goto invalid;
  }
  if (header[0] < WIT_EARLIEST_SUPPORTED_VERSION || header[0] > WIT_VERSION) {
    message = "unsupported word info table version";
    status = ERROR_STATUS_WMP_UNSUPPORTED_VERSION;
    goto invalid;
  }
  if (header[1] != BOARD_DIM) {
    message = "word info table board dimension does not match this build";
    status = ERROR_STATUS_WMP_INCOMPATIBLE_BOARD_DIM;
    goto invalid;
  }
  if (header[3] != 0 || (header[2] & ~WIT_FLAG_WORD_PLUS_FLOATER) != 0 ||
      (header[0] == 3 && header[2] != 0)) {
    message = "unsupported word info table flags";
    goto invalid;
  }
  wit->version = header[0];
  wit->kwg_hash = (uint64_t)fingerprint[0] | ((uint64_t)fingerprint[1] << 32);
  size_t remaining = (size_t)file_size - 12;
  for (int length = 1; length <= BOARD_DIM; length++) {
    if (!wit_read_trie(&wit->tries[length], length, &remaining, stream)) {
      goto invalid;
    }
  }
  const bool positional = (header[2] & WIT_FLAG_WORD_PLUS_FLOATER) != 0;
  if (positional &&
      (wit->kwg_hash == 0 || !wit_positional_alphabet_supported(wit))) {
    message = "unsupported positional word info table";
    goto invalid;
  }
  for (int length = WPF_MIN_BLOCK_LENGTH;
       positional && length <= WPF_MAX_BLOCK_LENGTH; length++) {
    const uint64_t bytes = (uint64_t)wit->tries[length].num_values *
                           word_plus_floater_cells_per_key(length) *
                           sizeof(uint32_t);
    if (bytes > remaining) {
      message = "truncated positional word info table";
      goto invalid;
    }
    remaining -= (size_t)bytes;
    const size_t count = (size_t)bytes / sizeof(uint32_t);
    wit->word_plus_floater[length] =
        count != 0 ? malloc_or_die((size_t)bytes) : NULL;
    if (!wit_read_uint32s(wit->word_plus_floater[length], count, stream)) {
      goto invalid;
    }
  }
  if (remaining != 0 || fgetc(stream) != EOF || ferror(stream)) {
    message = "unexpected data after word info table";
    goto invalid;
  }
  fclose_or_die(stream);
  wit->name = string_duplicate(name);
  return;

invalid:
  fclose_or_die(stream);
  wit_clear(wit);
  error_stack_push(error_stack, status, string_duplicate(message));
}

WordInfoTable *word_info_table_create(const char *data_paths,
                                      const char *wit_name,
                                      ErrorStack *error_stack) {
  char *filename = data_filepaths_get_readable_filename(
      data_paths, wit_name, DATA_FILEPATH_TYPE_WORD_INFO_TABLE, error_stack);
  WordInfoTable *wit = NULL;
  if (error_stack_is_empty(error_stack)) {
    wit = calloc_or_die(1, sizeof(WordInfoTable));
    word_info_table_load(wit, wit_name, filename, error_stack);
  }
  free(filename);
  if (!error_stack_is_empty(error_stack)) {
    word_info_table_destroy(wit);
    wit = NULL;
  }
  return wit;
}
