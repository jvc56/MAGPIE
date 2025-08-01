#include "../def/equity_defs.h"
#include "../def/game_defs.h"
#include "../def/game_history_defs.h"
#include "../def/gameplay_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/players_data_defs.h"
#include "../def/validated_move_defs.h"
#include "../ent/board_layout.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/players_data.h"
#include "../ent/rack.h"
#include "../ent/validated_move.h"
#include "../impl/gameplay.h"
#include "../str/game_string.h"
#include "../str/move_string.h"
#include "../str/rack_string.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "config.h"
#include <regex.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_GROUPS = 7 };

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
  GCG_END_RACK_PENALTY_TOKEN,
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
  int gcg_token_count[NUMBER_OF_GCG_TOKENS];
  Game *game;
  // Owned by the caller
  GameHistory *game_history;
  Config *config;
} GCGParser;

#define GCG_TITLE_STRING "title"
#define GCG_DESCRIPTION_STRING "description"
#define GCG_ID_STRING "id"
#define GCG_NOTE_STRING "note"
#define GCG_LEXICON_STRING "lexicon"
#define GCG_CHAR_ENCODING_STRING "character-encoding"
#define GCG_GAME_TYPE_STRING "game-type"
#define GCG_TILE_SET_STRING "tile-set"
#define GCG_BOARD_LAYOUT_STRING "board-layout"
#define GCG_TILE_DISTRIBUTION_STRING "tile-distribution"
#define GCG_PLAYER_STRING "player"
#define GCG_RACK_STRING "rack"

typedef struct TokenStringRegexPair {
  gcg_token_t token;
  const char *regex_string;
} TokenStringRegexPair;

// Define the array of token enum and regex pairs
const TokenStringRegexPair token_string_regex_pairs[] = {
    {GCG_PLAYER_TOKEN, "#" GCG_PLAYER_STRING
                       "([1-2])[[:space:]]+([^[:space:]]+)[[:space:]]+(.+)"},
    {GCG_TITLE_TOKEN, "#" GCG_TITLE_STRING "[[:space:]]*(.*)"},
    {GCG_DESCRIPTION_TOKEN, "#" GCG_DESCRIPTION_STRING "[[:space:]]*(.*)"},
    {GCG_ID_TOKEN, "#" GCG_ID_STRING
                   "[[:space:]]*([^[:space:]]+)[[:space:]]+([^[:space:]]+)"},
    {GCG_RACK1_TOKEN, "#" GCG_RACK_STRING "1 ([^[:space:]]+)"},
    {GCG_RACK2_TOKEN, "#" GCG_RACK_STRING "2 ([^[:space:]]+)"},
    {GCG_ENCODING_TOKEN, "#" GCG_CHAR_ENCODING_STRING " ([[:graph:]]+)"},
    {GCG_MOVE_TOKEN, ">([^[:space:]]+):[[:space:]]+([^[:space:]]+)[[:space:"
                     "]]+([[:alnum:]]+)[[:space:]]+([^[:space:]]+)[[:space:]]+"
                     "[+]([[:digit:]]+)[[:space:]]+(-?[[:digit:]]+)"},
    {GCG_NOTE_TOKEN, "#" GCG_NOTE_STRING " (.+)"},
    {GCG_LEXICON_TOKEN, "#" GCG_LEXICON_STRING " (.+)"},
    {GCG_PHONY_TILES_RETURNED_TOKEN, ">([^[:space:]]+):[[:space:]]+([^[:space:]"
                                     "]+)[[:space:]]+--[[:space:]]+-([["
                                     ":digit:]]+)[[:space:]]+(-?[[:digit:]]+)"},
    {GCG_PASS_TOKEN, ">([^[:space:]]+):[[:space:]]+([^[:space:]]+)[[:space:"
                     "]]+-[[:space:]]+\\+0[[:space:]]+(-?[[:digit:]]+)"},
    {GCG_CHALLENGE_BONUS_TOKEN, ">([^[:space:]]+):[[:space:]]+([^[:space:]]*)[["
                                ":space:]]+\\(challenge\\)[[:"
                                "space:]]+(\\+[[:digit:]]+"
                                ")[[:space:]]+(-?[[:digit:]]+)"},
    {GCG_EXCHANGE_TOKEN,
     ">([^[:space:]]+):[[:space:]]+([^[:space:]]+)[[:space:]]+-([^[:"
     "space:]]+)[[:space:]]+\\+0[[:space:]]+(-?[[:digit:]]+)"},
    {GCG_END_RACK_POINTS_TOKEN,
     ">([^[:space:]]+):[[:space:]]+\\(([^[:space:]]+)\\)[[:space:]]+(\\+[[:"
     "digit:]]+)[[:space:]]+(-?[[:digit:]]+)"},
    {GCG_TIME_PENALTY_TOKEN,
     ">([^[:space:]]+):[[:space:]]+([^[:space:]]*)[[:space:]]+\\(time\\)[[:"
     "space:]]+(\\-[[:digit:]]+)"
     "[[:space:]]+(-?[[:digit:]]+)"},
    {GCG_END_RACK_PENALTY_TOKEN, ">([^[:space:]]+):[[:space:]]+([^[:space:]]+)["
                                 "[:space:]]+\\(([^[:space:]]+)"
                                 "\\)[[:space:]]+(\\-[[:digit:]]+)"
                                 "[[:space:]]+(-?[[:digit:]]+)"},
    {GCG_GAME_TYPE_TOKEN, "#" GCG_GAME_TYPE_STRING " (.*)"},
    {GCG_TILE_SET_TOKEN, "#" GCG_TILE_SET_STRING " (.*)"},
    {GCG_BOARD_LAYOUT_TOKEN, "#" GCG_BOARD_LAYOUT_STRING " (.*)"},
    {GCG_TILE_DISTRIBUTION_NAME_TOKEN,
     "#" GCG_TILE_DISTRIBUTION_STRING " (.*)"}};

TokenRegexPair *token_regex_pair_create(gcg_token_t token,
                                        const char *regex_string) {
  TokenRegexPair *token_regex_pair = malloc_or_die(sizeof(TokenRegexPair));
  token_regex_pair->token = token;
  int regex_compilation_result =
      regcomp(&token_regex_pair->regex, regex_string, REG_EXTENDED);
  if (regex_compilation_result) {
    log_fatal("could not compile regex: %s", regex_string);
  }
  return token_regex_pair;
}

void token_regex_pair_destroy(TokenRegexPair *token_regex_pair) {
  if (!token_regex_pair) {
    return;
  }
  regfree(&token_regex_pair->regex);
  free(token_regex_pair);
}

GCGParser *gcg_parser_create(Config *config, GameHistory *game_history) {
  GCGParser *gcg_parser = malloc_or_die(sizeof(GCGParser));
  gcg_parser->game_history = game_history;
  gcg_parser->config = config;
  gcg_parser->game = NULL;
  gcg_parser->note_builder = string_builder_create();
  gcg_parser->token_regex_pairs =
      malloc_or_die(sizeof(TokenRegexPair *) * (NUMBER_OF_GCG_TOKENS));
  gcg_parser->number_of_token_regex_pairs =
      sizeof(token_string_regex_pairs) / sizeof(TokenStringRegexPair);
  for (int i = 0; i < gcg_parser->number_of_token_regex_pairs; i++) {
    gcg_parser->token_regex_pairs[i] =
        token_regex_pair_create(token_string_regex_pairs[i].token,
                                token_string_regex_pairs[i].regex_string);
  }
  memset(gcg_parser->gcg_token_count, 0, sizeof(gcg_parser->gcg_token_count));
  return gcg_parser;
}

void gcg_parser_destroy(GCGParser *gcg_parser) {
  if (!gcg_parser) {
    return;
  }
  for (int i = 0; i < (gcg_parser->number_of_token_regex_pairs); i++) {
    token_regex_pair_destroy(gcg_parser->token_regex_pairs[i]);
  }
  string_builder_destroy(gcg_parser->note_builder);
  game_destroy(gcg_parser->game);
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
  return get_substring(gcg_line, start_index, end_index);
}

int get_matching_group_as_int(const GCGParser *gcg_parser, const char *gcg_line,
                              int group_index, ErrorStack *error_stack) {
  char *matching_group_string =
      get_matching_group_as_string(gcg_parser, gcg_line, group_index);
  int matching_group_int = string_to_int(matching_group_string, error_stack);
  free(matching_group_string);
  return matching_group_int;
}

