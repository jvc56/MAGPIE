#include <ctype.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "klv.h"
#include "log.h"
#include "move.h"
#include "rack.h"

double get_leave_value_for_move(KLV *klv, Move *move, Rack *rack) {
  int valid_tiles = move->tiles_length;
  if (move->move_type == GAME_EVENT_EXCHANGE) {
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

void *malloc_or_die(size_t size) {
  void *uncasted_pointer = malloc(size);
  if (!uncasted_pointer) {
    log_fatal("failed to malloc size of %lu.\n", size);
  }
  return uncasted_pointer;
}

void *realloc_or_die(void *realloc_target, size_t size) {
  void *realloc_result = realloc(realloc_target, size);
  if (!realloc_result) {
    log_fatal("failed to realloc %p with size of %lu.\n", realloc_target, size);
  }
  return realloc_result;
}