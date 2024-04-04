#include "game.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>

#include "../def/board_defs.h"
#include "../def/cross_set_defs.h"
#include "../def/game_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/players_data_defs.h"

#include "bag.h"
#include "board.h"
#include "kwg.h"
#include "player.h"
#include "players_data.h"
#include "rack.h"

typedef struct MinimalGameBackup {
  Board *board;
  Bag *bag;
  Rack *p0rack;
  Rack *p1rack;
  int p0score;
  int p1score;
  int player_on_turn_index;
  int starting_player_index;
  int consecutive_scoreless_turns;
  game_end_reason_t game_end_reason;
} MinimalGameBackup;

// The Game struct does not own the
// letter distribution, which is the
// caller's responsibility. There are
// also some fields in Player which
// are owned by the caller. See the Player
// struct for more details.
struct Game {
  int player_on_turn_index;
  int starting_player_index;
  int consecutive_scoreless_turns;
  game_end_reason_t game_end_reason;
  bool data_is_shared[NUMBER_OF_DATA];
  Board *board;
  Bag *bag;
  // Some data in the Player objects
  // are owned by the caller. See Player
  // for details.
  Player *players[2];

  // Owned by the caller
  LetterDistribution *ld;
  // Backups
  MinimalGameBackup *game_backups[MAX_SEARCH_DEPTH];
  int backup_cursor;
  int backup_mode;
  bool backups_preallocated;
};

Board *game_get_board(const Game *game) { return game->board; }

Bag *game_get_bag(const Game *game) { return game->bag; }

const LetterDistribution *game_get_ld(const Game *game) { return game->ld; }

Player *game_get_player(const Game *game, int player_index) {
  return game->players[player_index];
}

int game_get_player_on_turn_index(const Game *game) {
  return game->player_on_turn_index;
}

game_end_reason_t game_get_game_end_reason(const Game *game) {
  return game->game_end_reason;
}

void game_set_game_end_reason(Game *game, game_end_reason_t game_end_reason) {
  game->game_end_reason = game_end_reason;
}

int game_get_player_draw_index(const Game *game, int player_index) {
  return player_index ^ game->starting_player_index;
}

int game_get_player_on_turn_draw_index(const Game *game) {
  return game_get_player_draw_index(game, game->player_on_turn_index);
}

backup_mode_t game_get_backup_mode(const Game *game) {
  return game->backup_mode;
}

int game_get_consecutive_scoreless_turns(const Game *game) {
  return game->consecutive_scoreless_turns;
}

bool game_get_data_is_shared(const Game *game,
                             players_data_t players_data_type) {
  return game->data_is_shared[(int)players_data_type];
}

void game_set_consecutive_scoreless_turns(Game *game, int value) {
  game->consecutive_scoreless_turns = value;
}

void game_increment_consecutive_scoreless_turns(Game *game) {
  game->consecutive_scoreless_turns++;
}

void game_start_next_player_turn(Game *game) {
  game->player_on_turn_index = 1 - game->player_on_turn_index;
}

int traverse_backwards_for_score(const Board *board,
                                 const LetterDistribution *ld, int row,
                                 int col) {
  int score = 0;
  while (board_is_position_in_bounds_and_not_bricked(board, row, col)) {
    uint8_t ml = board_get_letter(board, row, col);
    if (ml == ALPHABET_EMPTY_SQUARE_MARKER) {
      break;
    }
    if (get_is_blanked(ml)) {
      score += ld_get_score(ld, BLANK_MACHINE_LETTER);
    } else {
      score += ld_get_score(ld, ml);
    }
    col--;
  }
  return score;
}

static inline uint32_t traverse_backwards(const KWG *kwg, Board *board, int row,
                                          int col, uint32_t node_index,
                                          bool check_letter_set,
                                          int left_most_col) {
  while (board_is_position_in_bounds_and_not_bricked(board, row, col)) {
    uint8_t ml = board_get_letter(board, row, col);
    if (ml == ALPHABET_EMPTY_SQUARE_MARKER) {
      break;
    }

    if (node_index == 0) {
      return node_index;
    }

    if (check_letter_set && col == left_most_col) {
      if (kwg_in_letter_set(kwg, ml, node_index)) {
        return node_index;
      }
      return 0;
    }

    node_index = kwg_get_next_node_index(kwg, node_index,
                                         get_unblanked_machine_letter(ml));
    col--;
  }

  return node_index;
}

