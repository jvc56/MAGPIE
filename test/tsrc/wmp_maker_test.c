#include <assert.h>

#include "../../src/def/wmp_defs.h"

#include "../../src/ent/wmp.h"

#include "../../src/impl/kwg_maker.h"
#include "../../src/impl/wmp_maker.h"

#include "test_util.h"

void test_make_wmp_from_words(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  const LetterDistribution *ld = config_get_ld(config);
  Game *game = config_game_create(config);
  const Player *player = game_get_player(game, 0);
  const KWG *csw_kwg = player_get_kwg(player);
  DictionaryWordList *words = dictionary_word_list_create();
  kwg_write_words(csw_kwg, kwg_get_dawg_root_node_index(csw_kwg), words, NULL);

  DictionaryWordList *q_words_2to8 = dictionary_word_list_create();
  const MachineLetter q = ld_hl_to_ml(ld, "Q");
  for (int word_idx = 0; word_idx < dictionary_word_list_get_count(words);
       word_idx++) {
    const DictionaryWord *word = dictionary_word_list_get_word(words, word_idx);
    const uint8_t length = dictionary_word_get_length(word);
    if (length > 8) {
      continue;
    }
    for (int letter_idx = 0; letter_idx < length; letter_idx++) {
      if (dictionary_word_get_word(word)[letter_idx] == q) {
        dictionary_word_list_add_word(q_words_2to8,
                                      dictionary_word_get_word(word), length);
        break;
      }
    }
  }
  dictionary_word_list_destroy(words);

  WMP *wmp = make_wmp_from_words(q_words_2to8, ld);
  assert(wmp != NULL);
  assert(wmp->version == WMP_VERSION);
  assert(wmp->board_dim == BOARD_DIM);
  assert(wmp->max_word_lookup_bytes == 8 * 47); // EIQSTU??

  MachineLetter *buffer = malloc_or_die(wmp->max_word_lookup_bytes);

  BitRack iq = string_to_bit_rack(ld, "IQ");
  int bytes_written = wmp_write_words_to_buffer(wmp, &iq, 2, buffer);
  assert(bytes_written == 2);
  assert_word_in_buffer(buffer, "QI", ld, 0, 2);

  BitRack q_blank = string_to_bit_rack(ld, "Q?");
  bytes_written = wmp_write_words_to_buffer(wmp, &q_blank, 2, buffer);
  assert(bytes_written == 2);
  assert_word_in_buffer(buffer, "QI", ld, 0, 2);

  BitRack square_blank = string_to_bit_rack(ld, "SQUARE?");
  bytes_written = wmp_write_words_to_buffer(wmp, &square_blank, 7, buffer);
  assert(bytes_written == 7 * 13);
  assert_word_in_buffer(buffer, "BARQUES", ld, 0, 7);
  assert_word_in_buffer(buffer, "SQUARED", ld, 1, 7);
  assert_word_in_buffer(buffer, "QUAERES", ld, 2, 7);
  assert_word_in_buffer(buffer, "QUASHER", ld, 3, 7);
  assert_word_in_buffer(buffer, "QUAKERS", ld, 4, 7);
  assert_word_in_buffer(buffer, "MARQUES", ld, 5, 7);
  assert_word_in_buffer(buffer, "MASQUER", ld, 6, 7);
  assert_word_in_buffer(buffer, "SQUARER", ld, 7, 7);
  assert_word_in_buffer(buffer, "SQUARES", ld, 8, 7);
  assert_word_in_buffer(buffer, "QUAREST", ld, 9, 7);
  assert_word_in_buffer(buffer, "QUARTES", ld, 10, 7);
  assert_word_in_buffer(buffer, "QUATRES", ld, 11, 7);
  assert_word_in_buffer(buffer, "QUAVERS", ld, 12, 7);

  BitRack quebracho = string_to_bit_rack(ld, "QUEBRACHO");
  bytes_written = wmp_write_words_to_buffer(wmp, &quebracho, 9, buffer);
  assert(bytes_written == 0);

  BitRack quu_blank_blank = string_to_bit_rack(ld, "QUU??");
  bytes_written = wmp_write_words_to_buffer(wmp, &quu_blank_blank, 5, buffer);
  assert(bytes_written == 5 * 4);
  assert_word_in_buffer(buffer, "QUEUE", ld, 0, 5);
  assert_word_in_buffer(buffer, "USQUE", ld, 1, 5);
  assert_word_in_buffer(buffer, "TUQUE", ld, 2, 5);
  assert_word_in_buffer(buffer, "QUIPU", ld, 3, 5);

  free(buffer);
  wmp_destroy(wmp);
  dictionary_word_list_destroy(q_words_2to8);
  game_destroy(game);
  config_destroy(config);
}

void test_wmp_maker(void) { test_make_wmp_from_words(); }
