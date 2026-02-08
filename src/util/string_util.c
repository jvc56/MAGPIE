#include "string_util.h"

#include "io_util.h"
#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum {
  STRING_LIST_INITIAL_CAPACITY = 10,
  STRING_BUILDER_MIN_SIZE = 32,
};

#define STRING_CONV_ACCEPT_CHARS " \t\n\r"

// ****************************************************************************
// ****************************** String Builder ******************************
// ****************************************************************************

struct StringBuilder {
  char *string;
  size_t alloced;
  size_t len;
};

StringBuilder *string_builder_create(void) {
  StringBuilder *string_builder = malloc_or_die(sizeof(StringBuilder));
  string_builder->string = malloc_or_die(STRING_BUILDER_MIN_SIZE);
  *string_builder->string = '\0';
  string_builder->alloced = STRING_BUILDER_MIN_SIZE;
  string_builder->len = 0;
  return string_builder;
}

void string_builder_destroy(StringBuilder *string_builder) {
  if (!string_builder) {
    return;
  }
  free(string_builder->string);
  free(string_builder);
}

static void string_builder_ensure_space(StringBuilder *string_builder,
                                        size_t add_len) {
  if (!string_builder || add_len == 0) {
    return;
  }
  if (string_builder->alloced >= string_builder->len + add_len + 1) {
    return;
  }
  while (string_builder->alloced < string_builder->len + add_len + 1) {
    string_builder->alloced *= 2;
  }
  string_builder->string =
      realloc_or_die(string_builder->string, string_builder->alloced);
}

void string_builder_add_string(StringBuilder *string_builder, const char *str) {
  if (!string_builder || !str || *str == '\0') {
    return;
  }

  size_t len = string_length(str);

  string_builder_ensure_space(string_builder, len);
  memmove(string_builder->string + string_builder->len, str, len);
  string_builder->len += len;
  string_builder->string[string_builder->len] = '\0';
}

void string_builder_add_formatted_string(StringBuilder *string_builder,
                                         const char *format, ...) {
  va_list args;
  va_start(args, format);
  char *formatted_string = format_string_with_va_list(format, &args);
  va_end(args);
  string_builder_add_string(string_builder, formatted_string);
  free(formatted_string);
}

void string_builder_add_space_padded_string(StringBuilder *string_builder,
                                            const char *string,
                                            size_t total_length) {
  // string is a format string potentially containing escape sequences, but must
  // have no arguments.
  char *formatted_string = get_formatted_string(string);
  const size_t printed_length = strlen(formatted_string);
  string_builder_add_string(string_builder, formatted_string);
  const size_t number_of_spaces =
      total_length > printed_length ? total_length - printed_length : 0;
  string_builder_add_spaces(string_builder, (int)number_of_spaces);
  free(formatted_string);
}

void string_builder_add_table_row(StringBuilder *string_builder,
                                  size_t cell_width, const char *left_cell,
                                  const char *middle_cell,
                                  const char *right_cell) {
  // Cell strings are format strings potentially containing escape sequences,
  // but must have no arguments.
  string_builder_add_space_padded_string(string_builder, left_cell, cell_width);
  string_builder_add_space_padded_string(string_builder, middle_cell,
                                         cell_width);
  string_builder_add_space_padded_string(string_builder, right_cell,
                                         cell_width);
  string_builder_add_string(string_builder, "\n");
}

void string_builder_add_spaces(StringBuilder *string_builder,
                               int number_of_spaces) {
  string_builder_add_formatted_string(string_builder, "%*s", number_of_spaces,
                                      "");
}

void string_builder_add_int(StringBuilder *string_builder, int64_t n) {
  string_builder_add_formatted_string(string_builder, "%d", n);
}

void string_builder_add_char(StringBuilder *string_builder, char c) {
  string_builder_add_formatted_string(string_builder, "%c", c);
}

