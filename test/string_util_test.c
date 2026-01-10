#include "../src/util/string_util.h"
#include "test_util.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

static void test_gicr_single_line(const char *data, const char *expected) {
  FILE *f = fmemopen((void *)data, strlen(data), "r");
  assert(f);

  char *line = NULL;
  size_t cap = 0;

  getline_ignore_carriage_return(&line, &cap, f);

  assert_strings_equal(line, expected);

  free(line);
  fclose(f);
}

static void test_gicr_multiple_lines(const char *data, const char **expected,
                                     size_t count) {
  FILE *f = fmemopen((void *)data, strlen(data), "r");
  assert(f);

  char *line = NULL;
  size_t cap = 0;

  for (size_t i = 0; i < count; i++) {
    getline_ignore_carriage_return(&line, &cap, f);
    assert_strings_equal(line, expected[i]);
  }

  free(line);
  fclose(f);
}

void test_string_splitter(const char *input, const char delimiter,
                          bool ignore_empty, int expected_number_of_items,
                          const char **expected_items) {
  StringSplitter *string_splitter =
      split_string(input, delimiter, ignore_empty);
  assert(string_splitter_get_number_of_items(string_splitter) ==
         expected_number_of_items);
  for (int i = 0; i < expected_number_of_items; i++) {
    assert_strings_equal(string_splitter_get_item(string_splitter, i),
                         expected_items[i]);
  }
  string_splitter_destroy(string_splitter);
}

void test_whitespace_string_splitter(const char *input, bool ignore_empty,
                                     int expected_number_of_items,
                                     const char **expected_items) {
  StringSplitter *string_splitter =
      split_string_by_whitespace(input, ignore_empty);
  assert(string_splitter_get_number_of_items(string_splitter) ==
         expected_number_of_items);
  for (int i = 0; i < expected_number_of_items; i++) {
    assert_strings_equal(string_splitter_get_item(string_splitter, i),
                         expected_items[i]);
  }
  string_splitter_destroy(string_splitter);
}

