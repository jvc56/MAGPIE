#include "game_string.h"

#include "../def/board_defs.h"
#include "../def/game_defs.h"
#include "../def/game_history_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/rack_defs.h"
#include "../ent/bag.h"
#include "../ent/board.h"
#include "../ent/bonus_square.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../ent/heat_map.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/validated_move.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "equity_string.h"
#include "letter_distribution_string.h"
#include "move_string.h"
#include "rack_string.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Display formatting constants
enum {
  RACK_DISPLAY_WIDTH = RACK_SIZE + 2,
  UNSEEN_LETTER_ROWS = 5,
  MIN_UNSEEN_LETTERS_PER_ROW = 20,
  MAX_MOVES = 20,
  UNSEEN_START_ROW = 1,
  GAME_EVENT_START_ROW = 10,
  FINAL_PASS_PROMPT_ROW = BOARD_DIM - 1,
};

bool should_print_escape_codes(const GameStringOptions *game_string_options) {
  if (game_string_options == NULL) {
    return false;
  }
  return isatty(fileno(get_stream_out()));
}

bool use_ascii_on_turn_marker(const GameStringOptions *game_string_options) {
  if (game_string_options == NULL) {
    return true;
  }
  return game_string_options->on_turn_marker ==
         GAME_STRING_ON_TURN_MARKER_ASCII;
}

void string_builder_add_player_on_turn_color(
    StringBuilder *game_string, const GameStringOptions *game_string_options) {
  if (game_string_options == NULL) {
    return;
  }
  if (game_string_options->on_turn_color ==
      GAME_STRING_ON_TURN_COLOR_ANSI_GREEN) {
    string_builder_add_string(game_string, "\x1b[32m");
  }
}

bool game_string_option_has_on_turn_color(
    const GameStringOptions *game_string_options) {
  if (game_string_options == NULL) {
    return false;
  }
  return game_string_options->on_turn_color != GAME_STRING_ON_TURN_COLOR_NONE;
}

void string_builder_add_color_reset(StringBuilder *game_string) {
  string_builder_add_string(game_string, "\x1b[0m");
}

void string_builder_add_color_bold(StringBuilder *game_string) {
  string_builder_add_string(game_string, "\x1b[1m");
}

bool use_bold_for_score(const GameStringOptions *game_string_options) {
  if (game_string_options == NULL) {
    return false;
  }
  return game_string_options->on_turn_score_style ==
         GAME_STRING_ON_TURN_SCORE_BOLD;
}

void string_builder_add_player_row(const LetterDistribution *ld,
                                   const Player *player,
                                   const GameStringOptions *game_string_options,
                                   const char *player_name,
                                   StringBuilder *game_string,
                                   size_t player_name_max_length,
                                   bool player_on_turn, bool game_over) {

  const char *player_marker = " ";
  if (player_on_turn && !game_over) {
    player_marker = use_ascii_on_turn_marker(game_string_options) ? ">" : "➤";
  }

  if (player_on_turn && should_print_escape_codes(game_string_options) &&
      !game_over) {
    string_builder_add_player_on_turn_color(game_string, game_string_options);
  }

  string_builder_add_formatted_string(game_string, "%s %-*s", player_marker,
                                      player_name_max_length + 2, player_name);

  if (player_on_turn && should_print_escape_codes(game_string_options) &&
      game_string_option_has_on_turn_color(game_string_options) && !game_over) {
    string_builder_add_color_reset(game_string);
  }

  const Rack *player_rack = player_get_rack(player);
  string_builder_add_rack(game_string, player_rack, ld, false);
  string_builder_add_formatted_string(
      game_string, "%*s%d",
      RACK_DISPLAY_WIDTH - rack_get_total_letters(player_rack), "",
      equity_to_int(player_get_score(player)));
}