void string_builder_truncate(StringBuilder *string_builder, size_t len) {
  if (!string_builder || len >= string_builder->len) {
    return;
  }

  string_builder->len = len;
  string_builder->string[string_builder->len] = '\0';
}

void string_builder_clear(StringBuilder *string_builder) {
  if (!string_builder) {
    return;
  }
  string_builder_truncate(string_builder, 0);
}

size_t string_builder_length(const StringBuilder *string_builder) {
  if (!string_builder) {
    return 0;
  }
  return string_builder->len;
}

const char *string_builder_peek(const StringBuilder *string_builder) {
  if (!string_builder) {
    return NULL;
  }
  return string_builder->string;
}

char *string_builder_dump(const StringBuilder *string_builder, size_t *len) {
  char *out;

  if (!string_builder) {
    return NULL;
  }

  if (len) {
    *len = string_builder->len;
  }
  out = malloc_or_die(string_builder->len + 1);
  memcpy(out, string_builder->string, string_builder->len + 1);
  return out;
}

// ****************************************************************************
// ***************************** String Splitter ******************************
// ****************************************************************************

struct StringSplitter {
  int number_of_items;
  char **items;
};

typedef enum {
  STRING_DELIMITER_RANGED,
  STRING_DELIMITER_WHITESPACE,
} string_delimiter_class_t;

typedef struct StringDelimiter {
  char min_delimiter;
  char max_delimiter;
  string_delimiter_class_t string_delimiter_class;
} StringDelimiter;

StringSplitter *string_splitter_create(void) {
  StringSplitter *string_splitter = malloc_or_die(sizeof(StringSplitter));
  string_splitter->number_of_items = 0;
  string_splitter->items = NULL;
  return string_splitter;
}

void string_splitter_destroy(StringSplitter *string_splitter) {
  if (!string_splitter) {
    return;
  }
  for (int i = 0; i < string_splitter->number_of_items; i++) {
    free(string_splitter->items[i]);
  }
  free(string_splitter->items);
  free(string_splitter);
}

StringDelimiter *string_delimiter_create(void) {
  StringDelimiter *string_delimiter = malloc_or_die(sizeof(StringSplitter));
  return string_delimiter;
}

void string_delimiter_destroy(StringDelimiter *string_delimiter) {
  if (!string_delimiter) {
    return;
  }
  free(string_delimiter);
}

bool char_matches_string_delimiter(const StringDelimiter *string_delimiter,
                                   const char c) {
  switch (string_delimiter->string_delimiter_class) {
  case STRING_DELIMITER_RANGED:
    return c >= string_delimiter->min_delimiter &&
           c <= string_delimiter->min_delimiter;
    break;
  case STRING_DELIMITER_WHITESPACE:
    return isspace(c);
    break;
  }
  return false;
}

int string_splitter_get_number_of_items(const StringSplitter *string_splitter) {
  return string_splitter->number_of_items;
}

const char *string_splitter_get_item(const StringSplitter *string_splitter,
                                     int item_index) {
  if (item_index >= string_splitter->number_of_items || item_index < 0) {
    log_fatal("string item out of range (%d): %d",
              string_splitter->number_of_items, item_index);
  }
  return string_splitter->items[item_index];
}

void string_splitter_trim_char(StringSplitter *string_splitter, const char c) {
  int number_of_items = string_splitter_get_number_of_items(string_splitter);
  for (int i = 0; i < number_of_items; i++) {
    trim_char(string_splitter->items[i], c);
  }
}

char *string_splitter_join(const StringSplitter *string_splitter,
                           int start_index, int end_index,
                           const char *separator) {
  int number_of_items = string_splitter_get_number_of_items(string_splitter);
  if (start_index < 0 || end_index < 0 || start_index > number_of_items ||
      end_index > number_of_items) {
    log_fatal("invalid bounds for join: %d, %d, %d", start_index, end_index,
              number_of_items);
  }
  StringBuilder *joined_string_builder = string_builder_create();
  for (int i = start_index; i < end_index; i++) {
    string_builder_add_string(joined_string_builder,
                              string_splitter_get_item(string_splitter, i));
    if (i < end_index - 1) {
      string_builder_add_string(joined_string_builder, separator);
    }
  }
  char *joined_string = string_builder_dump(joined_string_builder, NULL);
  string_builder_destroy(joined_string_builder);
  return joined_string;
}

