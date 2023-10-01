#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "config.h"
#include "cross_set.h"
#include "game.h"
#include "log.h"
#include "movegen.h"
#include "player.h"
#include "string_util.h"
#include "util.h"

char add_player_score(const char *cgp, int *cgp_index, Game *game,
                      int player_index) {
  char cgp_char = cgp[*cgp_index];
  char score[10] = "";
  while (cgp_char != '/' && cgp_char != ' ') {
    sprintf(score + strlen(score), "%c", cgp_char);
    (*cgp_index)++;
    cgp_char = cgp[*cgp_index];
  }
  game->players[player_index]->score = atoi(score);
  return cgp_char;
}

void draw_letter_to_rack(Bag *bag, Rack *rack, uint8_t letter) {
  draw_letter(bag, letter);
  add_letter_to_rack(rack, letter);
}

char add_player_rack(const char *cgp, int *cgp_index, Game *game,
                     int player_index) {
  char rack_placeholder[30];
  char cgp_char = cgp[*cgp_index];
  int rpidx = 0;
  while (cgp_char != '/' && cgp_char != ' ') {
    rack_placeholder[rpidx] = cgp_char;
    rpidx++;
    (*cgp_index)++;
    cgp_char = cgp[*cgp_index];
  }
  rack_placeholder[rpidx] = '\0';
  uint8_t mls[RACK_SIZE];
  int num_mls = str_to_machine_letters(game->gen->letter_distribution,
                                       rack_placeholder, false, mls);
  assert(num_mls <= RACK_SIZE);
  for (int i = 0; i < num_mls; i++) {
    draw_letter_to_rack(game->gen->bag, game->players[player_index]->rack,
                        mls[i]);
  }
  return cgp_char;
}

void load_cgp(Game *game, const char *cgp) {
  if (is_all_whitespace_or_empty(cgp)) {
    return;
  }
  // Set all tiles:
  int cgp_index = 0;
  char cgp_char = cgp[cgp_index];
  int current_board_index = 0;
  int is_digit = 0;
  int previous_was_digit = 0;
  char current_digits[5] = "";

  int began_multi = 0;
  int multi_idx = 0;
  char multitile[MAX_LETTER_CHAR_LENGTH];
  char row_aggreg[45]; // idk, some big size.
  int row_aggreg_idx = 0;
  uint8_t mls[25];
  while (cgp_char != ' ') {
    is_digit = isdigit(cgp_char);

    if (is_digit) {
      sprintf(current_digits + strlen(current_digits), "%c", cgp_char);
    } else if (previous_was_digit) {
      current_board_index += atoi(current_digits);
      current_digits[0] = '\0';
    }
    if (!is_digit && cgp_char != '/') {
      // it's a letter or a portion of a letter.
      if (cgp_char == '[') {
        // this is a multi-letter tile by the cgp spec.
        // If we've been building up a string of characters already,
        // let's convert these first.
        if (row_aggreg_idx > 0) {
          row_aggreg[row_aggreg_idx] = '\0';

          int num_mls = str_to_machine_letters(game->gen->letter_distribution,
                                               row_aggreg, false, mls);
          row_aggreg_idx = 0;
          for (int i = 0; i < num_mls; i++) {
            set_letter_by_index(game->gen->board, current_board_index, mls[i]);
            draw_letter(
                game->gen->bag,
                get_letter_by_index(game->gen->board, current_board_index));
            current_board_index++;
            game->gen->board->tiles_played++;
          }
        }

        began_multi = 1;
        multi_idx = 0;
      } else if (cgp_char == ']') {
        began_multi = 0;
        multitile[multi_idx] = '\0';
        int ml = human_readable_letter_to_machine_letter(
            game->gen->letter_distribution, multitile);
        set_letter_by_index(game->gen->board, current_board_index, ml);
        draw_letter(game->gen->bag,
                    get_letter_by_index(game->gen->board, current_board_index));
        current_board_index++;
        game->gen->board->tiles_played++;

      } else {
        if (began_multi) {
          multitile[multi_idx] = cgp_char;
          multi_idx++;
        } else {
          row_aggreg[row_aggreg_idx] = cgp_char;
          row_aggreg_idx++;
        }
      }
    }

    if (row_aggreg_idx > 0) {
      row_aggreg[row_aggreg_idx] = '\0';

      int num_mls = str_to_machine_letters(game->gen->letter_distribution,
                                           row_aggreg, false, mls);
      row_aggreg_idx = 0;
      for (int i = 0; i < num_mls; i++) {
        set_letter_by_index(game->gen->board, current_board_index, mls[i]);
        draw_letter(game->gen->bag,
                    get_letter_by_index(game->gen->board, current_board_index));
        current_board_index++;
        game->gen->board->tiles_played++;
      }
    }

    cgp_index++;
    cgp_char = cgp[cgp_index];
    previous_was_digit = is_digit;
  }

  // Skip the whitespace
  while (cgp_char == ' ') {
    cgp_index++;
    cgp_char = cgp[cgp_index];
  }

  // Set the racks
  int player_index = 0;
  if (cgp_char == '/') {
    // player0 has an empty rack
    player_index = 1;
    // Advance the pointer
    cgp_index++;
  }

  cgp_char = add_player_rack(cgp, &cgp_index, game, player_index);

  if (cgp_char == '/') {
    player_index = 1;
    // Advance the pointer
    cgp_index++;
    cgp_char = add_player_rack(cgp, &cgp_index, game, 1);
  }

  // Skip the whitespace
  while (cgp_char == ' ') {
    cgp_index++;
    cgp_char = cgp[cgp_index];
  }

  add_player_score(cgp, &cgp_index, game, 0);
  cgp_index++;
  add_player_score(cgp, &cgp_index, game, 1);
  cgp_index++;

  cgp_char = cgp[cgp_index];
  // Skip the whitespace
  while (cgp_char == ' ') {
    cgp_index++;
    cgp_char = cgp[cgp_index];
  }

  // Set number of consecutive zeros
  game->consecutive_scoreless_turns = cgp_char - '0';
  game->player_on_turn_index = 0;

  generate_all_cross_sets(game->gen->board,
                          game->players[0]->strategy_params->kwg,
                          game->players[1]->strategy_params->kwg,
                          game->gen->letter_distribution, 0);
  update_all_anchors(game->gen->board);

  if (game->consecutive_scoreless_turns >= MAX_SCORELESS_TURNS) {
    game->game_end_reason = GAME_END_REASON_CONSECUTIVE_ZEROS;
  } else if (game->gen->bag->last_tile_index == -1 &&
             (game->players[0]->rack->empty || game->players[1]->rack->empty)) {
    game->game_end_reason = GAME_END_REASON_STANDARD;
  } else {
    game->game_end_reason = GAME_END_REASON_NONE;
  }
}

