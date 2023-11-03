#ifndef ORT_H
#define ORT_H

#include <stdint.h>

#include "rack.h"

typedef struct ORT {
  uint32_t *bucket_starts;
  uint32_t *values;
  uint32_t num_buckets;
} ORT;

ORT *create_ort(const char *ort_filename);
void destroy_ort(ORT *ort);
void get_word_sizes(ORT *ort, Rack *rack, LetterDistribution *ld,
                    uint8_t *word_sizes);

#endif