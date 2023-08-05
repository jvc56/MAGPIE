#include <string.h>

#include "constants.h"
#include "klv.h"
#include "kwg.h"
#include "move.h"
#include "rack.h"
#include "util.h"

double get_leave_value_for_move(KLV *klv, Move *move, Rack *rack) {
  int valid_tiles = move->tiles_length;
  if (move->move_type == MOVE_TYPE_EXCHANGE) {
    valid_tiles = move->tiles_played;
  }
  for (int i = 0; i < valid_tiles; i++) {
    if (move->tiles[i] != PLAYED_THROUGH_MARKER) {
      if (is_blanked(move->tiles[i])) {
        take_letter_from_rack(rack, BLANK_MACHINE_LETTER);
      } else {
        take_letter_from_rack(rack, move->tiles[i]);
      }
    }
  }
  return get_leave_value(klv, rack);
}

int prefix(const char *pre, const char *str) {
  return strncmp(pre, str, strlen(pre)) == 0;
}