int split_string_scan(const StringDelimiter *string_delimiter,
                      StringSplitter *string_splitter, const char *input_string,
                      bool ignore_empty, bool set_items) {
  int current_number_of_items = 0;
  char previous_char = '\0';
  int item_start_index = 0;
  int item_end_index = 0;
  size_t str_length = string_length(input_string);
  for (size_t i = 0; i < str_length; i++) {
    char current_char = input_string[i];
    if (set_items) {
      item_end_index++;
    }
    if (char_matches_string_delimiter(string_delimiter, current_char)) {
      if (!ignore_empty || (i != 0 && !char_matches_string_delimiter(
                                          string_delimiter, previous_char))) {
        if (set_items) {
          int end = item_end_index - 1;
          if (end > item_start_index && input_string[end - 1] == '\r') {
            end--;
          }
          string_splitter->items[current_number_of_items] =
              get_substring(input_string, item_start_index, end);
        }
        current_number_of_items++;
      }
      item_start_index = item_end_index;
    }
    previous_char = current_char;
  }

  if (!ignore_empty ||
      (str_length != 0 &&
       !char_matches_string_delimiter(string_delimiter, previous_char))) {
    if (set_items) {
      int end = item_end_index;
      if (end > item_start_index && input_string[end - 1] == '\r') {
        end--;
      }
      string_splitter->items[current_number_of_items] =
          get_substring(input_string, item_start_index, end);
    }
    current_number_of_items++;
  }

  return current_number_of_items;
}

StringSplitter *split_string_internal(const char *input_string,
                                      const StringDelimiter *string_delimiter,
                                      const bool ignore_empty) {

  int number_of_items = split_string_scan(string_delimiter, NULL, input_string,
                                          ignore_empty, false);

  StringSplitter *string_splitter = string_splitter_create();
  string_splitter->number_of_items = number_of_items;
  if (string_splitter->number_of_items > 0) {
    string_splitter->items =
        malloc_or_die(sizeof(char *) * string_splitter->number_of_items);
    split_string_scan(string_delimiter, string_splitter, input_string,
                      ignore_empty, true);
  }

  return string_splitter;
}

StringSplitter *split_string_by_range(const char *input_string,
                                      const char min_delimiter,
                                      const char max_delimiter,
                                      bool ignore_empty) {
  StringDelimiter *string_delimiter = string_delimiter_create();
  string_delimiter->min_delimiter = min_delimiter;
  string_delimiter->max_delimiter = max_delimiter;
  string_delimiter->string_delimiter_class = STRING_DELIMITER_RANGED;
  StringSplitter *string_splitter =
      split_string_internal(input_string, string_delimiter, ignore_empty);
  string_delimiter_destroy(string_delimiter);
  return string_splitter;
}

StringSplitter *split_string_by_whitespace(const char *input_string,
                                           bool ignore_empty) {
  StringDelimiter *string_delimiter = string_delimiter_create();
  string_delimiter->string_delimiter_class = STRING_DELIMITER_WHITESPACE;
  StringSplitter *string_splitter =
      split_string_internal(input_string, string_delimiter, ignore_empty);
  string_delimiter_destroy(string_delimiter);
  return string_splitter;
}

StringSplitter *split_string_by_newline(const char *input_string,
                                        bool ignore_empty) {
  return split_string(input_string, '\n', ignore_empty);
}

StringSplitter *split_string(const char *input_string, const char delimiter,
                             bool ignore_empty) {
  return split_string_by_range(input_string, delimiter, delimiter,
                               ignore_empty);
}

