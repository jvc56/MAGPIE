#include "word_playability_test.h"

#include "../src/def/game_history_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/dictionary_word.h"
#include "../src/ent/game.h"
#include "../src/ent/kwg.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/words.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/kwg_maker.h"
#include "../src/impl/word_playability.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

// Sets the player-on-turn's rack and returns the deterministic best play's
// formed-word full-lexicon indices (and the main word's index).
static int collect_best_play_words(Game *game,
                                   const WordPlayabilityContext *ctx,
                                   const char *rack_str, uint32_t *out_indices,
                                   uint32_t *out_main_index) {
  const LetterDistribution *ld = game_get_ld(game);
  const int player_index = game_get_player_on_turn_index(game);
  Rack *rack = player_get_rack(game_get_player(game, player_index));
  rack_set_to_string(ld, rack, rack_str);

  MoveList *move_list = move_list_create(10000);
  const Move *best = get_top_equity_move(game, move_list);
  assert(move_get_type(best) == GAME_EVENT_TILE_PLACEMENT_MOVE);

  FormedWords *fw = formed_words_create(game_get_board(game), best);
  const int num_words = formed_words_get_num_words(fw);
  int count = 0;
  for (int i = 0; i < num_words; i++) {
    const uint32_t idx = word_playability_context_word_index(
        ctx, formed_words_get_word(fw, i), formed_words_get_word_length(fw, i));
    assert(idx != UINT32_MAX); // every formed word is in the full lexicon
    out_indices[count++] = idx;
  }
  // The main word is the last entry in FormedWords.
  *out_main_index = out_indices[count - 1];
  formed_words_destroy(fw);
  move_list_destroy(move_list);
  return count;
}

// Polish (OSPS49) flex: a large alphabet (no 64-bit BitRack) exercises full-
// lexicon enumeration and word indexing over wider machine letters. Every
// enumerated word must index back to a valid id, and a non-word must not.
static void test_word_playability_polish_indexing(void) {
  Config *config = config_create_or_die("set -lex OSPS49 -wmp false");
  Game *game = config_game_create(config);
  const KWG *kwg = player_get_kwg(game_get_player(game, 0));

  WordPlayabilityContext *ctx = word_playability_context_create(
      kwg, NULL, game_get_ld(game),
      string_duplicate("playability_polish_test.csv"),
      WORD_PLAYABILITY_SORT_COUNT);
  const uint32_t num_words = word_playability_context_num_words(ctx);
  assert(num_words > 100000); // OSPS49 is a large lexicon

  DictionaryWordList *all_words = dictionary_word_list_create();
  kwg_write_words(kwg, kwg_get_dawg_root_node_index(kwg), all_words, NULL);
  const int total = dictionary_word_list_get_count(all_words);
  assert((uint32_t)total == num_words);

  // Sample across the enumeration; each enumerated word must map to a valid id.
  for (int word_idx = 0; word_idx < total; word_idx += total / 50 + 1) {
    const DictionaryWord *word =
        dictionary_word_list_get_word(all_words, word_idx);
    const uint32_t id = word_playability_context_word_index(
        ctx, dictionary_word_get_word(word), dictionary_word_get_length(word));
    assert(id != UINT32_MAX);
    assert(id < num_words);
  }

  // Pick a genuine two-tile non-word (one absent from the enumerated list) and
  // confirm it indexes as absent. Found by flagging the real two-letter words,
  // then taking the first combo that is not one -- language-agnostic.
  const int ld_size = ld_get_size(game_get_ld(game));
  bool *two_letter_exists = calloc((size_t)ld_size * ld_size, sizeof(bool));
  for (int word_idx = 0; word_idx < total; word_idx++) {
    const DictionaryWord *word =
        dictionary_word_list_get_word(all_words, word_idx);
    if (dictionary_word_get_length(word) == 2) {
      const MachineLetter *mls = dictionary_word_get_word(word);
      two_letter_exists[(int)mls[0] * ld_size + (int)mls[1]] = true;
    }
  }
  MachineLetter not_a_word[2] = {0, 0};
  bool found_non_word = false;
  for (int first = 1; first < ld_size && !found_non_word; first++) {
    for (int second = 1; second < ld_size; second++) {
      if (!two_letter_exists[first * ld_size + second]) {
        not_a_word[0] = (MachineLetter)first;
        not_a_word[1] = (MachineLetter)second;
        found_non_word = true;
        break;
      }
    }
  }
  free(two_letter_exists);
  assert(found_non_word);
  assert(word_playability_context_word_index(ctx, not_a_word, 2) == UINT32_MAX);

  dictionary_word_list_destroy(all_words);
  word_playability_context_destroy(ctx);
  game_destroy(game);
  config_destroy(config);
}

