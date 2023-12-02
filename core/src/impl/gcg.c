#include <ctype.h>
#include <regex.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "constants.h"
#include "game_history.h"
#include "gcg.h"
#include "log.h"
#include "string_util.h"
#include "../util/util.h"

#define MAX_GROUPS 7

typedef enum {
  GCG_ENCODING_ISO_8859_1,
  GCG_ENCODING_UTF8,
} gcg_encoding_t;

// A Token is an event in a gcg_string file.
typedef enum {
  GCG_UNKNOWN_TOKEN,
  GCG_PLAYER_TOKEN,
  GCG_TITLE_TOKEN,
  GCG_DESCRIPTION_TOKEN,
  GCG_ID_TOKEN,
  GCG_RACK1_TOKEN,
  GCG_RACK2_TOKEN,
  GCG_ENCODING_TOKEN,
  GCG_MOVE_TOKEN,
  GCG_NOTE_TOKEN,
  GCG_LEXICON_TOKEN,
  GCG_PHONY_TILES_RETURNED_TOKEN,
  GCG_PASS_TOKEN,
  GCG_CHALLENGE_BONUS_TOKEN,
  GCG_EXCHANGE_TOKEN,
  GCG_END_RACK_POINTS_TOKEN,
  GCG_TIME_PENALTY_TOKEN,
  GCG_LAST_RACK_PENALTY_TOKEN,
  GCG_GAME_TYPE_TOKEN,
  GCG_TILE_SET_TOKEN,
  GCG_BOARD_LAYOUT_TOKEN,
  GCG_TILE_DISTRIBUTION_NAME_TOKEN,
  NUMBER_OF_GCG_TOKENS,
} gcg_token_t;

typedef struct TokenRegexPair {
  gcg_token_t token;
  regex_t regex;
} TokenRegexPair;

typedef struct GCGParser {
  regmatch_t matching_groups[(MAX_GROUPS)];
  StringBuilder *note_builder;
  gcg_token_t previous_token;
  TokenRegexPair **token_regex_pairs;
  int number_of_token_regex_pairs;
  // Owned by the caller
  GameHistory *game_history;
} GCGParser;

const char *player_regex =
    "#player([1-2])[[:space:]]+([^[:space:]]+)[[:space:]]+(.+)";
const char *title_regex = "#title[[:space:]]*(.*)";
const char *description_regex = "#description[[:space:]]*(.*)";
const char *id_regex =
    "#id[[:space:]]*([^[:space:]]+)[[:space:]]+([^[:space:]]+)";
const char *rack1_regex = "#rack1 ([^[:space:]]+)";
const char *rack2_regex = "#rack2 ([^[:space:]]+)";
const char *move_regex =
    ">([^[:space:]]+):[[:space:]]+([^[:space:]]+)[[:space:"
    "]]+([[:alnum:]]+)[[:space:]]+([^[:space:]]+)[[:space:]]+"
    "[+]([[:digit:]]+)[[:space:]]+([[:digit:]]+)";
const char *note_regex = "#note (.+)";
const char *lexicon_name_regex = "#lexicon (.+)";
const char *character_encoding_regex = "#character-encoding ([[:graph:]]+)";
const char *game_type_regex = "#game-type (.*)";
const char *tile_set_regex = "#tile-set (.*)";
const char *game_board_regex = "#game-board (.*)";
const char *board_layout_regex = "#board-layout (.*)";
const char *tile_distribution_name_regex = "#tile-distribution (.*)";
const char *continuation_regex = "#- (.*)";
const char *phony_tiles_returned_regex =
    ">([^[:space:]]+):[[:space:]]+([^[:space:]]+)[[:space:]]+--[[:space:]]+-([["
    ":digit:]]+)[[:space:]]+([[:digit:]]+)";
const char *pass_regex = ">([^[:space:]]+):[[:space:]]+([^[:space:]]+)[[:space:"
                         "]]+-[[:space:]]+\\+0[[:space:]]+([[:digit:]]+)";
const char *challenge_bonus_regex =
    ">([^[:space:]]+):[[:space:]]+([^[:space:]]*)[[:space:]]+\\(challenge\\)[[:"
    "space:]]+\\+([[:digit:]]+"
    ")[[:space:]]+([[:digit:]]+)";
const char *exchange_regex =
    ">([^[:space:]]+):[[:space:]]+([^[:space:]]+)[[:space:]]+-([^[:"
    "space:]]+)[[:space:]]+\\+0[[:space:]]+([[:digit:]]+)";
const char *end_rack_points_regex =
    ">([^[:space:]]+):[[:space:]]+\\(([^[:space:]]+)\\)[[:space:]]+\\+([[:"
    "digit:]]+)[[:space:]]+(-?[[:digit:]]+)";
const char *time_penalty_regex =
    ">([^[:space:]]+):[[:space:]]+([^[:space:]]*)[[:space:]]+\\(time\\)[[:"
    "space:]]+\\-([[:digit:]]+)"
    "[[:space:]]+(-?[[:digit:]]+)";
const char *points_lost_for_last_rack_regex =
    ">([^[:space:]]+):[[:space:]]+([^[:space:]]+)[[:space:]]+\\(([^[:space:]]+)"
    "\\)[[:space:]]+\\-([[:digit:]]+)"
    "[[:space:]]+(-?[[:digit:]]+)";
const char *incomplete_regex = "#incomplete.*";
const char *tile_declaration_regex =
    "#tile ([^[:space:]]+)[[:space:]]+([^[:space:]]+)";