// ****************************************************************************
// ****************************** String List *********************************
// ****************************************************************************

struct StringList {
  char **strings;
  int count;
  int capacity;
};

StringList *string_list_create(void) {
  StringList *string_list = malloc_or_die(sizeof(StringList));
  string_list->strings =
      malloc_or_die(sizeof(char *) * STRING_LIST_INITIAL_CAPACITY);
  string_list->count = 0;
  string_list->capacity = STRING_LIST_INITIAL_CAPACITY;
  return string_list;
}

void string_list_add_string(StringList *string_list, const char *str) {
  if (string_list->count == string_list->capacity) {
    string_list->strings = realloc_or_die(
        string_list->strings, sizeof(char *) * string_list->capacity * 2);
    string_list->capacity *= 2;
  }

  string_list->strings[string_list->count] = string_duplicate(str);
  string_list->count++;
}

int string_list_get_count(const StringList *string_list) {
  return string_list->count;
}

const char *string_list_get_string(const StringList *string_list, int index) {
  if (index < 0 || index >= string_list->count) {
    log_fatal("string index out of range: %d", index);
  }
  return string_list->strings[index];
}

void string_list_destroy(StringList *string_list) {
  if (!string_list) {
    return;
  }
  for (int i = 0; i < string_list->count; i++) {
    free(string_list->strings[i]);
  }
  free(string_list->strings);
  free(string_list);
}

// ****************************************************************************
// ************************* String Miscellaneous *****************************
// ****************************************************************************

bool has_prefix(const char *pre, const char *str) {
  return strncmp(pre, str, string_length(pre)) == 0;
}

bool has_iprefix(const char *pre, const char *str) {
  return strncasecmp(pre, str, string_length(pre)) == 0;
}

bool has_suffix(const char *suf, const char *str) {
  size_t suf_len = string_length(suf);
  size_t str_len = string_length(str);
  if (suf_len > str_len) {
    return false;
  }
  return strncmp(suf, str + str_len - suf_len, suf_len) == 0;
}

// Raises a fatal error if str is null
bool is_string_empty_or_whitespace(const char *str) {
  if (!str) {
    log_fatal("unexpected null string when checking for whitespace or empty");
    // unreachable return, but silences static analyzer warnings
    return false;
  }
  while (*str != '\0') {
    if (!isspace((unsigned char)*str)) {
      return false;
    }
    str++;
  }
  return true;
}

bool is_string_empty_or_null(const char *str) {
  if (!str) {
    return true;
  }
  return strings_equal(str, "");
}

bool strings_equal(const char *str1, const char *str2) {
  if (!str1 && !str2) {
    return true;
  }
  if (!str1 || !str2) {
    return false;
  }
  return strcmp(str1, str2) == 0;
}

bool strings_iequal(const char *str1, const char *str2) {
  if (!str1 && !str2) {
    return true;
  }
  if (!str1 || !str2) {
    return false;
  }
  return strcasecmp(str1, str2) == 0;
}

// Returns false for negative numbers
// since they contain a '-' character
bool is_all_digits_or_empty(const char *str) {
  while (*str != '\0') {
    if (!isdigit((unsigned char)*str)) {
      return false;
    }
    str++;
  }
  return true;
}

bool contains_digit(const char *str) {
  while (*str != '\0') {
    if (isdigit((unsigned char)*str)) {
      return true;
    }
    str++;
  }
  return false;
}

bool has_substring(const char *str, const char *pattern) {
  // If the pattern is empty or both strings are equal, return true
  if (is_string_empty_or_null(pattern) || strings_equal(str, pattern)) {
    return true;
  }

  // Check if pattern exists in str using strstr function
  const char *ptr = strstr(str, pattern);
  return (ptr != NULL);
}

bool is_url(const char *content) {
  return strstr(content, "://") != NULL || strstr(content, ".com") != NULL;
}

