#include "rack_draw.h"

#include "../def/gameplay_defs.h"
#include "../def/rack_defs.h"
#include "../ent/bag.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

// Returns true if there are enough tiles in bag and player_rack
// to draw rack_to_draw.
bool rack_is_drawable(const Game *game, const int player_index,
                      const Rack *rack_to_draw) {
  const Bag *bag = game_get_bag(game);
  const Rack *player_rack =
      player_get_rack(game_get_player(game, player_index));
  const uint16_t dist_size = rack_get_dist_size(player_rack);
  for (int i = 0; i < dist_size; i++) {
    if (bag_get_letter(bag, i) + rack_get_letter(player_rack, i) <
        rack_get_letter(rack_to_draw, i)) {
      return false;
    }
  }
  return true;
}

// Draws a nonrandom set of letters specified by rack_to_draw from the
// bag to the rack. Assumes the rack is empty.
// Returns true on success.
// Return false when the rack letters are not in the bag.
bool draw_rack_from_bag(const Game *game, const int player_index,
                        const Rack *rack_to_draw) {
  if (rack_get_dist_size(rack_to_draw) == 0) {
    // Rack is effectively NULL
    return true;
  }
  // Every path that puts an externally supplied rack on a player (CGP, GCG
  // replay, known racks) comes through here. The callers validate the size
  // and report it; this is the backstop for the ones that forget.
  assert(rack_get_total_letters(rack_to_draw) <= RACK_SIZE);
  Bag *bag = game_get_bag(game);
  Rack *player_rack = player_get_rack(game_get_player(game, player_index));
  int player_draw_index = game_get_player_draw_index(game, player_index);
  rack_copy(player_rack, rack_to_draw);
  const uint16_t dist_size = rack_get_dist_size(player_rack);
  for (int i = 0; i < dist_size; i++) {
    const uint16_t rack_number_of_letter = rack_get_letter(player_rack, i);
    for (uint16_t j = 0; j < rack_number_of_letter; j++) {
      if (!bag_draw_letter(bag, i, player_draw_index)) {
        return false;
      }
    }
  }
  return true;
}

// Draws a nonrandom set of letters specified by rack_string from the
// bag to the rack. Assumes the rack is empty.
// Returns the number of letters drawn on success, or one of the
// DRAW_RACK_STRING_* codes declared in def/gameplay_defs.h.
int draw_rack_string_from_bag(const Game *game, const int player_index,
                              const char *rack_string) {
  const LetterDistribution *ld = game_get_ld(game);
  Rack player_rack_copy;
  rack_set_dist_size_and_reset(&player_rack_copy, ld_get_size(ld));
  const int number_of_letters_set =
      rack_set_to_string(ld, &player_rack_copy, rack_string);
  if (number_of_letters_set == -1) {
    // rack_set_to_string reports a malformed string as -1.
    return DRAW_RACK_STRING_MALFORMED;
  }
  if (number_of_letters_set > RACK_SIZE) {
    // rack_set_to_string bounds the string by MAX_RACK_SIZE, which is far
    // larger than RACK_SIZE. Racks wider than RACK_SIZE must be rejected
    // here: move generation indexes several RACK_SIZE-sized arrays by the
    // rack size, so drawing one would be memory-unsafe rather than merely
    // wrong.
    return DRAW_RACK_STRING_TOO_MANY_LETTERS;
  }
  if (!rack_is_drawable(game, player_index, &player_rack_copy)) {
    return DRAW_RACK_STRING_NOT_IN_BAG;
  }
  draw_rack_from_bag(game, player_index, &player_rack_copy);
  return number_of_letters_set;
}
