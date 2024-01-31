#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/validated_move.h"
#include "../ent/words.h"

#include "letter_distribution_string.h"

#include "../util/string_util.h"

char *validated_moves_get_phonies_string(const LetterDistribution *ld,
                                         ValidatedMoves *vms, int i) {
  Move *move = validated_moves_get_move(vms, i);
  FormedWords *fw = validated_moves_get_formed_words(vms, i);
  game_event_t move_type = move_get_type(move);
  StringBuilder *phonies_string_builder = create_string_builder();
  bool phonies_formed = false;
  if (move_type == GAME_EVENT_TILE_PLACEMENT_MOVE) {
    int number_of_words = formed_words_get_num_words(fw);
    for (int i = 0; i < number_of_words; i++) {
      if (!formed_words_get_word_valid(fw, i)) {
        for (int mli = 0; mli < formed_words_get_word_length(fw, i); mli++) {
          string_builder_add_user_visible_letter(
              ld, phonies_string_builder,
              formed_words_get_word_letter(fw, i, mli));
        }
        if (i < number_of_words - 1) {
          string_builder_add_string(phonies_string_builder, ",");
        }
        phonies_formed = true;
      }
    }
  }
  char *phonies_string = NULL;
  if (phonies_formed) {
    phonies_string = string_builder_dump(phonies_string_builder, 0);
  }
  destroy_string_builder(phonies_string_builder);
  return phonies_string;
}