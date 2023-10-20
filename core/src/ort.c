#include "ort.h"

#include <stdio.h>
#include <stdlib.h>

#include "fileproxy.h"
#include "log.h"
#include "util.h"

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