StringSplitter *decode_gcg_with_gcg_lines(const StringSplitter *gcg_lines,
                                          GCGParser *gcg_parser,
                                          ErrorStack *error_stack) {
  // Find encoding token
  TokenRegexPair *encoding_token_regex_pair = NULL;
  for (int i = 0; i < gcg_parser->number_of_token_regex_pairs; i++) {
    if (gcg_parser->token_regex_pairs[i]->token == GCG_ENCODING_TOKEN) {
      encoding_token_regex_pair = gcg_parser->token_regex_pairs[i];
      break;
    }
  }

  if (!encoding_token_regex_pair) {
    log_fatal("encoding token not found");
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
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_UNSUPPORTED_CHARACTER_ENCODING,
          get_formatted_string(
              "cannot parse GCG with unsupported character encoding: %s",
              first_gcg_line));
      return NULL;
    }
    // If the first line was the encoding line, we want
    // to ignore this when processing the GCG.
    join_start_index = 1;
  } else if (regexec_result != REG_NOMATCH) {
    char msgbuf[100];
    regerror(regexec_result, &encoding_token_regex_pair->regex, msgbuf,
             sizeof(msgbuf));
    log_fatal("regex match failed for encoding token: %s", msgbuf);
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

StringSplitter *decode_gcg(GCGParser *gcg_parser, const char *gcg_string,
                           ErrorStack *error_stack) {
  StringSplitter *gcg_lines = split_string_by_newline(gcg_string, true);
  StringSplitter *utf8_gcg_lines =
      decode_gcg_with_gcg_lines(gcg_lines, gcg_parser, error_stack);
  string_splitter_destroy(gcg_lines);
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
    }
    if (regexec_result != REG_NOMATCH) {
      char msgbuf[100];
      regerror(regexec_result, &gcg_parser->token_regex_pairs[i]->regex, msgbuf,
               sizeof(msgbuf));
      log_fatal("regex match failed: %s", msgbuf);
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
    if (strings_equal(
            game_history_player_get_nickname(gcg_parser->game_history, i),
            player_nickname)) {
      player_index = i;
      break;
    }
  }
  free(player_nickname);
  return player_index;
}

void copy_cumulative_score_to_game_event(const GCGParser *gcg_parser,
                                         GameEvent *game_event,
                                         const char *gcg_line, int group_index,
                                         ErrorStack *error_stack) {
  char *cumulative_score_string =
      get_matching_group_as_string(gcg_parser, gcg_line, group_index);
  const int cumulative_score_int =
      string_to_int(cumulative_score_string, error_stack);
  if (error_stack_is_empty(error_stack)) {
    game_event_set_cumulative_score(game_event,
                                    int_to_equity(cumulative_score_int));
  }
  free(cumulative_score_string);
}

void copy_score_adjustment_to_game_event(const GCGParser *gcg_parser,
                                         GameEvent *game_event,
                                         const char *gcg_line, int group_index,
                                         ErrorStack *error_stack) {
  char *score_adjustment_string =
      get_matching_group_as_string(gcg_parser, gcg_line, group_index);
  const int score_adjustment =
      string_to_int(score_adjustment_string, error_stack);
  if (error_stack_is_empty(error_stack)) {
    game_event_set_score_adjustment(game_event,
                                    int_to_equity(score_adjustment));
  }
  free(score_adjustment_string);
}

Rack *get_rack_from_matching(const GCGParser *gcg_parser, const char *gcg_line,
                             int group_index) {
  if (get_matching_group_string_length(gcg_parser, group_index) == 0) {
    return NULL;
  }
  char *player_rack_string =
      get_matching_group_as_string(gcg_parser, gcg_line, group_index);
  const LetterDistribution *ld = game_get_ld(gcg_parser->game);
  const int ld_size = ld_get_size(ld);
  Rack *rack = rack_create(ld_size);
  int number_of_letters_set = rack_set_to_string(ld, rack, player_rack_string);
  free(player_rack_string);
  if (number_of_letters_set <= 0) {
    rack_destroy(rack);
    return NULL;
  }
  return rack;
}

void add_matching_group_to_string_builder(StringBuilder *sb,
                                          const GCGParser *gcg_parser,
                                          const char *gcg_line,
                                          int group_index) {
  char *matching_group_string =
      get_matching_group_as_string(gcg_parser, gcg_line, group_index);
  string_builder_add_string(sb, matching_group_string);
  free(matching_group_string);
}

// Since the UCGI delimiter and the ASCII played through
// character are the same, we needed to convert the ASCII played
// through characters in the parse GCG move to ASCII UCGI played
// through characters so that the play can be validated.
void add_tiles_played_to_string_builder(StringBuilder *sb,
                                        const GCGParser *gcg_parser,
                                        const char *gcg_line, int group_index) {
  char *matching_group_string =
      get_matching_group_as_string(gcg_parser, gcg_line, group_index);
  size_t matching_group_string_length = string_length(matching_group_string);
  for (size_t i = 0; i < matching_group_string_length; i++) {
    if (matching_group_string[i] == ASCII_PLAYED_THROUGH) {
      matching_group_string[i] = ASCII_UCGI_PLAYED_THROUGH;
    }
  }
  string_builder_add_string(sb, matching_group_string);
  free(matching_group_string);
}

Equity get_move_score_from_gcg_line(const GCGParser *gcg_parser,
                                    const char *gcg_line, int group_index,
                                    ErrorStack *error_stack) {
  char *move_score_string =
      get_matching_group_as_string(gcg_parser, gcg_line, group_index);
  const int move_score_int = string_to_int(move_score_string, error_stack);
  Equity move_score_eq = EQUITY_INITIAL_VALUE;
  if (error_stack_is_empty(error_stack)) {
    move_score_eq = int_to_equity(move_score_int);
  }
  free(move_score_string);
  return move_score_eq;
}

void finalize_note(GCGParser *gcg_parser) {
  if (string_builder_length(gcg_parser->note_builder) == 0) {
    return;
  }

  int number_of_events =
      game_history_get_number_of_events(gcg_parser->game_history);
  GameEvent *event =
      game_history_get_event(gcg_parser->game_history, number_of_events - 1);

  game_event_set_note(event, string_builder_peek(gcg_parser->note_builder));
  string_builder_clear(gcg_parser->note_builder);
}

void load_config_with_game_history(const GameHistory *game_history,
                                   Config *config, ErrorStack *error_stack) {
  StringBuilder *cfg_load_cmd_builder = string_builder_create();
  const char *lexicon = game_history_get_lexicon_name(game_history);
  const char *ld_name = game_history_get_ld_name(game_history);
  const char *board_layout_name =
      game_history_get_board_layout_name(game_history);
  const game_variant_t game_variant =
      game_history_get_game_variant(game_history);
  const char *player_nicknames[2] = {NULL, NULL};

  for (int i = 0; i < 2; i++) {
    player_nicknames[i] = game_history_player_get_nickname(game_history, i);
  }

  string_builder_add_string(cfg_load_cmd_builder, "set ");

  if (lexicon) {
    string_builder_add_formatted_string(cfg_load_cmd_builder, "-lex %s ",
                                        lexicon);
  } else {
    log_fatal("missing lexicon for game history");
  }

  if (ld_name) {
    string_builder_add_formatted_string(cfg_load_cmd_builder, "-ld %s ",
                                        ld_name);
  } else {
    log_fatal("missing letter distribution for game history");
  }

  if (board_layout_name) {
    string_builder_add_formatted_string(cfg_load_cmd_builder, "-bdn %s ",
                                        board_layout_name);
  } else {
    log_fatal("missing board layout for game history");
  }

  switch (game_variant) {
  case GAME_VARIANT_CLASSIC:
    string_builder_add_formatted_string(cfg_load_cmd_builder, "-var %s ",
                                        GAME_VARIANT_CLASSIC_NAME);
    break;
  case GAME_VARIANT_WORDSMOG:
    string_builder_add_formatted_string(cfg_load_cmd_builder, "-var %s ",
                                        GAME_VARIANT_WORDSMOG_NAME);
    break;
  default:
    log_fatal("game history has unknown game variant enum: %d", game_variant);
  }

  for (int i = 0; i < 2; i++) {
    if (player_nicknames[i]) {
      string_builder_add_formatted_string(cfg_load_cmd_builder, "-p%d %s ",
                                          i + 1, player_nicknames[i]);
    }
  }

  char *cfg_load_cmd = string_builder_dump(cfg_load_cmd_builder, NULL);
  string_builder_destroy(cfg_load_cmd_builder);
  config_load_command(config, cfg_load_cmd, error_stack);
  free(cfg_load_cmd);
}

