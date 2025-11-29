#include "../def/game_history_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../ent/board.h"
#include "../ent/equity.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/rack.h"
#include "../ent/thread_control.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "letter_distribution_string.h"
#include "rack_string.h"
#include <stdint.h>

void string_builder_add_move_description(StringBuilder *move_string_builder,
                                         const Move *move,
                                         const LetterDistribution *ld) {
  if (move_get_type(move) != GAME_EVENT_PASS) {
    if (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      if (board_is_dir_vertical(move_get_dir(move))) {
        string_builder_add_formatted_string(move_string_builder, "%c%d ",
                                            move_get_col_start(move) + 'A',
                                            move_get_row_start(move) + 1);
      } else {
        string_builder_add_formatted_string(move_string_builder, "%d%c ",
                                            move_get_row_start(move) + 1,
                                            move_get_col_start(move) + 'A');
      }
    } else {
      string_builder_add_string(move_string_builder, "(Exch ");
    }

    int number_of_tiles_to_print = move_get_tiles_length(move);

    for (int i = 0; i < number_of_tiles_to_print; i++) {
      MachineLetter letter = move_get_tile(move, i);
      if (letter == PLAYED_THROUGH_MARKER &&
          move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
        string_builder_add_char(move_string_builder, ASCII_PLAYED_THROUGH);
      } else {
        string_builder_add_user_visible_letter(move_string_builder, ld, letter);
      }
    }
    if (move_get_type(move) == GAME_EVENT_EXCHANGE) {
      string_builder_add_string(move_string_builder, ")");
    }
  } else {
    string_builder_add_string(move_string_builder, "(Pass)");
  }
}

void string_builder_add_move(StringBuilder *string_builder, const Board *board,
                             const Move *move, const LetterDistribution *ld,
                             bool add_score) {
  if (move_get_type(move) == GAME_EVENT_PASS) {
    string_builder_add_string(string_builder, "pass 0");
    return;
  }

  if (move_get_type(move) == GAME_EVENT_EXCHANGE) {
    string_builder_add_string(string_builder, "(exch ");
    for (int i = 0; i < move_get_tiles_played(move); i++) {
      string_builder_add_user_visible_letter(string_builder, ld,
                                             move_get_tile(move, i));
    }
    string_builder_add_string(string_builder, ")");
    return;
  }

  if (board_is_dir_vertical(move_get_dir(move))) {
    string_builder_add_char(string_builder,
                            (char)(move_get_col_start(move) + 'A'));
    string_builder_add_int(string_builder, move_get_row_start(move) + 1);
  } else {
    string_builder_add_int(string_builder, move_get_row_start(move) + 1);
    string_builder_add_char(string_builder,
                            (char)(move_get_col_start(move) + 'A'));
  }

  string_builder_add_spaces(string_builder, 1);
  int current_row = move_get_row_start(move);
  int current_col = move_get_col_start(move);
  for (int i = 0; i < move_get_tiles_length(move); i++) {
    MachineLetter tile = move_get_tile(move, i);
    MachineLetter print_tile = tile;
    if (tile == PLAYED_THROUGH_MARKER) {
      if (board) {
        print_tile = board_get_letter(board, current_row, current_col);
      }
      if (i == 0 && board) {
        string_builder_add_string(string_builder, "(");
      }
    }

    if (tile == PLAYED_THROUGH_MARKER && !board) {
      string_builder_add_string(string_builder, ".");
    } else {
      string_builder_add_user_visible_letter(string_builder, ld, print_tile);
    }

    if (board && (tile == PLAYED_THROUGH_MARKER) &&
        (i == move_get_tiles_length(move) - 1 ||
         move_get_tile(move, i + 1) != PLAYED_THROUGH_MARKER)) {
      string_builder_add_string(string_builder, ")");
    }

    if (board && (tile != PLAYED_THROUGH_MARKER) &&
        (i + 1 < move_get_tiles_length(move)) &&
        move_get_tile(move, i + 1) == PLAYED_THROUGH_MARKER) {
      string_builder_add_string(string_builder, "(");
    }

    if (board_is_dir_vertical(move_get_dir(move))) {
      current_row++;
    } else {
      current_col++;
    }
  }
  if (board && add_score) {
    string_builder_add_spaces(string_builder, 1);
    string_builder_add_int(string_builder, equity_to_int(move_get_score(move)));
  }
}