void game_gen_cross_set(Game *game, int row, int col, int dir,
                        int cross_set_index) {
  if (!board_is_position_in_bounds(row, col)) {
    return;
  }

  Board *board = game_get_board(game);

  if (board_is_nonempty_or_bricked(board, row, col)) {
    board_set_cross_set(board, row, col, dir, cross_set_index, 0);
    board_set_cross_score(board, row, col, dir, cross_set_index, 0);
    return;
  }
  if (board_are_left_and_right_empty(board, row, col)) {
    board_set_cross_set(board, row, col, dir, cross_set_index,
                        TRIVIAL_CROSS_SET);
    board_set_cross_score(board, row, col, dir, cross_set_index, 0);
    return;
  }

  const KWG *kwg = player_get_kwg(game_get_player(game, cross_set_index));
  const uint32_t kwg_root = kwg_get_root_node_index(kwg);
  const LetterDistribution *ld = game_get_ld(game);

  const int through_dir = board_toggle_dir(dir);

  const int left_col =
      board_get_word_edge(board, row, col - 1, WORD_DIRECTION_LEFT);
  const int right_col =
      board_get_word_edge(board, row, col + 1, WORD_DIRECTION_RIGHT);
  int score = 0;
  uint64_t front_hook_set = 0;
  uint64_t back_hook_set = 0;
  uint32_t right_lnode_index = 0;
  bool left_lpath_is_valid = false;
  bool right_lpath_is_valid = false;
  uint64_t leftside_rightx_set = 0;

  const bool nonempty_to_left = left_col < col;
  if (nonempty_to_left) {
    uint64_t leftside_leftx_set = 0;
    const uint32_t lnode_index =
        traverse_backwards(kwg, board, row, col - 1, kwg_root, false, 0);
    left_lpath_is_valid = lnode_index != 0;
    score += traverse_backwards_for_score(board, ld, row, col - 1);
    if (left_lpath_is_valid) {
      kwg_get_letter_sets(kwg, lnode_index, &leftside_leftx_set);
      const uint32_t s_index =
          kwg_get_next_node_index(kwg, lnode_index, SEPARATION_MACHINE_LETTER);
      if (s_index != 0) {
        back_hook_set = kwg_get_letter_sets(kwg, s_index, &leftside_rightx_set);
      }
    }
    board_set_left_extension_set_with_blank(
        board, row, col - 1, through_dir, cross_set_index, leftside_leftx_set);
    board_set_right_extension_set_with_blank(
        board, row, col - 1, through_dir, cross_set_index, leftside_rightx_set);
    // Mark the empty square left of the leftside played tiles with the leftx
    // set for this sequence of tiles. Move generation can use this to avoid
    // trying to play through tiles we can prove to be dead ends.
    if (left_col > 0) {
      board_set_left_extension_set_with_blank(board, row, left_col - 1,
                                              through_dir, cross_set_index,
                                              leftside_leftx_set);
    }
  }

  const bool nonempty_to_right = right_col > col;
  if (nonempty_to_right) {
    uint64_t rightside_leftx_set = 0;
    uint64_t rightside_rightx_set = 0;
    right_lnode_index =
        traverse_backwards(kwg, board, row, right_col, kwg_root, false, 0);
    right_lpath_is_valid = right_lnode_index != 0;
    score += traverse_backwards_for_score(board, ld, row, right_col);
    if (right_lpath_is_valid) {
      front_hook_set =
          kwg_get_letter_sets(kwg, right_lnode_index, &rightside_leftx_set);
      const uint32_t s_index = kwg_get_next_node_index(
          kwg, right_lnode_index, SEPARATION_MACHINE_LETTER);
      if (s_index != 0) {
        kwg_get_letter_sets(kwg, s_index, &rightside_rightx_set);
      }
    }
    board_set_left_extension_set_with_blank(board, row, right_col, through_dir,
                                            cross_set_index,
                                            rightside_leftx_set);
    board_set_right_extension_set_with_blank(board, row, right_col, through_dir,
                                             cross_set_index,
                                             rightside_rightx_set);
    // Mark this empty square with the leftx set for rightside played tiles.
    board_set_left_extension_set_with_blank(
        board, row, col, through_dir, cross_set_index, rightside_leftx_set);
  }

  if (nonempty_to_left && nonempty_to_right) {
    uint64_t letter_set = 0;
    if (left_lpath_is_valid && right_lpath_is_valid) {
      for (int i = right_lnode_index;; i++) {
        const uint32_t node = kwg_node(kwg, i);
        const uint32_t ml = kwg_node_tile(node);
        // Only try letters that are possible in right extensions from the
        // left side of the empty square.
        if (board_is_letter_allowed_in_cross_set(leftside_rightx_set, ml)) {
          const uint32_t next_node_index =
              kwg_node_arc_index_prefetch(node, kwg);
          if (traverse_backwards(kwg, board, row, col - 1, next_node_index,
                                 true, left_col) != 0) {
            letter_set |= get_cross_set_bit(ml);
          }
        }
        if (kwg_node_is_end(node)) {
          break;
        }
      }
    }
    board_set_cross_set_with_blank(board, row, col, dir, cross_set_index,
                                   letter_set);
  } else if (nonempty_to_left) {
    board_set_cross_set_with_blank(board, row, col, dir, cross_set_index,
                                   back_hook_set);
  } else if (nonempty_to_right) {
    board_set_cross_set_with_blank(board, row, col, dir, cross_set_index,
                                   front_hook_set);
  }
  board_set_cross_score(board, row, col, dir, cross_set_index, score);
}