// Validates that the game event player indexes are
// aligned with the game and enforces game event sequence
// logic
void validate_game_event_order_and_index(const GameEvent *game_event,
                                         const GameEvent *previous_game_event,
                                         int game_player_on_turn_index,
                                         bool game_is_over,
                                         ErrorStack *error_stack) {
  game_event_t game_event_type = game_event_get_type(game_event);
  int game_event_player_index = game_event_get_player_index(game_event);
  switch (game_event_type) {
  case GAME_EVENT_TILE_PLACEMENT_MOVE:
  case GAME_EVENT_EXCHANGE:
  case GAME_EVENT_PASS:
    // If this is an actual turn as opposed to a time or points penalty,
    // the Game object player on turn and the GCG player on turn should
    // match, since the play_move function would have updated the
    // player on turn index.
    if (game_player_on_turn_index != game_event_player_index) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_GAME_EVENT_OFF_TURN,
          get_formatted_string("encountered an off turn game event: %s",
                               game_event_get_cgp_move_string(game_event)));
      return;
    }
    if (game_is_over) {
      error_stack_push(error_stack,
                       ERROR_STATUS_GCG_PARSE_MOVE_EVENT_AFTER_GAME_END,
                       get_formatted_string(
                           "encountered a game event after the game ended: %s",
                           game_event_get_cgp_move_string(game_event)));
      return;
    }
    break;
  case GAME_EVENT_CHALLENGE_BONUS:
    if (game_event_get_type(previous_game_event) !=
        GAME_EVENT_TILE_PLACEMENT_MOVE) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_CHALLENGE_BONUS_WITHOUT_PLAY,
          string_duplicate("encountered a challenge bonus without a play"));
      return;
    }
    if (game_event_get_player_index(previous_game_event) !=
        game_event_player_index) {
      error_stack_push(
          error_stack,
          ERROR_STATUS_GCG_PARSE_INVALID_CHALLENGE_BONUS_PLAYER_INDEX,
          string_duplicate(
              "encountered a challenge bonus for the wrong player"));
      return;
    }
    break;
  case GAME_EVENT_PHONY_TILES_RETURNED:
    if (game_event_get_type(previous_game_event) !=
        GAME_EVENT_TILE_PLACEMENT_MOVE) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_PHONY_TILES_RETURNED_WITHOUT_PLAY,
          string_duplicate("encountered a phony tiles return event without "
                           "a previous tile placement move"));
      return;
    }
    if (game_event_get_player_index(previous_game_event) !=
        game_event_player_index) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_INVALID_PHONY_TILES_PLAYER_INDEX,
          string_duplicate(
              "encountered a phony tiles return event for the wrong player"));
      return;
    }
    if (!racks_are_equal(game_event_get_rack(game_event),
                         game_event_get_rack(previous_game_event))) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_PHONY_TILES_RETURNED_MISMATCH,
          string_duplicate(
              "phony tiles played do not match the phony tiles returned"));
      return;
    }
    break;
  case GAME_EVENT_END_RACK_PENALTY:
  case GAME_EVENT_TIME_PENALTY:
  case GAME_EVENT_END_RACK_POINTS:
    if (!game_is_over) {
      char *err_str;
      if (game_event_type == GAME_EVENT_END_RACK_PENALTY) {
        err_str = string_duplicate(
            "encountered an end rack penalty event before the game "
            "ended");
      } else if (game_event_type == GAME_EVENT_END_RACK_POINTS) {
        err_str = string_duplicate(
            "encountered an end rack points event before the game "
            "ended");
      } else {
        err_str = string_duplicate(
            "encountered an end time penalty event before the game "
            "ended");
      }
      error_stack_push(error_stack,
                       ERROR_STATUS_GCG_PARSE_END_GAME_EVENT_BEFORE_GAME_END,
                       err_str);
      return;
    }
    break;
  case GAME_EVENT_UNKNOWN:
    log_fatal("encountered unknown game event in order and index validation");
    break;
  }
}

bool game_event_has_player_rack(const GameEvent *game_event, int player_index) {
  game_event_t game_event_type = game_event_get_type(game_event);
  int game_event_player_index = game_event_get_player_index(game_event);
  if (game_event_player_index == player_index) {
    return game_event_type == GAME_EVENT_TILE_PLACEMENT_MOVE ||
           game_event_type == GAME_EVENT_PASS ||
           game_event_type == GAME_EVENT_EXCHANGE ||
           game_event_type == GAME_EVENT_END_RACK_PENALTY;
  }
  return game_event_type == GAME_EVENT_END_RACK_POINTS;
}

// Returns NULL if there is no rack for the player and sets the next rack set
// boolean
const Rack *get_player_next_rack(GameHistory *game_history,
                                 int initial_game_event_index,
                                 int player_index) {
  int number_of_game_events = game_history_get_number_of_events(game_history);
  for (int game_event_index = initial_game_event_index + 1;
       game_event_index < number_of_game_events; game_event_index++) {
    const GameEvent *game_event =
        game_history_get_event(game_history, game_event_index);
    if (game_event_index == initial_game_event_index + 1 &&
        game_event_get_type(game_event) == GAME_EVENT_PHONY_TILES_RETURNED) {
      game_history_player_set_next_rack_set(game_history, player_index, true);
      return NULL;
    }
    if (game_event_has_player_rack(game_event, player_index)) {
      game_history_player_set_next_rack_set(game_history, player_index, true);
      return game_event_get_rack(game_event);
    }
  }
  game_history_player_set_next_rack_set(game_history, player_index, false);
  return NULL;
}