void string_builder_add_ucgi_move(StringBuilder *move_string_builder,
                                  const Move *move, const Board *board,
                                  const LetterDistribution *ld) {

  if (move_get_type(move) != GAME_EVENT_PASS) {
    if (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      if (board_is_dir_vertical(move_get_dir(move))) {
        string_builder_add_formatted_string(move_string_builder, "%c%d.",
                                            move_get_col_start(move) + 'a',
                                            move_get_row_start(move) + 1);
      } else {
        string_builder_add_formatted_string(move_string_builder, "%d%c.",
                                            move_get_row_start(move) + 1,
                                            move_get_col_start(move) + 'a');
      }
    } else {
      string_builder_add_string(move_string_builder, "ex.");
    }

    int number_of_tiles_to_print = move_get_tiles_length(move);

    int ri = 0;
    int ci = 0;
    if (board_is_dir_vertical(move_get_dir(move))) {
      ri = 1;
    } else {
      ci = 1;
    }

    for (int i = 0; i < number_of_tiles_to_print; i++) {
      MachineLetter letter = move_get_tile(move, i);
      if (letter == PLAYED_THROUGH_MARKER &&
          move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
        int r = move_get_row_start(move) + (ri * i);
        int c = move_get_col_start(move) + (ci * i);
        letter = board_get_letter(board, r, c);
      }
      string_builder_add_user_visible_letter(move_string_builder, ld, letter);
    }
  } else {
    string_builder_add_string(move_string_builder, "pass");
  }
}

// Add the move and the move score to the string builder
void string_builder_add_gcg_move(StringBuilder *move_string_builder,
                                 const Move *move,
                                 const LetterDistribution *ld) {
  const game_event_t move_type = move_get_type(move);
  switch (move_type) {
  case GAME_EVENT_PASS:
    string_builder_add_char(move_string_builder, '-');
    break;
  case GAME_EVENT_EXCHANGE:;
    string_builder_add_char(move_string_builder, '-');
    const int num_tiles_played = move_get_tiles_played(move);
    for (int i = 0; i < num_tiles_played; i++) {
      string_builder_add_user_visible_letter(move_string_builder, ld,
                                             move_get_tile(move, i));
    }
    break;
  case GAME_EVENT_TILE_PLACEMENT_MOVE:;
    if (board_is_dir_vertical(move_get_dir(move))) {
      string_builder_add_formatted_string(move_string_builder, "%c%d",
                                          move_get_col_start(move) + 'A',
                                          move_get_row_start(move) + 1);
    } else {
      string_builder_add_formatted_string(move_string_builder, "%d%c",
                                          move_get_row_start(move) + 1,
                                          move_get_col_start(move) + 'A');
    }
    string_builder_add_char(move_string_builder, ' ');
    const int tiles_length = move_get_tiles_length(move);
    for (int i = 0; i < tiles_length; i++) {
      MachineLetter letter = move_get_tile(move, i);
      if (letter == PLAYED_THROUGH_MARKER) {
        string_builder_add_char(move_string_builder, ASCII_PLAYED_THROUGH);
      } else {
        string_builder_add_user_visible_letter(move_string_builder, ld, letter);
      }
    }
    break;
  default:
    log_fatal("encountered unexpected move type while building move string: %d",
              move_type);
    break;
  }
  string_builder_add_formatted_string(move_string_builder, " +%d",
                                      equity_to_int(move_get_score(move)));
}

