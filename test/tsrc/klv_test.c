#include <assert.h>

#include "../../src/ent/dictionary_word.h"
#include "../../src/ent/klv.h"
#include "../../src/ent/letter_distribution.h"

#include "../../src/impl/config.h"
#include "../../src/impl/klv_csv.h"
#include "../../src/impl/kwg_maker.h"

#include "../../src/util/string_util.h"

#include "test_util.h"

void print_word_index_for_rack(const KLV *klv, const LetterDistribution *ld,
                               Rack *rack, const char *rack_str) {
  rack_set_to_string(ld, rack, rack_str);
  const int word_index = klv_get_word_index(klv, rack);
  printf("leave index and word count for >%s<: %d\n", rack_str, word_index);
}

void set_klv_leave_value(KLV *klv, const LetterDistribution *ld,
                         const char *rack_str, double value) {
  Rack *rack = rack_create(ld_get_size(ld));
  rack_set_to_string(ld, rack, rack_str);
  const int klv_word_index = klv_get_word_index(klv, rack);
  klv_set_indexed_leave_value(klv, klv_word_index, value);
  rack_destroy(rack);
}

void test_small_klv(void) {
  Config *config = config_create_or_die("set -lex CSW21 -ld english_small");
  const LetterDistribution *ld = config_get_ld(config);
  const char *data_path = "testdata";
  assert(ld_get_size(ld) == 3);

  KLV *small_klv = klv_create_empty(ld, "small");
  assert(klv_get_number_of_leaves(small_klv) == 11);

  set_klv_leave_value(small_klv, ld, "?", 1.0);
  set_klv_leave_value(small_klv, ld, "?A", 2.0);
  set_klv_leave_value(small_klv, ld, "?AAB", 3.0);
  set_klv_leave_value(small_klv, ld, "AAB", 4.0);

  klv_write_to_csv(small_klv, ld, data_path);

  char *leaves_filename = data_filepaths_get_readable_filename(
      data_path, small_klv->name, DATA_FILEPATH_TYPE_LEAVES);

  char *leaves_file_string = get_string_from_file(leaves_filename);

  free(leaves_filename);

  assert_strings_equal(leaves_file_string,
                       "?,1.000000\nA,0.000000\nB,0.000000\n?A,2.000000\n?B,0."
                       "000000\nAA,0.000000\nAB,0.000000\n?AA,0.000000\n?AB,0."
                       "000000\nAAB,4.000000\n?AAB,3.000000\n");

  free(leaves_file_string);
  klv_destroy(small_klv);
  config_destroy(config);
}

void test_normal_klv(void) {
  Config *config_normie = config_create_or_die("set -lex CSW21");

  const KWG *csw_kwg =
      players_data_get_kwg(config_get_players_data(config_normie), 0);
  DictionaryWordList *words = dictionary_word_list_create();
  kwg_write_words(csw_kwg, kwg_get_dawg_root_node_index(csw_kwg), words, NULL);

  for (int i = 0; i < 30000; i += 2000) {
    for (int j = 0; j < 5; j++) {
      const DictionaryWord *dw = dictionary_word_list_get_word(words, i + j);
      const uint8_t *w = dictionary_word_get_word(dw);
      printf("word at %d:", i);
      for (int k = 0; k < dictionary_word_get_length(dw); k++) {
        printf("%c", w[k] + 'A' - 1);
      }
      printf("\n");
    }
  }

  dictionary_word_list_destroy(words);

  const LetterDistribution *ld = config_get_ld(config_normie);
  const KLV *klv =
      players_data_get_klv(config_get_players_data(config_normie), 0);
  Rack *rack = rack_create(ld_get_size(ld));
  print_word_index_for_rack(klv, ld, rack, "?");
  print_word_index_for_rack(klv, ld, rack, "??");
  print_word_index_for_rack(klv, ld, rack, "??A");
  print_word_index_for_rack(klv, ld, rack, "??AA");
  print_word_index_for_rack(klv, ld, rack, "??AAA");
  print_word_index_for_rack(klv, ld, rack, "??AAAA");
  print_word_index_for_rack(klv, ld, rack, "??AAAB");
  print_word_index_for_rack(klv, ld, rack, "??AAAC");
  print_word_index_for_rack(klv, ld, rack, "??AAAD");
  print_word_index_for_rack(klv, ld, rack, "VWXYYZ");
  print_word_index_for_rack(klv, ld, rack, "WWXYYZ");
  print_word_index_for_rack(klv, ld, rack, "?");
  print_word_index_for_rack(klv, ld, rack, "?A");
  print_word_index_for_rack(klv, ld, rack, "?AB");
  rack_destroy(rack);
  config_destroy(config_normie);
}

void test_klv(void) {
  // FIXME: uncomment
  // test_normal_klv();
  test_small_klv();
}