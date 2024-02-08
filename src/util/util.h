#ifndef UTIL_H
#define UTIL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

bool has_prefix(const char *pre, const char *str);
void *malloc_or_die(size_t size);
void *calloc_or_die(size_t num, size_t size);
void *realloc_or_die(void *realloc_target, size_t size);
int char_to_int(char c);
int string_to_int(const char *str);
uint64_t string_to_uint64(const char *str);
double string_to_double(const char *str);
bool is_decimal_number(const char *str);
uint64_t choose(uint64_t n, uint64_t k);

#endif