void play_game_history_turn(GameHistory *game_history, Game *game,
                            int game_event_index, bool validate,
                            ErrorStack *error_stack) {
  GameEvent *game_event =
      game_history_get_event(game_history, game_event_index);
  const GameEvent *previous_game_event = NULL;
  if (game_event_index > 0) {
    previous_game_event =
        game_history_get_event(game_history, game_event_index - 1);
  }

  int game_player_on_turn_index = game_get_player_on_turn_index(game);

  if (validate) {
    validate_game_event_order_and_index(game_event, previous_game_event,
                                        game_player_on_turn_index,
                                        game_over(game), error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }

  int game_event_player_index = game_event_get_player_index(game_event);
  game_event_t game_event_type = game_event_get_type(game_event);

  ValidatedMoves *vms = NULL;
  const char *cgp_move_string = game_event_get_cgp_move_string(game_event);
  const Equity move_score = game_event_get_move_score(game_event);
  bool game_event_is_move = false;
  switch (game_event_type) {
  case GAME_EVENT_TILE_PLACEMENT_MOVE:
  case GAME_EVENT_EXCHANGE:
  case GAME_EVENT_PASS:
    game_event_is_move = true;
    if (validate) {
      vms =
          validated_moves_create(game, game_event_player_index, cgp_move_string,
                                 true, true, true, error_stack);
      // Set the validated move in the game event immediately so
      // that the game event can take ownership of the vms.
      game_event_set_vms(game_event, vms);

      if (!error_stack_is_empty(error_stack)) {
        error_stack_push(
            error_stack, ERROR_STATUS_GCG_PARSE_MOVE_VALIDATION_ERROR,
            string_duplicate(
                "encountered a move validation error during GCG parsing"));
        return;
      }

      // Confirm the score from the GCG matches the score from the validated
      // move
      if (move_get_score(validated_moves_get_move(vms, 0)) != move_score) {
        error_stack_push(
            error_stack, ERROR_STATUS_GCG_PARSE_MOVE_SCORING_ERROR,
            get_formatted_string(
                "calculated move score (%d) does not match the move "
                "score in the GCG (%d) for move: %s",
                move_get_score(validated_moves_get_move(vms, 0)), move_score,
                game_event_get_cgp_move_string(game_event)));
        return;
      }
    } else {
      vms = game_event_get_vms(game_event);
    }

    game_set_backup_mode(game, BACKUP_MODE_SIMULATION);
    play_move_status_t play_move_status =
        play_move(validated_moves_get_move(vms, 0), game,
                  get_player_next_rack(game_history, game_event_index,
                                       game_event_player_index),
                  NULL);
    if (play_move_status != PLAY_MOVE_STATUS_SUCCESS) {
      error_stack_push(error_stack, ERROR_STATUS_GCG_PARSE_RACK_NOT_IN_BAG,
                       get_formatted_string(
                           "play contains tiles that are not in the bag: %s",
                           game_event_get_cgp_move_string(game_event)));
      return;
    }

    game_set_backup_mode(game, BACKUP_MODE_OFF);
    if (game_event_type == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      Rack *previous_played_tiles =
          game_history_player_get_previous_played_tiles(
              game_history, game_event_player_index);
      Rack *known_rack_from_phonies =
          game_history_player_get_known_rack_from_phonies(
              game_history, game_event_player_index);
      validated_moves_set_rack_to_played_tiles(vms, 0, previous_played_tiles);
      rack_subtract_using_floor_zero(known_rack_from_phonies,
                                     previous_played_tiles);
    } else if (game_event_type == GAME_EVENT_EXCHANGE) {
      Rack *known_rack_from_phonies =
          game_history_player_get_known_rack_from_phonies(
              game_history, game_event_player_index);
      rack_reset(known_rack_from_phonies);
    }
    break;
  case GAME_EVENT_CHALLENGE_BONUS:
    player_add_to_score(game_get_player(game, game_event_player_index),
                        game_event_get_score_adjustment(game_event));
    break;
  case GAME_EVENT_PHONY_TILES_RETURNED:;
    const Rack *previous_played_tiles =
        game_history_player_get_previous_played_tiles(game_history,
                                                      game_event_player_index);
    Rack *known_rack_from_phonies =
        game_history_player_get_known_rack_from_phonies(
            game_history, game_event_player_index);
    rack_union(known_rack_from_phonies, previous_played_tiles);
    // This event is guaranteed to immediately succeed
    // a tile placement move.
    return_phony_tiles(game);
    break;
  case GAME_EVENT_TIME_PENALTY:
    player_add_to_score(game_get_player(game, game_event_player_index),
                        game_event_get_score_adjustment(game_event));
    break;
  case GAME_EVENT_END_RACK_PENALTY:
  case GAME_EVENT_END_RACK_POINTS:
    // The play_move function will have handled both end rack penalty
    // and end rack points cases. For unknown game events the GCG
    // token does not correspond to a game event and therefore does
    // not need to be processed in the Game object.
    break;
  case GAME_EVENT_UNKNOWN:
    log_fatal("encountered unknown game event when playing game history turn");
    break;
  }

  if (!validate) {
    return;
  }

  if (
      // When the Game object makes the final play, it also
      // automatically adds the end rack points, so the cumulative
      // scores for the GCG and Game will not match for the last play.
      (!game_over(game) || !game_event_is_move)) {
    const int game_event_cume = game_event_get_cumulative_score(game_event);
    const int player_score_cume =
        player_get_score(game_get_player(game, game_event_player_index));
    if (game_event_cume != player_score_cume) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_CUMULATIVE_SCORING_ERROR,
          get_formatted_string(
              "calculated cumulative score (%d) does not match the cumulative "
              "score in the GCG (%d)",
              game_event_cume, player_score_cume));
      return;
    }
  }

  game_history_player_set_score(game_history, game_event_player_index,
                                game_event_get_cumulative_score(game_event));
}

// Perform GCG token validations and operations that are shared
// across many GCG tokens, including:
// - Ensuring pragmas uniqueness
// - Ensuring pragmas occur before move events
// - Extracting player index from game events
void common_gcg_token_validation(GCGParser *gcg_parser, gcg_token_t token,
                                 const char *gcg_line, int number_of_events,
                                 int *player_index, ErrorStack *error_stack) {
  switch (token) {
    // The following pragmas must always be before move events
    // and must be unique
  case GCG_TITLE_TOKEN:
  case GCG_DESCRIPTION_TOKEN:
  case GCG_ID_TOKEN:
  case GCG_LEXICON_TOKEN:
  case GCG_BOARD_LAYOUT_TOKEN:
  case GCG_TILE_DISTRIBUTION_NAME_TOKEN:
  case GCG_GAME_TYPE_TOKEN:
    if (number_of_events > 0) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_PRAGMA_SUCCEEDED_EVENT,
          get_formatted_string("encountered pragma after game event: %s",
                               gcg_line));
      return;
    }
    if (gcg_parser->gcg_token_count[token] > 0) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_REDUNDANT_PRAGMA,
          get_formatted_string("encountered redundant pragma: %s", gcg_line));
      return;
    }
    break;
    // The following pragmas must always be before move events
  case GCG_PLAYER_TOKEN:
    if (number_of_events > 0) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_PRAGMA_SUCCEEDED_EVENT,
          get_formatted_string("encountered pragma after game event: %s",
                               gcg_line));
      return;
    }
    break;
    // The following pragmas must always be unique
  case GCG_RACK1_TOKEN:
  case GCG_RACK2_TOKEN:
    if (gcg_parser->gcg_token_count[token] > 0) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_REDUNDANT_PRAGMA,
          get_formatted_string("encountered redundant pragma: %s", gcg_line));
      return;
    }
    break;
  // The following game events must have a player index
  case GCG_MOVE_TOKEN:
  case GCG_PHONY_TILES_RETURNED_TOKEN:
  case GCG_TIME_PENALTY_TOKEN:
  case GCG_END_RACK_PENALTY_TOKEN:
  case GCG_PASS_TOKEN:
  case GCG_CHALLENGE_BONUS_TOKEN:
  case GCG_END_RACK_POINTS_TOKEN:
  case GCG_EXCHANGE_TOKEN:
    *player_index = get_player_index(gcg_parser, gcg_line, 1);
    if (*player_index < 0) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_PLAYER_DOES_NOT_EXIST,
          get_formatted_string("unrecognized player: %s", gcg_line));
      return;
    }
    if (number_of_events == MAX_GAME_EVENTS) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_GAME_EVENTS_OVERFLOW,
          get_formatted_string(
              "exceeded the maximum number (%d) of game events: %s",
              MAX_GAME_EVENTS, gcg_line));
      return;
    }
  default:
    break;
  }
  // The redundancy check for these tokens was performed above,
  // so any errors here are necessarily other tokens appearing
  // after the last rack token(s).
  if (gcg_parser->gcg_token_count[GCG_RACK1_TOKEN] > 0 ||
      gcg_parser->gcg_token_count[GCG_RACK2_TOKEN] > 0) {
    error_stack_push(
        error_stack, ERROR_STATUS_GCG_PARSE_EVENT_AFTER_LAST_RACK,
        get_formatted_string(
            "encountered a game event after a last rack pragma: %s", gcg_line));
    return;
  }
  gcg_parser->gcg_token_count[token]++;
}

