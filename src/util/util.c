#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "log.h"

void *malloc_or_die(size_t size) {
  void *uncasted_pointer = malloc(size);
  if (!uncasted_pointer) {
    log_fatal("failed to malloc size of %lu.\n", size);
  }
  return uncasted_pointer;
}

void *calloc_or_die(size_t number_of_elements, size_t size_of_element) {
  void *uncasted_pointer = calloc(number_of_elements, size_of_element);
  if (!uncasted_pointer) {
    log_fatal("failed to calloc %lu elements of size %lu.\n",
              number_of_elements, size_of_element);
  }
  return uncasted_pointer;
}

void *realloc_or_die(void *realloc_target, size_t size) {
  void *realloc_result = realloc(realloc_target, size);
  if (!realloc_result) {
    log_fatal("failed to realloc %p with size of %lu.\n", realloc_target, size);
  }
  return realloc_result;
}

bool is_decimal_number(const char *str) {
  if (!str || *str == '\0') {
    return false; // Empty string is not a valid decimal number
  }

  int i = 0;
  bool has_decimal_point = false;

  if (str[i] == '\0') {
    return false; // No digits in the string
  }

  while (str[i] != '\0') {
    if (isdigit(str[i])) {
      i++;
    } else if (str[i] == '.' && !has_decimal_point) {
      has_decimal_point = true;
      i++;
    } else {
      return false; // Invalid character in the string
    }
  }

  return true;
}

int char_to_int(char c) { return c - '0'; }

// Fatal if conversion fails
int string_to_int(const char *str) {
  char *endptr;
  long int result = strtol(str, &endptr, 10);
  if (*endptr != '\0') {
    log_fatal("string to int conversion failed for >%s<\n", str);
  }
  return (int)result;
}

// Sets success to false if conversion fails
int string_to_int_or_set_error(const char *str, bool *success) {
  char *endptr;
  long int result = strtol(str, &endptr, 10);
  if (*endptr != '\0') {
    *success = false;
  } else {
    *success = true;
  }
  return (int)result;
}

double string_to_double_or_set_error(const char *str, bool *success) {
  char *endptr;
  double result = strtod(str, &endptr);
  if (*endptr != '\0') {
    *success = false;
  } else {
    *success = true;
  }
  return result;
}

uint64_t string_to_uint64_or_set_error(const char *str, bool *success) {
  char *endptr;
  uint64_t result = strtoull(str, &endptr, 10);
  if (*endptr != '\0') {
    *success = false;
  } else {
    *success = true;
  }
  return result;
}

uint64_t string_to_uint64(const char *str) {
  char *endptr;
  uint64_t result = strtoull(str, &endptr, 10);
  if (*endptr != '\0') {
    log_fatal("string to uint64_t conversion failed for %s\n", str);
  }
  return result;
}

double string_to_double(const char *str) {
  if (!str) {
    log_fatal("called string_to_double on NULL string\n");
  }
  return strtod(str, NULL);
}

float string_to_float(const char *str) {
  if (!str) {
    log_fatal("called string_to_float on NULL string\n");
  }
  return strtof(str, NULL);
}

uint64_t choose(uint64_t n, uint64_t k) {
  if (n < k) {
    return 0;
  }
  if (k == 0) {
    return 1;
  }
  return (n * choose(n - 1, k - 1)) / k;
}