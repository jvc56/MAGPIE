#ifndef BLOCKING_SETUP_OPTIONS_H
#define BLOCKING_SETUP_OPTIONS_H

#include "../src/util/string_util.h"
#include <stdint.h>

// "key=value" options for the blocking/setup on-demand harnesses: every
// field of a split spec after the first (the stage name).
enum {
  BS_MAX_OPTIONS = 48,
};

typedef struct BSOptions {
  int count;
  char *keys[BS_MAX_OPTIONS];
  char *values[BS_MAX_OPTIONS];
} BSOptions;

void bs_options_parse(BSOptions *options, const StringSplitter *fields);
void bs_options_destroy(BSOptions *options);
const char *bs_options_get(const BSOptions *options, const char *key,
                           const char *fallback);
// Exits with a message when the key is missing.
const char *bs_options_require(const BSOptions *options, const char *key);
long bs_options_get_long(const BSOptions *options, const char *key,
                         long fallback);
uint64_t bs_options_get_u64(const BSOptions *options, const char *key,
                            uint64_t fallback);
double bs_options_get_double(const BSOptions *options, const char *key,
                             double fallback);
// A splitmix64 step, for deriving independent seeds.
uint64_t bs_mix(uint64_t value);

#endif