TokenRegexPair *create_token_regex_pair(gcg_token_t token,
                                        const char *regex_string) {
  TokenRegexPair *token_regex_pair = malloc_or_die(sizeof(TokenRegexPair));
  token_regex_pair->token = token;
  int regex_compilation_result =
      regcomp(&token_regex_pair->regex, regex_string, REG_EXTENDED);
  if (regex_compilation_result) {
    log_fatal("Could not compile regex: %s", regex_string);
  }
  return token_regex_pair;
}

void destroy_token_regex_pair(TokenRegexPair *token_regex_pair) {
  regfree(&token_regex_pair->regex);
  free(token_regex_pair);
}

GCGParser *create_gcg_parser(GameHistory *game_history) {
  GCGParser *gcg_parser = malloc_or_die(sizeof(GCGParser));
  gcg_parser->game_history = game_history;
  gcg_parser->note_builder = create_string_builder();
  // Allocate enough space for all of the tokens
  gcg_parser->token_regex_pairs =
      malloc_or_die(sizeof(TokenRegexPair) * (NUMBER_OF_GCG_TOKENS));
  gcg_parser->number_of_token_regex_pairs = 0;
  gcg_parser->token_regex_pairs[gcg_parser->number_of_token_regex_pairs++] =
      create_token_regex_pair(GCG_PLAYER_TOKEN, player_regex);
  gcg_parser->token_regex_pairs[gcg_parser->number_of_token_regex_pairs++] =
      create_token_regex_pair(GCG_TITLE_TOKEN, title_regex);
  gcg_parser->token_regex_pairs[gcg_parser->number_of_token_regex_pairs++] =
      create_token_regex_pair(GCG_DESCRIPTION_TOKEN, description_regex);
  gcg_parser->token_regex_pairs[gcg_parser->number_of_token_regex_pairs++] =
      create_token_regex_pair(GCG_ID_TOKEN, id_regex);
  gcg_parser->token_regex_pairs[gcg_parser->number_of_token_regex_pairs++] =
      create_token_regex_pair(GCG_RACK1_TOKEN, rack1_regex);
  gcg_parser->token_regex_pairs[gcg_parser->number_of_token_regex_pairs++] =
      create_token_regex_pair(GCG_RACK2_TOKEN, rack2_regex);
  gcg_parser->token_regex_pairs[gcg_parser->number_of_token_regex_pairs++] =
      create_token_regex_pair(GCG_ENCODING_TOKEN, character_encoding_regex);
  gcg_parser->token_regex_pairs[gcg_parser->number_of_token_regex_pairs++] =
      create_token_regex_pair(GCG_MOVE_TOKEN, move_regex);
  gcg_parser->token_regex_pairs[gcg_parser->number_of_token_regex_pairs++] =
      create_token_regex_pair(GCG_NOTE_TOKEN, note_regex);
  gcg_parser->token_regex_pairs[gcg_parser->number_of_token_regex_pairs++] =
      create_token_regex_pair(GCG_LEXICON_TOKEN, lexicon_name_regex);
  gcg_parser->token_regex_pairs[gcg_parser->number_of_token_regex_pairs++] =
      create_token_regex_pair(GCG_PHONY_TILES_RETURNED_TOKEN,
                              phony_tiles_returned_regex);
  gcg_parser->token_regex_pairs[gcg_parser->number_of_token_regex_pairs++] =
      create_token_regex_pair(GCG_PASS_TOKEN, pass_regex);
  gcg_parser->token_regex_pairs[gcg_parser->number_of_token_regex_pairs++] =
      create_token_regex_pair(GCG_CHALLENGE_BONUS_TOKEN, challenge_bonus_regex);
  gcg_parser->token_regex_pairs[gcg_parser->number_of_token_regex_pairs++] =
      create_token_regex_pair(GCG_EXCHANGE_TOKEN, exchange_regex);
  gcg_parser->token_regex_pairs[gcg_parser->number_of_token_regex_pairs++] =
      create_token_regex_pair(GCG_END_RACK_POINTS_TOKEN, end_rack_points_regex);
  gcg_parser->token_regex_pairs[gcg_parser->number_of_token_regex_pairs++] =
      create_token_regex_pair(GCG_TIME_PENALTY_TOKEN, time_penalty_regex);
  gcg_parser->token_regex_pairs[gcg_parser->number_of_token_regex_pairs++] =
      create_token_regex_pair(GCG_LAST_RACK_PENALTY_TOKEN,
                              points_lost_for_last_rack_regex);
  gcg_parser->token_regex_pairs[gcg_parser->number_of_token_regex_pairs++] =
      create_token_regex_pair(GCG_GAME_TYPE_TOKEN, game_type_regex);
  gcg_parser->token_regex_pairs[gcg_parser->number_of_token_regex_pairs++] =
      create_token_regex_pair(GCG_TILE_SET_TOKEN, tile_set_regex);
  gcg_parser->token_regex_pairs[gcg_parser->number_of_token_regex_pairs++] =
      create_token_regex_pair(GCG_BOARD_LAYOUT_TOKEN, board_layout_regex);
  gcg_parser->token_regex_pairs[gcg_parser->number_of_token_regex_pairs++] =
      create_token_regex_pair(GCG_TILE_DISTRIBUTION_NAME_TOKEN,
                              tile_distribution_name_regex);
  return gcg_parser;
}

void destroy_gcg_parser(GCGParser *gcg_parser) {
  for (int i = 0; i < (gcg_parser->number_of_token_regex_pairs); i++) {
    destroy_token_regex_pair(gcg_parser->token_regex_pairs[i]);
  }
  destroy_string_builder(gcg_parser->note_builder);
  free(gcg_parser->token_regex_pairs);
  free(gcg_parser);
}

