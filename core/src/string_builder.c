#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "string_builder.h"

static const size_t string_builder_min_size = 32;

struct StringBuilder {
  char *string;
  size_t alloced;
  size_t len;
};

StringBuilder *create_string_builder() {
  StringBuilder *string_builder;

  string_builder = calloc(1, sizeof(*string_builder));
  string_builder->str = malloc(string_builder_min_size);
  *string_builder->str = '\0';
  string_builder->alloced = string_builder_min_size;
  string_builder->len = 0;

  return string_builder;
}

void destroy_string_builder(StringBuilder *string_builder) {
  if (string_builder == NULL) {
    return;
  }
  free(string_builder->str);
  free(string_builder);
}

static void string_builder_ensure_space(StringBuilder *string_builder,
                                        size_t add_len) {
  if (string_builder == NULL || add_len == 0) {
    return;
  }

  if (string_builder->alloced >= string_builder->len + add_len + 1) {
    return;
  }

  while (string_builder->alloced < string_builder->len + add_len + 1) {
    /* Doubling growth strategy. */
    string_builder->alloced <<= 1;
    if (string_builder->alloced == 0) {
      /* Left shift of max bits will go to 0. An unsigned type set to
       * -1 will return the maximum possible size. However, we should
       *  have run out of memory well before we need to do this. Since
       *  this is the theoretical maximum total system memory we don't
       *  have a flag saying we can't grow any more because it should
       *  be impossible to get to this point. */
      string_builder->alloced--;
    }
  }
  string_builder->str = realloc(string_builder->str, string_builder->alloced);
}

void string_builder_add_string(StringBuilder *string_builder, const char *str,
                               size_t len) {
  if (string_builder == NULL || str == NULL || *str == '\0') {
    return;
  }

  if (len == 0) {
    len = strlen(str);
  }

  string_builder_ensure_space(string_builder, len);
  memmove(string_builder->str + string_builder->len, str, len);
  string_builder->len += len;
  string_builder->str[string_builder->len] = '\0';
}

void string_builder_clear(StringBuilder *string_builder) {
  if (string_builder == NULL) {
    return;
  }
  string_builder_truncate(string_builder, 0);
}

void string_builder_truncate(StringBuilder *string_builder, size_t len) {
  if (string_builder == NULL || len >= string_builder->len) {
    return;
  }

  string_builder->len = len;
  string_builder->str[string_builder->len] = '\0';
}

size_t string_builder_length(const StringBuilder *string_builder) {
  if (string_builder == NULL) {
    return 0;
  }
  return string_builder->len;
}

const char *string_builder_peek(const StringBuilder *string_builder) {
  if (string_builder == NULL) {
    return NULL;
  }
  return string_builder->str;
}

char *string_builder_dump(const StringBuilder *string_builder, size_t *len) {
  char *out;

  if (string_builder == NULL) {
    return NULL;
  }

  if (len != NULL) {
    *len = string_builder->len;
  }
  out = malloc(string_builder->len + 1);
  memcpy(out, string_builder->str, string_builder->len + 1);
  return out;
}