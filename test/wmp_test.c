#include "../src/compat/ctime.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/wmp_defs.h"
#include "../src/ent/bit_rack.h"
#include "../src/ent/data_filepaths.h"
#include "../src/ent/dictionary_word.h"
#include "../src/ent/game.h"
#include "../src/ent/kwg.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/player.h"
#include "../src/ent/wmp.h"
#include "../src/impl/config.h"
#include "../src/impl/kwg_maker.h"
#include "../src/impl/wmp_maker.h"
#include "../src/util/fileproxy.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

long get_file_size(const char *filename) {
  ErrorStack *error_stack = error_stack_create();
  FILE *stream = stream_from_filename(filename, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("wmp test failed to open wmp file: %s", filename);
  }
  error_stack_destroy(error_stack);
  fseek_or_die(stream, 0L, SEEK_END);
  const long file_size = ftell(stream);
  fseek_or_die(stream, 0L, SEEK_SET);
  return file_size;
}

void write_words_to_testdata_wmp(const DictionaryWordList *words,
                                 const LetterDistribution *ld,
                                 const char *wmp_filename) {
  Timer *timer = ctimer_create_monotonic();
  ctimer_start(timer);
  WMP *wmp = make_wmp_from_words(words, ld);
  double seconds_elapsed = ctimer_elapsed_seconds(timer);
  ctimer_destroy(timer);
  ErrorStack *error_stack = error_stack_create();
  wmp_write_to_file(wmp, wmp_filename, error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  const long file_size = get_file_size(wmp_filename);
  printf("wrote %ld bytes to %s in %f seconds\n", file_size, wmp_filename,
         seconds_elapsed);
  wmp_destroy(wmp);
}

void write_wmp_files(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  const LetterDistribution *ld = config_get_ld(config);
  Game *game = config_game_create(config);
  const Player *player = game_get_player(game, 0);
  const KWG *csw_kwg = player_get_kwg(player);
  DictionaryWordList *words = dictionary_word_list_create();
  kwg_write_words(csw_kwg, kwg_get_dawg_root_node_index(csw_kwg), words, NULL);
  write_words_to_testdata_wmp(words, ld, "testdata/lexica/CSW21.wmp");
  DictionaryWordList *csw2to7 = dictionary_word_list_create();
  DictionaryWordList *csw3and15 = dictionary_word_list_create();
  for (int word_idx = 0; word_idx < dictionary_word_list_get_count(words);
       word_idx++) {
    const DictionaryWord *word = dictionary_word_list_get_word(words, word_idx);
    const int length = dictionary_word_get_length(word);
    if (length <= 7) {
      dictionary_word_list_add_word(csw2to7, dictionary_word_get_word(word),
                                    length);
    }
    if (length == 3 || length == 15) {
      dictionary_word_list_add_word(csw3and15, dictionary_word_get_word(word),
                                    length);
    }
  }
  dictionary_word_list_destroy(words);
  write_words_to_testdata_wmp(csw2to7, ld, "testdata/lexica/CSW21_2to7.wmp");
  dictionary_word_list_destroy(csw2to7);
  write_words_to_testdata_wmp(csw3and15, ld, "testdata/lexica/CSW21_3or15.wmp");
  dictionary_word_list_destroy(csw3and15);
  game_destroy(game);
  config_destroy(config);
}

void test_short_and_long_words(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  const LetterDistribution *ld = config_get_ld(config);

  WMP *wmp = wmp_create_or_die("testdata", "CSW21_3or15");

  MachineLetter *buffer = malloc_or_die(wmp->max_word_lookup_bytes);
  BitRack inq = string_to_bit_rack(ld, "INQ");
  int bytes_written = wmp_write_words_to_buffer(wmp, &inq, 3, buffer);
  assert(bytes_written == 3);
  assert_word_in_buffer(buffer, "QIN", ld, 0, 3);

  BitRack vv_blank = string_to_bit_rack(ld, "VV?");
  bytes_written = wmp_write_words_to_buffer(wmp, &vv_blank, 3, buffer);
  assert(bytes_written == 3);
  assert_word_in_buffer(buffer, "VAV", ld, 0, 3);

  BitRack q_blank_blank = string_to_bit_rack(ld, "Q??");
  bytes_written = wmp_write_words_to_buffer(wmp, &q_blank_blank, 3, buffer);
  assert(bytes_written == 15);

  assert_word_in_buffer(buffer, "QAT", ld, 0, 3);
  assert_word_in_buffer(buffer, "QUA", ld, 1, 3);
  assert_word_in_buffer(buffer, "QIN", ld, 2, 3);
  assert_word_in_buffer(buffer, "QIS", ld, 3, 3);
  assert_word_in_buffer(buffer, "SUQ", ld, 4, 3);

  BitRack quarterbackin_double_blank =
      string_to_bit_rack(ld, "QUARTERBACKIN??");
  bytes_written =
      wmp_write_words_to_buffer(wmp, &quarterbackin_double_blank, 15, buffer);
  assert(bytes_written == 15);
  assert_word_in_buffer(buffer, "QUARTERBACKINGS", ld, 0, 15);

  wmp_destroy(wmp);
  free(buffer);
  config_destroy(config);
}

void check_all_wmp_result_sizes_fit_in_buffer(void) {
  Config *config = config_create_or_die("");
  ErrorStack *error_stack = error_stack_create();
  const char *data_paths = config_get_data_paths(config);
  StringList *wmp_files = data_filepaths_get_all_data_path_names(
      data_paths, DATA_FILEPATH_TYPE_WORDMAP, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(wmp_files != NULL);

  const int count = string_list_get_count(wmp_files);
  for (int file_idx = 0; file_idx < count; file_idx++) {
    const char *filename = string_list_get_string(wmp_files, file_idx);
    WMP *wmp = (WMP *)malloc_or_die(sizeof(WMP));
    wmp_load_from_filename(wmp, /*wmp_name=*/"", filename, error_stack);
    assert(error_stack_is_empty(error_stack));
    assert(wmp->max_word_lookup_bytes <= WMP_RESULT_BUFFER_SIZE);
    wmp_destroy(wmp);
  }

  error_stack_destroy(error_stack);
  string_list_destroy(wmp_files);
  config_destroy(config);
}

void test_wmp(void) {
  write_wmp_files();
  test_short_and_long_words();
  check_all_wmp_result_sizes_fit_in_buffer();
}