int get_matching_group_string_length(const GCGParser *gcg_parser,
                                     int group_index) {
  int start_index = gcg_parser->matching_groups[group_index].rm_so;
  int end_index = gcg_parser->matching_groups[group_index].rm_eo;
  return end_index - start_index;
}

char *get_matching_group_as_string(const GCGParser *gcg_parser,
                                   const char *gcg_line, int group_index) {
  int start_index = gcg_parser->matching_groups[group_index].rm_so;
  int end_index = gcg_parser->matching_groups[group_index].rm_eo;
  int matching_group_string_length =
      get_matching_group_string_length(gcg_parser, group_index);
  char *matching_group_string =
      (char *)malloc_or_die((matching_group_string_length + 1) * sizeof(char));
  for (int i = start_index; i < end_index; i++) {
    matching_group_string[i - start_index] = gcg_line[i];
  }
  matching_group_string[matching_group_string_length] = '\0';
  return matching_group_string;
}

int get_matching_group_as_int(const GCGParser *gcg_parser, const char *gcg_line,
                              int group_index) {
  char *matching_group_string =
      get_matching_group_as_string(gcg_parser, gcg_line, group_index);
  int matching_group_int = string_to_int(matching_group_string);
  free(matching_group_string);
  return matching_group_int;
}

StringSplitter *decode_gcg_with_gcg_lines(const StringSplitter *gcg_lines,
                                          GCGParser *gcg_parser) {
  // Find encoding token
  TokenRegexPair *encoding_token_regex_pair = NULL;
  for (int i = 0; i < gcg_parser->number_of_token_regex_pairs; i++) {
    if (gcg_parser->token_regex_pairs[i]->token == GCG_ENCODING_TOKEN) {
      encoding_token_regex_pair = gcg_parser->token_regex_pairs[i];
      break;
    }
  }

  if (!encoding_token_regex_pair) {
    log_fatal("Encoding token not found\n");
  }

  const char *first_gcg_line = string_splitter_get_item(gcg_lines, 0);

  // ISO_8859-1 is the default encoding
  gcg_encoding_t gcg_encoding = GCG_ENCODING_ISO_8859_1;

  int regexec_result =
      regexec(&encoding_token_regex_pair->regex, first_gcg_line, (MAX_GROUPS),
              gcg_parser->matching_groups, 0);
  int join_start_index = 0;
  if (!regexec_result) {
    char *encoding_string =
        get_matching_group_as_string(gcg_parser, first_gcg_line, 1);
    bool is_utf8 = strings_iequal("utf-8", encoding_string) ||
                   strings_iequal("utf8", encoding_string);
    bool is_iso_8859_1 = strings_iequal("iso-8859-1", encoding_string) ||
                         strings_iequal("iso 8859-1", encoding_string);
    free(encoding_string);
    if (is_utf8) {
      gcg_encoding = GCG_ENCODING_UTF8;
    } else if (!is_iso_8859_1) {
      return NULL;
    }
    // If the first line was the encoding line, we want
    // to ignore this when processing the GCG.
    join_start_index = 1;
  } else if (regexec_result != REG_NOMATCH) {
    char msgbuf[100];
    regerror(regexec_result, &encoding_token_regex_pair->regex, msgbuf,
             sizeof(msgbuf));
    log_fatal("Regex match failed for encoding token: %s\n", msgbuf);
  }

  StringSplitter *utf8_gcg_lines = NULL;
  int number_of_gcg_lines = string_splitter_get_number_of_items(gcg_lines);
  char *utf8_gcg_without_encoding = NULL;

  if (gcg_encoding == GCG_ENCODING_ISO_8859_1) {
    char *gcg_without_encoding = string_splitter_join(
        gcg_lines, join_start_index, number_of_gcg_lines, "\n");
    utf8_gcg_without_encoding = iso_8859_1_to_utf8(gcg_without_encoding);
    free(gcg_without_encoding);
  } else {
    utf8_gcg_without_encoding = string_splitter_join(
        gcg_lines, join_start_index, number_of_gcg_lines, "\n");
  }

  utf8_gcg_lines = split_string_by_newline(utf8_gcg_without_encoding, true);
  free(utf8_gcg_without_encoding);
  return utf8_gcg_lines;
}

StringSplitter *decode_gcg(GCGParser *gcg_parser, const char *gcg_string) {
  StringSplitter *gcg_lines = split_string_by_newline(gcg_string, true);
  StringSplitter *utf8_gcg_lines =
      decode_gcg_with_gcg_lines(gcg_lines, gcg_parser);
  destroy_string_splitter(gcg_lines);
  return utf8_gcg_lines;
}

gcg_token_t find_matching_gcg_token(GCGParser *gcg_parser,
                                    const char *gcg_line) {
  for (int i = 0; i < gcg_parser->number_of_token_regex_pairs; i++) {
    int regexec_result =
        regexec(&gcg_parser->token_regex_pairs[i]->regex, gcg_line,
                (MAX_GROUPS), gcg_parser->matching_groups, 0);
    if (!regexec_result) {
      return gcg_parser->token_regex_pairs[i]->token;
    } else if (regexec_result != REG_NOMATCH) {
      char msgbuf[100];
      regerror(regexec_result, &gcg_parser->token_regex_pairs[i]->regex, msgbuf,
               sizeof(msgbuf));
      log_fatal("Regex match failed: %s\n", msgbuf);
    }
  }
  return GCG_UNKNOWN_TOKEN;
}