void game_gen_all_cross_sets(Game *game) {
  Board *board = game_get_board(game);
  bool kwgs_are_shared = game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG);

  // We only use the vertical direction here since the board
  // direction changes with transposition. Each cross set write
  // will make the corresponding write in the opposite direction
  // on the other grid. See board.h for more details.
  for (int i = 0; i < BOARD_DIM; i++) {
    for (int j = 0; j < BOARD_DIM; j++) {
      game_gen_cross_set(game, i, j, BOARD_VERTICAL_DIRECTION, 0);
      if (!kwgs_are_shared) {
        game_gen_cross_set(game, i, j, BOARD_VERTICAL_DIRECTION, 1);
      }
    }
  }
  board_transpose(board);
  for (int i = 0; i < BOARD_DIM; i++) {
    for (int j = 0; j < BOARD_DIM; j++) {
      game_gen_cross_set(game, i, j, BOARD_VERTICAL_DIRECTION, 0);
      if (!kwgs_are_shared) {
        game_gen_cross_set(game, i, j, BOARD_VERTICAL_DIRECTION, 1);
      }
    }
  }
  board_transpose(board);
}

cgp_parse_status_t place_letters_on_board(Game *game, const char *letters,
                                          int row_start,
                                          int *current_column_index) {
  size_t letters_length = string_length(letters);
  uint8_t *machine_letters = malloc_or_die(sizeof(uint8_t) * letters_length);
  int number_of_machine_letters =
      ld_str_to_mls(game->ld, letters, false, machine_letters, letters_length);
  cgp_parse_status_t cgp_parse_status = CGP_PARSE_STATUS_SUCCESS;
  int col_start = *current_column_index;

  if (number_of_machine_letters < 0) {
    cgp_parse_status = CGP_PARSE_STATUS_MALFORMED_BOARD_LETTERS;
  } else {
    for (int i = 0; i < number_of_machine_letters; i++) {
      board_set_letter(game->board, row_start, col_start + i,
                       machine_letters[i]);
      // When placing letters on the board, we
      // assume that player 0 placed all of the tiles
      // for convenience.
      bag_draw_letter(game->bag, machine_letters[i], 0);
      board_increment_tiles_played(game->board, 1);
    }
    *current_column_index = *current_column_index + number_of_machine_letters;
  }
  free(machine_letters);
  return cgp_parse_status;
}