// return lexicon and letter distribution from the cgp string.
void lexicon_ld_from_cgp(char *cgp, char *lexicon, char *ldname) {
  // copy string since we are going to modify it with strtok :(
  char cgpcopy[512];
  strcpy(cgpcopy, cgp);
  char *token;
  token = strtok(cgpcopy, " ");
  // cgp consists of FEN racks scores zeroturns opcode val; opcode val; ...
  int getting_lex = 0;
  int getting_ld = 0;
  while (token != NULL) {
    if (getting_lex) {
      strcpy(lexicon, token);
      lexicon[strlen(lexicon) - 1] = '\0'; // overwrite the semicolon
    } else if (getting_ld) {
      strcpy(ldname, token);
      ldname[strlen(ldname) - 1] = '\0'; // overwrite the semicolon
    }

    getting_lex = strcmp(token, "lex") == 0;
    getting_ld = strcmp(token, "ld") == 0;
    token = strtok(NULL, " ");
  }
  // if not specified, the default ld will be english, at least according to our
  // program.
  if (strcmp(ldname, "") == 0) {
    strcpy(ldname, "english");
  }
}

int tiles_unseen(Game *game) {
  int bag_idx = game->gen->bag->last_tile_index;
  int their_rack_tiles =
      game->players[1 - game->player_on_turn_index]->rack->number_of_letters;

  return (their_rack_tiles + bag_idx + 1);
}

void reset_game(Game *game) {
  reset_generator(game->gen);
  reset_player(game->players[0]);
  reset_player(game->players[1]);
  game->player_on_turn_index = 0;
  game->consecutive_scoreless_turns = 0;
  game->game_end_reason = GAME_END_REASON_NONE;
  game->backup_cursor = 0;
}

void set_player_on_turn(Game *game, int player_on_turn_index) {
  game->player_on_turn_index = player_on_turn_index;
}

