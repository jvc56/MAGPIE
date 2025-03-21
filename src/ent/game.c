#include "game.h"

#include <stdint.h>
#include <stdlib.h>

#include "../def/board_defs.h"
#include "../def/cross_set_defs.h"
#include "../def/game_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/players_data_defs.h"

#include "../ent/equity.h"

#include "bag.h"
#include "bit_rack.h"
#include "board.h"
#include "kwg.h"
#include "kwg_alpha.h"
#include "letter_distribution.h"
#include "player.h"
#include "players_data.h"
#include "rack.h"

#include "../str/game_string.h"

typedef struct MinimalGameBackup {
  Board *board;
  Bag *bag;
  Rack *p0rack;
  Rack *p1rack;
  Equity p0score;
  Equity p1score;
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
  int max_scoreless_turns;
  Equity bingo_bonus;
  game_end_reason_t game_end_reason;
  bool data_is_shared[NUMBER_OF_DATA];
  Board *board;
  Bag *bag;
  // Some data in the Player objects
  // are owned by the caller. See Player
  // for details.
  Player *players[2];

  // Owned by the caller
  const LetterDistribution *ld;
  // Used by cross set generation
  Rack *cross_set_rack;
  game_variant_t variant;
  // Backups
  MinimalGameBackup *game_backups[MAX_SEARCH_DEPTH];
  int backup_cursor;
  backup_mode_t backup_mode;
  bool backups_preallocated;
};

game_variant_t game_get_variant(const Game *game) { return game->variant; }

Equity game_get_bingo_bonus(const Game *game) { return game->bingo_bonus; }

game_variant_t get_game_variant_type_from_name(const char *variant_name) {
  game_variant_t game_variant = GAME_VARIANT_UNKNOWN;
  if (strings_iequal(variant_name, GAME_VARIANT_CLASSIC_NAME)) {
    game_variant = GAME_VARIANT_CLASSIC;
  } else if (strings_iequal(variant_name, GAME_VARIANT_WORDSMOG_NAME)) {
    game_variant = GAME_VARIANT_WORDSMOG;
  }
  return game_variant;
}

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

bool game_over(const Game *game) {
  return game->game_end_reason != GAME_END_REASON_NONE;
}

void game_set_game_end_reason(Game *game, game_end_reason_t game_end_reason) {
  game->game_end_reason = game_end_reason;
}