cgp_parse_status_t parse_cgp_board_row(Game *game, const char *cgp_board_row,
                                       int row_index) {
  cgp_parse_status_t cgp_parse_status = CGP_PARSE_STATUS_SUCCESS;
  StringBuilder *tile_string_builder = create_string_builder();
  int row_length = string_length(cgp_board_row);

  int current_row_number_of_spaces = 0;
  int current_column_index = 0;
  for (int i = 0; i < row_length; i++) {
    char current_char = cgp_board_row[i];
    if (isdigit(current_char)) {
      current_row_number_of_spaces =
          (current_row_number_of_spaces * 10) + char_to_int(current_char);
      if (string_builder_length(tile_string_builder) > 0) {
        cgp_parse_status = place_letters_on_board(
            game, string_builder_peek(tile_string_builder), row_index,
            &current_column_index);
        string_builder_clear(tile_string_builder);
        if (cgp_parse_status != CGP_PARSE_STATUS_SUCCESS) {
          break;
        }
      }
    } else {
      if (i == 0 || current_row_number_of_spaces > 0) {
        current_column_index += current_row_number_of_spaces;
        current_row_number_of_spaces = 0;
      }
      string_builder_add_char(tile_string_builder, current_char);
    }
  }

  if (string_builder_length(tile_string_builder) > 0) {
    cgp_parse_status =
        place_letters_on_board(game, string_builder_peek(tile_string_builder),
                               row_index, &current_column_index);
  } else {
    current_column_index += current_row_number_of_spaces;
  }
  destroy_string_builder(tile_string_builder);

  if (current_column_index != BOARD_DIM &&
      cgp_parse_status == CGP_PARSE_STATUS_SUCCESS) {
    cgp_parse_status = CGP_PARSE_STATUS_INVALID_NUMBER_OF_BOARD_COLUMNS;
  }

  return cgp_parse_status;
}

cgp_parse_status_t parse_cgp_board(Game *game, const char *cgp_board) {
  cgp_parse_status_t cgp_parse_status = CGP_PARSE_STATUS_SUCCESS;
  StringSplitter *board_rows = split_string(cgp_board, '/', true);

  if (string_splitter_get_number_of_items(board_rows) != BOARD_DIM) {
    cgp_parse_status = CGP_PARSE_STATUS_INVALID_NUMBER_OF_BOARD_ROWS;
  } else {
    for (int i = 0; i < BOARD_DIM; i++) {
      cgp_parse_status =
          parse_cgp_board_row(game, string_splitter_get_item(board_rows, i), i);
      if (cgp_parse_status != CGP_PARSE_STATUS_SUCCESS) {
        break;
      }
    }
  }
  destroy_string_splitter(board_rows);
  return cgp_parse_status;
}

int draw_rack_from_bag(const LetterDistribution *ld, Bag *bag, Rack *rack,
                       const char *rack_string, int player_draw_index) {
  int number_of_letters_set = rack_set_to_string(ld, rack, rack_string);
  for (int i = 0; i < rack_get_dist_size(rack); i++) {
    for (int j = 0; j < rack_get_letter(rack, i); j++) {
      bag_draw_letter(bag, i, player_draw_index);
    }
  }
  return number_of_letters_set;
}

cgp_parse_status_t
parse_cgp_racks_with_string_splitter(const StringSplitter *player_racks,
                                     Game *game) {
  cgp_parse_status_t cgp_parse_status = CGP_PARSE_STATUS_SUCCESS;
  int number_of_letters_added =
      draw_rack_from_bag(game->ld, game->bag, player_get_rack(game->players[0]),
                         string_splitter_get_item(player_racks, 0),
                         game_get_player_draw_index(game, 0));
  if (number_of_letters_added < 0) {
    return CGP_PARSE_STATUS_MALFORMED_RACK_LETTERS;
  }
  number_of_letters_added =
      draw_rack_from_bag(game->ld, game->bag, player_get_rack(game->players[1]),
                         string_splitter_get_item(player_racks, 1),
                         game_get_player_draw_index(game, 1));
  if (number_of_letters_added < 0) {
    cgp_parse_status = CGP_PARSE_STATUS_MALFORMED_RACK_LETTERS;
  }
  return cgp_parse_status;
}

