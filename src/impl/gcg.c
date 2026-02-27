#include "gcg.h"

#include "../def/equity_defs.h"
#include "../def/game_defs.h"
#include "../def/game_history_defs.h"
#include "../def/validated_move_defs.h"
#include "../ent/board_layout.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/rack.h"
#include "../ent/validated_move.h"
#include "../impl/gameplay.h"
#include "../str/game_string.h"
#include "../str/move_string.h"
#include "../str/rack_string.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include <regex.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INTERNAL_DEFAULT_BOARD_LAYOUT_NAME "standard15"
#define INTERNAL_SUPER_BOARD_LAYOUT_NAME "standard21"
#define EXTERNAL_DEFAULT_BOARD_LAYOUT_NAME "CrosswordGame"
#define EXTERNAL_SUPER_BOARD_LAYOUT_NAME "SuperCrosswordGame"

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

typedef enum {
  PARSE_GCG_MODE_SETTINGS,
  PARSE_GCG_MODE_EVENTS,
} parse_gcg_mode_t;

struct GCGParser {
  regmatch_t matching_groups[(MAX_GROUPS)];
  StringBuilder *note_builder;
  gcg_token_t previous_token;
  TokenRegexPair **token_regex_pairs;
  int number_of_token_regex_pairs;
  int gcg_token_count[NUMBER_OF_GCG_TOKENS];
  int current_gcg_line_index;
  bool player_is_reset[2];
  StringSplitter *gcg_lines;
  const char *existing_p0_lexicon;
  // Owned by the caller
  GameHistory *game_history;
  const LetterDistribution *ld;
};

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