void test_word_playability(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -k1 CSW21 -k2 CSW21 -s1 equity -s2 equity -r1 all -r2 "
      "all -numplays 1");
  Game *game = config_game_create(config);
  const LetterDistribution *ld = game_get_ld(game);
  const KWG *kwg = player_get_kwg(game_get_player(game, 0));

  // --- count + penalty (no small set) ---
  WordPlayabilityContext *ctx = word_playability_context_create(
      kwg, NULL, ld, string_duplicate("playability_test.csv"),
      WORD_PLAYABILITY_SORT_COUNT);
  assert(word_playability_context_num_words(ctx) > 100000); // CSW21 ~280k words
  assert(!word_playability_context_has_bonus(ctx));

  // Deterministic opening: a fixed rack on the empty board has a fixed best
  // play. (Movegen reads the rack; the bag state is irrelevant here.)
  uint32_t formed[RACK_SIZE + 1];
  uint32_t main_index = 0;
  const int num_formed =
      collect_best_play_words(game, ctx, "AEINRST", formed, &main_index);
  assert(num_formed >= 1);

  WordPlayabilityCounts *counts = word_playability_counts_create(ctx);
  word_playability_counts_add_position(counts, ctx, game);

  // Every word the best play forms is counted once; the removal penalty is a
  // sound (non-negative) equity loss.
  for (int i = 0; i < num_formed; i++) {
    assert(word_playability_counts_get_count(counts, formed[i]) == 1);
    // Removal penalty is a sound (non-negative) equity loss. It can be 0 even
    // for the best play: with rack AEINRST the opening has several equal-equity
    // anagram bingos, so removing any one word loses nothing (play another).
    assert(word_playability_counts_get_penalty(counts, formed[i]) >= 0);
    assert(word_playability_counts_get_bonus(counts, formed[i]) == 0);
  }

  // Determinism: analyzing the same position again exactly doubles the counts.
  word_playability_counts_add_position(counts, ctx, game);
  assert(word_playability_counts_get_count(counts, main_index) == 2);

  // Merge: a second table folded in adds its counts.
  WordPlayabilityCounts *other = word_playability_counts_create(ctx);
  word_playability_counts_add_position(other, ctx, game);
  word_playability_counts_merge(counts, other);
  assert(word_playability_counts_get_count(counts, main_index) == 3);
  word_playability_counts_destroy(other);

  word_playability_counts_destroy(counts);
  word_playability_context_destroy(ctx);

  // --- bonus (with a smaller lexicon as the small set) ---
  ErrorStack *error_stack = error_stack_create();
  KWG *small_kwg =
      kwg_create(config_get_data_paths(config), "NWL20", error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(small_kwg != NULL);
  error_stack_destroy(error_stack);
  WordPlayabilityContext *bonus_ctx = word_playability_context_create(
      kwg, small_kwg, ld, string_duplicate("playability_bonus_test.csv"),
      WORD_PLAYABILITY_SORT_BONUS);
  assert(word_playability_context_has_bonus(bonus_ctx));

  WordPlayabilityCounts *bonus_counts =
      word_playability_counts_create(bonus_ctx);
  word_playability_counts_add_position(bonus_counts, bonus_ctx, game);
  // Bonus is a non-negative equity gain for every word.
  for (uint32_t i = 0; i < word_playability_context_num_words(bonus_ctx); i++) {
    assert(word_playability_counts_get_bonus(bonus_counts, i) >= 0);
  }
  word_playability_counts_destroy(bonus_counts);
  word_playability_context_destroy(bonus_ctx);
  kwg_destroy(small_kwg);

  game_destroy(game);
  config_destroy(config);

  test_word_playability_polish_indexing();
}