cgp_parse_status_t parse_cgp_racks(Game *game, const char *cgp_racks) {
  cgp_parse_status_t cgp_parse_status = CGP_PARSE_STATUS_SUCCESS;
  StringSplitter *player_racks = split_string(cgp_racks, '/', false);

  if (string_splitter_get_number_of_items(player_racks) != 2) {
    cgp_parse_status = CGP_PARSE_STATUS_INVALID_NUMBER_OF_PLAYER_RACKS;
  } else {
    cgp_parse_status = parse_cgp_racks_with_string_splitter(player_racks, game);
  }
  destroy_string_splitter(player_racks);
  return cgp_parse_status;
}

cgp_parse_status_t parse_cgp_scores(Game *game, const char *cgp_scores) {
  cgp_parse_status_t cgp_parse_status = CGP_PARSE_STATUS_SUCCESS;
  StringSplitter *player_scores = split_string(cgp_scores, '/', false);
  if (string_splitter_get_number_of_items(player_scores) != 2) {
    cgp_parse_status = CGP_PARSE_STATUS_INVALID_NUMBER_OF_PLAYER_SCORES;
  } else if (!is_all_digits_or_empty(
                 string_splitter_get_item(player_scores, 0)) ||
             !is_all_digits_or_empty(
                 string_splitter_get_item(player_scores, 1))) {
    cgp_parse_status = CGP_PARSE_STATUS_MALFORMED_SCORES;
  } else {
    player_set_score(game->players[0],
                     string_to_int(string_splitter_get_item(player_scores, 0)));
    player_set_score(game->players[1],
                     string_to_int(string_splitter_get_item(player_scores, 1)));
  }
  destroy_string_splitter(player_scores);
  return cgp_parse_status;
}

cgp_parse_status_t
parse_cgp_consecutive_zeros(Game *game, const char *cgp_consecutive_zeros) {
  if (!is_all_digits_or_empty(cgp_consecutive_zeros)) {
    return CGP_PARSE_STATUS_MALFORMED_CONSECUTIVE_ZEROS;
  }
  game->consecutive_scoreless_turns = string_to_int(cgp_consecutive_zeros);
  return CGP_PARSE_STATUS_SUCCESS;
}

cgp_parse_status_t parse_cgp_with_cgp_fields(const StringSplitter *cgp_fields,
                                             Game *game) {
  cgp_parse_status_t cgp_parse_status = CGP_PARSE_STATUS_SUCCESS;

  cgp_parse_status =
      parse_cgp_board(game, string_splitter_get_item(cgp_fields, 0));
  if (cgp_parse_status != CGP_PARSE_STATUS_SUCCESS) {
    return cgp_parse_status;
  }

  cgp_parse_status =
      parse_cgp_racks(game, string_splitter_get_item(cgp_fields, 1));
  if (cgp_parse_status != CGP_PARSE_STATUS_SUCCESS) {
    return cgp_parse_status;
  }

  cgp_parse_status =
      parse_cgp_scores(game, string_splitter_get_item(cgp_fields, 2));
  if (cgp_parse_status != CGP_PARSE_STATUS_SUCCESS) {
    return cgp_parse_status;
  }

  return parse_cgp_consecutive_zeros(game,
                                     string_splitter_get_item(cgp_fields, 3));
}

cgp_parse_status_t parse_cgp(Game *game, const char *cgp) {
  cgp_parse_status_t cgp_parse_status = CGP_PARSE_STATUS_SUCCESS;
  StringSplitter *cgp_fields = split_string_by_whitespace(cgp, true);

  if (string_splitter_get_number_of_items(cgp_fields) < 4) {
    cgp_parse_status = CGP_PARSE_STATUS_MISSING_REQUIRED_FIELDS;
  } else {
    cgp_parse_status = parse_cgp_with_cgp_fields(cgp_fields, game);
  }
  destroy_string_splitter(cgp_fields);
  return cgp_parse_status;
}