size_t string_length(const char *str) {
  if (!str) {
    log_fatal("cannot get the length of a null string");
    // unreachable return, but silences static analyzer warnings
    return 0;
  }
  return strlen(str);
}

char *string_duplicate_internal(const char *str) {
  char *duplicate = malloc_or_die(sizeof(char) * (string_length(str) + 1));
  strncpy(duplicate, str, string_length(str));
  duplicate[string_length(str)] = '\0';
  return duplicate;
}

char *string_duplicate_allow_null(const char *str) {
  if (!str) {
    return NULL;
  }
  return string_duplicate_internal(str);
}

char *string_duplicate(const char *str) {
  if (!str) {
    log_fatal("cannot duplicate null string");
    // unreachable return, but silences static analyzer warnings
    return NULL;
  }
  return string_duplicate_internal(str);
}

char *empty_string(void) { return string_duplicate(""); }

char *get_substring(const char *input_string, int start_index, int end_index) {
  if (!input_string) {
    log_fatal("cannot get substring of null string");
  }

  int input_length = (int)string_length(input_string);

  if (start_index < 0 || end_index < start_index ||
      start_index > input_length || end_index > input_length) {
    log_fatal(
        "cannot get substring for invalid bounds: string_length is %d, bounds "
        "are %d to %d\n",
        input_length, start_index, end_index);
  }

  int substring_length = (end_index - start_index);

  char *result = malloc_or_die(sizeof(char) * (substring_length + 1));

  strncpy(result, input_string + start_index, substring_length);
  result[substring_length] = '\0';

  return result;
}

char *iso_8859_1_to_utf8(const char *iso_8859_1_string) {
  if (!iso_8859_1_string) {
    return NULL;
  }

  size_t iso_len = string_length(iso_8859_1_string);
  char *utf8_string = (char *)malloc_or_die(
      (iso_len * 4 + 1) *
      sizeof(char)); // UTF-8 can be up to 4 times longer than ISO-8859-1

  char *iso_8859_1_string_pointer = (char *)iso_8859_1_string;
  char *utf8_string_pointer = utf8_string;

  while (*iso_8859_1_string_pointer != '\0') {
    if (*iso_8859_1_string_pointer < 0) {
      // Handle non-ASCII characters
      *utf8_string_pointer++ =
          (char)(0xC0 | (*(unsigned char *)iso_8859_1_string_pointer >> 6));
      *utf8_string_pointer++ =
          (char)(0x80 | (*iso_8859_1_string_pointer & 0x3F));
    } else {
      // Handle ASCII characters
      *utf8_string_pointer++ = *iso_8859_1_string_pointer;
    }
    iso_8859_1_string_pointer++;
  }

  *utf8_string_pointer = '\0'; // Null-terminate the UTF-8 string

  return utf8_string;
}

char *get_dirpath_from_filepath(const char *filepath) {
  // Make a copy of the input filepath because strtok modifies the string
  char *filepath_copy = string_duplicate(filepath);
  assert(filepath_copy);
  // Find the last occurrence of the '/' or '\' character
  char *last_slash = strrchr(filepath_copy, '/');
  char *last_backslash = strrchr(filepath_copy, '\\');
  char *last_separator =
      last_slash > last_backslash ? last_slash : last_backslash;

  if (last_separator != NULL) {
    // Terminate the string at the last separator to get the directory path
    *(last_separator + 1) = '\0';
  } else {
    // If there's no separator, the input is considered invalid for this
    // function
    log_fatal("cannot find directory path from filepath: %s", filepath);
  }

  // Allocate memory for the directory path to return
  char *directory = string_duplicate(filepath_copy);
  free(filepath_copy); // Free the copy as it's no longer needed

  return directory;
}