int get_player_index(const GCGParser *gcg_parser, const char *gcg_line,
                     int group_index) {
  char *player_nickname =
      get_matching_group_as_string(gcg_parser, gcg_line, group_index);

  int player_index = -1;
  for (int i = 0; i < 2; i++) {
    if (strings_equal(gcg_parser->game_history->players[i]->nickname,
                      player_nickname)) {
      player_index = i;
      break;
    }
  }
  free(player_nickname);
  return player_index;
}

void copy_score_to_game_event(const GCGParser *gcg_parser,
                              GameEvent *game_event, const char *gcg_line,
                              int group_index) {
  char *move_score_string =
      get_matching_group_as_string(gcg_parser, gcg_line, group_index);
  game_event->move->score = string_to_int(move_score_string);
  free(move_score_string);
}

void copy_cumulative_score_to_game_event(const GCGParser *gcg_parser,
                                         GameEvent *game_event,
                                         const char *gcg_line,
                                         int group_index) {
  char *cumulative_score_string =
      get_matching_group_as_string(gcg_parser, gcg_line, group_index);
  game_event->cumulative_score = string_to_int(cumulative_score_string);
  free(cumulative_score_string);
}

uint8_t *convert_tiles_string_to_machine_letters(
    const GCGParser *gcg_parser, const char *gcg_line, int group_index,
    bool allow_played_through_marker, int *number_of_machine_letters) {

  int start_index = gcg_parser->matching_groups[group_index].rm_so;
  int end_index = gcg_parser->matching_groups[group_index].rm_eo;
  int matching_group_length = end_index - start_index;
  char *tiles_string =
      get_matching_group_as_string(gcg_parser, gcg_line, group_index);

  int machine_letters_size = matching_group_length + 1;
  uint8_t *machine_letters =
      malloc_or_die(sizeof(uint8_t) * machine_letters_size);
  *number_of_machine_letters = str_to_machine_letters(
      gcg_parser->game_history->letter_distribution, tiles_string,
      allow_played_through_marker, machine_letters, machine_letters_size);
  free(tiles_string);
  if (*number_of_machine_letters < 0) {
    free(machine_letters);
    return NULL;
  }
  return machine_letters;
}

bool copy_played_tiles_to_game_event(const GCGParser *gcg_parser,
                                     GameEvent *game_event,
                                     const char *gcg_line, int group_index) {
  int number_of_machine_letters;
  uint8_t *played_tiles = convert_tiles_string_to_machine_letters(
      gcg_parser, gcg_line, group_index, true, &number_of_machine_letters);

  bool success =
      number_of_machine_letters <= BOARD_DIM && number_of_machine_letters > 0;
  if (success) {
    game_event->move->tiles_length = number_of_machine_letters;
    for (int i = 0; i < game_event->move->tiles_length; i++) {
      game_event->move->tiles[i] = played_tiles[i];
    }
    // Calculate tiles played
    game_event->move->tiles_played = 0;
    for (int i = 0; i < game_event->move->tiles_length; i++) {
      if (game_event->move->tiles[i] != PLAYED_THROUGH_MARKER) {
        game_event->move->tiles_played++;
      }
    }
  }

  free(played_tiles);

  return success;
}

bool copy_exchanged_tiles_to_game_event(const GCGParser *gcg_parser,
                                        GameEvent *game_event,
                                        const char *gcg_line, int group_index) {
  int number_of_machine_letters;
  uint8_t *played_tiles = convert_tiles_string_to_machine_letters(
      gcg_parser, gcg_line, group_index, false, &number_of_machine_letters);

  bool success =
      number_of_machine_letters < RACK_SIZE && number_of_machine_letters > 0;
  if (success) {
    game_event->move->tiles_played = number_of_machine_letters;
    for (int i = 0; i < game_event->move->tiles_played; i++) {
      game_event->move->tiles[i] = played_tiles[i];
    }
    game_event->move->tiles_length = game_event->move->tiles_played;
  }

  free(played_tiles);

  return success;
}

Rack *get_rack_from_matching(const GCGParser *gcg_parser, const char *gcg_line,
                             int group_index) {
  if (get_matching_group_string_length(gcg_parser, group_index) == 0) {
    return NULL;
  }
  char *player_rack_string =
      get_matching_group_as_string(gcg_parser, gcg_line, group_index);
  Rack *rack = create_rack(gcg_parser->game_history->letter_distribution->size);
  int number_of_letters_set = set_rack_to_string(
      gcg_parser->game_history->letter_distribution, rack, player_rack_string);
  free(player_rack_string);
  if (number_of_letters_set <= 0) {
    destroy_rack(rack);
    return NULL;
  }
  return rack;
}

gcg_parse_status_t copy_position_to_game_event(const GCGParser *gcg_parser,
                                               GameEvent *game_event,
                                               const char *gcg_line,
                                               int group_index) {
  game_event->move->row_start = 0;
  int start_index = gcg_parser->matching_groups[group_index].rm_so;
  int end_index = gcg_parser->matching_groups[group_index].rm_eo;
  for (int i = start_index; i < end_index; i++) {
    char position_char = gcg_line[i];
    if (position_char >= '0' && position_char <= '9') {
      if (i == start_index) {
        game_event->move->dir = BOARD_HORIZONTAL_DIRECTION;
      }
      // Build the 1-indexed row_start
      game_event->move->row_start =
          game_event->move->row_start * 10 + (position_char - '0');
    } else if (position_char >= 'A' && position_char <= 'Z') {
      if (i == start_index) {
        game_event->move->dir = BOARD_VERTICAL_DIRECTION;
      }
      game_event->move->col_start = position_char - 'A';
    } else {
      return GCG_PARSE_STATUS_INVALID_TILE_PLACEMENT_POSITION;
    }
  }
  // Convert the 1-index row start into 0-indexed row start
  game_event->move->row_start--;
  if (game_event->move->col_start < 0 ||
      game_event->move->col_start > BOARD_DIM ||
      game_event->move->row_start < 0 ||
      game_event->move->row_start > BOARD_DIM) {
    return GCG_PARSE_STATUS_INVALID_TILE_PLACEMENT_POSITION;
  }
  return GCG_PARSE_STATUS_SUCCESS;
}