void pre_allocate_backups(Game *game) {
  // pre-allocate heap backup structures to make backups as fast as possible.
  for (int i = 0; i < MAX_SEARCH_DEPTH; i++) {
    game->game_backups[i] = malloc_or_die(sizeof(MinimalGameBackup));
    game->game_backups[i]->bag = create_bag(game->gen->letter_distribution);
    game->game_backups[i]->board = create_board();
    game->game_backups[i]->p0rack =
        create_rack(game->gen->letter_distribution->size);
    game->game_backups[i]->p1rack =
        create_rack(game->gen->letter_distribution->size);
  }
}

void set_backup_mode(Game *game, int backup_mode) {
  game->backup_mode = backup_mode;
  if (backup_mode == BACKUP_MODE_SIMULATION && !game->backups_preallocated) {
    game->backup_cursor = 0;
    pre_allocate_backups(game);
    game->backups_preallocated = 1;
  }
}

Game *create_game(Config *config) {
  Game *game = malloc_or_die(sizeof(Game));
  game->gen = create_generator(config);
  game->players[0] =
      create_player(0, "player_1", config->letter_distribution->size);
  game->players[1] =
      create_player(1, "player_2", config->letter_distribution->size);
  game->players[0]->strategy_params =
      copy_strategy_params(config->player_1_strategy_params);
  game->players[1]->strategy_params =
      copy_strategy_params(config->player_2_strategy_params);
  game->player_on_turn_index = 0;
  game->consecutive_scoreless_turns = 0;
  game->game_end_reason = GAME_END_REASON_NONE;
  game->backup_cursor = 0;
  game->backup_mode = BACKUP_MODE_OFF;
  game->backups_preallocated = 0;
  return game;
}

Game *copy_game(Game *game, int move_list_size) {
  Game *new_game = malloc_or_die(sizeof(Game));
  new_game->gen = copy_generator(game->gen, move_list_size);
  for (int j = 0; j < 2; j++) {
    new_game->players[j] = copy_player(game->players[j]);
  }
  new_game->player_on_turn_index = game->player_on_turn_index;
  new_game->consecutive_scoreless_turns = game->consecutive_scoreless_turns;
  new_game->game_end_reason = game->game_end_reason;
  // note: game backups must be explicitly handled by the caller if they want
  // game copies to have backups.
  new_game->backup_cursor = 0;
  new_game->backup_mode = BACKUP_MODE_OFF;
  new_game->backups_preallocated = 0;
  return new_game;
}

void backup_game(Game *game) {
  if (game->backup_mode == BACKUP_MODE_OFF) {
    return;
  }
  if (game->backup_mode == BACKUP_MODE_SIMULATION) {
    MinimalGameBackup *state = game->game_backups[game->backup_cursor];
    copy_board_into(state->board, game->gen->board);
    copy_bag_into(state->bag, game->gen->bag);
    state->game_end_reason = game->game_end_reason;
    state->player_on_turn_index = game->player_on_turn_index;
    state->consecutive_scoreless_turns = game->consecutive_scoreless_turns;
    copy_rack_into(state->p0rack, game->players[0]->rack);
    state->p0score = game->players[0]->score;
    copy_rack_into(state->p1rack, game->players[1]->rack);
    state->p1score = game->players[1]->score;

    game->backup_cursor++;
  }
}

void unplay_last_move(Game *game) {
  // restore from backup (pop the last element).
  if (game->backup_cursor == 0) {
    log_fatal("error: no backup\n");
  }
  MinimalGameBackup *state = game->game_backups[game->backup_cursor - 1];
  game->backup_cursor--;

  game->consecutive_scoreless_turns = state->consecutive_scoreless_turns;
  game->game_end_reason = state->game_end_reason;
  game->player_on_turn_index = state->player_on_turn_index;
  game->players[0]->score = state->p0score;
  game->players[1]->score = state->p1score;
  copy_rack_into(game->players[0]->rack, state->p0rack);
  copy_rack_into(game->players[1]->rack, state->p1rack);
  copy_bag_into(game->gen->bag, state->bag);
  copy_board_into(game->gen->board, state->board);
}