void test_string_util(void) {
  // One char string splitter
  test_string_splitter("", ',', false, 1, (const char *[]){""});
  test_string_splitter("", ',', true, 0, (const char *[]){});
  test_string_splitter(",", ',', false, 2, (const char *[]){"", ""});
  test_string_splitter(",", ',', true, 0, (const char *[]){});
  test_string_splitter("a", ',', false, 1, (const char *[]){"a"});
  test_string_splitter("a", ',', true, 1, (const char *[]){"a"});
  test_string_splitter("a,", ',', false, 2, (const char *[]){"a", ""});
  test_string_splitter("a,", ',', true, 1, (const char *[]){"a"});
  test_string_splitter(",a", ',', false, 2, (const char *[]){"", "a"});
  test_string_splitter(",a", ',', true, 1, (const char *[]){"a"});
  test_string_splitter(",,,,", ',', false, 5,
                       (const char *[]){"", "", "", "", ""});
  test_string_splitter(",,,,", ',', true, 0, (const char *[]){});
  test_string_splitter("aaaaa", ',', false, 1, (const char *[]){"aaaaa"});
  test_string_splitter("aaaaa", ',', true, 1, (const char *[]){"aaaaa"});
  test_string_splitter("a,b", ',', false, 2, (const char *[]){"a", "b"});
  test_string_splitter("a,b", ',', true, 2, (const char *[]){"a", "b"});
  test_string_splitter(",a,", ',', false, 3, (const char *[]){"", "a", ""});
  test_string_splitter(",a,", ',', true, 1, (const char *[]){"a"});
  test_string_splitter(",,,,abc", ',', false, 5,
                       (const char *[]){"", "", "", "", "abc"});
  test_string_splitter(",,,,abc", ',', true, 1, (const char *[]){"abc"});
  test_string_splitter("abc,,,", ',', false, 4,
                       (const char *[]){"abc", "", "", ""});
  test_string_splitter("abc,,,", ',', true, 1, (const char *[]){"abc"});
  test_string_splitter(",,,,abc,,,", ',', false, 8,
                       (const char *[]){"", "", "", "", "abc", "", "", ""});
  test_string_splitter(",,,,abc,,,", ',', true, 1, (const char *[]){"abc"});
  test_string_splitter(
      "def,,a,,h,,ijk,xyz", ',', false, 8,
      (const char *[]){"def", "", "a", "", "h", "", "ijk", "xyz"});
  test_string_splitter("def,,a,,h,,ijk,xyz", ',', true, 5,
                       (const char *[]){"def", "a", "h", "ijk", "xyz"});
  test_string_splitter(
      ",def,,a,,h,,ijk,xyz,", ',', false, 10,
      (const char *[]){"", "def", "", "a", "", "h", "", "ijk", "xyz", ""});
  test_string_splitter(",def,,a,,h,,ijk,xyz,", ',', true, 5,
                       (const char *[]){"def", "a", "h", "ijk", "xyz"});

  // White space string splitter
  // Ensure nonwhitespace isn't a delimiter
  test_whitespace_string_splitter("", false, 1, (const char *[]){""});
  test_whitespace_string_splitter("", true, 0, (const char *[]){});
  test_whitespace_string_splitter(",", false, 1, (const char *[]){","});
  test_whitespace_string_splitter(",", true, 1, (const char *[]){","});
  test_whitespace_string_splitter("a,", false, 1, (const char *[]){"a,"});
  test_whitespace_string_splitter("a,", true, 1, (const char *[]){"a,"});
  test_whitespace_string_splitter(",a", false, 1, (const char *[]){",a"});
  test_whitespace_string_splitter(",a", true, 1, (const char *[]){",a"});
  test_whitespace_string_splitter(",,,,", false, 1, (const char *[]){",,,,"});
  test_whitespace_string_splitter("a,b", false, 1, (const char *[]){"a,b"});
  // Delimiter whitespace
  test_whitespace_string_splitter(" ", false, 2, (const char *[]){"", ""});
  test_whitespace_string_splitter(" ", true, 0, (const char *[]){});
  test_whitespace_string_splitter("a", false, 1, (const char *[]){"a"});
  test_whitespace_string_splitter("a", true, 1, (const char *[]){"a"});
  test_whitespace_string_splitter("a\t", false, 2, (const char *[]){"a", ""});
  test_whitespace_string_splitter("a ", true, 1, (const char *[]){"a"});
  test_whitespace_string_splitter("\ra", false, 2, (const char *[]){"", "a"});
  test_whitespace_string_splitter("\na", true, 1, (const char *[]){"a"});
  test_whitespace_string_splitter("\r\n\v\f", false, 5,
                                  (const char *[]){"", "", "", "", ""});
  test_whitespace_string_splitter("\r\n\v\f\t", true, 0, (const char *[]){});
  test_whitespace_string_splitter("aaaaa", false, 1, (const char *[]){"aaaaa"});
  test_whitespace_string_splitter("aaaaa", true, 1, (const char *[]){"aaaaa"});
  test_whitespace_string_splitter("a\nb", false, 2, (const char *[]){"a", "b"});
  test_whitespace_string_splitter("a b", true, 2, (const char *[]){"a", "b"});
  test_whitespace_string_splitter(" a ", false, 3,
                                  (const char *[]){"", "a", ""});
  test_whitespace_string_splitter("\ta ", true, 1, (const char *[]){"a"});
  test_whitespace_string_splitter(" \r \tabc", false, 5,
                                  (const char *[]){"", "", "", "", "abc"});
  test_whitespace_string_splitter(" \f\v\nabc", true, 1,
                                  (const char *[]){"abc"});
  test_whitespace_string_splitter("abc   ", false, 4,
                                  (const char *[]){"abc", "", "", ""});
  test_whitespace_string_splitter("abc   ", true, 1, (const char *[]){"abc"});
  test_whitespace_string_splitter(
      "\f\v  abc\t\r ", false, 8,
      (const char *[]){"", "", "", "", "abc", "", "", ""});
  test_whitespace_string_splitter("\f\v  abc\t\r ", true, 1,
                                  (const char *[]){"abc"});
  test_whitespace_string_splitter(
      "def  a  h  ijk xyz", false, 8,
      (const char *[]){"def", "", "a", "", "h", "", "ijk", "xyz"});
  test_whitespace_string_splitter(
      "def  a  h  ijk xyz", true, 5,
      (const char *[]){"def", "a", "h", "ijk", "xyz"});
  test_whitespace_string_splitter(
      " def  a  h  ijk xyz ", false, 10,
      (const char *[]){"", "def", "", "a", "", "h", "", "ijk", "xyz", ""});
  test_whitespace_string_splitter(
      " def  a  h  ijk xyz ", true, 5,
      (const char *[]){"def", "a", "h", "ijk", "xyz"});

  // Test trim
  char str1[] = "";
  trim_whitespace(str1);
  assert_strings_equal(str1, "");

  char str2[] = "   Hello, World   ";
  trim_whitespace(str2);
  assert_strings_equal(str2, "Hello, World");

  char str3[] = "   ";
  trim_whitespace(str3);
  assert_strings_equal(str3, "");

  char str4[] = "NoWhitespace";
  trim_whitespace(str4);
  assert_strings_equal(str4, "NoWhitespace");

  char str5[] = "   In    between   \n whitespace   ";
  trim_whitespace(str5);
  assert_strings_equal(str5, "In    between   \n whitespace");

  char str6[] = "##Hello, World##";
  trim_char(str6, '#');
  assert_strings_equal(str6, "Hello, World");

  char str7[] = "###";
  trim_char(str7, '#');
  assert_strings_equal(str7, "");

  char str8[] = "NoCharsToTrim";
  trim_char(str8, '#');
  assert_strings_equal(str8, "NoCharsToTrim");

  char str9[] = "";
  trim_char(str9, '#');
  assert_strings_equal(str9, "");

  char str10[] = ";;;In;;between;;semicolons;;;";
  trim_char(str10, ';');
  assert_strings_equal(str10, "In;;between;;semicolons");

  // Test removal of carriage returns

  test_string_splitter("a\r\nb", '\n', false, 2, (const char *[]){"a", "b"});
  test_string_splitter("a\r\nb\r\n", '\n', false, 3,
                       (const char *[]){"a", "b", ""});
  test_string_splitter("\r\na\r\nb\r\n", '\n', false, 4,
                       (const char *[]){"", "a", "b", ""});
  test_string_splitter("\na\r\nb\r", '\n', false, 3,
                       (const char *[]){"", "a", "b"});

  test_gicr_single_line("hello\n", "hello\n");
  test_gicr_single_line("hello\r\n", "hello\n");
  test_gicr_single_line("hello\r", "hello");
  test_gicr_single_line("\r\n", "\n");
  test_gicr_single_line("\r", "");
  test_gicr_single_line("hello", "hello");
  test_gicr_multiple_lines("a\r\nb\nc\rd",
                           (const char *[]){"a\n", "b\n", "c\rd"}, 3);
}