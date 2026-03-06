#ifndef BAI_DEFS_H
#define BAI_DEFS_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  BAI_THRESHOLD_NONE,
  BAI_THRESHOLD_GK16,
} bai_threshold_t;

#define BAI_THRESHOLD_NONE_STRING "none"
#define BAI_THRESHOLD_GK16_STRING "gk16"

typedef enum {
  BAI_SAMPLING_RULE_ROUND_ROBIN,
  BAI_SAMPLING_RULE_TOP_TWO_IDS,
} bai_sampling_rule_t;

#define BAI_SAMPLING_RULE_ROUND_ROBIN_STRING "rr"
#define BAI_SAMPLING_RULE_TOP_TWO_IDS_STRING "tt"

typedef struct BAIOptions {
  bai_sampling_rule_t sampling_rule;
  bai_threshold_t threshold;
  double delta;
  uint64_t sample_limit;
  uint64_t sample_minimum;
  uint64_t time_limit_seconds;
  int num_threads;
  double cutoff;
} BAIOptions;

#endif