
#ifndef STRING_BUILDER_H
#define STRING_BUILDER_H

#include <stddef.h>
#include <stdint.h>

int has_prefix(const char *pre, const char *str);
int is_all_whitespace_or_empty(const char *str);
int is_all_digits_or_empty(const char *str);
char *get_formatted_string(const char *format, ...);
bool strings_equal(const char *str1, const char *str2);
char *string_copy(char *dest, const char *src);
void *memory_copy(void *dest, const void *src, size_t n);
int memory_compare(const void *s1, const void *s2, size_t n);
bool is_string_empty(const char *str1);
void remove_first_newline(char *str);
size_t string_length(const char *str);
void trim_semicolon(char *str);
void trim_whitespace(char *str);

struct StringSplitter;
typedef struct StringSplitter StringSplitter;

int string_splitter_get_number_of_items(StringSplitter *string_splitter);
char *string_splitter_get_item(StringSplitter *string_splitter, int item_index);
char *string_splitter_join(StringSplitter *string_splitter, int start_index,
                           int end_index);
StringSplitter *split_string_by_whitespace(const char *input_string,
                                           bool ignore_empty);
StringSplitter *split_string(const char *input_string, const char delimiter,
                             bool ignore_empty);
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