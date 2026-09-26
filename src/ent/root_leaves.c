#include "root_leaves.h"

#include "../compat/endian_io.h"
#include "../def/bit_rack_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "data_filepaths.h"
#include "equity.h"
#include "rack.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  ROOT_LEAVES_EARLIEST_VERSION = 1,
  ROOT_LEAVES_LATEST_VERSION = 2,
  ROOT_LEAVES_HEADER_FIELDS = 4,
  ROOT_LEAVES_MAGIC_LENGTH = 8,
};

static const uint8_t root_leaves_magic[ROOT_LEAVES_MAGIC_LENGTH] = {
    'M', 'A', 'G', 'K', 'L', 'V', '3', '\0'};

struct RootLeaves {
  char *name;
  int alphabet_size;
  int num_pool_bins;
  uint16_t pool_bin_upper_bounds[ROOT_LEAVES_MAX_POOL_SIZE_BINS];
  // [pool_bin][draw_count][held_tile]
  Equity *biases;
  // [draw_count][held_tile][unseen_tile]
  Equity *weights;
};

const char *root_leaves_get_name(const RootLeaves *root_leaves) {
  return root_leaves->name;
}

int root_leaves_get_alphabet_size(const RootLeaves *root_leaves) {
  return root_leaves->alphabet_size;
}

void root_leaves_destroy(RootLeaves *root_leaves) {
  if (!root_leaves) {
    return;
  }
  free(root_leaves->name);
  free(root_leaves->biases);
  free(root_leaves->weights);
  free(root_leaves);
}

// Skips the KLV2 body: the KWG node count and nodes, then the leave count and
// the leave values.
static bool root_leaves_skip_klv2_body(FILE *stream) {
  uint32_t kwg_size;
  if (!fread_le_uint32s(&kwg_size, 1, stream) ||
      fseek(stream, (long)kwg_size * (long)sizeof(uint32_t), SEEK_CUR) != 0) {
    return false;
  }
  uint32_t number_of_leaves;
  return fread_le_uint32s(&number_of_leaves, 1, stream) &&
         fseek(stream, (long)number_of_leaves * (long)sizeof(float),
               SEEK_CUR) == 0;
}

// Reads count float32 values into equities, rejecting non-finite ones.
static bool root_leaves_read_equities(FILE *stream, size_t count,
                                      Equity *equities) {
  float *floats = malloc_or_die(count * sizeof(float));
  bool valid = fread_le_floats(floats, count, stream);
  for (size_t value_idx = 0; valid && value_idx < count; value_idx++) {
    const double value = (double)floats[value_idx];
    if (!isfinite(value)) {
      valid = false;
    } else {
      equities[value_idx] = double_to_equity(value);
    }
  }
  free(floats);
  return valid;
}

static bool root_leaves_read_trailer(FILE *stream, RootLeaves *root_leaves) {
  uint8_t magic[ROOT_LEAVES_MAGIC_LENGTH];
  uint32_t header[ROOT_LEAVES_HEADER_FIELDS];
  if (fread(magic, sizeof(magic), 1, stream) != 1 ||
      memcmp(magic, root_leaves_magic, sizeof(magic)) != 0 ||
      !fread_le_uint32s(header, ROOT_LEAVES_HEADER_FIELDS, stream)) {
    return false;
  }
  const uint32_t version = header[0];
  const uint32_t alphabet_size = header[1];
  const uint32_t draw_count_heads = header[2];
  const uint32_t num_pool_bins = header[3];
  if (version < ROOT_LEAVES_EARLIEST_VERSION ||
      version > ROOT_LEAVES_LATEST_VERSION || alphabet_size == 0 ||
      alphabet_size > BIT_RACK_MAX_ALPHABET_SIZE ||
      draw_count_heads != ROOT_LEAVES_DRAW_COUNT_HEADS || num_pool_bins == 0 ||
      num_pool_bins > ROOT_LEAVES_MAX_POOL_SIZE_BINS) {
    return false;
  }
  root_leaves->alphabet_size = (int)alphabet_size;
  root_leaves->num_pool_bins = (int)num_pool_bins;
  if (!fread_le_uint16s(root_leaves->pool_bin_upper_bounds, num_pool_bins,
                        stream)) {
    return false;
  }
  for (uint32_t bin = 0; bin < num_pool_bins; bin++) {
    if (root_leaves->pool_bin_upper_bounds[bin] == 0 ||
        (bin > 0 && root_leaves->pool_bin_upper_bounds[bin] <=
                        root_leaves->pool_bin_upper_bounds[bin - 1])) {
      return false;
    }
  }
  const size_t num_biases =
      (size_t)num_pool_bins * draw_count_heads * alphabet_size;
  const size_t num_weights =
      (size_t)draw_count_heads * alphabet_size * alphabet_size;
  root_leaves->biases = malloc_or_die(num_biases * sizeof(Equity));
  root_leaves->weights = malloc_or_die(num_weights * sizeof(Equity));
  // A version 2 trailer continues with per-leave caps that only matter to a
  // contextual RIT; the root ranking evaluates every candidate exactly and
  // stops reading here.
  return root_leaves_read_equities(stream, num_biases, root_leaves->biases) &&
         root_leaves_read_equities(stream, num_weights, root_leaves->weights);
}