void string_builder_add_board_square_color(StringBuilder *game_string,
                                           const Board *board, int row, int col,
                                           const HeatMap *heat_map, int play,
                                           int ply, heat_map_t heat_map_type) {
  const uint8_t current_letter = board_get_letter(board, row, col);
  if (current_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
    string_builder_add_string(
        game_string,
        bonus_square_to_color_code(board_get_bonus_square(board, row, col)));
    if (heat_map) {
      // Add background color
      const uint64_t square_count =
          heat_map_get_count(heat_map, play, ply, row, col, heat_map_type);
      const uint64_t total =
          heat_map_get_board_count_max(heat_map, play, ply, heat_map_type);
      const double frac = (double)square_count / (double)total;
      double threshold = HEAT_MAP_FRAC_DELIMITER;
      int color_index = 0;
      while (frac > threshold &&
             color_index < HEAT_MAP_NUM_BACKGROUND_COLORS - 1) {
        threshold += HEAT_MAP_FRAC_DELIMITER;
        color_index++;
      }
      string_builder_add_formatted_string(
          game_string, ";%d", heat_map_ascending_color_codes[color_index]);
    }
    string_builder_add_char(game_string, 'm');
  } else {
    string_builder_add_color_reset(game_string);
    string_builder_add_color_bold(game_string);
  }
}

bool use_board_color(const GameStringOptions *game_string_options) {
  if (game_string_options == NULL) {
    return false;
  }
  return game_string_options->board_color != GAME_STRING_BOARD_COLOR_NONE;
}

bool should_print_alt_tiles(const GameStringOptions *game_string_options) {
  if (game_string_options == NULL) {
    return false;
  }
  return (game_string_options->board_tile_glyphs ==
          GAME_STRING_BOARD_TILE_GLYPHS_ALT);
}

void string_builder_add_board_top_border(
    const GameStringOptions *game_string_options, StringBuilder *game_string) {
  string_builder_add_string(game_string, "  ");
  if ((game_string_options == NULL) ||
      (game_string_options->board_border == GAME_STRING_BOARD_BORDER_ASCII)) {
    string_builder_add_string(game_string, " ");
    for (int i = 0; i < BOARD_DIM; i++) {
      string_builder_add_string(game_string, "--");
    }
    string_builder_add_string(game_string, " ");
  } else {
    string_builder_add_string(game_string, "┏");
    for (int i = 0; i < BOARD_DIM; i++) {
      string_builder_add_string(game_string, "━━");
    }
    string_builder_add_string(game_string, "┓");
  }
  string_builder_add_string(game_string, " ");
}

void string_builder_add_board_bottom_border(
    const GameStringOptions *game_string_options, StringBuilder *game_string) {
  string_builder_add_string(game_string, "  ");
  if ((game_string_options == NULL) ||
      (game_string_options->board_border == GAME_STRING_BOARD_BORDER_ASCII)) {
    string_builder_add_string(game_string, " ");
    for (int i = 0; i < BOARD_DIM; i++) {
      string_builder_add_string(game_string, "--");
    }
    string_builder_add_string(game_string, " ");
  } else {
    string_builder_add_string(game_string, "┗");
    for (int i = 0; i < BOARD_DIM; i++) {
      string_builder_add_string(game_string, "━━");
    }
    string_builder_add_string(game_string, "┛");
  }
  string_builder_add_string(game_string, " ");
}

void string_builder_add_board_side_border(
    const GameStringOptions *game_string_options, StringBuilder *game_string) {
  if ((game_string_options == NULL) ||
      (game_string_options->board_border == GAME_STRING_BOARD_BORDER_ASCII)) {
    string_builder_add_string(game_string, "|");
  } else {
    string_builder_add_string(game_string, "┃");
  }
}

static const char *full_width_column_label_strings[] = {
    "Ａ", "Ｂ", "Ｃ", "Ｄ", "Ｅ", "Ｆ", "Ｇ", "Ｈ", "Ｉ",
    "Ｊ", "Ｋ", "Ｌ", "Ｍ", "Ｎ", "Ｏ", "Ｐ", "Ｑ", "Ｒ",
    "Ｓ", "Ｔ", "Ｕ", "Ｖ", "Ｗ", "Ｘ", "Ｙ", "Ｚ"};

