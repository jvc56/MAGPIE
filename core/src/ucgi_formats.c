#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "letter_distribution.h"
#include "move.h"
#include "movegen.h"
#include "string_util.h"

void string_builder_add_ucgi_move(Move *move, Board *board,
                                  LetterDistribution *ld,
                                  StringBuilder *move_string_builder) {

  if (move->move_type != GAME_EVENT_PASS) {
    if (move->move_type == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      if (move->vertical) {
        string_builder_add_formatted_string(move_string_builder, "%c%d.",
                                            move->col_start + 'a',
                                            move->row_start + 1);
      } else {
        string_builder_add_formatted_string(move_string_builder, "%d%c.",
                                            move->row_start + 1,
                                            move->col_start + 'a');
      }
    } else {
      string_builder_add_string(move_string_builder, "ex.", 0);
    }

    int number_of_tiles_to_print = move->tiles_length;

    // FIXME: make sure tiles_length == tiles_played for exchanges
    // this is not true currently.
    if (move->move_type == GAME_EVENT_EXCHANGE) {
      number_of_tiles_to_print = move->tiles_played;
    }

    int ri = 0;
    int ci = 0;
    if (move->vertical) {
      ri = 1;
    } else {
      ci = 1;
    }

    for (int i = 0; i < number_of_tiles_to_print; i++) {
      uint8_t letter = move->tiles[i];
      if (letter == PLAYED_THROUGH_MARKER &&
          move->move_type == GAME_EVENT_TILE_PLACEMENT_MOVE) {
        int r = move->row_start + (ri * i);
        int c = move->col_start + (ci * i);
        letter = get_letter(board, r, c);
      }
      string_builder_add_user_visible_letter(ld, letter, 0,
                                             move_string_builder);
    }
  } else {
    string_builder_add_string(move_string_builder, "pass", 0);
  }
}

// to_move converts the UCGI move string to a move object. Modify the move
// object that is passed in.
void to_move(LetterDistribution *ld, char *desc, Move *move, Board *board) {
  if (strings_equal(desc, "pass")) {
    move->move_type = GAME_EVENT_PASS;
    move->score = 0;
    return;
  }
  char *token = strtok(desc, ".");
  if (strings_equal(token, "ex")) {
    move->move_type = GAME_EVENT_EXCHANGE;
    token = strtok(NULL, ".");
    char *tiles_exchanged = token;
    // move->tiles
    int ntiles =
        str_to_machine_letters(ld, tiles_exchanged, false, move->tiles);
    move->score = 0;
    move->tiles_length = ntiles;
    move->tiles_played = ntiles;
    token = strtok(NULL, ".");

    assert(token == NULL);

    return;
  }
  // Otherwise, it's a tile placement move. (Note, we don't handle anything else
  // yet)
  move->row_start = 0;
  move->col_start = 0;
  move->move_type = GAME_EVENT_TILE_PLACEMENT_MOVE;
  char *pos = token;
  assert(strlen(pos) >= 2);
  for (size_t i = 0; i < strlen(pos); i++) {
    if (pos[i] >= '0' && pos[i] <= '9') {
      if (i == 0) {
        move->vertical = 0;
      }
      move->row_start = move->row_start * 10 + (pos[i] - '0');
    } else if (pos[i] >= 'a' && pos[i] <= 'z') {
      if (i == 0) {
        move->vertical = 1;
      }
      move->col_start = pos[i] - 'a';
    } else {
      // can't get here.
      assert(0);
    }
  }
  // row start should be 0-indexed.
  move->row_start--;

  token = strtok(NULL, ".");
  char *word = token;
  int ntiles = str_to_machine_letters(ld, word, false, move->tiles);

  int ri, ci;
  ri = 0;
  ci = 0;
  if (move->vertical) {
    ri = 1;
  } else {
    ci = 1;
  }
  int tp = 0;

  for (int r = move->row_start, c = move->col_start, wi = 0; wi < ntiles;
       r += ri, c += ci, wi++) {

    if (get_letter(board, r, c) == 0) {
      // This square of the board is empty, so play this tile.
      tp++;
    } else {
      // It's a play through. Overwrite this tile in move.
      move->tiles[wi] = PLAYED_THROUGH_MARKER;
    }
  }
  move->tiles_length = ntiles;
  move->tiles_played = tp;
  // score the play.
  int row = move->row_start;
  int col = move->col_start;
  // The score routine always assumes the play is horizontal.
  if (move->vertical) {
    transpose(board);
    int ph = row;
    row = col;
    col = ph;
  }
  move->score = score_move(board, move->tiles, 0, ntiles - 1, row, col, tp,
                           !move->vertical, 0, ld);
  if (move->vertical) {
    transpose(board);
  }
  token = strtok(NULL, ".");
  assert(token == NULL);
}