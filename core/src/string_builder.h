#ifndef STRING_BUILDER_H
#define STRING_BUILDER_H

#include <stddef.h>

struct StringBuilder;
typedef struct StringBuilder StringBuilder;

StringBuilder *create_string_builder();
void destroy_string_builder(StringBuilder *string_builder);
void string_builder_add_string(StringBuilder *string_builder,
                               const char *string, size_t length);
void string_builder_clear(StringBuilder *string_builder);
size_t string_builder_length(const StringBuilder *string_builder);
const char *string_builder_peek(const StringBuilder *string_builder);
char *string_builder_dump(const StringBuilder *string_builder, size_t *length);

#endif