void destroy_backups(Game *game) {
  for (int i = 0; i < MAX_SEARCH_DEPTH; i++) {
    destroy_rack(game->game_backups[i]->p0rack);
    destroy_rack(game->game_backups[i]->p1rack);
    destroy_bag(game->game_backups[i]->bag);
    destroy_board(game->game_backups[i]->board);
    free(game->game_backups[i]);
  }
}

void destroy_game(Game *game) {
  destroy_generator(game->gen);
  destroy_player(game->players[0]);
  destroy_player(game->players[1]);
  if (game->backups_preallocated) {
    destroy_backups(game);
  }
  free(game);
}

game_variant_t get_game_variant_type_from_name(const char *variant_name) {
  game_variant_t game_variant = GAME_VARIANT_UNKNOWN;
  if (!strcmp(variant_name, "classic")) {
    game_variant = GAME_VARIANT_CLASSIC;
  } else if (!strcmp(variant_name, "wordsmog")) {
    game_variant = GAME_VARIANT_WORDSMOG;
  }
  return game_variant;
}

// Human readable print functions

void string_builder_add_player_row(LetterDistribution *letter_distribution,
                                   Player *player, bool player_on_turn,
                                   StringBuilder *game_string) {

  char *player_on_turn_marker = "-> ";
  char *player_off_turn_marker = "   ";
  char *player_marker = player_on_turn_marker;
  if (!player_on_turn) {
    player_marker = player_off_turn_marker;
  }

  string_builder_add_formatted_string(game_string, "%s%s%*s", player_marker,
                                      player->name, 25 - strlen(player->name),
                                      "");
  string_builder_add_rack(player->rack, letter_distribution, game_string);
  string_builder_add_formatted_string(game_string, "%*s%d",
                                      10 - player->rack->number_of_letters, "",
                                      player->score);
}

void string_builder_add_board_row(LetterDistribution *letter_distribution,
                                  Board *board, int row,
                                  StringBuilder *game_string) {
  string_builder_add_formatted_string(game_string, "%2d|", row + 1);
  for (int i = 0; i < BOARD_DIM; i++) {
    uint8_t current_letter = get_letter(board, row, i);
    if (current_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
      string_builder_add_char(game_string,
                              CROSSWORD_GAME_BOARD[(row * BOARD_DIM) + i], 0);
    } else {
      string_builder_add_user_visible_letter(letter_distribution,
                                             current_letter, 0, game_string);
    }
    string_builder_add_string(game_string, " ", 0);
  }
  string_builder_add_string(game_string, "|", 0);
}

void string_builder_add_move_with_rank_and_equity(Game *game, int move_index,
                                                  StringBuilder *game_string) {
  Move *move = game->gen->move_list->moves[move_index];
  string_builder_add_int(game_string, move_index + 1, 0);
  string_builder_add_move(game->gen->board, move,
                          game->gen->letter_distribution, game_string);
  string_builder_add_double(game_string, move->equity, 0);
}

void string_builder_add_game(Game *game, StringBuilder *game_string) {
  // TODO: update for super crossword game
  string_builder_add_string(game_string, "   A B C D E F G H I J K L M N O   ",
                            0);
  string_builder_add_player_row(game->gen->letter_distribution,
                                game->players[0],
                                game->player_on_turn_index == 0, game_string);
  string_builder_add_string(game_string,
                            "\n   ------------------------------  ", 0);
  string_builder_add_player_row(game->gen->letter_distribution,
                                game->players[1],
                                game->player_on_turn_index == 1, game_string);
  string_builder_add_string(game_string, "\n", 0);

  for (int i = 0; i < BOARD_DIM; i++) {
    string_builder_add_board_row(game->gen->letter_distribution,
                                 game->gen->board, i, game_string);
    if (i == 0) {
      string_builder_add_string(
          game_string, " --Tracking-----------------------------------", 0);
    } else if (i == 1) {
      string_builder_add_string(game_string, " ", 0);
      string_builder_add_bag(game->gen->bag, game->gen->letter_distribution, 0,
                             game_string);
      string_builder_add_string(game_string, "  ", 0);
      string_builder_add_int(game_string, game->gen->bag->last_tile_index + 1,
                             0);
    } else if (i - 2 < game->gen->move_list->count) {
      string_builder_add_move_with_rank_and_equity(game, i - 2, game_string);
    }
    string_builder_add_string(game_string, "\n", 0);
  }

  string_builder_add_string(game_string, "   ------------------------------\n",
                            0);
}