GCGParser *gcg_parser_create(const char *gcg_string, GameHistory *game_history,
                             const char *existing_p0_lexicon,
                             ErrorStack *error_stack) {
  GCGParser *gcg_parser = malloc_or_die(sizeof(GCGParser));
  gcg_parser->game_history = game_history;
  gcg_parser->ld = NULL;
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
  gcg_parser->player_is_reset[0] = false;
  gcg_parser->player_is_reset[1] = false;
  gcg_parser->existing_p0_lexicon = existing_p0_lexicon;
  gcg_parser->current_gcg_line_index = 0;
  // The gcg_lines StringSplitter is NULL if the error stack is not empty
  gcg_parser->gcg_lines = decode_gcg(gcg_parser, gcg_string, error_stack);
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
  string_splitter_destroy(gcg_parser->gcg_lines);
  free(gcg_parser->token_regex_pairs);
  free(gcg_parser);
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

// Returns true if successful
bool set_rack_from_matching_impl(const GCGParser *gcg_parser,
                                 const char *gcg_line, int group_index,
                                 Rack *rack_to_set, bool allow_empty) {
  rack_set_dist_size_and_reset(rack_to_set, ld_get_size(gcg_parser->ld));
  if (get_matching_group_string_length(gcg_parser, group_index) == 0) {
    return allow_empty;
  }
  char *player_rack_string =
      get_matching_group_as_string(gcg_parser, gcg_line, group_index);
  const bool success =
      rack_set_to_string(gcg_parser->ld, rack_to_set, player_rack_string) > 0;
  free(player_rack_string);
  return success;
}

bool set_rack_from_matching(const GCGParser *gcg_parser, const char *gcg_line,
                            int group_index, Rack *rack_to_set) {
  return set_rack_from_matching_impl(gcg_parser, gcg_line, group_index,
                                     rack_to_set, false);
}

bool set_rack_from_matching_allow_empty(const GCGParser *gcg_parser,
                                        const char *gcg_line, int group_index,
                                        Rack *rack_to_set) {
  return set_rack_from_matching_impl(gcg_parser, gcg_line, group_index,
                                     rack_to_set, true);
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
void add_letters_played_to_string_builder(StringBuilder *sb,
                                          const GCGParser *gcg_parser,
                                          const char *gcg_line,
                                          int group_index) {
  char *matching_group_string =
      get_matching_group_as_string(gcg_parser, gcg_line, group_index);
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

  int number_of_events = game_history_get_num_events(gcg_parser->game_history);
  GameEvent *event =
      game_history_get_event(gcg_parser->game_history, number_of_events - 1);

  game_event_set_note(event, string_builder_peek(gcg_parser->note_builder));
  string_builder_clear(gcg_parser->note_builder);
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
  case GCG_DESCRIPTION_TOKEN:
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

bool token_is_pragma(const gcg_token_t token) {
  switch (token) {
  case GCG_PLAYER_TOKEN:
  case GCG_TITLE_TOKEN:
  case GCG_DESCRIPTION_TOKEN:
  case GCG_ID_TOKEN:
  case GCG_ENCODING_TOKEN:
  case GCG_LEXICON_TOKEN:
  case GCG_GAME_TYPE_TOKEN:
  case GCG_TILE_SET_TOKEN:
  case GCG_BOARD_LAYOUT_TOKEN:
  case GCG_TILE_DISTRIBUTION_NAME_TOKEN:
    return true;
  default:
    return false;
  }
}

char *get_internal_board_layout_name(const char *board_layout_name) {
  if (strings_equal(board_layout_name, EXTERNAL_DEFAULT_BOARD_LAYOUT_NAME)) {
    return string_duplicate(INTERNAL_DEFAULT_BOARD_LAYOUT_NAME);
  }
  if (strings_equal(board_layout_name, EXTERNAL_SUPER_BOARD_LAYOUT_NAME)) {
    return string_duplicate(INTERNAL_SUPER_BOARD_LAYOUT_NAME);
  }
  return string_duplicate(board_layout_name);
}

char *get_external_board_layout_name(const char *board_layout_name) {
  if (strings_equal(board_layout_name, INTERNAL_DEFAULT_BOARD_LAYOUT_NAME)) {
    return string_duplicate(EXTERNAL_DEFAULT_BOARD_LAYOUT_NAME);
  }
  if (strings_equal(board_layout_name, INTERNAL_SUPER_BOARD_LAYOUT_NAME)) {
    return string_duplicate(EXTERNAL_SUPER_BOARD_LAYOUT_NAME);
  }
  return string_duplicate(board_layout_name);
}

// Returns true if processing should continue
bool parse_gcg_line(GCGParser *gcg_parser, const char *gcg_line,
                    parse_gcg_mode_t parse_gcg_mode, ErrorStack *error_stack) {
  GameHistory *game_history = gcg_parser->game_history;
  gcg_token_t token = find_matching_gcg_token(gcg_parser, gcg_line);
  gcg_token_t previous_token = gcg_parser->previous_token;
  if (token != GCG_UNKNOWN_TOKEN) {
    gcg_parser->previous_token = token;
  }
  const int number_of_events =
      game_history_get_num_events(gcg_parser->game_history);
  // Perform logic with previous token here because it
  // is set.
  if (previous_token == GCG_NOTE_TOKEN && token != GCG_NOTE_TOKEN &&
      token != GCG_UNKNOWN_TOKEN) {
    finalize_note(gcg_parser);
  }

  if (!token_is_pragma(token)) {
    if (!gcg_parser->player_is_reset[0] || !gcg_parser->player_is_reset[1]) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_GAME_EVENT_BEFORE_PLAYER,
          get_formatted_string(
              "encountered game event '%s' before both players were set",
              gcg_line));
      return false;
    }

    if (parse_gcg_mode == PARSE_GCG_MODE_SETTINGS) {
      const char *lexicon_name = game_history_get_lexicon_name(game_history);
      if (!lexicon_name) {
        if (!gcg_parser->existing_p0_lexicon) {
          error_stack_push(
              error_stack, ERROR_STATUS_GCG_PARSE_LEXICON_NOT_SPECIFIED,
              string_duplicate("cannot parse gcg without a specified lexicon "
                               "for player 1"));
          return false;
        }
        lexicon_name = gcg_parser->existing_p0_lexicon;
        game_history_set_lexicon_name(game_history, lexicon_name);
      }
      if (!game_history_get_ld_name(game_history)) {
        char *default_ld_name =
            ld_get_default_name_from_lexicon_name(lexicon_name, error_stack);
        if (!error_stack_is_empty(error_stack)) {
          return false;
        }
        game_history_set_ld_name(game_history, default_ld_name);
        free(default_ld_name);
      }
      if (!game_history_get_board_layout_name(game_history)) {
        char *default_layout = board_layout_get_default_name();
        game_history_set_board_layout_name(game_history, default_layout);
        free(default_layout);
      }
      return false;
    }
  }

  int player_index = -1;

  // Sets the player index
  common_gcg_token_validation(gcg_parser, token, gcg_line, number_of_events,
                              &player_index, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return false;
  }

  GameEvent *game_event = NULL;
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
    if (gcg_parser->player_is_reset[player_index]) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_PLAYER_NUMBER_REDUNDANT,
          get_formatted_string("redundant player number: %s", gcg_line));
      return false;
    }
    char *player_nickname =
        get_matching_group_as_string(gcg_parser, gcg_line, 2);
    char *player_name = get_matching_group_as_string(gcg_parser, gcg_line, 3);
    game_history_player_reset(game_history, player_index, player_name,
                              player_nickname);
    gcg_parser->player_is_reset[player_index] = true;
    free(player_name);
    free(player_nickname);
    if (gcg_parser->player_is_reset[1 - player_index] &&
        strings_equal(
            game_history_player_get_name(game_history, player_index),
            game_history_player_get_name(game_history, 1 - player_index))) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_DUPLICATE_NAMES,
          get_formatted_string("duplicate player name: %s", gcg_line));
      return false;
    }
    if (gcg_parser->player_is_reset[1 - player_index] &&
        strings_equal(
            game_history_player_get_nickname(game_history, player_index),
            game_history_player_get_nickname(game_history, 1 - player_index))) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_DUPLICATE_NICKNAMES,
          get_formatted_string("duplicate player nickname: %s", gcg_line));
      return false;
    }
    break;
  case GCG_TITLE_TOKEN: {
    char *title = get_matching_group_as_string(gcg_parser, gcg_line, 1);
    game_history_set_title(game_history, title);
    free(title);
    break;
  }
  case GCG_DESCRIPTION_TOKEN: {
    char *new_description =
        get_matching_group_as_string(gcg_parser, gcg_line, 1);
    const char *existing_description =
        game_history_get_description(game_history);
    if (existing_description != NULL) {
      char *combined =
          get_formatted_string("%s\n%s", existing_description, new_description);
      game_history_set_description(game_history, combined);
      free(combined);
    } else {
      game_history_set_description(game_history, new_description);
    }
    free(new_description);
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
  case GCG_RACK1_TOKEN:;
    if (!set_rack_from_matching(
            gcg_parser, gcg_line, 1,
            game_history_player_get_last_rack(game_history, 0))) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_RACK_MALFORMED,
          get_formatted_string("could not parse rack1 event: %s", gcg_line));
      return false;
    }
    break;
  case GCG_RACK2_TOKEN:;
    if (!set_rack_from_matching(
            gcg_parser, gcg_line, 1,
            game_history_player_get_last_rack(game_history, 1))) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_RACK_MALFORMED,
          get_formatted_string("could not parse rack2 event: %s", gcg_line));
      return false;
    }
    break;
  case GCG_ENCODING_TOKEN:
    error_stack_push(
        error_stack, ERROR_STATUS_GCG_PARSE_MISPLACED_ENCODING,
        get_formatted_string("encountered unexpected encoding pragma: %s",
                             gcg_line));
    return false;
    break;
  case GCG_MOVE_TOKEN:
    game_event = game_history_add_game_event(game_history, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return false;
    }
    game_event_set_player_index(game_event, player_index);
    // Write the move rack
    game_event_set_type(game_event, GAME_EVENT_TILE_PLACEMENT_MOVE);

    move_string_builder = string_builder_create();

    // Position
    add_matching_group_to_string_builder(move_string_builder, gcg_parser,
                                         gcg_line, 3);
    string_builder_add_char(move_string_builder, ' ');

    // Play
    add_letters_played_to_string_builder(move_string_builder, gcg_parser,
                                         gcg_line, 4);

    if (!set_rack_from_matching(gcg_parser, gcg_line, 2,
                                game_event_get_rack(game_event))) {
      string_builder_destroy(move_string_builder);
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_RACK_MALFORMED,
          get_formatted_string("could not parse move rack: %s", gcg_line));
      return false;
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
      return false;
    }

    // Cumulative score
    copy_cumulative_score_to_game_event(gcg_parser, game_event, gcg_line, 6,
                                        error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(error_stack, ERROR_STATUS_GCG_PARSE_MOVE_SCORE_MALFORMED,
                       get_formatted_string(
                           "invalid cumulative score for move: %s", gcg_line));
      return false;
    }
    break;
  case GCG_NOTE_TOKEN:
    if (number_of_events == 0) {
      error_stack_push(error_stack, ERROR_STATUS_GCG_PARSE_NOTE_PRECEDENT_EVENT,
                       get_formatted_string(
                           "encountered note before game event: %s", gcg_line));
      return false;
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
    char *internal_board_layout_name =
        get_internal_board_layout_name(board_layout_name);
    free(board_layout_name);
    game_history_set_board_layout_name(game_history,
                                       internal_board_layout_name);
    free(internal_board_layout_name);
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
      return false;
    }
    game_history_set_game_variant(game_history, game_variant);
    break;
  }
  case GCG_PHONY_TILES_RETURNED_TOKEN:
    game_event = game_history_add_game_event(game_history, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return false;
    }
    if (!set_rack_from_matching(gcg_parser, gcg_line, 2,
                                game_event_get_rack(game_event))) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_RACK_MALFORMED,
          get_formatted_string("could not parse phony tiles returned rack: %s",
                               gcg_line));
      return false;
    }
    game_event_set_player_index(game_event, player_index);
    game_event_set_type(game_event, GAME_EVENT_PHONY_TILES_RETURNED);
    copy_cumulative_score_to_game_event(gcg_parser, game_event, gcg_line, 4,
                                        error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(error_stack, ERROR_STATUS_GCG_PARSE_MOVE_SCORE_MALFORMED,
                       get_formatted_string(
                           "invalid cumulative score for move: %s", gcg_line));
      return false;
    }
    break;
  case GCG_TIME_PENALTY_TOKEN:
    game_event = game_history_add_game_event(game_history, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return false;
    }
    game_event_set_player_index(game_event, player_index);
    game_event_set_type(game_event, GAME_EVENT_TIME_PENALTY);
    // Rack is allowed to be empty for time penalty
    if (!set_rack_from_matching_allow_empty(gcg_parser, gcg_line, 2,
                                            game_event_get_rack(game_event))) {
      error_stack_push(error_stack, ERROR_STATUS_GCG_PARSE_RACK_MALFORMED,
                       get_formatted_string(
                           "could not parse time penalty rack: %s", gcg_line));
      return false;
    }

    copy_score_adjustment_to_game_event(gcg_parser, game_event, gcg_line, 3,
                                        error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_MOVE_SCORE_MALFORMED,
          get_formatted_string("invalid event score for move: %s", gcg_line));
      return false;
    }

    copy_cumulative_score_to_game_event(gcg_parser, game_event, gcg_line, 4,
                                        error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(error_stack, ERROR_STATUS_GCG_PARSE_MOVE_SCORE_MALFORMED,
                       get_formatted_string(
                           "invalid cumulative score for move: %s", gcg_line));
      return false;
    }
    break;
  case GCG_END_RACK_PENALTY_TOKEN:
    game_event = game_history_add_game_event(game_history, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return false;
    }
    game_event_set_player_index(game_event, player_index);
    game_event_set_type(game_event, GAME_EVENT_END_RACK_PENALTY);
    if (!set_rack_from_matching(gcg_parser, gcg_line, 2,
                                game_event_get_rack(game_event))) {
      error_stack_push(error_stack, ERROR_STATUS_GCG_PARSE_RACK_MALFORMED,
                       get_formatted_string(
                           "could not parse rack penalty rack: %s", gcg_line));
      return false;
    }

    Rack penalty_letters;
    if (!set_rack_from_matching(gcg_parser, gcg_line, 3, &penalty_letters)) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_RACK_MALFORMED,
          get_formatted_string("could not parse rack penalty letters: %s",
                               gcg_line));
      return false;
    }
    bool penalty_letters_equals_rack =
        racks_are_equal(game_event_get_rack(game_event), &penalty_letters);

    if (!penalty_letters_equals_rack) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_PLAYED_LETTERS_NOT_IN_RACK,
          get_formatted_string("end rack penalty letters not in bag: %s",
                               gcg_line));
      return false;
    }

    copy_score_adjustment_to_game_event(gcg_parser, game_event, gcg_line, 4,
                                        error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(error_stack, ERROR_STATUS_GCG_PARSE_MOVE_SCORE_MALFORMED,
                       get_formatted_string(
                           "invalid cumulative score for move: %s", gcg_line));
      return false;
    }

    const Equity end_rack_penalty_equity =
        calculate_end_rack_penalty(&penalty_letters, gcg_parser->ld);
    const Equity game_event_score_adj =
        game_event_get_score_adjustment(game_event);

    if (end_rack_penalty_equity != game_event_score_adj) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_END_RACK_PENALTY_INCORRECT,
          get_formatted_string(
              "rack score (%d) does not match end rack penalty (%d): %s",
              equity_to_int(end_rack_penalty_equity),
              equity_to_int(game_event_score_adj), gcg_line));
      return false;
    }

    copy_cumulative_score_to_game_event(gcg_parser, game_event, gcg_line, 5,
                                        error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(error_stack, ERROR_STATUS_GCG_PARSE_MOVE_SCORE_MALFORMED,
                       get_formatted_string(
                           "invalid cumulative score for move: %s", gcg_line));
      return false;
    }

    break;
  case GCG_PASS_TOKEN:
    game_event = game_history_add_game_event(game_history, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return false;
    }
    game_event_set_player_index(game_event, player_index);
    game_event_set_type(game_event, GAME_EVENT_PASS);

    if (!set_rack_from_matching(gcg_parser, gcg_line, 2,
                                game_event_get_rack(game_event))) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_RACK_MALFORMED,
          get_formatted_string("could not parse pass rack: %s", gcg_line));
      return false;
    }

    cgp_move_string = string_duplicate(UCGI_PASS_MOVE);

    copy_cumulative_score_to_game_event(gcg_parser, game_event, gcg_line, 3,
                                        error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(error_stack, ERROR_STATUS_GCG_PARSE_MOVE_SCORE_MALFORMED,
                       get_formatted_string(
                           "invalid cumulative score for move: %s", gcg_line));
      return false;
    }
    break;
  case GCG_CHALLENGE_BONUS_TOKEN:
    game_event = game_history_add_game_event(game_history, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return false;
    }
    game_event_set_player_index(game_event, player_index);
    game_event_set_type(game_event, GAME_EVENT_CHALLENGE_BONUS);
    if (!set_rack_from_matching_allow_empty(gcg_parser, gcg_line, 2,
                                            game_event_get_rack(game_event))) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_RACK_MALFORMED,
          get_formatted_string("could not parse challenge bonus rack: %s",
                               gcg_line));
      return false;
    }
    copy_score_adjustment_to_game_event(gcg_parser, game_event, gcg_line, 3,
                                        error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_MOVE_SCORE_MALFORMED,
          get_formatted_string("invalid event score for move: %s", gcg_line));
      return false;
    }
    copy_cumulative_score_to_game_event(gcg_parser, game_event, gcg_line, 4,
                                        error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(error_stack, ERROR_STATUS_GCG_PARSE_MOVE_SCORE_MALFORMED,
                       get_formatted_string(
                           "invalid cumulative score for move: %s", gcg_line));
      return false;
    }
    break;
  case GCG_END_RACK_POINTS_TOKEN:
    game_event = game_history_add_game_event(game_history, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return false;
    }
    game_event_set_type(game_event, GAME_EVENT_END_RACK_POINTS);
    game_event_set_player_index(game_event, player_index);
    if (!set_rack_from_matching(gcg_parser, gcg_line, 2,
                                game_event_get_rack(game_event))) {
      error_stack_push(error_stack, ERROR_STATUS_GCG_PARSE_RACK_MALFORMED,
                       get_formatted_string(
                           "could not parse end points rack: %s", gcg_line));
      return false;
    }

    copy_score_adjustment_to_game_event(gcg_parser, game_event, gcg_line, 3,
                                        error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_MOVE_SCORE_MALFORMED,
          get_formatted_string("invalid event score for move: %s", gcg_line));
      return false;
    }

    const Equity end_rack_score_equity = calculate_end_rack_points(
        game_event_get_rack(game_event), gcg_parser->ld);
    const Equity game_event_rack_points_equity =
        game_event_get_score_adjustment(game_event);
    if (end_rack_score_equity != game_event_rack_points_equity) {
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_RACK_END_POINTS_INCORRECT,
          get_formatted_string(
              "double rack score (%d) does not match end rack points (%d): %s",
              equity_to_int(end_rack_score_equity),
              equity_to_int(game_event_rack_points_equity), gcg_line));
      return false;
    }

    copy_cumulative_score_to_game_event(gcg_parser, game_event, gcg_line, 4,
                                        error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(error_stack, ERROR_STATUS_GCG_PARSE_MOVE_SCORE_MALFORMED,
                       get_formatted_string(
                           "invalid cumulative score for move: %s", gcg_line));
      return false;
    }
    break;
  case GCG_EXCHANGE_TOKEN:
    game_event = game_history_add_game_event(game_history, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return false;
    }
    game_event_set_player_index(game_event, player_index);
    game_event_set_type(game_event, GAME_EVENT_EXCHANGE);

    move_string_builder = string_builder_create();

    // Exchange token and tiles exchanged
    string_builder_add_formatted_string(move_string_builder, "%s ",
                                        UCGI_EXCHANGE_MOVE);
    add_matching_group_to_string_builder(move_string_builder, gcg_parser,
                                         gcg_line, 3);

    if (!set_rack_from_matching(gcg_parser, gcg_line, 2,
                                game_event_get_rack(game_event))) {
      string_builder_destroy(move_string_builder);
      error_stack_push(
          error_stack, ERROR_STATUS_GCG_PARSE_RACK_MALFORMED,
          get_formatted_string("could not parse exchange rack: %s", gcg_line));
      return false;
    }

    cgp_move_string = string_builder_dump(move_string_builder, NULL);
    string_builder_destroy(move_string_builder);

    copy_cumulative_score_to_game_event(gcg_parser, game_event, gcg_line, 4,
                                        error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(error_stack, ERROR_STATUS_GCG_PARSE_MOVE_SCORE_MALFORMED,
                       get_formatted_string(
                           "invalid cumulative score for move: %s", gcg_line));
      return false;
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
      return false;
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
  return true;
}