char *cut_off_after_last_char(const char *str, char ch) {
  const char *pos = strrchr(str, ch);

  if (pos == NULL) {
    return string_duplicate(str);
  }

  size_t new_len = pos - str;

  char *new_str = (char *)malloc_or_die(new_len + 1);

  strncpy(new_str, str, new_len);
  new_str[new_len] = '\0';

  return new_str;
}

char *cut_off_after_first_char(const char *str, char ch) {
  const char *pos = strchr(str, ch);

  if (pos == NULL) {
    return string_duplicate(str);
  }

  size_t new_len = pos - str;

  char *new_str = (char *)malloc_or_die(new_len + 1);

  strncpy(new_str, str, new_len);
  new_str[new_len] = '\0';

  return new_str;
}

char *insert_before_dot(const char *str, const char *insert) {
  const char *dot_position =
      strrchr(str, '.'); // Find the last occurrence of '.'
  size_t str_len = string_length(str);
  size_t insert_len = string_length(insert);

  // Calculate the new length of the string
  size_t new_len = str_len + insert_len;
  char *new_str = (char *)malloc_or_die(new_len + 1);
  if (!new_str) {
    return NULL; // Handle allocation failure
  }

  if (dot_position) {
    // Copy the part before the dot
    size_t prefix_len = dot_position - str;
    memcpy(new_str, str, prefix_len);

    // Insert the new content
    memcpy(new_str + prefix_len, insert, insert_len);

    // Copy the part from the dot onwards
    memcpy(new_str + prefix_len + insert_len, dot_position,
           str_len - prefix_len);

    // Null terminate
    new_str[new_len] = '\0';
  } else {
    // If there's no dot, concatenate the original string and the insert string
    memcpy(new_str, str, str_len);
    memcpy(new_str + str_len, insert, insert_len);
    new_str[new_len] = '\0';
  }

  return new_str;
}

char *to_lower_case(const char *content) {
  if (!content) {
    return NULL;
  }
  size_t len = strlen(content);
  char *lower_content = (char *)malloc_or_die(len + 1);
  for (size_t i = 0; i < len; ++i) {
    lower_content[i] = (char)tolower((unsigned char)content[i]);
  }
  lower_content[len] = '\0';
  return lower_content;
}

char *replace_whitespace_with_underscore(const char *str) {
  if (str == NULL) {
    return NULL;
  }
  size_t len = strlen(str);
  char *new_str = (char *)malloc_or_die(len + 1);
  for (size_t i = 0; i < len; i++) {
    if (isspace((unsigned char)str[i])) {
      new_str[i] = '_';
    } else {
      new_str[i] = str[i];
    }
  }
  new_str[len] = '\0';
  return new_str;
}

const char *get_base_filename(const char *filepath) {
  // Find the last occurrence of the Unix directory separator
  const char *base_filename_unix = strrchr(filepath, '/');
  // Find the last occurrence of the Windows directory separator
  const char *base_filename_win = strrchr(filepath, '\\');

  // Use the appropriate separator position
  const char *base_filename = base_filename_unix > base_filename_win
                                  ? base_filename_unix
                                  : base_filename_win;

  // If the directory separator is not found, return the original filepath
  if (base_filename == NULL) {
    base_filename = filepath;
  } else {
    // Move past the directory separator to get the base filename
    base_filename++;
  }

  return base_filename;
}

bool matches_trim_condition(const char input_c, const char trim_c,
                            bool trim_whitespace) {
  return (isspace(input_c) && trim_whitespace) ||
         (input_c == trim_c && !trim_whitespace);
}

void trim_internal(char *str, const char c, bool trim_whitespace) {
  if (!str) {
    return;
  }

  char *ptr = str;
  size_t len = strlen(ptr);
  if (len == 0) {
    return;
  }

  while (len - 1 > 0 &&
         matches_trim_condition(ptr[len - 1], c, trim_whitespace)) {
    ptr[--len] = 0;
  }

  while (*ptr && matches_trim_condition(*ptr, c, trim_whitespace)) {
    ++ptr;
    --len;
  }

  memmove(str, ptr, len + 1);
}

