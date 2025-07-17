
#ifndef STRING_BUILDER_H
#define STRING_BUILDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "io_util.h"

typedef struct StringBuilder StringBuilder;

StringBuilder *string_builder_create(void);
void string_builder_destroy(StringBuilder *string_builder);
void string_builder_add_string(StringBuilder *string_builder,
                               const char *string);
void string_builder_add_formatted_string(StringBuilder *string_builder,
                                         const char *format, ...);
void string_builder_add_table_row(StringBuilder *string_builder,
                                  size_t cell_width, const char *left_cell,
                                  const char *middle_cell,
                                  const char *right_cell);
void string_builder_add_spaces(StringBuilder *string_builder,
                               int number_of_spaces);
void string_builder_add_int(StringBuilder *string_builder, int64_t n);
void string_builder_add_char(StringBuilder *string_builder, char c);
void string_builder_clear(StringBuilder *string_builder);
size_t string_builder_length(const StringBuilder *string_builder);
const char *string_builder_peek(const StringBuilder *string_builder);
char *string_builder_dump(const StringBuilder *string_builder, size_t *length);

typedef struct StringSplitter StringSplitter;

int string_splitter_get_number_of_items(const StringSplitter *string_splitter);
const char *string_splitter_get_item(const StringSplitter *string_splitter,
                                     int item_index);
void string_splitter_trim_char(StringSplitter *string_splitter, const char c);
char *string_splitter_join(const StringSplitter *string_splitter,
                           int start_index, int end_index,
                           const char *separator);
StringSplitter *split_string_by_whitespace(const char *input_string,
                                           bool ignore_empty);
StringSplitter *split_string(const char *input_string, const char delimiter,
                             bool ignore_empty);
void string_splitter_destroy(StringSplitter *string_splitter);
StringSplitter *split_string_by_newline(const char *input_string,
                                        bool ignore_empty);
StringSplitter *split_file_by_newline(const char *filename,
                                      ErrorStack *error_stack);

typedef struct StringList StringList;

StringList *string_list_create(void);
void string_list_add_string(StringList *string_list, const char *string);
int string_list_get_count(const StringList *string_list);
const char *string_list_get_string(const StringList *string_list, int index);
void string_list_destroy(StringList *string_list);

// String Miscellaneous functions

// Boolean string functions
bool has_prefix(const char *pre, const char *str);
bool has_iprefix(const char *pre, const char *str);
bool is_string_empty_or_whitespace(const char *str);
bool is_string_empty_or_null(const char *str);
bool strings_equal(const char *str1, const char *str2);
bool strings_iequal(const char *str1, const char *str2);
bool is_all_digits_or_empty(const char *str);
bool has_substring(const char *str, const char *pattern);
size_t string_length(const char *str);

// Malloc'ing string functions
char *string_duplicate(const char *str);
char *string_copy(char *dest, const char *src);
char *get_substring(const char *input_string, int start_index, int end_index);
char *iso_8859_1_to_utf8(const char *iso_8859_1_string);
char *get_dirpath_from_filepath(const char *filepath);
char *cut_off_after_last_char(const char *str, char ch);
char *cut_off_after_first_char(const char *str, char ch);
char *insert_before_dot(const char *str, const char *insert);

// Non-malloc'ing string functions
const char *get_base_filename(const char *filepath);

// Inplace string functions
void trim_whitespace(char *str);
void trim_char(char *str, const char c);

// String conversions
int string_to_int(const char *str, ErrorStack *error_stack);
uint64_t string_to_uint64(const char *str, ErrorStack *error_stack);
double string_to_double(const char *str, ErrorStack *error_stack);

#endif
