#include <assert.h>

#include "test_util.h"

#include "../src/string_util.h"

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
  destroy_string_splitter(string_splitter);
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
  destroy_string_splitter(string_splitter);
}

void test_string_util() {
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
}