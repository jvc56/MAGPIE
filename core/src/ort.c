#include "ort.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "fileproxy.h"
#include "log.h"
#include "util.h"

#define MAX_PLAYTHROUGH_IN_TABLE 5
#define QUOTIENT_BITS 14
#define QUOTIENT_MASK ((1 << QUOTIENT_BITS) - 1)
#define BITS_PER_TILE 5
#define BITS_PER_WORD_SIZE 3
#define WORD_SIZE_MASK ((1 << BITS_PER_WORD_SIZE) - 1)

void load_ort(ORT *ort, const char *ort_filename) {
  FILE *stream = stream_from_filename(ort_filename);
  if (!stream) {
    log_fatal("failed to open stream from filename: %s\n", ort_filename);
  }

  uint32_t num_buckets;
  size_t result;

  result = fread(&num_buckets, sizeof(num_buckets), 1, stream);
  if (result != 1) {
    log_fatal("ort num_buckets fread failure: %zd != %d\n", result, 1);
  }
  ort->num_buckets = num_buckets;

  uint32_t num_values;
  result = fread(&num_values, sizeof(num_values), 1, stream);
  if (result != 1) {
    log_fatal("ort num_values fread failure: %zd != %d\n", result, 1);
  }

  uint32_t num_bucket_starts = num_buckets + 1;  // one extra for the end
  ort->bucket_starts =
      (uint32_t *)malloc_or_die((num_bucket_starts) * sizeof(uint32_t));
  result =
      fread(ort->bucket_starts, sizeof(uint32_t), num_bucket_starts, stream);
  if (result != num_bucket_starts) {
    log_fatal("ort bucket_starts fread failure: %zd != %d\n", result,
              num_bucket_starts);
  }

  ort->values = (uint32_t *)malloc_or_die(num_values * sizeof(uint32_t));
  result = fread(ort->values, sizeof(uint32_t), num_values, stream);
  if (result != num_values) {
    log_fatal("ort values fread failure: %zd != %d\n", result, num_values);
  }
}

ORT *create_ort(const char *ort_filename) {
  ORT *ort = malloc(sizeof(ORT));
  load_ort(ort, ort_filename);
  return ort;
}

void destroy_ort(ORT *ort) {
  free(ort->bucket_starts);
  free(ort->values);
  free(ort);
}

uint64_t make_rack_key(Rack *rack, LetterDistribution *ld) {
  uint64_t key = 0;
  for (int i = 1; i < ld->size; i++) {
    const int num_this = rack->array[i];
    for (int j = 0; j < num_this; j++) {
      key = (key << BITS_PER_TILE) | i;
    }
  }
  // MAGPIE has blank = 0, not e.g. blank = 27 in English
  for (int j = 0; j < rack->array[0]; j++) {
    key = (key << BITS_PER_TILE) | ld->size;
  }
  return key;
}

void get_word_sizes(ORT *ort, Rack *rack, LetterDistribution *ld, uint8_t *word_sizes) {
  int max_playthrough_in_table = -1;
  uint32_t packed_word_sizes = 0;
  if (ort != NULL) {
    const uint64_t rack_key = make_rack_key(rack, ld);
    const uint32_t bucket = rack_key % ort->num_buckets;
    const uint32_t quotient = rack_key / ort->num_buckets;
    const uint32_t bucket_start = ort->bucket_starts[bucket];
    const uint32_t bucket_end = ort->bucket_starts[bucket + 1];
    for (uint32_t i = bucket_start; i < bucket_end; i++) {
      const uint32_t value = ort->values[i];
      const uint32_t value_quotient = value & QUOTIENT_MASK;
      if (quotient != value_quotient) {
        continue;
      }
      max_playthrough_in_table = MAX_PLAYTHROUGH_IN_TABLE;
      packed_word_sizes = value >> QUOTIENT_BITS;
      break;
    }
  }
  for (int i = 0; i <= max_playthrough_in_table; i++) {
    word_sizes[i] = packed_word_sizes & WORD_SIZE_MASK;
    packed_word_sizes >>= BITS_PER_WORD_SIZE;
  }
  for (int i = max_playthrough_in_table + 1; i <= BOARD_DIM; i++) {
    word_sizes[i] = rack->number_of_letters;
  }
}