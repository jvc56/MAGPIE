#include "wmp_test.h"

#include "../src/compat/ctime.h"
#include "../src/def/bit_rack_defs.h"
#include "../src/def/board_defs.h"
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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  fclose_or_die(stream);
  return file_size;
}

void write_words_to_testdata_wmp(const DictionaryWordList *words,
                                 const LetterDistribution *ld,
                                 const char *wmp_filename) {
  Timer timer;
  ctimer_start(&timer);
  WMP *wmp = make_wmp_from_words(words, ld, 0);
  double seconds_elapsed = ctimer_elapsed_seconds(&timer);
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

// The entry for bit_rack found the way the file lays the words out: through
// the bucket starts, by the full BitRack mix.
static const WMPEntry *get_entry_by_bucket_starts(const WMPForLength *wfl,
                                                  const BitRack *bit_rack) {
  const uint32_t bucket_idx =
      bit_rack_get_bucket_index(bit_rack, wfl->num_word_buckets);
  for (uint32_t entry_idx = wfl->word_bucket_starts[bucket_idx];
       entry_idx < wfl->word_bucket_starts[bucket_idx + 1]; entry_idx++) {
    const BitRack key =
        wmp_entry_read_bit_rack(&wfl->word_map_entries[entry_idx]);
    if (bit_rack_equals(&key, bit_rack)) {
      return &wfl->word_map_entries[entry_idx];
    }
  }
  return NULL;
}

static void assert_same_entry(const WMPEntry *expected, const WMPEntry *got) {
  if (expected == NULL) {
    assert(got == NULL);
    return;
  }
  assert(got != NULL);
  assert(memcmp(expected, got, sizeof(WMPEntry)) == 0);
}

// Every blankless entry is found through the filter and the index, and racks
// one tile away from an entry get the same answer from the index as from the
// bucket starts.
void test_word_index_matches_bucket_starts(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  const int ld_size = ld_get_size(config_get_ld(config));
  WMP *wmp = wmp_create_or_die("testdata", "CSW21");
  for (int length = 2; length <= BOARD_DIM; length++) {
    const WMPForLength *wfl = &wmp->wfls[length];
    for (uint32_t entry_idx = 0; entry_idx < wfl->num_word_entries;
         entry_idx++) {
      const WMPEntry *entry = &wfl->word_map_entries[entry_idx];
      const BitRack key = wmp_entry_read_bit_rack(entry);
      assert_same_entry(entry, wfl_get_word_entry(wfl, &key));
      assert_same_entry(entry, wfl_get_present_word_entry(wfl, &key));
      if (entry_idx % 16 != 0) {
        continue;
      }
      for (int from_ml = 1; from_ml < ld_size; from_ml++) {
        if (bit_rack_get_letter(&key, from_ml) == 0) {
          continue;
        }
        for (int to_ml = 1; to_ml < ld_size; to_ml++) {
          if (to_ml == from_ml) {
            continue;
          }
          BitRack neighbor = key;
          bit_rack_take_letter(&neighbor, from_ml);
          bit_rack_add_letter(&neighbor, to_ml);
          assert_same_entry(get_entry_by_bucket_starts(wfl, &neighbor),
                            wfl_get_word_entry(wfl, &neighbor));
        }
      }
    }
  }
  wmp_destroy(wmp);
  config_destroy(config);
}

// The inline byte count as a loop: the words end with the last word slot
// whose last byte is nonzero, testing every slot after the first from the
// last one down.
static int reference_number_of_inlined_bytes(const WMPEntry *entry,
                                             int word_length) {
  int num_bytes = (WMP_INLINE_VALUE_BYTES / word_length) * word_length;
  while (num_bytes > word_length) {
    if (entry->bucket_or_inline[num_bytes - 1] != 0) {
      break;
    }
    num_bytes -= word_length;
  }
  return num_bytes;
}

// The blank expansion as letter loops over a rack written in place, with
// each blankless rack found through the bucket starts and its words copied
// at their exact length.
static int reference_write_blankless_words(const WMPForLength *wfl,
                                           const BitRack *bit_rack,
                                           int word_length, uint8_t *buffer) {
  const WMPEntry *entry = get_entry_by_bucket_starts(wfl, bit_rack);
  if (entry == NULL) {
    return 0;
  }
  if (wmp_entry_is_inlined(entry)) {
    const int num_bytes = reference_number_of_inlined_bytes(entry, word_length);
    memcpy(buffer, entry->bucket_or_inline, num_bytes);
    return num_bytes;
  }
  const int num_bytes = (int)entry->num_words * word_length;
  memcpy(buffer, wfl->word_letters + entry->word_start, num_bytes);
  return num_bytes;
}

static int reference_write_blanks(const WMPEntry *entry,
                                  const WMPForLength *wfl, BitRack *bit_rack,
                                  int word_length, int min_ml,
                                  uint8_t *buffer) {
  int bytes_written = 0;
  bit_rack_set_letter_count(bit_rack, BLANK_MACHINE_LETTER, 0);
  for (int ml = min_ml; ml < BIT_RACK_MAX_ALPHABET_SIZE; ml++) {
    if ((entry->blank_letters & (1U << ml)) != 0) {
      bit_rack_add_letter(bit_rack, (MachineLetter)ml);
      bytes_written += reference_write_blankless_words(
          wfl, bit_rack, word_length, buffer + bytes_written);
      bit_rack_take_letter(bit_rack, (MachineLetter)ml);
    }
  }
  bit_rack_set_letter_count(bit_rack, BLANK_MACHINE_LETTER, 1);
  return bytes_written;
}

static int reference_write_double_blanks(const WMPEntry *entry,
                                         const WMPForLength *wfl,
                                         BitRack *bit_rack, int word_length,
                                         uint8_t *buffer) {
  int bytes_written = 0;
  bit_rack_set_letter_count(bit_rack, BLANK_MACHINE_LETTER, 1);
  for (int ml = 1; ml < BIT_RACK_MAX_ALPHABET_SIZE; ml++) {
    if ((entry->first_blank_letters & (1U << ml)) != 0) {
      bit_rack_add_letter(bit_rack, (MachineLetter)ml);
      const WMPEntry *blank_entry = wfl_get_blank_entry(wfl, bit_rack);
      if (blank_entry != NULL) {
        bytes_written +=
            reference_write_blanks(blank_entry, wfl, bit_rack, word_length, ml,
                                   buffer + bytes_written);
      }
      bit_rack_take_letter(bit_rack, (MachineLetter)ml);
    }
  }
  bit_rack_set_letter_count(bit_rack, BLANK_MACHINE_LETTER, 2);
  return bytes_written;
}

static void assert_expansion_matches_reference(const WMP *wmp,
                                               const WMPEntry *entry,
                                               int length, uint8_t *buffer,
                                               uint8_t *expected) {
  const WMPForLength *wfl = &wmp->wfls[length];
  const BitRack bit_rack = wmp_entry_read_bit_rack(entry);
  BitRack reference_rack = bit_rack;
  const int expected_bytes =
      bit_rack_get_letter(&bit_rack, BLANK_MACHINE_LETTER) == 1
          ? reference_write_blanks(entry, wfl, &reference_rack, length, 1,
                                   expected)
          : reference_write_double_blanks(entry, wfl, &reference_rack, length,
                                          expected);
  // The loops leave the rack as they found it.
  assert(bit_rack_equals(&reference_rack, &bit_rack));
  const int bytes_written =
      wmp_entry_write_words_to_buffer(entry, wmp, &bit_rack, length, buffer);
  assert(bytes_written == expected_bytes);
  assert(memcmp(buffer, expected, (size_t)bytes_written) == 0);
}

// Every single and double blank entry of every length expands to the same
// bytes as the reference loops.
static void test_blank_expansion_matches_reference(void) {
  WMP *wmp = wmp_create_or_die("testdata", "CSW21");
  uint8_t *buffer = malloc_or_die(wmp->max_word_lookup_bytes);
  uint8_t *expected = malloc_or_die(wmp->max_word_lookup_bytes);
  for (int length = 2; length <= BOARD_DIM; length++) {
    const WMPForLength *wfl = &wmp->wfls[length];
    for (uint32_t entry_idx = 0; entry_idx < wfl->num_blank_entries;
         entry_idx++) {
      assert_expansion_matches_reference(
          wmp, &wfl->blank_map_entries[entry_idx], length, buffer, expected);
    }
    for (uint32_t entry_idx = 0; entry_idx < wfl->num_double_blank_entries;
         entry_idx++) {
      assert_expansion_matches_reference(
          wmp, &wfl->double_blank_map_entries[entry_idx], length, buffer,
          expected);
    }
  }
  free(expected);
  free(buffer);
  wmp_destroy(wmp);
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
  test_word_index_matches_bucket_starts();
  test_blank_expansion_matches_reference();
  check_all_wmp_result_sizes_fit_in_buffer();
}