void finalize_note(GCGParser *gcg_parser) {
  if (string_builder_length(gcg_parser->note_builder) == 0) {
    return;
  }
  gcg_parser->game_history
      ->events[gcg_parser->game_history->number_of_events - 1]
      ->note = string_builder_dump(gcg_parser->note_builder, NULL);
  string_builder_clear(gcg_parser->note_builder);
}

GameEvent *create_game_event_and_add_to_history(GameHistory *game_history) {
  if (game_history->number_of_events == MAX_GAME_EVENTS) {
    log_fatal("game events overflow");
  }
  GameEvent *game_event = create_game_event();
  game_history->events[game_history->number_of_events++] = game_event;
  return game_event;
}

gcg_parse_status_t parse_gcg_line(GCGParser *gcg_parser, const char *gcg_line) {
  GameHistory *game_history = gcg_parser->game_history;
  gcg_token_t token = find_matching_gcg_token(gcg_parser, gcg_line);
  gcg_token_t previous_token = gcg_parser->previous_token;
  if (token != GCG_UNKNOWN_TOKEN) {
    gcg_parser->previous_token = token;
  }
  GameEvent *previous_game_event = NULL;
  if (game_history->number_of_events > 0) {
    previous_game_event =
        game_history->events[game_history->number_of_events - 1];
  }
  // Perform logic with previous token here because it
  // is set.
  if (previous_token == GCG_NOTE_TOKEN && token != GCG_NOTE_TOKEN &&
      token != GCG_UNKNOWN_TOKEN) {
    finalize_note(gcg_parser);
  }

  if (token == GCG_MOVE_TOKEN || token == GCG_PASS_TOKEN ||
      token == GCG_EXCHANGE_TOKEN) {
    if (!game_history->players[0] || !game_history->players[1]) {
      return GCG_PARSE_STATUS_MOVE_BEFORE_PLAYER;
    }
    if (!game_history->letter_distribution) {

      if (!game_history->lexicon_name) {
        // FIXME: add test for this
        return GCG_PARSE_STATUS_LEXICON_NOT_FOUND;
      }
      if (!game_history->letter_distribution_name) {
        game_history->letter_distribution_name =
            get_default_letter_distribution_name(game_history->lexicon_name);
      }
      if (game_history->board_layout == BOARD_LAYOUT_UNKNOWN) {
        game_history->board_layout = BOARD_LAYOUT_CROSSWORD_GAME;
      }
      if (game_history->game_variant == GAME_VARIANT_UNKNOWN) {
        game_history->game_variant = GAME_VARIANT_CLASSIC;
      }
      game_history->letter_distribution =
          create_letter_distribution(game_history->letter_distribution_name);
    }
  }

  GameEvent *game_event = NULL;
  int player_index = -1;
  bool success;
  switch (token) {
  case GCG_PLAYER_TOKEN:
    if (game_history->number_of_events > 0) {
      return GCG_PARSE_STATUS_PRAGMA_SUCCEEDED_EVENT;
    }
    player_index = get_matching_group_as_int(gcg_parser, gcg_line, 1) - 1;
    if (game_history->players[player_index]) {
      return GCG_PARSE_STATUS_PLAYER_NUMBER_REDUNDANT;
    }
    char *player_nickname =
        get_matching_group_as_string(gcg_parser, gcg_line, 2);
    char *player_name = get_matching_group_as_string(gcg_parser, gcg_line, 3);
    game_history->players[player_index] =
        create_game_history_player(player_name, player_nickname);
    free(player_name);
    free(player_nickname);
    if (game_history->players[1 - player_index] &&
        strings_equal(game_history->players[player_index]->name,
                      game_history->players[1 - player_index]->name)) {
      return GCG_PARSE_STATUS_DUPLICATE_NAMES;
    }
    if (game_history->players[1 - player_index] &&
        strings_equal(game_history->players[player_index]->nickname,
                      game_history->players[1 - player_index]->nickname)) {
      return GCG_PARSE_STATUS_DUPLICATE_NICKNAMES;
    }
    break;
  case GCG_TITLE_TOKEN:
    if (game_history->number_of_events > 0) {
      return GCG_PARSE_STATUS_PRAGMA_SUCCEEDED_EVENT;
    }
    if (game_history->title) {
      return GCG_PARSE_STATUS_REDUNDANT_PRAGMA;
    }
    game_history->title = get_matching_group_as_string(gcg_parser, gcg_line, 1);
    break;
  case GCG_DESCRIPTION_TOKEN:
    if (game_history->number_of_events > 0) {
      return GCG_PARSE_STATUS_PRAGMA_SUCCEEDED_EVENT;
    }
    if (game_history->description) {
      return GCG_PARSE_STATUS_REDUNDANT_PRAGMA;
    }
    game_history->description =
        get_matching_group_as_string(gcg_parser, gcg_line, 1);
    break;
  case GCG_ID_TOKEN:
    if (game_history->number_of_events > 0) {
      return GCG_PARSE_STATUS_PRAGMA_SUCCEEDED_EVENT;
    }
    if (game_history->id_auth || game_history->uid) {
      return GCG_PARSE_STATUS_REDUNDANT_PRAGMA;
    }
    game_history->id_auth =
        get_matching_group_as_string(gcg_parser, gcg_line, 1);
    game_history->uid = get_matching_group_as_string(gcg_parser, gcg_line, 2);
    break;
  case GCG_RACK1_TOKEN:
    game_history->players[0]->last_known_rack =
        get_rack_from_matching(gcg_parser, gcg_line, 1);
    if (!game_history->players[0]->last_known_rack) {
      return GCG_PARSE_STATUS_RACK_MALFORMED;
    }
    break;
  case GCG_RACK2_TOKEN:
    game_history->players[1]->last_known_rack =
        get_rack_from_matching(gcg_parser, gcg_line, 1);
    if (!game_history->players[1]->last_known_rack) {
      return GCG_PARSE_STATUS_RACK_MALFORMED;
    }
    break;
  case GCG_ENCODING_TOKEN:
    return GCG_PARSE_STATUS_ENCODING_WRONG_PLACE;
    break;
  case GCG_MOVE_TOKEN:
    player_index = get_player_index(gcg_parser, gcg_line, 1);
    if (player_index < 0) {
      return GCG_PARSE_STATUS_PLAYER_DOES_NOT_EXIST;
    }
    if (game_history->number_of_events == MAX_GAME_EVENTS) {
      return GCG_PARSE_STATUS_GAME_EVENTS_OVERFLOW;
    }
    game_event = create_game_event_and_add_to_history(game_history);
    game_event->player_index = player_index;
    // Write the move rack
    game_event->event_type = GAME_EVENT_TILE_PLACEMENT_MOVE;
    game_event->move = create_move();

    game_event->move->move_type = GAME_EVENT_TILE_PLACEMENT_MOVE;
    // Rack
    game_event->rack = get_rack_from_matching(gcg_parser, gcg_line, 2);
    if (!game_event->rack) {
      return GCG_PARSE_STATUS_RACK_MALFORMED;
    }
    // Position
    gcg_parse_status_t gcg_parse_status =
        copy_position_to_game_event(gcg_parser, game_event, gcg_line, 3);
    if (gcg_parse_status != GCG_PARSE_STATUS_SUCCESS) {
      return gcg_parse_status;
    }

    // Played tiles
    success =
        copy_played_tiles_to_game_event(gcg_parser, game_event, gcg_line, 4);
    if (!success) {
      return GCG_PARSE_STATUS_PLAY_MALFORMED;
    }

    // Check if the play goes off the board
    if ((dir_is_vertical(game_event->move->dir) &&
         game_event->move->row_start + game_event->move->tiles_length >
             BOARD_DIM) ||
        (!dir_is_vertical(game_event->move->dir) &&
         game_event->move->col_start + game_event->move->tiles_length >
             BOARD_DIM)) {
      return GCG_PARSE_STATUS_PLAY_OUT_OF_BOUNDS;
    }

    for (int i = 0; i < game_event->move->tiles_length; i++) {
      if (game_event->move->tiles[i] != PLAYED_THROUGH_MARKER) {
        uint8_t played_letter = game_event->move->tiles[i];
        if (is_blanked(played_letter)) {
          played_letter = BLANK_MACHINE_LETTER;
        }
        game_event->rack->array[played_letter]--;
        if (game_event->rack->array[played_letter] < 0) {
          return GCG_PARSE_STATUS_PLAYED_LETTERS_NOT_IN_RACK;
        }
      }
    }

    // Restore the rack
    for (int i = 0; i < game_event->move->tiles_length; i++) {
      if (game_event->move->tiles[i] != PLAYED_THROUGH_MARKER) {
        uint8_t played_letter = game_event->move->tiles[i];
        if (is_blanked(played_letter)) {
          played_letter = BLANK_MACHINE_LETTER;
        }
        game_event->rack->array[played_letter]++;
      }
    }

    // Score
    copy_score_to_game_event(gcg_parser, game_event, gcg_line, 5);

    // Cumulative score
    copy_cumulative_score_to_game_event(gcg_parser, game_event, gcg_line, 6);
    break;
  case GCG_NOTE_TOKEN:
    if (game_history->number_of_events == 0) {
      return GCG_PARSE_STATUS_NOTE_PRECEDENT_EVENT;
    }
    char *note = get_matching_group_as_string(gcg_parser, gcg_line, 1);
    string_builder_add_formatted_string(gcg_parser->note_builder, "%s ", note);
    free(note);
    break;
  case GCG_LEXICON_TOKEN:
    if (game_history->number_of_events > 0) {
      return GCG_PARSE_STATUS_PRAGMA_SUCCEEDED_EVENT;
    }
    if (game_history->lexicon_name) {
      return GCG_PARSE_STATUS_REDUNDANT_PRAGMA;
    }
    game_history->lexicon_name =
        get_matching_group_as_string(gcg_parser, gcg_line, 1);
    break;
  case GCG_BOARD_LAYOUT_TOKEN:
    if (game_history->number_of_events > 0) {
      return GCG_PARSE_STATUS_PRAGMA_SUCCEEDED_EVENT;
    }
    if (game_history->board_layout != BOARD_LAYOUT_UNKNOWN) {
      return GCG_PARSE_STATUS_REDUNDANT_PRAGMA;
    }
    char *board_layout_string =
        get_matching_group_as_string(gcg_parser, gcg_line, 1);
    game_history->board_layout =
        board_layout_string_to_board_layout(board_layout_string);
    free(board_layout_string);
    break;
  case GCG_TILE_DISTRIBUTION_NAME_TOKEN:
    if (game_history->number_of_events > 0) {
      return GCG_PARSE_STATUS_PRAGMA_SUCCEEDED_EVENT;
    }
    if (game_history->letter_distribution_name) {
      return GCG_PARSE_STATUS_REDUNDANT_PRAGMA;
    }
    game_history->letter_distribution_name =
        get_matching_group_as_string(gcg_parser, gcg_line, 1);
    break;
  case GCG_GAME_TYPE_TOKEN:
    if (game_history->number_of_events > 0) {
      return GCG_PARSE_STATUS_PRAGMA_SUCCEEDED_EVENT;
    }
    if (game_history->game_variant != GAME_VARIANT_UNKNOWN) {
      return GCG_PARSE_STATUS_REDUNDANT_PRAGMA;
    }
    char *game_variant_name =
        get_matching_group_as_string(gcg_parser, gcg_line, 1);
    game_history->game_variant =
        get_game_variant_type_from_name(game_variant_name);
    free(game_variant_name);
    break;
  case GCG_PHONY_TILES_RETURNED_TOKEN:
    player_index = get_player_index(gcg_parser, gcg_line, 1);
    if (player_index < 0) {
      return GCG_PARSE_STATUS_PLAYER_DOES_NOT_EXIST;
    }
    if (game_history->number_of_events == MAX_GAME_EVENTS) {
      return GCG_PARSE_STATUS_GAME_EVENTS_OVERFLOW;
    }
    game_event = create_game_event_and_add_to_history(game_history);

    game_event->rack = get_rack_from_matching(gcg_parser, gcg_line, 2);
    if (!game_event->rack) {
      return GCG_PARSE_STATUS_RACK_MALFORMED;
    }
    if (game_history->number_of_events == 0 || !previous_game_event ||
        previous_game_event->player_index != player_index ||
        !racks_are_equal(game_event->rack, previous_game_event->rack)) {
      return GCG_PARSE_STATUS_PHONY_TILES_RETURNED_WITHOUT_PLAY;
    }

    game_event->player_index = player_index;

    game_event->event_type = GAME_EVENT_PHONY_TILES_RETURNED;
    copy_cumulative_score_to_game_event(gcg_parser, game_event, gcg_line, 4);

    break;
  case GCG_TIME_PENALTY_TOKEN:
    player_index = get_player_index(gcg_parser, gcg_line, 1);
    if (player_index < 0) {
      return GCG_PARSE_STATUS_PLAYER_DOES_NOT_EXIST;
    }
    if (game_history->number_of_events == MAX_GAME_EVENTS) {
      return GCG_PARSE_STATUS_GAME_EVENTS_OVERFLOW;
    }
    game_event = create_game_event_and_add_to_history(game_history);
    game_event->player_index = player_index;
    game_event->event_type = GAME_EVENT_TIME_PENALTY;
    // Rack is allowed to be empty for time penalty
    if (get_matching_group_string_length(gcg_parser, 2) == 0) {
      game_event->rack = NULL;
    } else {
      game_event->rack = get_rack_from_matching(gcg_parser, gcg_line, 2);
      if (!game_event->rack) {
        return GCG_PARSE_STATUS_RACK_MALFORMED;
      }
    }
    copy_cumulative_score_to_game_event(gcg_parser, game_event, gcg_line, 4);
    break;
  case GCG_LAST_RACK_PENALTY_TOKEN:
    player_index = get_player_index(gcg_parser, gcg_line, 1);
    if (player_index < 0) {
      return GCG_PARSE_STATUS_PLAYER_DOES_NOT_EXIST;
    }
    if (game_history->number_of_events == MAX_GAME_EVENTS) {
      return GCG_PARSE_STATUS_GAME_EVENTS_OVERFLOW;
    }
    game_event = create_game_event_and_add_to_history(game_history);
    game_event->player_index = player_index;
    game_event->event_type = GAME_EVENT_END_RACK_PENALTY;
    game_event->rack = get_rack_from_matching(gcg_parser, gcg_line, 2);
    if (!game_event->rack) {
      return GCG_PARSE_STATUS_RACK_MALFORMED;
    }
    Rack *penalty_tiles = get_rack_from_matching(gcg_parser, gcg_line, 3);
    if (!penalty_tiles) {
      return GCG_PARSE_STATUS_RACK_MALFORMED;
    }
    bool penalty_tiles_equals_rack =
        racks_are_equal(game_event->rack, penalty_tiles);

    destroy_rack(penalty_tiles);

    if (!penalty_tiles_equals_rack) {
      return GCG_PARSE_STATUS_PLAYED_LETTERS_NOT_IN_RACK;
    }

    copy_cumulative_score_to_game_event(gcg_parser, game_event, gcg_line, 5);

    break;
  case GCG_PASS_TOKEN:
    player_index = get_player_index(gcg_parser, gcg_line, 1);
    if (player_index < 0) {
      return GCG_PARSE_STATUS_PLAYER_DOES_NOT_EXIST;
    }
    if (game_history->number_of_events == MAX_GAME_EVENTS) {
      return GCG_PARSE_STATUS_GAME_EVENTS_OVERFLOW;
    }
    game_event = create_game_event_and_add_to_history(game_history);
    game_event->player_index = player_index;
    game_event->event_type = GAME_EVENT_PASS;
    game_event->rack = get_rack_from_matching(gcg_parser, gcg_line, 2);
    if (!game_event->rack) {
      return GCG_PARSE_STATUS_RACK_MALFORMED;
    }
    copy_cumulative_score_to_game_event(gcg_parser, game_event, gcg_line, 3);
    game_event->move = create_move();
    set_move_as_pass(game_event->move);
    break;
  case GCG_CHALLENGE_BONUS_TOKEN:
  case GCG_END_RACK_POINTS_TOKEN:
    player_index = get_player_index(gcg_parser, gcg_line, 1);
    if (player_index < 0) {
      return GCG_PARSE_STATUS_PLAYER_DOES_NOT_EXIST;
    }
    if (game_history->number_of_events == MAX_GAME_EVENTS) {
      return GCG_PARSE_STATUS_GAME_EVENTS_OVERFLOW;
    }
    game_event = create_game_event_and_add_to_history(game_history);
    game_event->player_index = player_index;
    game_event->rack = get_rack_from_matching(gcg_parser, gcg_line, 2);
    if (!game_event->rack) {
      return GCG_PARSE_STATUS_RACK_MALFORMED;
    }
    copy_cumulative_score_to_game_event(gcg_parser, game_event, gcg_line, 4);
    if (token == GCG_CHALLENGE_BONUS_TOKEN) {
      game_event->event_type = GAME_EVENT_CHALLENGE_BONUS;
    } else if (token == GCG_END_RACK_POINTS_TOKEN) {
      game_event->event_type = GAME_EVENT_END_RACK_POINTS;
    }
    break;
  case GCG_EXCHANGE_TOKEN:
    player_index = get_player_index(gcg_parser, gcg_line, 1);
    if (player_index < 0) {
      return GCG_PARSE_STATUS_PLAYER_DOES_NOT_EXIST;
    }
    if (game_history->number_of_events == MAX_GAME_EVENTS) {
      return GCG_PARSE_STATUS_GAME_EVENTS_OVERFLOW;
    }
    game_event = create_game_event_and_add_to_history(game_history);

    game_event->move = create_move();
    game_event->move->move_type = GAME_EVENT_EXCHANGE;
    game_event->move->score = 0;

    game_event->player_index = player_index;
    game_event->event_type = GAME_EVENT_EXCHANGE;
    game_event->rack = get_rack_from_matching(gcg_parser, gcg_line, 2);
    if (!game_event->rack) {
      return GCG_PARSE_STATUS_RACK_MALFORMED;
    }
    success =
        copy_exchanged_tiles_to_game_event(gcg_parser, game_event, gcg_line, 3);
    if (!success) {
      return GCG_PARSE_STATUS_PLAY_MALFORMED;
    }
    copy_cumulative_score_to_game_event(gcg_parser, game_event, gcg_line, 4);
    bool exchanged_tiles_not_in_rack = false;
    for (int i = 0; i < game_event->move->tiles_played; i++) {
      game_event->rack->array[game_event->move->tiles[i]]--;
      if (game_event->rack->array[game_event->move->tiles[i]] < 0) {
        exchanged_tiles_not_in_rack = true;
      }
    }
    // Undo the operation so we don't have to allocate
    // an additional rack.
    for (int i = 0; i < game_event->move->tiles_played; i++) {
      game_event->rack->array[game_event->move->tiles[i]]++;
    }
    if (exchanged_tiles_not_in_rack) {
      return GCG_PARSE_STATUS_PLAYED_LETTERS_NOT_IN_RACK;
    }
    break;
  case GCG_UNKNOWN_TOKEN:
    if (previous_token == GCG_NOTE_TOKEN) {
      // Assume this is the continuation of a note
      string_builder_add_formatted_string(gcg_parser->note_builder, "%s ",
                                          gcg_line);

    } else if (!is_all_whitespace_or_empty(gcg_line)) {
      return GCG_PARSE_STATUS_NO_MATCHING_TOKEN;
    }
    break;
  case GCG_TILE_SET_TOKEN:
    // For now, don't do anything
    break;
  case NUMBER_OF_GCG_TOKENS:
    log_fatal("invalid gcg token");
    break;
  }
  return GCG_PARSE_STATUS_SUCCESS;
}

