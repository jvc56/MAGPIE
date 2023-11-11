#include <ctype.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>

#include "constants.h"
#include "klv.h"
#include "log.h"
#include "move.h"
#include "rack.h"

double get_leave_value_for_move(const KLV *klv, Move *move, Rack *rack) {
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

bool is_decimal_number(const char *str) {
  if (!str || *str == '\0') {
    return false; // Empty string is not a valid decimal number
  }

  int i = 0;
  bool has_decimal_point = false;

  if (str[i] == '\0') {
    return false; // No digits in the string
  }

  while (str[i] != '\0') {
    if (isdigit(str[i])) {
      i++;
    } else if (str[i] == '.' && !has_decimal_point) {
      has_decimal_point = true;
      i++;
    } else {
      return false; // Invalid character in the string
    }
  }

  return true;
}

int char_to_int(char c) { return c - '0'; }

int string_to_int(const char *str) {
  char *endptr;
  long int result = strtol(str, &endptr, 10);
  if (*endptr != '\0') {
    log_fatal("string to int conversion failed for >%s<\n", str);
  }
  return (int)result;
}

uint64_t string_to_uint64(const char *str) {
  char *endptr;
  uint64_t result = strtoull(str, &endptr, 10);
  if (*endptr != '\0') {
    log_fatal("string to uint64_t conversion failed for %s\n", str);
  }
  return result;
}

double string_to_double(const char *str) {
  if (!str) {
    log_fatal("called string_to_double on NULL string\n");
  }
  return strtof(str, NULL);
}