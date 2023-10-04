#include <assert.h>

#include "test_util.h"

#include "../src/string_util.h"

void test_string_util() {
  StringSplitter *empty_string_splitter = split_string("", ',');
  assert(string_splitter_get_number_of_items(empty_string_splitter) == 1);
  assert_strings_equal(string_splitter_get_item(empty_string_splitter, 0), "");
  destroy_string_splitter(empty_string_splitter);

  StringSplitter *empty_ignored_string_splitter =
      split_string_ignore_empty("", ',');
  assert(string_splitter_get_number_of_items(empty_ignored_string_splitter) ==
         0);
  destroy_string_splitter(empty_ignored_string_splitter);

  StringSplitter *single_delimiter_string_splitter = split_string(",", ',');
  assert(string_splitter_get_number_of_items(
             single_delimiter_string_splitter) == 2);
  assert_strings_equal(
      string_splitter_get_item(single_delimiter_string_splitter, 0), "");
  assert_strings_equal(
      string_splitter_get_item(single_delimiter_string_splitter, 1), "");
  destroy_string_splitter(single_delimiter_string_splitter);

  StringSplitter *single_delimiter_ignore_empty_string_splitter =
      split_string_ignore_empty(",", ',');
  assert(string_splitter_get_number_of_items(
             single_delimiter_ignore_empty_string_splitter) == 0);
  destroy_string_splitter(single_delimiter_ignore_empty_string_splitter);

  StringSplitter *single_char_string_splitter = split_string("a", ',');
  assert(string_splitter_get_number_of_items(single_char_string_splitter) == 1);
  assert_strings_equal(string_splitter_get_item(single_char_string_splitter, 0),
                       "a");
  destroy_string_splitter(single_char_string_splitter);

  StringSplitter *single_char_ignore_empty_string_splitter =
      split_string_ignore_empty("a", ',');
  assert(string_splitter_get_number_of_items(
             single_char_ignore_empty_string_splitter) == 1);
  assert_strings_equal(
      string_splitter_get_item(single_char_ignore_empty_string_splitter, 0),
      "a");
  destroy_string_splitter(single_char_ignore_empty_string_splitter);

  StringSplitter *char_and_delim_string_splitter = split_string("a,", ',');
  assert(string_splitter_get_number_of_items(char_and_delim_string_splitter) ==
         2);
  assert_strings_equal(
      string_splitter_get_item(char_and_delim_string_splitter, 0), "a");
  assert_strings_equal(
      string_splitter_get_item(char_and_delim_string_splitter, 1), "");
  destroy_string_splitter(char_and_delim_string_splitter);

  StringSplitter *char_and_delim_ignore_empty_string_splitter =
      split_string_ignore_empty("a,", ',');
  assert(string_splitter_get_number_of_items(
             char_and_delim_ignore_empty_string_splitter) == 1);
  assert_strings_equal(
      string_splitter_get_item(char_and_delim_ignore_empty_string_splitter, 0),
      "a");
  destroy_string_splitter(char_and_delim_ignore_empty_string_splitter);

  StringSplitter *delim_and_char_string_splitter = split_string(",a", ',');
  assert(string_splitter_get_number_of_items(delim_and_char_string_splitter) ==
         2);
  assert_strings_equal(
      string_splitter_get_item(delim_and_char_string_splitter, 0), "");
  assert_strings_equal(
      string_splitter_get_item(delim_and_char_string_splitter, 1), "a");
  destroy_string_splitter(delim_and_char_string_splitter);

  StringSplitter *delim_and_char_ignore_empty_string_splitter =
      split_string_ignore_empty(",a", ',');
  assert(string_splitter_get_number_of_items(
             delim_and_char_ignore_empty_string_splitter) == 1);
  assert_strings_equal(
      string_splitter_get_item(delim_and_char_ignore_empty_string_splitter, 0),
      "a");
  destroy_string_splitter(delim_and_char_ignore_empty_string_splitter);

  StringSplitter *many_delim_string_splitter = split_string(",,,,", ',');
  assert(string_splitter_get_number_of_items(many_delim_string_splitter) == 5);
  assert_strings_equal(string_splitter_get_item(many_delim_string_splitter, 0),
                       "");
  assert_strings_equal(string_splitter_get_item(many_delim_string_splitter, 1),
                       "");
  assert_strings_equal(string_splitter_get_item(many_delim_string_splitter, 2),
                       "");
  assert_strings_equal(string_splitter_get_item(many_delim_string_splitter, 3),
                       "");
  assert_strings_equal(string_splitter_get_item(many_delim_string_splitter, 4),
                       "");
  destroy_string_splitter(many_delim_string_splitter);

  StringSplitter *many_delim_ignore_empty_string_splitter =
      split_string_ignore_empty(",,,,", ',');
  assert(string_splitter_get_number_of_items(
             many_delim_ignore_empty_string_splitter) == 0);
  destroy_string_splitter(many_delim_ignore_empty_string_splitter);

  StringSplitter *many_char_string_splitter = split_string("aaaaa", ',');
  assert(string_splitter_get_number_of_items(many_char_string_splitter) == 1);
  assert_strings_equal(string_splitter_get_item(many_char_string_splitter, 0),
                       "aaaaa");
  destroy_string_splitter(many_char_string_splitter);

  StringSplitter *many_char_ignore_empty_string_splitter =
      split_string_ignore_empty("aaaaa", ',');
  assert(string_splitter_get_number_of_items(
             many_char_ignore_empty_string_splitter) == 1);
  assert_strings_equal(
      string_splitter_get_item(many_char_ignore_empty_string_splitter, 0),
      "aaaaa");
  destroy_string_splitter(many_char_ignore_empty_string_splitter);

  StringSplitter *mix_1_string_splitter = split_string("a,b", ',');
  assert(string_splitter_get_number_of_items(mix_1_string_splitter) == 2);
  assert_strings_equal(string_splitter_get_item(mix_1_string_splitter, 0), "a");
  assert_strings_equal(string_splitter_get_item(mix_1_string_splitter, 1), "b");
  destroy_string_splitter(mix_1_string_splitter);

  StringSplitter *mix_1_ignore_empty_string_splitter =
      split_string_ignore_empty("a,b", ',');
  assert(string_splitter_get_number_of_items(
             mix_1_ignore_empty_string_splitter) == 2);
  assert_strings_equal(
      string_splitter_get_item(mix_1_ignore_empty_string_splitter, 0), "a");
  assert_strings_equal(
      string_splitter_get_item(mix_1_ignore_empty_string_splitter, 1), "b");
  destroy_string_splitter(mix_1_ignore_empty_string_splitter);

  StringSplitter *mix_2_string_splitter = split_string(",a,", ',');
  assert(string_splitter_get_number_of_items(mix_2_string_splitter) == 3);
  assert_strings_equal(string_splitter_get_item(mix_2_string_splitter, 0), "");
  assert_strings_equal(string_splitter_get_item(mix_2_string_splitter, 1), "a");
  assert_strings_equal(string_splitter_get_item(mix_2_string_splitter, 2), "");
  destroy_string_splitter(mix_2_string_splitter);

  StringSplitter *mix_2_ignore_empty_string_splitter =
      split_string_ignore_empty(",a,", ',');
  assert(string_splitter_get_number_of_items(
             mix_2_ignore_empty_string_splitter) == 1);
  assert_strings_equal(
      string_splitter_get_item(mix_2_ignore_empty_string_splitter, 0), "a");
  destroy_string_splitter(mix_2_ignore_empty_string_splitter);

  StringSplitter *mix_3_string_splitter = split_string(",,,,abc", ',');
  assert(string_splitter_get_number_of_items(mix_3_string_splitter) == 5);
  assert_strings_equal(string_splitter_get_item(mix_3_string_splitter, 0), "");
  assert_strings_equal(string_splitter_get_item(mix_3_string_splitter, 1), "");
  assert_strings_equal(string_splitter_get_item(mix_3_string_splitter, 2), "");
  assert_strings_equal(string_splitter_get_item(mix_3_string_splitter, 3), "");
  assert_strings_equal(string_splitter_get_item(mix_3_string_splitter, 4),
                       "abc");
  destroy_string_splitter(mix_3_string_splitter);

  StringSplitter *mix_3_ignore_empty_string_splitter =
      split_string_ignore_empty(",,,,abc", ',');
  assert(string_splitter_get_number_of_items(
             mix_3_ignore_empty_string_splitter) == 1);
  assert_strings_equal(
      string_splitter_get_item(mix_3_ignore_empty_string_splitter, 0), "abc");
  destroy_string_splitter(mix_3_ignore_empty_string_splitter);

  StringSplitter *mix_4_string_splitter = split_string("abc,,,", ',');
  assert(string_splitter_get_number_of_items(mix_4_string_splitter) == 4);
  assert_strings_equal(string_splitter_get_item(mix_4_string_splitter, 0),
                       "abc");
  assert_strings_equal(string_splitter_get_item(mix_4_string_splitter, 1), "");
  assert_strings_equal(string_splitter_get_item(mix_4_string_splitter, 2), "");
  assert_strings_equal(string_splitter_get_item(mix_4_string_splitter, 3), "");
  destroy_string_splitter(mix_4_string_splitter);

  StringSplitter *mix_4_ignore_empty_string_splitter =
      split_string_ignore_empty("abc,,,", ',');
  assert(string_splitter_get_number_of_items(
             mix_4_ignore_empty_string_splitter) == 1);
  assert_strings_equal(
      string_splitter_get_item(mix_4_ignore_empty_string_splitter, 0), "abc");
  destroy_string_splitter(mix_4_ignore_empty_string_splitter);

  StringSplitter *mix_5_string_splitter = split_string(",,,,abc,,,", ',');
  assert(string_splitter_get_number_of_items(mix_5_string_splitter) == 8);
  assert_strings_equal(string_splitter_get_item(mix_5_string_splitter, 0), "");
  assert_strings_equal(string_splitter_get_item(mix_5_string_splitter, 1), "");
  assert_strings_equal(string_splitter_get_item(mix_5_string_splitter, 2), "");
  assert_strings_equal(string_splitter_get_item(mix_5_string_splitter, 3), "");
  assert_strings_equal(string_splitter_get_item(mix_5_string_splitter, 4),
                       "abc");
  assert_strings_equal(string_splitter_get_item(mix_5_string_splitter, 5), "");
  assert_strings_equal(string_splitter_get_item(mix_5_string_splitter, 6), "");
  assert_strings_equal(string_splitter_get_item(mix_5_string_splitter, 7), "");
  destroy_string_splitter(mix_5_string_splitter);

  StringSplitter *mix_5_ignore_empty_string_splitter =
      split_string_ignore_empty(",,,,abc,,,", ',');
  assert(string_splitter_get_number_of_items(
             mix_5_ignore_empty_string_splitter) == 1);
  assert_strings_equal(
      string_splitter_get_item(mix_5_ignore_empty_string_splitter, 0), "abc");
  destroy_string_splitter(mix_5_ignore_empty_string_splitter);
}