void trim_whitespace(char *str) { trim_internal(str, 0, true); }

void trim_char(char *str, const char c) { trim_internal(str, c, false); }

int string_to_int(const char *str, ErrorStack *error_stack) {
  const char *str_copy = str;
  char *endptr;
  str = str + strspn(str, STRING_CONV_ACCEPT_CHARS);
  long int result = strtol(str, &endptr, 10);
  str = endptr;
  str = str + strspn(str, STRING_CONV_ACCEPT_CHARS);
  if (*str != '\0') {
    error_stack_push(error_stack, ERROR_STATUS_STRING_TO_INT_CONVERSION_FAILED,
                     get_formatted_string(
                         "string to int conversion failed for '%s'", str_copy));
    result = 0;
  }
  return (int)result;
}

uint64_t string_to_uint64(const char *str, ErrorStack *error_stack) {
  const char *str_copy = str;
  char *endptr;
  str = str + strspn(str, STRING_CONV_ACCEPT_CHARS);
  while (*str != '\0' && isspace(*str)) {
    str++;
  }
  if (*str == '-') {
    error_stack_push(
        error_stack, ERROR_STATUS_FOUND_SIGN_FOR_UNSIGNED_INT,
        get_formatted_string("negative sign not allowed in string to unsigned "
                             "integer conversion for '%s'",
                             str_copy));
    return 0;
  }
  uint64_t result = strtoull(str, &endptr, 10);
  str = endptr;
  str = str + strspn(str, STRING_CONV_ACCEPT_CHARS);
  if (*str != '\0') {
    error_stack_push(
        error_stack, ERROR_STATUS_STRING_TO_INT_CONVERSION_FAILED,
        get_formatted_string(
            "string to unsigned integer conversion failed for '%s'", str_copy));
  }
  return result;
}

double string_to_double(const char *str, ErrorStack *error_stack) {
  const char *str_copy = str;
  char *endptr;
  const char *trimmed_str = str + strspn(str, STRING_CONV_ACCEPT_CHARS);
  double result = strtod(trimmed_str, &endptr);
  if (*endptr != '\0' && !isspace(*endptr)) {
    error_stack_push(
        error_stack, ERROR_STATUS_STRING_TO_DOUBLE_CONVERSION_FAILED,
        get_formatted_string("string to decimal conversion failed for '%s'",
                             str_copy));
    result = 0;
  }
  return result;
}

char *json_unescape_string(const char *json_string) {
  size_t input_len = string_length(json_string);
  char *unescaped = malloc_or_die(input_len + 1);
  char *dst = unescaped;
  const char *src = json_string;

  while (*src) {
    if (*src == '\\' && *(src + 1)) {
      switch (*(src + 1)) {
      case 'n':
        *dst++ = '\n';
        src += 2;
        break;
      case 't':
        *dst++ = '\t';
        src += 2;
        break;
      case 'r':
        *dst++ = '\r';
        src += 2;
        break;
      case '\\':
        *dst++ = '\\';
        src += 2;
        break;
      case '"':
        *dst++ = '"';
        src += 2;
        break;
      case '/':
        *dst++ = '/';
        src += 2;
        break;
      case 'b':
        *dst++ = '\b';
        src += 2;
        break;
      case 'f':
        *dst++ = '\f';
        src += 2;
        break;
      default:
        *dst++ = *src++;
        break; // Copy the backslash if unknown escape
      }
    } else {
      *dst++ = *src++;
    }
  }
  *dst = '\0';

  return unescaped;
}

char *get_process_output(const char *cmd) {
  FILE *pipe = popen_or_die(cmd, "r");

  StringBuilder *content_builder = string_builder_create();
  char *buffer = NULL;
  size_t buffer_size = 0;

  while (getline_ignore_carriage_return(&buffer, &buffer_size, pipe) != -1) {
    string_builder_add_string(content_builder, buffer);
  }

  free(buffer);
  int status = pclose(pipe);
  char *output = NULL;

  if (status == 0) {
    output = string_builder_dump(content_builder, NULL);
  }

  string_builder_destroy(content_builder);
  return output; // Returns NULL if command failed
}

