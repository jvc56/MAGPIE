#include <assert.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "../../src/ent/wmp.h"

#include "../../src/impl/config.h"
#include "../../src/impl/gameplay.h"
#include "../../src/impl/kwg_maker.h"
#include "../../src/impl/wmp_maker.h"

#include "../../src/str/rack_string.h"

#include "test_util.h"

int get_file_size(const char *filename) {
  FILE *stream = stream_from_filename(filename);
  fseek(stream, 0L, SEEK_END);
  const int file_size = ftell(stream);
  fseek(stream, 0L, SEEK_SET);
  return file_size;
}

void write_words_to_testdata_wmp(const DictionaryWordList *words,
                                 const char *wmp_filename) {
  const clock_t start = clock();
  WMP *wmp = make_wmp_from_words(words);
  const clock_t end = clock();
  wmp_write_to_file(wmp, wmp_filename);
  const int file_size = get_file_size(wmp_filename);
  printf("wrote %d bytes to %s in %f seconds\n", file_size, wmp_filename,
         (double)(end - start) / CLOCKS_PER_SEC);
  wmp_destroy(wmp);
}

void write_wmp_files(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  Game *game = config_game_create(config);
  const Player *player = game_get_player(game, 0);
  const KWG *csw_kwg = player_get_kwg(player);
  DictionaryWordList *words = dictionary_word_list_create();
  kwg_write_words(csw_kwg, kwg_get_dawg_root_node_index(csw_kwg), words, NULL);
  write_words_to_testdata_wmp(words, "testdata/lexica/CSW21.wmp");
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
  write_words_to_testdata_wmp(csw2to7, "testdata/lexica/CSW21_2to7.wmp");
  dictionary_word_list_destroy(csw2to7);
  write_words_to_testdata_wmp(csw3and15, "testdata/lexica/CSW21_3or15.wmp");
  dictionary_word_list_destroy(csw3and15);
  game_destroy(game);
  config_destroy(config);
}

void time_wmp_buffer_writes(Game *game, WMP *wmp) {
  const LetterDistribution *ld = game_get_ld(game);
  uint8_t *buffer = malloc_or_die(wmp->max_word_lookup_bytes);
  int bytes_written = 0;
  int lookups = 0;
  int total_bytes_written = 0;
  const clock_t start = clock();
  for (int i = 0; i < 1e3; i++) {
    game_reset(game);
    draw_to_full_rack(game, 0);
    Player *player = game_get_player(game, 0);

    Rack *full_rack = player_get_rack(player);
    BitRack full_bit_rack = bit_rack_create_from_rack(ld, full_rack);

    BitRackPowerSet set;
    bit_rack_power_set_init(&set, &full_bit_rack);
    for (int size = 2; size <= RACK_SIZE; size++) {
      const int offset = bit_rack_get_combination_offset(size);
      const int count = set.count_by_size[size];
      for (int idx_for_size = 0; idx_for_size < count; idx_for_size++) {
        BitRack *bit_rack = &set.subracks[offset + idx_for_size];
        bytes_written = wmp_write_words_to_buffer(wmp, bit_rack, size, buffer);
        assert(bytes_written % size == 0);
        total_bytes_written += bytes_written;
        lookups++;
      }
    }
  }
  const clock_t end = clock();
  free(buffer);
  printf("performed %d lookups, %d bytes written in %f seconds\n", lookups,
         total_bytes_written, (double)(end - start) / CLOCKS_PER_SEC);
}

void time_wmp_existence_checks(Game *game, WMP *wmp) {
  const LetterDistribution *ld = game_get_ld(game);
  int lookups = 0;
  int num_with_solution = 0;
  const clock_t start = clock();
  for (int i = 0; i < 1e3; i++) {
    game_reset(game);
    draw_to_full_rack(game, 0);
    Player *player = game_get_player(game, 0);

    Rack *full_rack = player_get_rack(player);
    BitRack full_bit_rack = bit_rack_create_from_rack(ld, full_rack);

    BitRackPowerSet set;
    bit_rack_power_set_init(&set, &full_bit_rack);
    for (int size = 2; size <= RACK_SIZE; size++) {
      const int offset = bit_rack_get_combination_offset(size);
      const int count = set.count_by_size[size];
      for (int idx_for_size = 0; idx_for_size < count; idx_for_size++) {
        BitRack *bit_rack = &set.subracks[offset + idx_for_size];
        const bool has_solution = wmp_has_word(wmp, bit_rack, size);
        num_with_solution += has_solution;
        lookups++;
      }
    }
  }
  const clock_t end = clock();
  printf("performed %d lookups, %d having solutions in %f seconds\n",
         lookups, num_with_solution, (double)(end - start) / CLOCKS_PER_SEC);
}

void benchmark_csw_wmp(void) {
  WMP *wmp = wmp_create("testdata", "CSW21");
  assert(wmp != NULL);

  Config *config = config_create_or_die("set -lex CSW21");
  Game *game = config_game_create(config);
  time_wmp_buffer_writes(game, wmp);
  time_wmp_existence_checks(game, wmp);

  game_destroy(game);
  config_destroy(config);
  wmp_destroy(wmp);
}

void test_short_and_long_words(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  const LetterDistribution *ld = config_get_ld(config);

  WMP *wmp = wmp_create("testdata", "CSW21_3or15");
  assert(wmp != NULL);

  uint8_t *buffer = malloc_or_die(wmp->max_word_lookup_bytes);
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

void test_wmp(void) {
  write_wmp_files();
  benchmark_csw_wmp();
  test_short_and_long_words();
}