void parse_gcg_line(GCGParser *gcg_parser, const char *gcg_line,
                    ErrorStack *error_stack) {
  GameHistory *game_history = gcg_parser->game_history;
  gcg_token_t token = find_matching_gcg_token(gcg_parser, gcg_line);
  gcg_token_t previous_token = gcg_parser->previous_token;
  if (token != GCG_UNKNOWN_TOKEN) {
    gcg_parser->previous_token = token;
  }
  int number_of_events =
      game_history_get_number_of_events(gcg_parser->game_history);
  // Perform logic with previous token here because it
  // is set.
  if (previous_token == GCG_NOTE_TOKEN && token != GCG_NOTE_TOKEN &&
      token != GCG_UNKNOWN_TOKEN) {
    finalize_note(gcg_parser);
  }

  if (token == GCG_MOVE_TOKEN || token == GCG_PASS_TOKEN ||
      token == GCG_EXCHANGE_TOKEN || token == GCG_RACK1_TOKEN ||
      token == GCG_RACK2_TOKEN) {
    if (!game_history_both_players_are_set(gcg_parser->game_history)) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_MOVE_BEFORE_PLAYER,
          get_formatted_string(
              "encountered a move or rack before both players are set: %s",
              gcg_line));
      return;
    }

    if (!gcg_parser->game) {
      const char *lexicon_name = game_history_get_lexicon_name(game_history);
      if (!lexicon_name) {
        const PlayersData *players_data =
            config_get_players_data(gcg_parser->config);
        if (!players_data_get_data(players_data, PLAYERS_DATA_TYPE_KWG, 0) ||
            !players_data_get_data(players_data, PLAYERS_DATA_TYPE_KWG, 1)) {
          error_stack_push(
              error_stack, ERROR_STATUS_GCG_PARSE_LEXICON_NOT_SPECIFIED,
              string_duplicate("cannot parse gcg without a specified lexicon"));
          return;
        }
        lexicon_name =
            players_data_get_data_name(players_data, PLAYERS_DATA_TYPE_KWG, 0);
        game_history_set_lexicon_name(game_history, lexicon_name);
      }
      if (!game_history_get_ld_name(game_history)) {
        char *default_ld_name =
            ld_get_default_name_from_lexicon_name(lexicon_name, error_stack);
        if (!error_stack_is_empty(error_stack)) {
          return;
        }
        game_history_set_ld_name(game_history, default_ld_name);
        free(default_ld_name);
      }
      if (!game_history_get_board_layout_name(game_history)) {
        char *default_layout = board_layout_get_default_name();
        game_history_set_board_layout_name(game_history, default_layout);
        free(default_layout);
      }
      load_config_with_game_history(game_history, gcg_parser->config,
                                    error_stack);
      if (!error_stack_is_empty(error_stack)) {
        return;
      }

      gcg_parser->game = config_game_create(gcg_parser->config);
    }
  }

  int player_index = -1;

  // Sets the player index
  common_gcg_token_validation(gcg_parser, token, gcg_line, number_of_events,
                              &player_index, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  GameEvent *game_event = NULL;
  Rack *player_last_known_rack = NULL;
  Rack *game_event_rack = NULL;
  StringBuilder *move_string_builder = NULL;
  char *cgp_move_string = NULL;
  int move_score = 0;
  switch (token) {
  case GCG_PLAYER_TOKEN:
    // The value of player_index is guaranteed to be either 0 or 1 by regex
    // matching
    player_index =
        get_matching_group_as_int(gcg_parser, gcg_line, 1, error_stack) - 1;
    if (!error_stack_is_empty(error_stack)) {
      log_fatal("encountered unexpected player index: %d", player_index);
    }
    if (game_history_player_is_set(game_history, player_index)) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_PLAYER_NUMBER_REDUNDANT,
          get_formatted_string("redundant player number: %s", gcg_line));
      return;
    }
    char *player_nickname =
        get_matching_group_as_string(gcg_parser, gcg_line, 2);
    char *player_name = get_matching_group_as_string(gcg_parser, gcg_line, 3);
    game_history_set_player(game_history, player_index, player_name,
                            player_nickname);
    free(player_name);
    free(player_nickname);
    if (game_history_player_is_set(game_history, 1 - player_index) &&
        strings_equal(
            game_history_player_get_name(game_history, player_index),
            game_history_player_get_name(game_history, 1 - player_index))) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_DUPLICATE_NAMES,
          get_formatted_string("duplicate player name: %s", gcg_line));
      return;
    }
    if (game_history_player_is_set(game_history, 1 - player_index) &&
        strings_equal(
            game_history_player_get_nickname(game_history, player_index),
            game_history_player_get_nickname(game_history, 1 - player_index))) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_DUPLICATE_NICKNAMES,
          get_formatted_string("duplicate player nickname: %s", gcg_line));
      return;
    }
    break;
  case GCG_TITLE_TOKEN: {
    char *title = get_matching_group_as_string(gcg_parser, gcg_line, 1);
    game_history_set_title(game_history, title);
    free(title);
    break;
  }
  case GCG_DESCRIPTION_TOKEN: {
    char *description = get_matching_group_as_string(gcg_parser, gcg_line, 1);
    game_history_set_description(game_history, description);
    free(description);
    break;
  }
  case GCG_ID_TOKEN: {
    char *id_auth = get_matching_group_as_string(gcg_parser, gcg_line, 1);
    char *uid = get_matching_group_as_string(gcg_parser, gcg_line, 2);
    game_history_set_id_auth(game_history, id_auth);
    game_history_set_uid(game_history, uid);
    free(id_auth);
    free(uid);
    break;
  }
  case GCG_RACK1_TOKEN:
    player_last_known_rack = get_rack_from_matching(gcg_parser, gcg_line, 1);
    if (!player_last_known_rack) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_RACK_MALFORMED,
          get_formatted_string("could not parse rack: %s", gcg_line));
      return;
    }
    game_history_player_set_last_known_rack(game_history, 0,
                                            player_last_known_rack);
    rack_destroy(player_last_known_rack);
    player_last_known_rack =
        game_history_player_get_last_known_rack(game_history, 0);
    if (!draw_rack_from_bag(gcg_parser->game, 0, player_last_known_rack)) {
      error_stack_push(error_stack, ERROR_STATUS_GCG_PARSE_RACK_NOT_IN_BAG,
                       get_formatted_string(
                           "rack is not available in the bag: %s", gcg_line));
      return;
    }
    break;
  case GCG_RACK2_TOKEN:
    player_last_known_rack = get_rack_from_matching(gcg_parser, gcg_line, 1);
    if (!player_last_known_rack) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_RACK_MALFORMED,
          get_formatted_string("could not parse rack: %s", gcg_line));
      return;
    }
    game_history_player_set_last_known_rack(game_history, 1,
                                            player_last_known_rack);
    rack_destroy(player_last_known_rack);
    player_last_known_rack =
        game_history_player_get_last_known_rack(game_history, 1);
    if (!draw_rack_from_bag(gcg_parser->game, 1, player_last_known_rack)) {
      error_stack_push(error_stack, ERROR_STATUS_GCG_PARSE_RACK_NOT_IN_BAG,
                       get_formatted_string(
                           "rack is not available in the bag: %s", gcg_line));
      return;
    }
    break;
  case GCG_ENCODING_TOKEN:
    error_stack_push(
        error_stack, ERROR_STATUS_GCG_PARSE_MISPLACED_ENCODING,
        get_formatted_string("encountered unexpected encoding pragma: %s",
                             gcg_line));
    return;
    break;
  case GCG_MOVE_TOKEN:
    game_event =
        game_history_create_and_add_game_event(game_history, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
    game_event_set_player_index(game_event, player_index);
    // Write the move rack
    game_event_set_type(game_event, GAME_EVENT_TILE_PLACEMENT_MOVE);

    move_string_builder = string_builder_create();

    // Position
    add_matching_group_to_string_builder(move_string_builder, gcg_parser,
                                         gcg_line, 3);
    string_builder_add_char(move_string_builder, '.');

    // Play
    add_tiles_played_to_string_builder(move_string_builder, gcg_parser,
                                       gcg_line, 4);
    string_builder_add_char(move_string_builder, '.');

    // Rack
    add_matching_group_to_string_builder(move_string_builder, gcg_parser,
                                         gcg_line, 2);
    game_event_rack = get_rack_from_matching(gcg_parser, gcg_line, 2);
    game_event_set_rack(game_event, game_event_rack);
    if (!game_event_rack) {
      string_builder_destroy(move_string_builder);
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_RACK_MALFORMED,
          get_formatted_string("could not parse rack: %s", gcg_line));
      return;
    }

    cgp_move_string = string_builder_dump(move_string_builder, NULL);
    string_builder_destroy(move_string_builder);

    // Get the GCG score so it can be compared to the validated move score
    move_score =
        get_move_score_from_gcg_line(gcg_parser, gcg_line, 5, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_MOVE_SCORE_MALFORMED,
          get_formatted_string("invalid move score for move: %s", gcg_line));
      return;
    }

    // Cumulative score
    copy_cumulative_score_to_game_event(gcg_parser, game_event, gcg_line, 6,
                                        error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(error_stack, ERROR_STATUS_GCG_PARSE_MOVE_SCORE_MALFORMED,
                       get_formatted_string(
                           "invalid cumulative score for move: %s", gcg_line));
      return;
    }
    break;
  case GCG_NOTE_TOKEN:
    if (number_of_events == 0) {
      error_stack_push(error_stack, ERROR_STATUS_GCG_PARSE_NOTE_PRECEDENT_EVENT,
                       get_formatted_string(
                           "encountered note before game event: %s", gcg_line));
      return;
    }
    char *note = get_matching_group_as_string(gcg_parser, gcg_line, 1);
    string_builder_add_formatted_string(gcg_parser->note_builder, "%s\n", note);
    free(note);
    break;
  case GCG_LEXICON_TOKEN: {
    char *lexicon_name = get_matching_group_as_string(gcg_parser, gcg_line, 1);
    game_history_set_lexicon_name(game_history, lexicon_name);
    free(lexicon_name);
    break;
  }
  case GCG_BOARD_LAYOUT_TOKEN: {
    char *board_layout_name =
        get_matching_group_as_string(gcg_parser, gcg_line, 1);
    game_history_set_board_layout_name(game_history, board_layout_name);
    free(board_layout_name);
    break;
  }
  case GCG_TILE_DISTRIBUTION_NAME_TOKEN: {
    char *tile_distribution_name =
        get_matching_group_as_string(gcg_parser, gcg_line, 1);
    game_history_set_ld_name(game_history, tile_distribution_name);
    free(tile_distribution_name);
    break;
  }
  case GCG_GAME_TYPE_TOKEN: {
    char *game_variant_name =
        get_matching_group_as_string(gcg_parser, gcg_line, 1);
    game_variant_t game_variant =
        get_game_variant_type_from_name(game_variant_name);
    free(game_variant_name);
    if (game_variant == GAME_VARIANT_UNKNOWN) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_UNRECOGNIZED_GAME_VARIANT,
          get_formatted_string("unrecognized game variant: %s", gcg_line));
      return;
    }
    game_history_set_game_variant(game_history, game_variant);
    break;
  }
  case GCG_PHONY_TILES_RETURNED_TOKEN:
    game_event =
        game_history_create_and_add_game_event(game_history, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }

    game_event_rack = get_rack_from_matching(gcg_parser, gcg_line, 2);
    game_event_set_rack(game_event, game_event_rack);

    if (!game_event_rack) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_RACK_MALFORMED,
          get_formatted_string("could not parse rack: %s", gcg_line));
      return;
    }
    game_event_set_player_index(game_event, player_index);
    game_event_set_type(game_event, GAME_EVENT_PHONY_TILES_RETURNED);
    copy_cumulative_score_to_game_event(gcg_parser, game_event, gcg_line, 4,
                                        error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(error_stack, ERROR_STATUS_GCG_PARSE_MOVE_SCORE_MALFORMED,
                       get_formatted_string(
                           "invalid cumulative score for move: %s", gcg_line));
      return;
    }
    break;
  case GCG_TIME_PENALTY_TOKEN:
    game_event =
        game_history_create_and_add_game_event(game_history, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
    game_event_set_player_index(game_event, player_index);
    game_event_set_type(game_event, GAME_EVENT_TIME_PENALTY);
    // Rack is allowed to be empty for time penalty
    if (get_matching_group_string_length(gcg_parser, 2) == 0) {
      game_event_rack = NULL;
    } else {
      game_event_rack = get_rack_from_matching(gcg_parser, gcg_line, 2);
      if (!game_event_rack) {
        error_stack_push(
            error_stack, ERROR_STATUS_GCG_PARSE_RACK_MALFORMED,
            get_formatted_string("could not parse rack: %s", gcg_line));
        return;
      }
    }
    game_event_set_rack(game_event, game_event_rack);

    copy_score_adjustment_to_game_event(gcg_parser, game_event, gcg_line, 3,
                                        error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_MOVE_SCORE_MALFORMED,
          get_formatted_string("invalid event score for move: %s", gcg_line));
      return;
    }

    copy_cumulative_score_to_game_event(gcg_parser, game_event, gcg_line, 4,
                                        error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(error_stack, ERROR_STATUS_GCG_PARSE_MOVE_SCORE_MALFORMED,
                       get_formatted_string(
                           "invalid cumulative score for move: %s", gcg_line));
      return;
    }
    break;
  case GCG_END_RACK_PENALTY_TOKEN:
    game_event =
        game_history_create_and_add_game_event(game_history, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
    game_event_set_player_index(game_event, player_index);
    game_event_set_type(game_event, GAME_EVENT_END_RACK_PENALTY);
    game_event_rack = get_rack_from_matching(gcg_parser, gcg_line, 2);
    game_event_set_rack(game_event, game_event_rack);
    if (!game_event_rack) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_RACK_MALFORMED,
          get_formatted_string("could not parse rack: %s", gcg_line));
      return;
    }

    Rack *penalty_tiles = get_rack_from_matching(gcg_parser, gcg_line, 3);
    if (!penalty_tiles) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_RACK_MALFORMED,
          get_formatted_string("could not parse rack: %s", gcg_line));
      return;
    }
    bool penalty_tiles_equals_rack =
        racks_are_equal(game_event_rack, penalty_tiles);

    if (!penalty_tiles_equals_rack) {
      rack_destroy(penalty_tiles);
      error_stack_push(error_stack,
                       ERROR_STATUS_GCG_PARSE_PLAYED_LETTERS_NOT_IN_RACK,
                       get_formatted_string(
                           "end rack penalty tiles not in bag: %s", gcg_line));
      return;
    }

    copy_score_adjustment_to_game_event(gcg_parser, game_event, gcg_line, 4,
                                        error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(error_stack, ERROR_STATUS_GCG_PARSE_MOVE_SCORE_MALFORMED,
                       get_formatted_string(
                           "invalid cumulative score for move: %s", gcg_line));
      return;
    }

    const int rack_score =
        rack_get_score(game_get_ld(gcg_parser->game), penalty_tiles);
    const int game_event_score_adj =
        game_event_get_score_adjustment(game_event);

    if (-rack_score != game_event_score_adj) {
      rack_destroy(penalty_tiles);
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_END_RACK_PENALTY_INCORRECT,
          get_formatted_string(
              "rack score (%d) does not match end rack penalty (%d): %s",
              rack_score, -game_event_score_adj, gcg_line));
      return;
    }

    rack_destroy(penalty_tiles);

    copy_cumulative_score_to_game_event(gcg_parser, game_event, gcg_line, 5,
                                        error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(error_stack, ERROR_STATUS_GCG_PARSE_MOVE_SCORE_MALFORMED,
                       get_formatted_string(
                           "invalid cumulative score for move: %s", gcg_line));
      return;
    }

    break;
  case GCG_PASS_TOKEN:
    game_event =
        game_history_create_and_add_game_event(game_history, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
    game_event_set_player_index(game_event, player_index);
    game_event_set_type(game_event, GAME_EVENT_PASS);

    move_string_builder = string_builder_create();

    // Add the pass to the builder
    string_builder_add_formatted_string(move_string_builder, "%s.",
                                        UCGI_PASS_MOVE);
    // Add the rack to the builder
    add_matching_group_to_string_builder(move_string_builder, gcg_parser,
                                         gcg_line, 2);
    game_event_rack = get_rack_from_matching(gcg_parser, gcg_line, 2);
    game_event_set_rack(game_event, game_event_rack);
    if (!game_event_rack) {
      string_builder_destroy(move_string_builder);
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_RACK_MALFORMED,
          get_formatted_string("could not parse rack: %s", gcg_line));
      return;
    }

    cgp_move_string = string_builder_dump(move_string_builder, NULL);
    string_builder_destroy(move_string_builder);

    copy_cumulative_score_to_game_event(gcg_parser, game_event, gcg_line, 3,
                                        error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(error_stack, ERROR_STATUS_GCG_PARSE_MOVE_SCORE_MALFORMED,
                       get_formatted_string(
                           "invalid cumulative score for move: %s", gcg_line));
      return;
    }
    break;
  case GCG_CHALLENGE_BONUS_TOKEN:
    game_event =
        game_history_create_and_add_game_event(game_history, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
    game_event_set_player_index(game_event, player_index);
    game_event_set_type(game_event, GAME_EVENT_CHALLENGE_BONUS);
    game_event_rack = get_rack_from_matching(gcg_parser, gcg_line, 2);
    game_event_set_rack(game_event, game_event_rack);
    if (!game_event_rack) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_RACK_MALFORMED,
          get_formatted_string("could not parse rack: %s", gcg_line));
      return;
    }
    copy_score_adjustment_to_game_event(gcg_parser, game_event, gcg_line, 3,
                                        error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_MOVE_SCORE_MALFORMED,
          get_formatted_string("invalid event score for move: %s", gcg_line));
      return;
    }
    copy_cumulative_score_to_game_event(gcg_parser, game_event, gcg_line, 4,
                                        error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(error_stack, ERROR_STATUS_GCG_PARSE_MOVE_SCORE_MALFORMED,
                       get_formatted_string(
                           "invalid cumulative score for move: %s", gcg_line));
      return;
    }
    break;
  case GCG_END_RACK_POINTS_TOKEN:
    game_event =
        game_history_create_and_add_game_event(game_history, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
    game_event_set_type(game_event, GAME_EVENT_END_RACK_POINTS);
    game_event_set_player_index(game_event, player_index);
    game_event_rack = get_rack_from_matching(gcg_parser, gcg_line, 2);
    game_event_set_rack(game_event, game_event_rack);
    if (!game_event_rack) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_RACK_MALFORMED,
          get_formatted_string("could not parse rack: %s", gcg_line));
      return;
    }

    copy_score_adjustment_to_game_event(gcg_parser, game_event, gcg_line, 3,
                                        error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_MOVE_SCORE_MALFORMED,
          get_formatted_string("invalid event score for move: %s", gcg_line));
      return;
    }

    const int end_rack_score =
        rack_get_score(game_get_ld(gcg_parser->game), game_event_rack);
    const int game_event_rack_points =
        game_event_get_score_adjustment(game_event);
    if (end_rack_score * 2 != game_event_rack_points) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_RACK_END_POINTS_INCORRECT,
          get_formatted_string(
              "double rack score (%d) does not match end rack points (%d): %s",
              end_rack_score * 2, game_event_rack_points, gcg_line));
      return;
    }

    copy_cumulative_score_to_game_event(gcg_parser, game_event, gcg_line, 4,
                                        error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(error_stack, ERROR_STATUS_GCG_PARSE_MOVE_SCORE_MALFORMED,
                       get_formatted_string(
                           "invalid cumulative score for move: %s", gcg_line));
      return;
    }
    break;
  case GCG_EXCHANGE_TOKEN:
    game_event =
        game_history_create_and_add_game_event(game_history, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
    game_event_set_player_index(game_event, player_index);
    game_event_set_type(game_event, GAME_EVENT_EXCHANGE);

    move_string_builder = string_builder_create();

    // Exchange token
    string_builder_add_formatted_string(move_string_builder, "%s.",
                                        UCGI_EXCHANGE_MOVE);
    // Tiles exchanged
    add_matching_group_to_string_builder(move_string_builder, gcg_parser,
                                         gcg_line, 3);
    string_builder_add_char(move_string_builder, '.');

    // Rack
    add_matching_group_to_string_builder(move_string_builder, gcg_parser,
                                         gcg_line, 2);
    game_event_rack = get_rack_from_matching(gcg_parser, gcg_line, 2);
    game_event_set_rack(game_event, game_event_rack);
    if (!game_event_rack) {
      string_builder_destroy(move_string_builder);
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_RACK_MALFORMED,
          get_formatted_string("could not parse rack: %s", gcg_line));
      return;
    }

    cgp_move_string = string_builder_dump(move_string_builder, NULL);
    string_builder_destroy(move_string_builder);

    copy_cumulative_score_to_game_event(gcg_parser, game_event, gcg_line, 4,
                                        error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(error_stack, ERROR_STATUS_GCG_PARSE_MOVE_SCORE_MALFORMED,
                       get_formatted_string(
                           "invalid cumulative score for move: %s", gcg_line));
      return;
    }
    break;
  case GCG_UNKNOWN_TOKEN:
    if (previous_token == GCG_NOTE_TOKEN) {
      // Assume this is the continuation of a note
      string_builder_add_formatted_string(gcg_parser->note_builder, "%s\n",
                                          gcg_line);

    } else if (!is_string_empty_or_whitespace(gcg_line)) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_NO_MATCHING_TOKEN,
          get_formatted_string("unrecognized GCG token: %s", gcg_line));
      return;
    }
    break;
  case GCG_TILE_SET_TOKEN:
    // For now, don't do anything
    break;
  case NUMBER_OF_GCG_TOKENS:
    log_fatal("invalid gcg token");
    break;
  }

  if (game_event) {
    game_event_set_cgp_move_string(game_event, cgp_move_string);
    game_event_set_move_score(game_event, move_score);
  }
}