int game_get_starting_player_index(const Game *game) {
  return game->starting_player_index;
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

void game_set_endgame_solving_mode(Game *game) {
  if (game->consecutive_scoreless_turns + 1 >= game->max_scoreless_turns) {
    game->consecutive_scoreless_turns = 1;
  } else {
    game->consecutive_scoreless_turns = 0;
  }
  game->max_scoreless_turns = 2;
}

void game_start_next_player_turn(Game *game) {
  game->player_on_turn_index = 1 - game->player_on_turn_index;
}

Equity traverse_backwards_for_score(const Board *board,
                                    const LetterDistribution *ld, int row,
                                    int col) {
  Equity score = 0;
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

static inline uint32_t traverse_backwards(const KWG *kwg, const Board *board,
                                          int row, int col, uint32_t node_index,
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

static inline void traverse_backwards_add_to_rack(const Board *board, int row,
                                                  int col, Rack *rack) {
  while (board_is_position_in_bounds_and_not_bricked(board, row, col)) {
    uint8_t ml = board_get_letter(board, row, col);
    if (ml == ALPHABET_EMPTY_SQUARE_MARKER) {
      break;
    }
    rack_add_letter(rack, get_unblanked_machine_letter(ml));
    col--;
  }
}

static inline void game_gen_alpha_cross_set(Game *game, int row, int col,
                                            int dir, int cross_set_index) {
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

  const LetterDistribution *ld = game_get_ld(game);

  const int left_col =
      board_get_word_edge(board, row, col - 1, WORD_DIRECTION_LEFT);
  const int right_col =
      board_get_word_edge(board, row, col + 1, WORD_DIRECTION_RIGHT);
  Equity score = 0;

  rack_reset(game->cross_set_rack);
  if (left_col < col) {
    traverse_backwards_add_to_rack(board, row, col - 1, game->cross_set_rack);
    score += traverse_backwards_for_score(board, ld, row, col - 1);
  }

  if (right_col > col) {
    traverse_backwards_add_to_rack(board, row, right_col, game->cross_set_rack);
    score += traverse_backwards_for_score(board, ld, row, right_col);
  }

  const KWG *kwg = player_get_kwg(game_get_player(game, cross_set_index));

  board_set_cross_set_with_blank(
      board, row, col, dir, cross_set_index,
      kwg_compute_alpha_cross_set(kwg, game->cross_set_rack));
  board_set_cross_score(board, row, col, dir, cross_set_index, score);
}

static inline void game_gen_classic_cross_set(Game *game, int row, int col,
                                              int dir, int cross_set_index) {
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
  Equity score = 0;
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

void game_gen_cross_set(Game *game, int row, int col, int dir,
                        int cross_set_index) {
  if (game_get_variant(game) == GAME_VARIANT_CLASSIC) {
    game_gen_classic_cross_set(game, row, col, dir, cross_set_index);
  } else {
    game_gen_alpha_cross_set(game, row, col, dir, cross_set_index);
  }
}

void game_gen_all_cross_sets(Game *game) {
  // printf("game_gen_all_cross_sets\n");
  // StringBuilder *sb = string_builder_create();
  // string_builder_add_game(sb, game, NULL);
  // printf("%s\n", string_builder_peek(sb));
  // string_builder_destroy(sb);
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

void game_reset(Game *game) {
  board_reset(game->board);
  bag_reset(game->ld, game->bag);
  player_reset(game->players[0]);
  player_reset(game->players[1]);
  game_update_all_spots(game);
  game->player_on_turn_index = 0;
  game->starting_player_index = 0;
  game->consecutive_scoreless_turns = 0;
  game->game_end_reason = GAME_END_REASON_NONE;
  game->backup_cursor = 0;
}

// Sets the bag state with the given seed, ensuring the
// next iteration will be consistent. Any leftover random
// racks should be returned before calling this function.
void game_seed(Game *game, uint64_t seed) { bag_seed(game->bag, seed); }

// This assumes the game has not started yet.
void game_set_starting_player_index(Game *game, int starting_player_index) {
  game->starting_player_index = starting_player_index;
  game->player_on_turn_index = starting_player_index;
}

void game_set_player_on_turn_index(Game *game, int player_on_turn_index) {
  game->player_on_turn_index = player_on_turn_index;
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

void game_set_backup_mode(Game *game, backup_mode_t backup_mode) {
  game->backup_mode = backup_mode;
  if (backup_mode == BACKUP_MODE_SIMULATION && !game->backups_preallocated) {
    game->backup_cursor = 0;
    pre_allocate_backups(game);
    game->backups_preallocated = true;
  }
}

void game_update(Game *game, const GameArgs *game_args) {
  game->ld = game_args->ld;
  game->bingo_bonus = int_to_equity(game_args->bingo_bonus);
  for (int player_index = 0; player_index < 2; player_index++) {
    player_update(game_args->players_data, game->players[player_index]);
  }
  for (int i = 0; i < NUMBER_OF_DATA; i++) {
    game->data_is_shared[i] =
        players_data_get_is_shared(game_args->players_data, (players_data_t)i);
  }
  board_apply_layout(game_args->board_layout, game->board);

  game->variant = game_args->game_variant;
  rack_destroy(game->cross_set_rack);
  if (game->variant == GAME_VARIANT_WORDSMOG) {
    game->cross_set_rack = rack_create(ld_get_size(game->ld));
  } else {
    game->cross_set_rack = NULL;
  }
}

Game *game_create(const GameArgs *game_args) {
  Game *game = malloc_or_die(sizeof(Game));
  game->ld = game_args->ld;
  game->bingo_bonus = int_to_equity(game_args->bingo_bonus);
  game->bag = bag_create(game->ld);
  game->board = board_create(game_args->board_layout);
  for (int player_index = 0; player_index < 2; player_index++) {
    game->players[player_index] =
        player_create(game_args->players_data, game_args->ld, player_index);
  }
  for (int i = 0; i < NUMBER_OF_DATA; i++) {
    game->data_is_shared[i] =
        players_data_get_is_shared(game_args->players_data, (players_data_t)i);
  }
  game_update_all_spots(game);

  game->starting_player_index = 0;
  game->player_on_turn_index = 0;
  game->consecutive_scoreless_turns = 0;
  game->max_scoreless_turns = MAX_SCORELESS_TURNS;
  game->game_end_reason = GAME_END_REASON_NONE;

  game->variant = game_args->game_variant;
  if (game->variant == GAME_VARIANT_WORDSMOG) {
    game->cross_set_rack = rack_create(ld_get_size(game->ld));
  } else {
    game->cross_set_rack = NULL;
  }

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
  new_game->bingo_bonus = game->bingo_bonus;

  for (int j = 0; j < 2; j++) {
    new_game->players[j] = player_duplicate(game->players[j]);
  }
  for (int i = 0; i < NUMBER_OF_DATA; i++) {
    new_game->data_is_shared[i] = game->data_is_shared[i];
  }
  new_game->player_on_turn_index = game->player_on_turn_index;
  new_game->starting_player_index = game->starting_player_index;
  new_game->consecutive_scoreless_turns = game->consecutive_scoreless_turns;
  new_game->max_scoreless_turns = game->max_scoreless_turns;
  new_game->game_end_reason = game->game_end_reason;

  new_game->variant = game->variant;
  if (new_game->variant == GAME_VARIANT_WORDSMOG) {
    new_game->cross_set_rack = rack_create(ld_get_size(new_game->ld));
  } else {
    new_game->cross_set_rack = NULL;
  }

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

void backups_destroy(Game *game) {
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
  rack_destroy(game->cross_set_rack);
  if (game->backups_preallocated) {
    backups_destroy(game);
  }
  free(game);
}

int game_get_max_scoreless_turns(Game *game) {
  return game->max_scoreless_turns;
}

/*
static inline void game_update_spot(Game *game, int row, int col, int num_tiles,
                                    int dir, int ci, int starting_sq_row,
                                    int starting_sq_col) {
  // printf("game_update_spot %d %d %d %d %d %d %d\n", row, col, num_tiles, dir,
  //        ci, starting_sq_row, starting_sq_col);
  Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  BoardSpot *spot =
      board_get_writable_spot(board, row, col, dir, num_tiles, ci);
  assert(!spot->is_usable);
  spot->is_usable = false;
  const int row_incr = dir == BOARD_HORIZONTAL_DIRECTION ? 0 : 1;
  const int col_incr = dir == BOARD_HORIZONTAL_DIRECTION ? 1 : 0;
  const int prev_row = row - row_incr;
  const int prev_col = col - col_incr;
  // This square can't begin a word if it is preceded by a non-empty square.
  if (board_is_position_in_bounds(prev_row, prev_col) &&
      !board_is_empty(board, prev_row, prev_col)) {
    return;
  }
  int tiles = 0;
  spot->word_length = 0;
  spot->playthrough_bit_rack = bit_rack_create_empty();
  int word_multiplier = 1;
  int unmultiplied_playthrough_score = 0;
  int perpendicular_score = 0;
  spot->additional_score = 0;
  uint8_t letter_multipliers[WORD_ALIGNING_RACK_SIZE];
  uint8_t xw_multipliers[WORD_ALIGNING_RACK_SIZE] = {0};
  bool touching = false;
  bool hooking = false;
  while (true) {
    // if (row == 7 && dir == 0) {
    //   printf("row: %d, col: %d\n", row, col);
    // }
    if (!board_is_position_in_bounds(row, col)) {
      // printf("goes out of bounds\n");
      if (tiles == num_tiles) {
        // printf(" but we already have all tiles\n");
        break;
      }
      return;
    }
    const Square *sq = board_get_readonly_square(board, row, col, dir, ci);
    const uint8_t bonus_square = square_get_bonus_square(sq);
    // printf("bonus_square: %d\n", bonus_square);
    if (bonus_square == BRICK_VALUE) {
      // printf("hits brick\n");
      return;
    }

    const uint8_t ml = square_get_letter(sq);
    if (ml == ALPHABET_EMPTY_SQUARE_MARKER) {
      const uint64_t cross_set = board_get_cross_set(board, row, col, dir, ci);
      // printf("empty square\n");
      if (tiles == num_tiles) {
        // printf("already have all tiles\n");
        break;
      }

      if (cross_set == 0) {
        // printf("unhookable cross set (0)\n");
        return;
      }
      if (cross_set == TRIVIAL_CROSS_SET) {
        // printf("trivial cross set\n");
      } else {
        hooking = true;
        // printf("nontrivial cross set [");
        // for (int i = 1; i <= 26; i++) {
        //   if (cross_set & (1 << i)) {
        //     printf("%c", 'A' + i - 1);
        //   }
        // }
        // printf("]\n");
      }

      if ((cross_set != TRIVIAL_CROSS_SET) ||
          ((row == starting_sq_row) && (col == starting_sq_col))) {
        // printf("touches starting square or hooks something\n");
        touching = true;
      }
      const int sq_word_multiplier = bonus_square >> 4;
      const int sq_letter_multiplier = bonus_square & 0x0F;
      word_multiplier *= sq_word_multiplier;
      const int cross_score = square_get_cross_score(sq);
      perpendicular_score += cross_score * sq_word_multiplier;
      letter_multipliers[tiles] = sq_letter_multiplier;
      if (cross_set != TRIVIAL_CROSS_SET) {
        xw_multipliers[tiles] = sq_letter_multiplier * sq_word_multiplier;
      }
      tiles++;
    } else {
      // printf("non-empty square\n");
      touching = true;
      if (!get_is_blanked(ml)) {
        unmultiplied_playthrough_score += ld_get_score(ld, ml);
      }
      bit_rack_add_letter(&spot->playthrough_bit_rack,
                          get_unblanked_machine_letter(ml));
    }
    row += row_incr;
    col += col_incr;
    spot->word_length++;
  }
  assert(tiles == num_tiles);
  if (!touching) {
    // printf("not touching\n");
    return;
  }
  if (tiles == 1) {
    if (spot->word_length == 1) {
      // printf("single tile play, record in proper direction\n");
      return;
    } else if (hooking && (dir == BOARD_VERTICAL_DIRECTION)) {
      // printf("single tile, 2 words, record in horizontal direction\n");
      return;
    }
  }
  memset(spot->descending_effective_multipliers, 0,
         sizeof(spot->descending_effective_multipliers));
  // printf("word_multiplier: %d\n", word_multiplier);
  // printf("letter_multipliers: ");
  // for (int i = 0; i < num_tiles; i++) {
  //   printf("%d ", letter_multipliers[i]);
  // }
  // printf("\n");
  // printf("xw_multipliers: ");
  // for (int i = 0; i < num_tiles; i++) {
  //   printf("%d ", xw_multipliers[i]);
  // }
  // printf("\n");
  for (int tile_idx = 0; tile_idx < num_tiles; tile_idx++) {
    const int multiplier = word_multiplier * letter_multipliers[tile_idx] +
                           xw_multipliers[tile_idx];
    int insert_idx = tile_idx;
    while (insert_idx > 0 &&
           spot->descending_effective_multipliers[insert_idx - 1] <
               multiplier) {
      spot->descending_effective_multipliers[insert_idx] =
          spot->descending_effective_multipliers[insert_idx - 1];
      insert_idx--;
    }
    spot->descending_effective_multipliers[insert_idx] = multiplier;
  }
  const Equity bingo_bonus = num_tiles == RACK_SIZE ? game->bingo_bonus : 0;
  spot->additional_score = unmultiplied_playthrough_score * word_multiplier +
                           perpendicular_score + bingo_bonus;
  spot->is_usable = true;
  // printf("spot->is_usable = true\n");
}
*/

void game_update_spots_from_square_aux(Game *game, int start_row, int start_col,
                                       int min_num_tiles, int dir, int ci,
                                       int star_sq_row, int star_sq_col) {
  // printf("game_update_spots_from_square %d %d %s\n", start_row, start_col,
  //        dir == BOARD_HORIZONTAL_DIRECTION ? "horizontal" : "vertical");
  const int row_incr = dir == BOARD_HORIZONTAL_DIRECTION ? 0 : 1;
  const int col_incr = dir == BOARD_HORIZONTAL_DIRECTION ? 1 : 0;
  const int prev_row = start_row - row_incr;
  const int prev_col = start_col - col_incr;
  Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  uint8_t letter_multipliers[WORD_ALIGNING_RACK_SIZE];
  uint8_t xw_multipliers[WORD_ALIGNING_RACK_SIZE] = {0};
  BitRack playthrough_bit_rack = bit_rack_create_empty();
  int word_length = 0;
  bool hooking = false;
  bool touching = false;
  // We have reached a brick, an unhookable square, or the edge of the board.
  // The spot with the current number of tiles might be usable, but no more
  // tiles can be added.
  bool all_spots_now_unusable = false;
  if (board_is_position_in_bounds(prev_row, prev_col) &&
      !board_is_empty(board, prev_row, prev_col)) {
    all_spots_now_unusable = true;
  }
  int word_multiplier = 1;
  int unmultiplied_playthrough_score = 0;
  int perpendicular_score = 0;
  int row = start_row;
  int col = start_col;
  for (int num_tiles = 0; num_tiles <= RACK_SIZE; num_tiles++) {
    BoardSpot *spot = NULL;
    if (num_tiles >= min_num_tiles) {
      spot = board_get_writable_spot(board, start_row, start_col, dir,
                                     num_tiles, ci);
    }
    if (all_spots_now_unusable) {
      if (spot != NULL) {
        // printf("assigning to spot at %d %d %d %d %d\n", start_row,
        // start_col, dir,
        //        num_tiles, ci);
        spot->is_usable = false;
      }
      continue;
    }
    // Absorb all playthrough starting at this square up to either the next
    // empty square or the end of the board.
    while (board_is_position_in_bounds_and_nonempty(board, row, col)) {
      touching = true;
      const Square *sq = board_get_readonly_square(board, row, col, dir, ci);
      const uint8_t ml = square_get_letter(sq);
      if (!get_is_blanked(ml)) {
        unmultiplied_playthrough_score += ld_get_score(ld, ml);
      }
      bit_rack_add_letter(&playthrough_bit_rack,
                          get_unblanked_machine_letter(ml));
      row += row_incr;
      col += col_incr;
      word_length++;
    }
    // We are at an empty square or edge of the board, and enough tiles have
    // been placed. Record the spot using the current information.
    const bool ignored_one_tile_play =
        (num_tiles == 1) &&
        ((word_length == 1) || (hooking && (dir == BOARD_VERTICAL_DIRECTION)));
    if ((hooking || touching) && (num_tiles >= min_num_tiles) &&
        !ignored_one_tile_play) {
      // printf("usable play with %d tiles\n", num_tiles);
      // printf("assigning to spot at %d %d %d %d %d\n", start_row, start_col,
      // dir, num_tiles,
      //        ci);
      spot->is_usable = true;
      spot->word_length = word_length;
      spot->playthrough_bit_rack = playthrough_bit_rack;
      const Equity bingo_bonus = num_tiles == RACK_SIZE ? game->bingo_bonus : 0;
      spot->additional_score =
          unmultiplied_playthrough_score * word_multiplier +
          perpendicular_score + bingo_bonus;
      memset(spot->descending_effective_multipliers, 0,
             sizeof(spot->descending_effective_multipliers));
      // printf("word_multiplier: %d\n", word_multiplier);
      // printf("letter_multipliers: ");
      // for (int i = 0; i < num_tiles; i++) {
      //   printf("%d ", letter_multipliers[i]);
      // }
      // printf("\n");
      // printf("xw_multipliers: ");
      // for (int i = 0; i < num_tiles; i++) {
      //   printf("%d ", xw_multipliers[i]);
      // }
      // printf("\n");
      for (int tile_idx = 0; tile_idx < num_tiles; tile_idx++) {
        const int multiplier = word_multiplier * letter_multipliers[tile_idx] +
                               xw_multipliers[tile_idx];
        int insert_idx = tile_idx;
        while (insert_idx > 0 &&
               spot->descending_effective_multipliers[insert_idx - 1] <
                   multiplier) {
          spot->descending_effective_multipliers[insert_idx] =
              spot->descending_effective_multipliers[insert_idx - 1];
          insert_idx--;
        }
        spot->descending_effective_multipliers[insert_idx] = multiplier;
      }
      // printf("descending_effective_multipliers: ");
      // for (int i = 0; i < num_tiles; i++) {
      //   printf("%d ", spot->descending_effective_multipliers[i]);
      // }
      // printf("\n");
    }
    if (!board_is_position_in_bounds(row, col)) {
      all_spots_now_unusable = true;
      continue;
    }
    const Square *sq = board_get_readonly_square(board, row, col, dir, ci);
    const uint8_t ml = square_get_letter(sq);
    const uint8_t bonus_square = square_get_bonus_square(sq);
    // printf("bonus_square: 0x%02X\n", bonus_square);
    if (bonus_square == BRICK_VALUE) {
      all_spots_now_unusable = true;
      continue;
    }
    assert(ml == ALPHABET_EMPTY_SQUARE_MARKER);
    const uint64_t cross_set = board_get_cross_set(board, row, col, dir, ci);
    if (cross_set == 0) {
      all_spots_now_unusable = true;
      continue;
    }
    if (cross_set != TRIVIAL_CROSS_SET) {
      hooking = true;
    }
    if (row == star_sq_row && col == star_sq_col) {
      touching = true;
    }
    const int sq_word_multiplier = bonus_square >> 4;
    const int sq_letter_multiplier = bonus_square & 0x0F;
    // printf("usable empty square at %d %d %dxW %dxL\n", row, col,
    //        sq_word_multiplier, sq_letter_multiplier);
    word_multiplier *= sq_word_multiplier;
    const int cross_score = square_get_cross_score(sq);
    perpendicular_score += cross_score * sq_word_multiplier;
    letter_multipliers[num_tiles] = sq_letter_multiplier;
    if (cross_set != TRIVIAL_CROSS_SET) {
      xw_multipliers[num_tiles] = sq_letter_multiplier * sq_word_multiplier;
    }
    row += row_incr;
    col += col_incr;
    word_length++;
  }
}

void game_update_spots_from_square(Game *game, int start_row, int start_col,
                                   int min_num_tiles, int dir) {
  const int num_cis =
      game_get_data_is_shared(game, PLAYERS_DATA_TYPE_WMP) ? 1 : 2;
  for (int ci = 0; ci < num_cis; ci++) {
    game_update_spots_from_square_aux(game, start_row, start_col, min_num_tiles,
                                      dir, ci, /*star_row=*/-1,
                                      /*star_col=*/-1);
  }
}

void game_update_all_spots(Game *game) {
  // printf("game_update_all_spots\n");
  // StringBuilder *sb = string_builder_create();
  // string_builder_add_game(sb, game, NULL);
  // printf("%s\n", string_builder_peek(sb));
  // string_builder_destroy(sb);
  Board *board = game_get_board(game);
  memset(board->board_spots, 0, sizeof(board->board_spots));
  // To avoid confusion with variables for spots' starting squares, call these
  // star row and star col. On a standard board this is the center square.
  const int star_row = board->start_coords[0];
  const int star_col = board->start_coords[1];
  const bool generate_vertical_opening_plays =
      star_row != star_col ||
      !board_are_bonus_squares_symmetric_by_transposition(board);
  const int num_cis =
      game_get_data_is_shared(game, PLAYERS_DATA_TYPE_WMP) ? 1 : 2;
  for (int dir = 0; dir < 2; dir++) {
    for (int row = 0; row < BOARD_DIM; row++) {
      for (int col = 0; col < BOARD_DIM; col++) {
        int star_sq_row = star_row;
        int star_sq_col = star_col;
        if (dir == BOARD_VERTICAL_DIRECTION &&
            !generate_vertical_opening_plays) {
          star_sq_row = -1;
          star_sq_col = -1;
        }
        for (int ci = 0; ci < num_cis; ci++) {
          game_update_spots_from_square_aux(game, row, col, 1, dir, ci,
                                            star_sq_row, star_sq_col);
        }
      }
    }
  }
}

bool game_has_wmp(Game *game) {
  const Player *player0 = game_get_player(game, 0);
  const Player *player1 = game_get_player(game, 1);
  if (player_get_wmp(player0) != NULL) {
    return true;
  }
  if (player_get_wmp(player1) != NULL) {
    return true;
  }
  return false;
}