void string_builder_add_board_row(const LetterDistribution *ld,
                                  const Board *board,
                                  const GameStringOptions *game_string_options,
                                  StringBuilder *game_string, int row,
                                  const HeatMap *heat_map, int play, int ply,
                                  heat_map_t heat_map_type) {
  string_builder_add_formatted_string(game_string, "%2d", row + 1);
  string_builder_add_board_side_border(game_string_options, game_string);
  for (int i = 0; i < BOARD_DIM; i++) {
    if (should_print_escape_codes(game_string_options) &&
        use_board_color(game_string_options)) {
      string_builder_add_board_square_color(game_string, board, row, i,
                                            heat_map, play, ply, heat_map_type);
    }
    const uint8_t current_letter = board_get_letter(board, row, i);
    if (current_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
      if (should_print_alt_tiles(game_string_options)) {
        string_builder_add_string(
            game_string,
            bonus_square_to_alt_string(board_get_bonus_square(board, row, i)));
      } else {
        string_builder_add_char(
            game_string,
            bonus_square_to_char(board_get_bonus_square(board, row, i)));
        string_builder_add_string(game_string, " ");
      }
    } else {
      if (should_print_alt_tiles(game_string_options)) {
        string_builder_add_user_visible_alt_letter(game_string, ld,
                                                   current_letter);
      } else {
        string_builder_add_user_visible_letter(game_string, ld, current_letter);
        string_builder_add_string(game_string, " ");
      }
    }
    if (should_print_escape_codes(game_string_options)) {
      string_builder_add_color_reset(game_string);
    }
  }
  string_builder_add_board_side_border(game_string_options, game_string);
}

void string_builder_add_move_with_rank_and_equity(const Game *game,
                                                  const MoveList *move_list,
                                                  StringBuilder *game_string,
                                                  int move_index) {
  const Board *board = game_get_board(game);
  const Move *move = move_list_get_move(move_list, move_index);
  const LetterDistribution *ld = game_get_ld(game);
  string_builder_add_formatted_string(game_string, " %d ", move_index + 1);
  string_builder_add_move(game_string, board, move, ld, true);
  string_builder_add_string(game_string, " ");
  string_builder_add_equity(game_string, move_get_equity(move), "%0.2f");
}

void string_builder_add_board_column_header(
    const GameStringOptions *game_string_options, int col,
    StringBuilder *game_string) {
  if ((game_string_options == NULL) ||
      game_string_options->board_column_label ==
          GAME_STRING_BOARD_COLUMN_LABEL_ASCII) {
    string_builder_add_formatted_string(game_string, "%c ",
                                        col + ASCII_UPPERCASE_A);
  } else {
    if (col < BOARD_NUM_COLUMN_LABELS) {
      string_builder_add_string(game_string,
                                full_width_column_label_strings[col]);
    } else {
      string_builder_add_string(game_string, " ");
    }
  }
}