void draw_initial_racks(Game *game, GameHistory *game_history,
                        ErrorStack *error_stack) {
  if (game_history_get_number_of_events(game_history) == 0) {
    return;
  }
  for (int player_index = 0; player_index < 2; player_index++) {
    const Rack *next_player_rack =
        get_player_next_rack(game_history, -1, player_index);
    if (next_player_rack) {
      if (!draw_rack_from_bag(game, player_index, next_player_rack)) {
        error_stack_push(
            error_stack, ERROR_STATUS_GCG_PARSE_RACK_NOT_IN_BAG,
            string_duplicate(
                "player %d starting rack not available in the bag"));
        return;
      }
    }
  }
}

void parse_gcg_with_parser(GCGParser *gcg_parser, const char *gcg_string,
                           ErrorStack *error_stack) {
  StringSplitter *gcg_lines = decode_gcg(gcg_parser, gcg_string, error_stack);

  if (!error_stack_is_empty(error_stack)) {
    // The gcg_lines StringSplitter is NULL if the error stack is not empty
    return;
  }

  int number_of_gcg_lines = string_splitter_get_number_of_items(gcg_lines);
  for (int i = 0; i < number_of_gcg_lines; i++) {
    parse_gcg_line(gcg_parser, string_splitter_get_item(gcg_lines, i),
                   error_stack);
    if (!error_stack_is_empty(error_stack)) {
      break;
    }
  }

  if (error_stack_is_empty(error_stack)) {
    finalize_note(gcg_parser);
  }

  string_splitter_destroy(gcg_lines);

  if (!error_stack_is_empty(error_stack) ||
      game_history_get_number_of_events(gcg_parser->game_history) == 0) {
    return;
  }

  // Play through the game solely to detected errors
  draw_initial_racks(gcg_parser->game, gcg_parser->game_history, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  const int ld_size = ld_get_size(game_get_ld(gcg_parser->game));
  game_history_init_player_phony_calc_racks(gcg_parser->game_history, ld_size);
  int number_of_game_events =
      game_history_get_number_of_events(gcg_parser->game_history);
  for (int game_event_index = 0; game_event_index < number_of_game_events;
       game_event_index++) {
    play_game_history_turn(gcg_parser->game_history, gcg_parser->game,
                           game_event_index, true, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      break;
    }
  }
}

void parse_gcg_string(const char *gcg_string, Config *config,
                      GameHistory *game_history, ErrorStack *error_stack) {
  if (is_string_empty_or_whitespace(gcg_string)) {
    error_stack_push(error_stack, ERROR_STATUS_GCG_PARSE_GCG_EMPTY,
                     string_duplicate("GCG is empty"));
    return;
  }
  GCGParser *gcg_parser = gcg_parser_create(config, game_history);
  parse_gcg_with_parser(gcg_parser, gcg_string, error_stack);
  gcg_parser_destroy(gcg_parser);
}

void parse_gcg(const char *gcg_filename, Config *config,
               GameHistory *game_history, ErrorStack *error_stack) {
  char *gcg_string = get_string_from_file(gcg_filename, error_stack);
  if (error_stack_is_empty(error_stack)) {
    parse_gcg_string(gcg_string, config, game_history, error_stack);
  }
  free(gcg_string);
}

void game_play_to_turn(GameHistory *game_history, Game *game, int turn_index,
                       ErrorStack *error_stack) {
  game_reset(game);
  // Draw the initial racks
  draw_initial_racks(game, game_history, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  int number_of_game_events = game_history_get_number_of_events(game_history);
  int current_turn_index = 0;
  const int ld_size = ld_get_size(game_get_ld(game));
  game_history_init_player_phony_calc_racks(game_history, ld_size);
  for (int game_event_index = 0; game_event_index < number_of_game_events;
       game_event_index++) {
    const GameEvent *game_event =
        game_history_get_event(game_history, game_event_index);
    current_turn_index += game_event_get_turn_value(game_event);
    if (current_turn_index > turn_index && !game_over(game)) {
      break;
    }
    play_game_history_turn(game_history, game, game_event_index, false,
                           error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }
  if (game_over(game)) {
    return;
  }

  int player_on_turn_index = game_get_player_on_turn_index(game);
  // Only show the rack for the player on turn
  int player_off_turn_index = 1 - player_on_turn_index;

  return_rack_to_bag(game, player_off_turn_index);
  const Rack *player_off_turn_known_phony_tiles =
      game_history_player_get_known_rack_from_phonies(game_history,
                                                      player_off_turn_index);

  if (!draw_rack_from_bag(game, player_off_turn_index,
                          player_off_turn_known_phony_tiles)) {
    error_stack_push(
        error_stack, ERROR_STATUS_GCG_PARSE_RACK_NOT_IN_BAG,
        get_formatted_string(
            "last rack for player %d not in bag when replaying game",
            player_off_turn_index + 1));
    return;
  }

  if (game_history_player_get_next_rack_set(game_history,
                                            player_on_turn_index)) {
    return;
  }

  return_rack_to_bag(game, player_on_turn_index);
  const Rack *player_on_turn_last_known_rack =
      game_history_player_get_last_known_rack(game_history,
                                              player_on_turn_index);
  if (player_on_turn_last_known_rack &&
      !draw_rack_from_bag(game, player_on_turn_index,
                          player_on_turn_last_known_rack)) {
    error_stack_push(
        error_stack, ERROR_STATUS_GCG_PARSE_RACK_NOT_IN_BAG,
        get_formatted_string(
            "last rack for player %d not in bag when replaying game",
            player_on_turn_index + 1));
    return;
  }
}

void game_play_to_end(GameHistory *game_history, Game *game,
                      ErrorStack *error_stack) {
  game_play_to_turn(game_history, game,
                    game_history_get_number_of_events(game_history),
                    error_stack);
}

// Assumes the game history is valid
void write_gcg(const char *gcg_filename, const LetterDistribution *ld,
               const GameHistory *game_history, ErrorStack *error_stack) {
  StringBuilder *gcg_sb = string_builder_create();
  string_builder_add_formatted_string(gcg_sb, "#%s UTF-8\n",
                                      GCG_CHAR_ENCODING_STRING);
  string_builder_add_formatted_string(gcg_sb, "#%s Created with MAGPIE\n",
                                      GCG_DESCRIPTION_STRING);
  const char *id_auth = game_history_get_id_auth(game_history);
  const char *uid = game_history_get_uid(game_history);
  if (id_auth && !uid) {
    log_fatal("game history has auth id '%s' but no uid", id_auth);
  }
  if (!id_auth && uid) {
    log_fatal("game history has uid '%s' but no auth id", uid);
  }
  if (uid) {
    string_builder_add_formatted_string(gcg_sb, "#%s %s %s\n", GCG_ID_STRING,
                                        id_auth, uid);
  }
  const char *lexicon = game_history_get_lexicon_name(game_history);
  if (lexicon) {
    string_builder_add_formatted_string(gcg_sb, "#%s %s\n", GCG_LEXICON_STRING,
                                        lexicon);
  }
  const game_variant_t game_variant =
      game_history_get_game_variant(game_history);
  if (game_variant != GAME_VARIANT_UNKNOWN) {
    string_builder_add_formatted_string(gcg_sb, "#%s ", GCG_GAME_TYPE_STRING);
    string_builder_add_game_variant(gcg_sb, game_variant);
    string_builder_add_string(gcg_sb, "\n");
  }
  const char *title = game_history_get_title(game_history);
  if (title) {
    string_builder_add_formatted_string(gcg_sb, "#%s %s\n", GCG_TITLE_STRING,
                                        title);
  }
  const char *board_layout_name =
      game_history_get_board_layout_name(game_history);
  if (board_layout_name) {
    string_builder_add_formatted_string(
        gcg_sb, "#%s %s\n", GCG_BOARD_LAYOUT_STRING, board_layout_name);
  }
  const char *ld_name = game_history_get_ld_name(game_history);
  if (ld_name) {
    string_builder_add_formatted_string(gcg_sb, "#%s %s\n",
                                        GCG_TILE_DISTRIBUTION_STRING, ld_name);
  }

  for (int player_index = 0; player_index < 2; player_index++) {
    const char *player_nickname =
        game_history_player_get_nickname(game_history, player_index);
    if (!player_nickname) {
      log_fatal("player %d has no nickname", player_index);
    }
    const char *player_name =
        game_history_player_get_name(game_history, player_index);
    if (!player_name) {
      log_fatal("player %d has no name", player_index);
    }
    string_builder_add_formatted_string(gcg_sb, "#%s%d %s %s\n",
                                        GCG_PLAYER_STRING, player_index + 1,
                                        player_nickname, player_name);
  }
  const int number_of_events = game_history_get_number_of_events(game_history);
  Equity previous_move_score = 0;
  int player_on_turn = 0;
  bool game_is_over = false;
  for (int event_index = 0; event_index < number_of_events; event_index++) {
    const GameEvent *event = game_history_get_event(game_history, event_index);
    const Rack *rack = game_event_get_rack(event);
    string_builder_add_formatted_string(
        gcg_sb, ">%s: ",
        game_history_player_get_nickname(game_history,
                                         game_event_get_player_index(event)));
    switch (game_event_get_type(event)) {
    case GAME_EVENT_PASS:
    case GAME_EVENT_TILE_PLACEMENT_MOVE:
    case GAME_EVENT_EXCHANGE:
      string_builder_add_rack(gcg_sb, rack, ld, true);
      string_builder_add_char(gcg_sb, ' ');
      const ValidatedMoves *vms = game_event_get_vms(event);
      const Move *move = validated_moves_get_move(vms, 0);
      string_builder_add_gcg_move(gcg_sb, move, ld);
      previous_move_score = move_get_score(move);
      player_on_turn = 1 - game_event_get_player_index(event);
      break;
    case GAME_EVENT_PHONY_TILES_RETURNED:
      string_builder_add_rack(gcg_sb, rack, ld, true);
      string_builder_add_formatted_string(gcg_sb, " -- -%d",
                                          equity_to_int(previous_move_score));
      break;
    case GAME_EVENT_CHALLENGE_BONUS:
      string_builder_add_rack(gcg_sb, rack, ld, true);
      string_builder_add_formatted_string(
          gcg_sb, " (challenge) +%d",
          equity_to_int(game_event_get_score_adjustment(event)));
      break;
    case GAME_EVENT_END_RACK_POINTS:
      string_builder_add_char(gcg_sb, '(');
      string_builder_add_rack(gcg_sb, rack, ld, true);
      string_builder_add_formatted_string(
          gcg_sb, ") +%d",
          equity_to_int(game_event_get_score_adjustment(event)));
      game_is_over = true;
      break;
    case GAME_EVENT_TIME_PENALTY:
      if (rack) {
        string_builder_add_rack(gcg_sb, rack, ld, true);
      }
      string_builder_add_char(gcg_sb, ' ');
      string_builder_add_formatted_string(
          gcg_sb, "(time) %d",
          equity_to_int(game_event_get_score_adjustment(event)));
      game_is_over = true;
      break;
    case GAME_EVENT_END_RACK_PENALTY:
      string_builder_add_rack(gcg_sb, rack, ld, true);
      string_builder_add_string(gcg_sb, " (");
      string_builder_add_rack(gcg_sb, rack, ld, true);
      string_builder_add_formatted_string(
          gcg_sb, ") %d",
          equity_to_int(game_event_get_score_adjustment(event)));
      game_is_over = true;
      break;
    case GAME_EVENT_UNKNOWN:
      log_fatal("game history contains unknown event type");
      break;
    }
    string_builder_add_formatted_string(
        gcg_sb, " %d\n", equity_to_int(game_event_get_cumulative_score(event)));
    const char *note = game_event_get_note(event);
    if (note) {
      // The note already contains a trailing newline
      string_builder_add_formatted_string(gcg_sb, "#%s %s", GCG_NOTE_STRING,
                                          note);
    }
  }
  if (!game_is_over) {
    const Rack *last_known_rack =
        game_history_player_get_last_known_rack(game_history, player_on_turn);
    if (last_known_rack) {
      string_builder_add_formatted_string(gcg_sb, "#%s%d ", GCG_RACK_STRING,
                                          player_on_turn + 1);
      string_builder_add_rack(gcg_sb, last_known_rack, ld, true);
    }
  }
  write_string_to_file(gcg_filename, "w", string_builder_peek(gcg_sb),
                       error_stack);
  string_builder_destroy(gcg_sb);
}