// ****************************************************************************
// ***************************** String Grid **********************************
// ****************************************************************************

struct StringGrid {
  int rows;
  int cols;
  int col_padding;
  int *max_col_widths;
  char **cells;
};

StringGrid *string_grid_create(int rows, int cols, int col_padding) {
  StringGrid *string_grid = malloc_or_die(sizeof(StringGrid));
  string_grid->rows = rows;
  string_grid->cols = cols;
  string_grid->col_padding = col_padding;
  string_grid->max_col_widths = calloc_or_die(cols, sizeof(int));
  string_grid->cells =
      calloc_or_die((size_t)rows * (size_t)cols, sizeof(char *));
  return string_grid;
}

void string_grid_destroy(StringGrid *string_grid) {
  if (!string_grid) {
    return;
  }
  for (int i = 0; i < string_grid->rows * string_grid->cols; i++) {
    free(string_grid->cells[i]);
  }
  free(string_grid->max_col_widths);
  free(string_grid->cells);
  free(string_grid);
}

int string_grid_get_cell_index(const StringGrid *string_grid, int row,
                               int col) {
  if (row < 0 || row >= string_grid->rows || col < 0 ||
      col >= string_grid->cols) {
    log_fatal("string grid cell index out of range: (%d, %d)", row, col);
  }
  return row * string_grid->cols + col;
}

// Takes ownership of the value
void string_grid_set_cell(StringGrid *string_grid, int row, int col,
                          char *value) {
  const int index = string_grid_get_cell_index(string_grid, row, col);
  free(string_grid->cells[index]);
  string_grid->cells[index] = value;
}

static void
string_builder_draw_horizontal_border(StringBuilder *string_builder,
                                      const StringGrid *string_grid) {
  string_builder_add_string(string_builder, "+");
  for (int c = 0; c < string_grid->cols; c++) {
    for (int i = 0; i < string_grid->max_col_widths[c]; i++) {
      string_builder_add_string(string_builder, "-");
    }
    string_builder_add_string(string_builder, "+");
  }
  string_builder_add_string(string_builder, "\n");
}

void string_builder_add_string_grid(StringBuilder *sb, const StringGrid *sg,
                                    const bool add_border) {
  for (int c = 0; c < sg->cols; c++) {
    sg->max_col_widths[c] = 0; // Initialize to 0 or 1
    for (int r = 0; r < sg->rows; r++) {
      const int index = string_grid_get_cell_index(sg, r, c);
      const char *cell_value = sg->cells[index];
      if (cell_value) {
        const size_t cell_value_length =
            string_length(cell_value) + sg->col_padding;
        if (cell_value_length > (size_t)sg->max_col_widths[c]) {
          sg->max_col_widths[c] = (int)cell_value_length;
        }
      }
    }
    if (sg->max_col_widths[c] < 1) {
      sg->max_col_widths[c] = 1;
    }
  }

  if (add_border) {
    string_builder_draw_horizontal_border(sb, sg);
  }

  for (int r = 0; r < sg->rows; r++) {
    if (add_border) {
      string_builder_add_string(sb, "|");
    }

    for (int c = 0; c < sg->cols; c++) {
      const int index = string_grid_get_cell_index(sg, r, c);
      const char *cell_value = sg->cells[index];
      if (!cell_value) {
        cell_value = "";
      }

      string_builder_add_formatted_string(sb, "%-*s", sg->max_col_widths[c],
                                          cell_value);

      if (add_border) {
        string_builder_add_string(sb, "|");
      }
    }

    string_builder_add_string(sb, "\n");

    if (add_border) {
      string_builder_draw_horizontal_border(sb, sg);
    }
  }
}