bool parse_gcg_line_and_increment_index(GCGParser *gcg_parser,
                                        parse_gcg_mode_t parse_gcg_mode,
                                        ErrorStack *error_stack) {
  const bool continue_parsing = parse_gcg_line(
      gcg_parser,
      string_splitter_get_item(gcg_parser->gcg_lines,
                               gcg_parser->current_gcg_line_index),
      parse_gcg_mode, error_stack);
  gcg_parser->current_gcg_line_index++;
  return continue_parsing;
}

void parse_gcg_settings(GCGParser *gcg_parser, ErrorStack *error_stack) {
  int number_of_gcg_lines =
      string_splitter_get_number_of_items(gcg_parser->gcg_lines);
  for (int i = gcg_parser->current_gcg_line_index; i < number_of_gcg_lines;
       i++) {
    const bool continue_parsing = parse_gcg_line(
        gcg_parser,
        string_splitter_get_item(gcg_parser->gcg_lines,
                                 gcg_parser->current_gcg_line_index),
        PARSE_GCG_MODE_SETTINGS, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
    if (!continue_parsing) {
      break;
    }
    gcg_parser->current_gcg_line_index++;
  }
}

void parse_gcg_events(GCGParser *gcg_parser, Game *game,
                      ErrorStack *error_stack) {
  gcg_parser->ld = game_get_ld(game);
  int number_of_gcg_lines =
      string_splitter_get_number_of_items(gcg_parser->gcg_lines);
  for (int i = gcg_parser->current_gcg_line_index; i < number_of_gcg_lines;
       i++) {
    const bool continue_parsing = parse_gcg_line(
        gcg_parser,
        string_splitter_get_item(gcg_parser->gcg_lines,
                                 gcg_parser->current_gcg_line_index),
        PARSE_GCG_MODE_EVENTS, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
    if (!continue_parsing) {
      break;
    }
    gcg_parser->current_gcg_line_index++;
  }

  finalize_note(gcg_parser);

  // Remove trailing overtime penalties for incomplete games
  game_history_trim_trailing_overtime_penalties(gcg_parser->game_history);

  // Play through the game to detected errors
  game_play_n_events(gcg_parser->game_history, game,
                     game_history_get_num_events(gcg_parser->game_history),
                     true, error_stack);
}

void string_builder_add_gcg(StringBuilder *gcg_sb, const LetterDistribution *ld,
                            const GameHistory *game_history,
                            const bool star_last_played_move) {
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
    char *external_board_layout_name =
        get_external_board_layout_name(board_layout_name);
    string_builder_add_formatted_string(gcg_sb, "#%s %s\n",
                                        GCG_BOARD_LAYOUT_STRING,
                                        external_board_layout_name);
    free(external_board_layout_name);
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
  const int number_of_events = game_history_get_num_events(game_history);
  Equity previous_move_score = 0;
  int player_on_turn = 0;
  bool game_is_over = false;
  const int last_played_move_index = number_of_events - 1;
  for (int event_index = 0; event_index < number_of_events; event_index++) {
    const GameEvent *event = game_history_get_event(game_history, event_index);
    const Rack *rack = game_event_get_const_rack(event);
    char is_last_played_move_char = ' ';
    if (star_last_played_move && event_index == last_played_move_index) {
      is_last_played_move_char = '*';
    }
    string_builder_add_formatted_string(
        gcg_sb, ">%s:%c",
        game_history_player_get_nickname(game_history,
                                         game_event_get_player_index(event)),
        is_last_played_move_char);
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
    case GAME_EVENT_CHALLENGE_BONUS:;
      const Rack *challenge_bonus_rack = rack;
      // To be compatible with other programs that read GCG files,
      // the challenge bonus rack should be nonempty if possible.
      if (rack_is_empty(challenge_bonus_rack)) {
        const int game_event_player_index = game_event_get_player_index(event);
        for (int i = event_index + 1; i < number_of_events; i++) {
          const GameEvent *future_event =
              game_history_get_event(game_history, i);
          if (game_event_get_player_index(future_event) ==
              game_event_player_index) {
            const Rack *future_rack = game_event_get_const_rack(future_event);
            if (rack_get_dist_size(future_rack) > 0 &&
                !rack_is_empty(future_rack)) {
              challenge_bonus_rack = game_event_get_const_rack(future_event);
            }
            break;
          }
        }
      }
      string_builder_add_rack(gcg_sb, challenge_bonus_rack, ld, true);
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
      string_builder_add_formatted_string(gcg_sb, "#%s %s", GCG_NOTE_STRING,
                                          note);
      // If note does not end with newline, add one
      if (note[string_length(note) - 1] != '\n') {
        string_builder_add_char(gcg_sb, '\n');
      }
    }
  }
  if (!game_is_over) {
    const Rack *last_rack =
        game_history_player_get_last_rack_const(game_history, player_on_turn);
    if (rack_get_dist_size(last_rack) != 0) {
      string_builder_add_formatted_string(gcg_sb, "#%s%d ", GCG_RACK_STRING,
                                          player_on_turn + 1);
      string_builder_add_rack(gcg_sb, last_rack, ld, true);
    }
  }
}

// Assumes the game history is valid
void write_gcg(const char *gcg_filename, const LetterDistribution *ld,
               const GameHistory *game_history, ErrorStack *error_stack) {
  StringBuilder *gcg_sb = string_builder_create();
  string_builder_add_gcg(gcg_sb, ld, game_history, false);
  write_string_to_file(gcg_filename, "w", string_builder_peek(gcg_sb),
                       error_stack);
  string_builder_destroy(gcg_sb);
}