void string_builder_add_game_internal(
    const Game *game, const MoveList *move_list,
    const GameStringOptions *game_string_options,
    const GameHistory *game_history, const HeatMap *heat_map, int play, int ply,
    heat_map_t heat_map_type, StringBuilder *game_string) {
  const Board *board = game_get_board(game);
  const Bag *bag = game_get_bag(game);
  const Player *player0 = game_get_player(game, 0);
  const Player *player1 = game_get_player(game, 1);
  const LetterDistribution *ld = game_get_ld(game);

  string_builder_add_string(game_string, "\n   ");

  for (int i = 0; i < BOARD_DIM; i++) {
    string_builder_add_board_column_header(game_string_options, i, game_string);
  }

  string_builder_add_string(game_string, "  ");

  const char *player1_name = PLAYER_ONE_DEFAULT_NAME;
  const char *player2_name = PLAYER_TWO_DEFAULT_NAME;
  if (game_history) {
    player1_name = game_history_player_get_name(game_history, 0);
    player2_name = game_history_player_get_name(game_history, 1);
  }

  const size_t player1_name_length = string_length(player1_name);
  const size_t player2_name_length = string_length(player2_name);
  size_t player_name_max_length = player1_name_length;
  if (player2_name_length > player_name_max_length) {
    player_name_max_length = player2_name_length;
  }

  int player_on_turn_index = game_get_player_on_turn_index(game);
  const bool game_is_over =
      game_get_game_end_reason(game) != GAME_END_REASON_NONE;
  string_builder_add_player_row(ld, player0, game_string_options, player1_name,
                                game_string, player_name_max_length,
                                player_on_turn_index == 0, game_is_over);
  string_builder_add_string(game_string, "\n");

  string_builder_add_board_top_border(game_string_options, game_string);
  string_builder_add_player_row(ld, player1, game_string_options, player2_name,
                                game_string, player_name_max_length,
                                player_on_turn_index == 1, game_is_over);
  string_builder_add_string(game_string, "\n");

  const int num_bag_letters = bag_get_letters(bag);
  int letters_per_row =
      (num_bag_letters + UNSEEN_LETTER_ROWS - 1) / UNSEEN_LETTER_ROWS;
  if (letters_per_row < MIN_UNSEEN_LETTERS_PER_ROW) {
    letters_per_row = MIN_UNSEEN_LETTERS_PER_ROW;
  }
  int bag_letter_count[MAX_ALPHABET_SIZE];
  memset(bag_letter_count, 0, sizeof(bag_letter_count));
  bag_increment_unseen_count(bag, bag_letter_count);
  MachineLetter bag_letters[MAX_BAG_SIZE];

  int letter_index = 0;
  for (int i = 0; i < MAX_ALPHABET_SIZE; i++) {
    for (int j = 0; j < bag_letter_count[i]; j++) {
      bag_letters[letter_index++] = i;
    }
  }
  letter_index = 0;

  const bool add_game_event =
      game_history && game_history_get_num_played_events(game_history) > 0;

  int game_event_index = -1;
  const GameEvent *game_event = NULL;
  for (int i = 0; i < BOARD_DIM; i++) {
    string_builder_add_board_row(ld, board, game_string_options, game_string, i,
                                 heat_map, play, ply, heat_map_type);
    string_builder_add_spaces(game_string, 3);
    if (i == UNSEEN_START_ROW) {
      string_builder_add_formatted_string(game_string, "Unseen: (%d)",
                                          num_bag_letters);
    } else if (i > UNSEEN_START_ROW + 1 && letter_index < num_bag_letters) {
      for (int j = 0; j < letters_per_row && letter_index < num_bag_letters;
           j++) {
        string_builder_add_user_visible_letter(game_string, ld,
                                               bag_letters[letter_index++]);
        string_builder_add_spaces(game_string, 1);
      }
    } else if (i == FINAL_PASS_PROMPT_ROW && game_history &&
               game_history_get_waiting_for_final_pass_or_challenge(
                   game_history) &&
               game_history_get_num_events(game_history) ==
                   game_history_get_num_played_events(game_history)) {
      string_builder_add_string(game_string,
                                "Waiting for final pass or challenge...");
    } else if (add_game_event) {
      if (i == GAME_EVENT_START_ROW) {
        game_event_index = game_history_get_num_played_events(game_history) - 1;
        game_event = game_history_get_event(game_history, game_event_index);
        string_builder_add_formatted_string(game_string,
                                            "Event %d:", game_event_index + 1);
      } else if (i == GAME_EVENT_START_ROW + 1) {
        string_builder_add_string(
            game_string,
            game_history_player_get_name(
                game_history, game_event_get_player_index(game_event)));
      } else if (i == GAME_EVENT_START_ROW + 2) {
        const Rack *player_rack = game_event_get_const_rack(game_event);
        const GameEvent *previous_game_event = NULL;
        switch (game_event_get_type(game_event)) {
        case GAME_EVENT_TILE_PLACEMENT_MOVE:
          string_builder_add_move(
              game_string, board,
              validated_moves_get_move(game_event_get_vms(game_event), 0), ld,
              true);
          string_builder_add_string(game_string, " ");
          string_builder_add_rack(game_string, player_rack, ld, false);
          break;
        case GAME_EVENT_PASS:
          string_builder_add_string(game_string, "(pass) ");
          string_builder_add_rack(game_string, player_rack, ld, false);
          break;
        case GAME_EVENT_EXCHANGE:;
          Rack exchanged_tiles;
          rack_set_dist_size_and_reset(&exchanged_tiles, ld_get_size(ld));
          const Move *move =
              validated_moves_get_move(game_event_get_vms(game_event), 0);
          const int num_exch = move_get_tiles_played(move);
          for (int j = 0; j < num_exch; j++) {
            rack_add_letter(&exchanged_tiles, move_get_tile(move, j));
          }
          string_builder_add_string(game_string, "(exch ");
          string_builder_add_rack(game_string, &exchanged_tiles, ld, false);
          string_builder_add_string(game_string, ") ");
          string_builder_add_rack(game_string, player_rack, ld, false);
          break;
        case GAME_EVENT_PHONY_TILES_RETURNED:;
          previous_game_event =
              game_history_get_event(game_history, game_event_index - 1);
          string_builder_add_move(
              game_string, board,
              validated_moves_get_move(game_event_get_vms(previous_game_event),
                                       0),
              ld, true);
          string_builder_add_string(game_string, " ");
          string_builder_add_rack(
              game_string, game_event_get_const_rack(previous_game_event), ld,
              false);
          string_builder_add_string(game_string, " (challenged off)");
          break;
        case GAME_EVENT_CHALLENGE_BONUS:;
          previous_game_event =
              game_history_get_event(game_history, game_event_index - 1);
          string_builder_add_move(
              game_string, board,
              validated_moves_get_move(game_event_get_vms(previous_game_event),
                                       0),
              ld, true);
          string_builder_add_formatted_string(
              game_string, " + %d = %d ",
              equity_to_int(game_event_get_score_adjustment(game_event)),
              equity_to_int(game_event_get_move_score(previous_game_event)));
          string_builder_add_rack(
              game_string, game_event_get_const_rack(previous_game_event), ld,
              false);
          break;
        case GAME_EVENT_END_RACK_POINTS:
          string_builder_add_rack(game_string, player_rack, ld, false);
          string_builder_add_formatted_string(
              game_string, " +%d (end rack points)",
              equity_to_int(game_event_get_score_adjustment(game_event)));
          break;
        case GAME_EVENT_TIME_PENALTY:
          string_builder_add_formatted_string(
              game_string, " %d (overtime penalty)",
              equity_to_int(game_event_get_score_adjustment(game_event)));
          break;
        case GAME_EVENT_END_RACK_PENALTY:
          string_builder_add_rack(game_string, player_rack, ld, false);
          string_builder_add_formatted_string(
              game_string, " %d (end rack penalty)",
              equity_to_int(game_event_get_score_adjustment(game_event)));
          break;
        case GAME_EVENT_UNKNOWN:
          log_fatal("encountered unknown game event type when generating game "
                    "string\n");
          break;
        }
      }
    }
    string_builder_add_string(game_string, "\n");
  }
  string_builder_add_board_bottom_border(game_string_options, game_string);
  string_builder_add_string(game_string, "\n");
  int num_moves_to_display = 0;
  if (move_list) {
    num_moves_to_display = move_list_get_count(move_list);
    if (num_moves_to_display > MAX_MOVES) {
      num_moves_to_display = MAX_MOVES;
    }
  }
  for (int i = 0; i < num_moves_to_display; i++) {
    string_builder_add_move_with_rank_and_equity(game, move_list, game_string,
                                                 i);
  }
  string_builder_add_string(game_string, "\n");
}

