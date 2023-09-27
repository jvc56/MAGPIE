#include <ctype.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "klv.h"
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

int prefix(const char *pre, const char *str) {
  return strncmp(pre, str, strlen(pre)) == 0;
}

void write_user_visible_letter_to_end_of_buffer(
    char *dest, LetterDistribution *letter_distribution, uint8_t ml) {

  char human_letter[MAX_LETTER_CHAR_LENGTH];
  machine_letter_to_human_readable_letter(letter_distribution, ml,
                                          human_letter);
  for (size_t i = 0; i < strlen(human_letter); i++) {
    sprintf(dest + strlen(dest), "%c", human_letter[i]);
  }
}

void write_rack_to_end_of_buffer(char *dest,
                                 LetterDistribution *letter_distribution,
                                 Rack *rack) {
  for (int i = 0; i < (rack->array_size); i++) {
    for (int k = 0; k < rack->array[i]; k++) {
      write_user_visible_letter_to_end_of_buffer(dest, letter_distribution, i);
    }
  }
}

int contains_all_whitespace(const char *str) {
  while (*str) {
    if (!isspace((unsigned char)*str)) {
      return 0; // Found a non-whitespace character
    }
    str++;
  }
  return 1; // All characters are whitespace
}

char *concatenate_strings(const char *str1, const char *str2) {
  // Check for invalid inputs
  if (str1 == NULL || str2 == NULL) {
    return NULL;
  }

  // Calculate the lengths of the input strings
  size_t len1 = strlen(str1);
  size_t len2 = strlen(str2);

  // Allocate memory for the concatenated string plus one additional byte for
  // the null terminator
  char *result = (char *)malloc((len1 + len2 + 1) * sizeof(char));

  if (result == NULL) {
    return NULL; // Memory allocation failed
  }

  // Copy the first string into the result
  strcpy(result, str1);

  // Concatenate the second string to the result
  strcat(result, str2);

  return result;
}