RootLeaves *root_leaves_create(const char *data_paths, const char *name,
                               ErrorStack *error_stack) {
  char *filename = data_filepaths_get_readable_filename(
      data_paths, name, DATA_FILEPATH_TYPE_KLV3, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    free(filename);
    return NULL;
  }
  FILE *stream = stream_from_filename(filename, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    free(filename);
    return NULL;
  }
  RootLeaves *root_leaves = calloc_or_die(1, sizeof(RootLeaves));
  root_leaves->name = string_duplicate(name);
  if (!root_leaves_skip_klv2_body(stream) ||
      !root_leaves_read_trailer(stream, root_leaves)) {
    error_stack_push(
        error_stack, ERROR_STATUS_ROOT_LEAVES_INVALID_FILE,
        get_formatted_string("invalid or unsupported KLV3 file: %s", filename));
    root_leaves_destroy(root_leaves);
    root_leaves = NULL;
  }
  fclose_or_die(stream);
  free(filename);
  return root_leaves;
}

static int root_leaves_get_pool_bin(const RootLeaves *root_leaves,
                                    int unseen_total) {
  for (int bin = 0; bin < root_leaves->num_pool_bins; bin++) {
    if (unseen_total <= root_leaves->pool_bin_upper_bounds[bin]) {
      return bin;
    }
  }
  return root_leaves->num_pool_bins - 1;
}

void root_leaves_compute_tile_adjustments(
    const RootLeaves *root_leaves, const int *unseen_counts, int unseen_total,
    Equity adjustments[ROOT_LEAVES_DRAW_COUNT_HEADS]
                      [MACHINE_LETTER_MAX_VALUE]) {
  memset(adjustments, 0,
         sizeof(Equity) * ROOT_LEAVES_DRAW_COUNT_HEADS *
             MACHINE_LETTER_MAX_VALUE);
  if (unseen_total <= 0) {
    return;
  }
  const int alphabet_size = root_leaves->alphabet_size;
  const int pool_bin = root_leaves_get_pool_bin(root_leaves, unseen_total);
  // A move that draws nothing gets no contextual term: head 0 stays zero.
  for (int draw_count = 1; draw_count < ROOT_LEAVES_DRAW_COUNT_HEADS;
       draw_count++) {
    for (int held = 0; held < alphabet_size; held++) {
      const Equity *weight_row =
          root_leaves->weights +
          ((size_t)draw_count * alphabet_size + held) * alphabet_size;
      int64_t weighted_sum = 0;
      for (int unseen = 0; unseen < alphabet_size; unseen++) {
        weighted_sum += (int64_t)weight_row[unseen] * unseen_counts[unseen];
      }
      // Rounds half away from zero, as the KLV3 runtime does.
      const int64_t rounded_average =
          weighted_sum >= 0
              ? (weighted_sum + unseen_total / 2) / unseen_total
              : -((-weighted_sum + unseen_total / 2) / unseen_total);
      const int64_t value =
          (int64_t)root_leaves
              ->biases[((size_t)pool_bin * ROOT_LEAVES_DRAW_COUNT_HEADS +
                        draw_count) *
                           alphabet_size +
                       held] +
          rounded_average;
      if (value > EQUITY_MAX_VALUE || value < EQUITY_MIN_VALUE) {
        log_fatal("KLV3 tile adjustment out of Equity range");
      }
      adjustments[draw_count][held] = (Equity)value;
    }
  }
}

Equity root_leaves_get_leave_adjustment(
    const Equity adjustments[ROOT_LEAVES_DRAW_COUNT_HEADS]
                            [MACHINE_LETTER_MAX_VALUE],
    const Rack *leave, int draw_count) {
  if (draw_count <= 0) {
    return 0;
  }
  if (draw_count >= ROOT_LEAVES_DRAW_COUNT_HEADS) {
    draw_count = ROOT_LEAVES_DRAW_COUNT_HEADS - 1;
  }
  int64_t adjustment = 0;
  for (int ml = 0; ml < rack_get_dist_size(leave); ml++) {
    adjustment +=
        (int64_t)rack_get_letter(leave, ml) * adjustments[draw_count][ml];
  }
  if (adjustment > EQUITY_MAX_VALUE || adjustment < EQUITY_MIN_VALUE) {
    log_fatal("KLV3 leave adjustment out of Equity range");
  }
  return (Equity)adjustment;
}