void string_builder_add_game(const Game *game, const MoveList *move_list,
                             const GameStringOptions *game_string_options,
                             const GameHistory *game_history,
                             StringBuilder *game_string) {
  string_builder_add_game_internal(game, move_list, game_string_options,
                                   game_history, NULL, 0, 0, 0, game_string);
}

void string_builder_add_game_with_heat_map(
    const Game *game, const MoveList *move_list,
    const GameStringOptions *game_string_options,
    const GameHistory *game_history, const HeatMap *heat_map, int play, int ply,
    heat_map_t heat_map_type, StringBuilder *game_string) {
  string_builder_add_game_internal(game, move_list, game_string_options,
                                   game_history, heat_map, play, ply,
                                   heat_map_type, game_string);
}

GameStringOptions *game_string_options_create_default(void) {
  GameStringOptions *gso = malloc_or_die(sizeof(GameStringOptions));
  gso->board_color = GAME_STRING_BOARD_COLOR_NONE;
  gso->board_tile_glyphs = GAME_STRING_BOARD_TILE_GLYPHS_PRIMARY;
  gso->board_border = GAME_STRING_BOARD_BORDER_ASCII;
  gso->board_column_label = GAME_STRING_BOARD_COLUMN_LABEL_ASCII;
  gso->on_turn_marker = GAME_STRING_ON_TURN_MARKER_ASCII;
  gso->on_turn_color = GAME_STRING_ON_TURN_COLOR_NONE;
  gso->on_turn_score_style = GAME_STRING_ON_TURN_SCORE_NORMAL;
  return gso;
}