void string_builder_add_move_leave(StringBuilder *sb, const Rack *rack,
                                   const Move *move,
                                   const LetterDistribution *ld) {
  Rack leave = *rack;
  const int move_tiles_length = move_get_tiles_length(move);
  for (int i = 0; i < move_tiles_length; i++) {
    if (move_get_tile(move, i) != PLAYED_THROUGH_MARKER) {
      if (get_is_blanked(move_get_tile(move, i))) {
        rack_take_letter(&leave, BLANK_MACHINE_LETTER);
      } else {
        rack_take_letter(&leave, move_get_tile(move, i));
      }
    }
  }
  string_builder_add_rack(sb, &leave, ld, false);
}

// Board can be null
void string_builder_add_move_list(StringBuilder *string_builder,
                                  const MoveList *move_list, const Board *board,
                                  const LetterDistribution *ld,
                                  bool use_ucgi_format) {
  // Use +1 for the header
  const int num_moves = move_list_get_count(move_list);
  int num_rows = num_moves;
  if (!use_ucgi_format) {
    num_rows += 1;
  }
  const int num_cols = 5;
  StringGrid *string_grid = string_grid_create(num_rows, num_cols, 1);

  int curr_row = 0;
  int curr_col = 0;
  if (!use_ucgi_format) {
    string_grid_set_cell(string_grid, 0, curr_col++, string_duplicate(""));
    string_grid_set_cell(string_grid, 0, curr_col++, string_duplicate("Move"));
    string_grid_set_cell(string_grid, 0, curr_col++, string_duplicate("Leave"));
    string_grid_set_cell(string_grid, 0, curr_col++, string_duplicate("Score"));
    string_grid_set_cell(string_grid, 0, curr_col++,
                         string_duplicate("Static Eq"));
    curr_row++;
  }

  StringBuilder *tmp_sb = string_builder_create();
  const Rack *rack = move_list_get_rack(move_list);
  const uint16_t rack_dist_size = rack_get_dist_size(rack);
  for (int i = 0; i < num_moves; i++) {
    curr_col = 0;
    Move *move = move_list_get_move(move_list, i);

    string_grid_set_cell(string_grid, curr_row, curr_col++,
                         get_formatted_string("%d: ", i + 1));

    if (board) {
      string_builder_add_move(tmp_sb, board, move, ld, false);
    } else {
      string_builder_add_move_description(tmp_sb, move, ld);
    }
    string_grid_set_cell(string_grid, curr_row, curr_col++,
                         string_builder_dump(tmp_sb, NULL));
    string_builder_clear(tmp_sb);

    // The rack from which the move is made should always
    // be set, but in case it isn't, skip leave display
    if (rack_dist_size > 0) {
      string_builder_add_move_leave(tmp_sb, rack, move, ld);
      string_grid_set_cell(string_grid, curr_row, curr_col++,
                           string_builder_dump(tmp_sb, NULL));
      string_builder_clear(tmp_sb);
    } else {
      curr_col++;
    }

    string_grid_set_cell(
        string_grid, curr_row, curr_col++,
        get_formatted_string("%d", equity_to_int(move_get_score(move))));

    string_grid_set_cell(
        string_grid, curr_row, curr_col++,
        get_formatted_string("%.2f", equity_to_double(move_get_equity(move))));

    curr_row++;
  }
  string_builder_add_string_grid(string_builder, string_grid, false);
  string_builder_add_string(string_builder, "\n");
  string_grid_destroy(string_grid);
  string_builder_destroy(tmp_sb);
}

char *move_list_get_string(const MoveList *move_list, const Board *board,
                           const LetterDistribution *ld, bool use_ucgi_format) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_move_list(sb, move_list, board, ld, use_ucgi_format);
  char *move_list_string = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  return move_list_string;
}

void move_list_print(ThreadControl *thread_control, const MoveList *move_list,
                     const Board *board, const LetterDistribution *ld,
                     bool use_ucgi_format) {
  char *move_list_string =
      move_list_get_string(move_list, board, ld, use_ucgi_format);
  thread_control_print(thread_control, move_list_string);
  free(move_list_string);
}