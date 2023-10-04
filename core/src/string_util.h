
#ifndef STRING_BUILDER_H
#define STRING_BUILDER_H

#include <stddef.h>
#include <stdint.h>

int prefix(const char *pre, const char *str);

int is_all_whitespace_or_empty(const char *str);

char *get_formatted_string(const char *format, ...);

struct StringSplitter;
typedef struct StringSplitter StringSplitter;

int string_splitter_get_number_of_items(StringSplitter *string_splitter);
char *string_splitter_get_item(StringSplitter *string_splitter, int item_index);
StringSplitter *split_string_ignore_empty(const char *input_string,
                                          const char delimiter);
StringSplitter *split_string(const char *input_string, const char delimiter);
void destroy_string_splitter(StringSplitter *string_splitter);

struct StringBuilder;
typedef struct StringBuilder StringBuilder;

StringBuilder *create_string_builder();
void destroy_string_builder(StringBuilder *string_builder);
void string_builder_add_string(StringBuilder *string_builder,
                               const char *string, size_t length);
void string_builder_add_formatted_string(StringBuilder *string_builder,
                                         const char *format, ...);
void string_builder_add_spaces(StringBuilder *string_builder,
                               int number_of_spaces);
void string_builder_add_int(StringBuilder *string_builder, int64_t n);
void string_builder_add_uint(StringBuilder *string_builder, uint64_t n);
void string_builder_add_double(StringBuilder *string_builder, double val);
void string_builder_add_char(StringBuilder *string_builder, char c);
void string_builder_clear(StringBuilder *string_builder);
size_t string_builder_length(const StringBuilder *string_builder);
const char *string_builder_peek(const StringBuilder *string_builder);
char *string_builder_dump(const StringBuilder *string_builder, size_t *length);

#endif