gcg_parse_status_t parse_gcg_with_parser(GCGParser *gcg_parser,
                                         const char *gcg_string) {
  StringSplitter *gcg_lines = decode_gcg(gcg_parser, gcg_string);

  if (!gcg_lines) {
    return GCG_PARSE_STATUS_UNSUPPORTED_CHARACTER_ENCODING;
  }

  int number_of_gcg_lines = string_splitter_get_number_of_items(gcg_lines);
  gcg_parse_status_t gcg_parse_status = GCG_PARSE_STATUS_SUCCESS;
  for (int i = 0; i < number_of_gcg_lines; i++) {
    gcg_parse_status =
        parse_gcg_line(gcg_parser, string_splitter_get_item(gcg_lines, i));
    if (gcg_parse_status != GCG_PARSE_STATUS_SUCCESS) {
      break;
    }
  }
  if (gcg_parse_status == GCG_PARSE_STATUS_SUCCESS) {
    finalize_note(gcg_parser);
    set_cumulative_scores(gcg_parser->game_history);
  }

  destroy_string_splitter(gcg_lines);

  return gcg_parse_status;
}

gcg_parse_status_t parse_gcg_string(const char *gcg_string,
                                    GameHistory *game_history) {

  if (is_string_empty(gcg_string)) {
    return GCG_PARSE_STATUS_GCG_EMPTY;
  }
  GCGParser *gcg_parser = create_gcg_parser(game_history);
  gcg_parse_status_t gcg_parse_status =
      parse_gcg_with_parser(gcg_parser, gcg_string);
  destroy_gcg_parser(gcg_parser);
  return gcg_parse_status;
}

gcg_parse_status_t parse_gcg(const char *gcg_filename,
                             GameHistory *game_history) {
  char *gcg_string = get_string_from_file(gcg_filename);
  gcg_parse_status_t gcg_parse_status =
      parse_gcg_string(gcg_string, game_history);
  free(gcg_string);
  return gcg_parse_status;
}