GameStringOptions *game_string_options_create_pretty(void) {
  GameStringOptions *gso = malloc_or_die(sizeof(GameStringOptions));
  gso->board_color = GAME_STRING_BOARD_COLOR_ANSI;
  gso->board_tile_glyphs = GAME_STRING_BOARD_TILE_GLYPHS_ALT;
  gso->board_border = GAME_STRING_BOARD_BORDER_BOX_DRAWING;
  gso->board_column_label = GAME_STRING_BOARD_COLUMN_LABEL_FULLWIDTH;
  gso->on_turn_marker = GAME_STRING_ON_TURN_MARKER_ARROWHEAD;
  gso->on_turn_color = GAME_STRING_ON_TURN_COLOR_ANSI_GREEN;
  gso->on_turn_score_style = GAME_STRING_ON_TURN_SCORE_BOLD;
  return gso;
}

GameStringOptions *game_string_options_create(
    game_string_board_color_t board_color,
    game_string_board_tile_glyphs_t board_tile_glyphs,
    game_string_board_border_t board_border,
    game_string_board_column_label_t column_label,
    game_string_on_turn_marker_t on_turn_marker,
    game_string_on_turn_color_t on_turn_color,
    game_string_on_turn_score_style_t on_turn_score_style) {
  GameStringOptions *gso = game_string_options_create_default();
  gso->board_color = board_color;
  gso->board_tile_glyphs = board_tile_glyphs;
  gso->board_border = board_border;
  gso->board_column_label = column_label;
  gso->on_turn_marker = on_turn_marker;
  gso->on_turn_color = on_turn_color;
  gso->on_turn_score_style = on_turn_score_style;
  return gso;
}

void game_string_options_destroy(GameStringOptions *gso) { free(gso); }

void string_builder_add_game_variant(StringBuilder *sb,
                                     game_variant_t variant) {
  const char *variant_name;
  switch (variant) {
  case GAME_VARIANT_CLASSIC:
    variant_name = GAME_VARIANT_CLASSIC_NAME;
    break;
  case GAME_VARIANT_WORDSMOG:
    variant_name = GAME_VARIANT_WORDSMOG_NAME;
    break;
  default:
    variant_name = GAME_VARIANT_UNKNOWN_NAME;
    break;
  }
  string_builder_add_string(sb, variant_name);
}