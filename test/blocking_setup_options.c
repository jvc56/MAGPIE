#include "blocking_setup_options.h"

#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void bs_options_parse(BSOptions *options, const StringSplitter *fields) {
  options->count = 0;
  for (int field_idx = 1;
       field_idx < string_splitter_get_number_of_items(fields); field_idx++) {
    const char *field = string_splitter_get_item(fields, field_idx);
    const char *equals = strchr(field, '=');
    if (equals == NULL || options->count >= BS_MAX_OPTIONS) {
      log_fatal("expected key=value, got '%s'", field);
      continue;
    }
    const size_t key_length = (size_t)(equals - field);
    char *key = string_duplicate(field);
    key[key_length] = '\0';
    options->keys[options->count] = key;
    options->values[options->count] = string_duplicate(field + key_length + 1);
    options->count++;
  }
}

void bs_options_destroy(BSOptions *options) {
  for (int option_idx = 0; option_idx < options->count; option_idx++) {
    free(options->keys[option_idx]);
    free(options->values[option_idx]);
  }
  options->count = 0;
}

const char *bs_options_get(const BSOptions *options, const char *key,
                           const char *fallback) {
  for (int option_idx = 0; option_idx < options->count; option_idx++) {
    if (strings_equal(options->keys[option_idx], key)) {
      return options->values[option_idx];
    }
  }
  return fallback;
}

const char *bs_options_require(const BSOptions *options, const char *key) {
  const char *value = bs_options_get(options, key, NULL);
  if (value == NULL) {
    log_fatal("missing option %s=", key);
  }
  return value;
}

long bs_options_get_long(const BSOptions *options, const char *key,
                         long fallback) {
  const char *value = bs_options_get(options, key, NULL);
  return value == NULL ? fallback : strtol(value, NULL, 10);
}

uint64_t bs_options_get_u64(const BSOptions *options, const char *key,
                            uint64_t fallback) {
  const char *value = bs_options_get(options, key, NULL);
  return value == NULL ? fallback : strtoull(value, NULL, 10);
}

double bs_options_get_double(const BSOptions *options, const char *key,
                             double fallback) {
  const char *value = bs_options_get(options, key, NULL);
  return value == NULL ? fallback : strtod(value, NULL);
}

uint64_t bs_mix(uint64_t value) {
  value += UINT64_C(0x9e3779b97f4a7c15);
  value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
  return value ^ (value >> 31);
}