cgp_parse_status_t game_load_cgp(Game *game, const char *cgp) {
  game_reset(game);
  cgp_parse_status_t cgp_parse_status = parse_cgp(game, cgp);
  if (cgp_parse_status != CGP_PARSE_STATUS_SUCCESS) {
    return cgp_parse_status;
  }

  game->player_on_turn_index = 0;

  Player *player0 = game->players[0];
  Player *player1 = game->players[1];

  game_gen_all_cross_sets(game);
  board_update_all_anchors(game->board);

  if (game->consecutive_scoreless_turns >= MAX_SCORELESS_TURNS) {
    game->game_end_reason = GAME_END_REASON_CONSECUTIVE_ZEROS;
  } else if (bag_is_empty(game->bag) &&
             (rack_is_empty(player_get_rack(player0)) ||
              rack_is_empty(player_get_rack(player1)))) {
    game->game_end_reason = GAME_END_REASON_STANDARD;
  } else {
    game->game_end_reason = GAME_END_REASON_NONE;
  }
  return cgp_parse_status;
}

void game_reset(Game *game) {
  board_reset(game->board);
  bag_reset(game->ld, game->bag);
  player_reset(game->players[0]);
  player_reset(game->players[1]);
  game->player_on_turn_index = 0;
  game->starting_player_index = 0;
  game->consecutive_scoreless_turns = 0;
  game->game_end_reason = GAME_END_REASON_NONE;
  game->backup_cursor = 0;
}

// This assumes the game has not started yet.
void game_set_starting_player_index(Game *game, int starting_player_index) {
  game->starting_player_index = starting_player_index;
  game->player_on_turn_index = starting_player_index;
}

void pre_allocate_backups(Game *game) {
  // pre-allocate heap backup structures to make backups as fast as possible.
  const LetterDistribution *ld = game_get_ld(game);
  uint32_t ld_size = ld_get_size(ld);
  for (int i = 0; i < MAX_SEARCH_DEPTH; i++) {
    game->game_backups[i] = malloc_or_die(sizeof(MinimalGameBackup));
    game->game_backups[i]->bag = bag_create(ld);
    game->game_backups[i]->board = board_duplicate(game_get_board(game));
    game->game_backups[i]->p0rack = rack_create(ld_size);
    game->game_backups[i]->p1rack = rack_create(ld_size);
  }
}

void game_set_backup_mode(Game *game, int backup_mode) {
  game->backup_mode = backup_mode;
  if (backup_mode == BACKUP_MODE_SIMULATION && !game->backups_preallocated) {
    game->backup_cursor = 0;
    pre_allocate_backups(game);
    game->backups_preallocated = true;
  }
}

void game_update(const Config *config, Game *game) {
  game->ld = config_get_ld(config);
  for (int player_index = 0; player_index < 2; player_index++) {
    player_update(config, game->players[player_index]);
  }
  for (int i = 0; i < NUMBER_OF_DATA; i++) {
    game->data_is_shared[i] = players_data_get_is_shared(
        config_get_players_data(config), (players_data_t)i);
  }
}

Game *game_create(const Config *config) {
  Game *game = malloc_or_die(sizeof(Game));
  game->ld = config_get_ld(config);
  game->bag = bag_create(game->ld);
  game->board = board_create(config_get_board_layout(config));
  for (int player_index = 0; player_index < 2; player_index++) {
    game->players[player_index] = player_create(config, player_index);
  }
  for (int i = 0; i < NUMBER_OF_DATA; i++) {
    game->data_is_shared[i] = players_data_get_is_shared(
        config_get_players_data(config), (players_data_t)i);
  }
  game->starting_player_index = 0;
  game->player_on_turn_index = 0;
  game->consecutive_scoreless_turns = 0;
  game->game_end_reason = GAME_END_REASON_NONE;
  for (int i = 0; i < MAX_SEARCH_DEPTH; i++) {
    game->game_backups[i] = NULL;
  }
  game->backup_cursor = 0;
  game->backup_mode = BACKUP_MODE_OFF;
  game->backups_preallocated = false;
  return game;
}

// This creates a move list and a generator but does
// not copy them from game.
Game *game_duplicate(const Game *game) {
  Game *new_game = malloc_or_die(sizeof(Game));
  new_game->bag = bag_duplicate(game->bag);
  new_game->board = board_duplicate(game->board);
  new_game->ld = game->ld;

  for (int j = 0; j < 2; j++) {
    new_game->players[j] = player_duplicate(game->players[j]);
  }
  for (int i = 0; i < NUMBER_OF_DATA; i++) {
    new_game->data_is_shared[i] = game->data_is_shared[i];
  }
  new_game->player_on_turn_index = game->player_on_turn_index;
  new_game->starting_player_index = game->starting_player_index;
  new_game->consecutive_scoreless_turns = game->consecutive_scoreless_turns;
  new_game->game_end_reason = game->game_end_reason;
  // note: game backups must be explicitly handled by the caller if they want
  // game copies to have backups.
  for (int i = 0; i < MAX_SEARCH_DEPTH; i++) {
    new_game->game_backups[i] = NULL;
  }
  new_game->backup_cursor = 0;
  new_game->backup_mode = BACKUP_MODE_OFF;
  new_game->backups_preallocated = false;
  return new_game;
}

// Backups do not restore the move list or
// generator.
void game_backup(Game *game) {
  if (game->backup_mode == BACKUP_MODE_OFF) {
    return;
  }
  if (game->backup_mode == BACKUP_MODE_SIMULATION) {
    MinimalGameBackup *state = game->game_backups[game->backup_cursor];
    board_copy(state->board, game->board);
    bag_copy(state->bag, game->bag);
    state->game_end_reason = game->game_end_reason;
    state->player_on_turn_index = game->player_on_turn_index;
    state->starting_player_index = game->starting_player_index;
    state->consecutive_scoreless_turns = game->consecutive_scoreless_turns;
    Player *player0 = game->players[0];
    Player *player1 = game->players[1];
    rack_copy(state->p0rack, player_get_rack(player0));
    state->p0score = player_get_score(player0);
    rack_copy(state->p1rack, player_get_rack(player1));
    state->p1score = player_get_score(player1);

    game->backup_cursor++;
  }
}

void game_unplay_last_move(Game *game) {
  // restore from backup (pop the last element).
  if (game->backup_cursor == 0) {
    log_fatal("error: no backup\n");
  }
  MinimalGameBackup *state = game->game_backups[game->backup_cursor - 1];
  game->backup_cursor--;

  game->consecutive_scoreless_turns = state->consecutive_scoreless_turns;
  game->game_end_reason = state->game_end_reason;
  game->player_on_turn_index = state->player_on_turn_index;
  game->starting_player_index = state->starting_player_index;

  Player *player0 = game->players[0];
  Player *player1 = game->players[1];
  player_set_score(player0, state->p0score);
  player_set_score(player1, state->p1score);
  rack_copy(player_get_rack(player0), state->p0rack);
  rack_copy(player_get_rack(player1), state->p1rack);
  bag_copy(game->bag, state->bag);
  board_copy(game->board, state->board);
}

void destroy_backups(Game *game) {
  for (int i = 0; i < MAX_SEARCH_DEPTH; i++) {
    rack_destroy(game->game_backups[i]->p0rack);
    rack_destroy(game->game_backups[i]->p1rack);
    bag_destroy(game->game_backups[i]->bag);
    board_destroy(game->game_backups[i]->board);
    free(game->game_backups[i]);
  }
}

// This does not destroy the letter distribution,
// the caller must handle that.
void game_destroy(Game *game) {
  if (!game) {
    return;
  }
  board_destroy(game->board);
  bag_destroy(game->bag);
  player_destroy(game->players[0]);
  player_destroy(game->players[1]);
  if (game->backups_preallocated) {
    destroy_backups(game);
  }
  free(game);
}
