#include <regex.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "game_history.h"

#define MAX_GCG_LINE_LENGTH 256
#define MAX_GROUPS 3

typedef enum {
  GCG_ENCODING_ISO_8859_1,
  GCG_ENCODING_UTF8,
  GCG_ENCODING_UNKNOWN,
} gcg_encoding_t;

typedef enum {
  GCG_PARSE_STATUS_SUCCESS,
  GCG_PARSE_STATUS_DUPLICATE_NAMES,
  GCG_PARSE_STATUS_PRAGMA_PRECEDENT_EVENT,
  GCG_PARSE_STATUS_ENCODING_WRONG_PLACE,
  GCG_PARSE_STATUS_PLAYER_NOT_SUPPORTED,
  GCG_PARSE_STATUS_PLAYER_DOES_NOT_EXIST,
  GCG_PARSE_STATUS_LINE_OVERFLOW,
  GCG_PARSE_STATUS_UNSUPPORTED_CHARACTER_ENCODING,
  GCG_PARSE_STATUS_GCG_EMPTY,
  GCG_PARSE_STATUS_REGEX_FAILURE,
  GCG_PARSE_STATUS_NO_MATCHING_TOKEN,
} gcg_parse_status_t;

// A Token is an event in a gcg_string file.

typedef enum {
  PLAYER_TOKEN,
  TITLE_TOKEN,
  DESCRIPTION_TOKEN,
  ID_TOKEN,
  RACK1_TOKEN,
  RACK2_TOKEN,
  ENCODING_TOKEN,
  MOVE_TOKEN,
  NOTE_TOKEN,
  LEXICON_TOKEN,
  PHONY_TILES_RETURNED_TOKEN,
  PASS_TOKEN,
  CHALLENGE_BONUS_TOKEN,
  EXCHANGE_TOKEN,
  END_RACK_POINTS_TOKEN,
  TIME_PENALTY_TOKEN,
  LAST_RACK_PENALTY_TOKEN,
  GAME_TYPE_TOKEN,
  TILE_SET_TOKEN,
  GAME_BOARD_TOKEN,
  BOARD_LAYOUT_TOKEN,
  TILE_DISTRIBUTION_NAME_TOKEN,
  CONTINUATION_TOKEN,
  INCOMPLETE_TOKEN,
  TILE_DECLARATION_TOKEN,
} gcg_token_t;

#define NUMBER_OF_TOKENS (TILE_DECLARATION_TOKEN + 1)

typedef struct TokenRegexPair {
  gcg_token_t token;
  regex_t regex;
} TokenRegexPair;

typedef struct GCGParser {
  const char *input_gcg_string;
  char *utf8_gcg_string;
  int current_gcg_char_index;
  char gcg_line_buffer[MAX_GCG_LINE_LENGTH];
  bool at_end_of_gcg;
  regmatch_t group_array[(MAX_GROUPS)];
  gcg_token_t token;
  TokenRegexPair **token_regex_pairs;
  // Owned by the caller
  GameHistory *game_history;
} GCGParser;

const char *player_regex =
    "#player(?P<p_number>[1-2])\\s+(?P<nick>\\S+)\\s+(?P<real_name>.+)";
const char *title_regex = "#title\\s*(?P<title>.*)";
const char *description_regex = "#description\\s*(?P<description>.*)";
const char *id_regex = "#id\\s*(?P<id_authority>\\S+)\\s+(?P<id>\\S+)";
const char *rack1_regex = "#rack1 (?P<rack>\\S+)";
const char *rack2_regex = "#rack2 (?P<rack>\\S+)";
const char *move_regex =
    ">(?P<nick>\\S+):\\s+(?P<rack>\\S+)\\s+(?P<pos>\\w+)\\s+(?P<play>\\S+)\\s+"
    "\\+(?P<score>\\d+)\\s+(?P<cumul>\\d+)";
const char *note_regex = "#note (?P<note>.+)";
const char *lexicon_regex = "#lexicon (?P<lexicon>.+)";
const char *character_encoding_regex =
    "#character-encoding (?P<encoding>[[:graph:]]+)";
const char *game_type_regex = "#game-type (?P<gameType>.*)";
const char *tile_set_regex = "#tile-set (?P<tileSet>.*)";
const char *game_board_regex = "#game-board (?P<gameBoard>.*)";
const char *board_layout_regex = "#board-layout (?P<boardLayoutName>.*)";
const char *tile_distribution_name_regex =
    "#tile-distribution (?P<tileDistributionName>.*)";
const char *continuation_regex = "#- (?P<continuation>.*)";
const char *phony_tiles_returned_regex =
    ">(?P<nick>\\S+):\\s+(?P<rack>\\S+)\\s+--\\s+-(?P<lost_score>\\d+)\\s+(?P<"
    "cumul>\\d+)";
const char *pass_regex =
    ">(?P<nick>\\S+):\\s+(?P<rack>\\S+)\\s+-\\s+\\+0\\s+(?P<cumul>\\d+)";
const char *challenge_bonus_regex =
    ">(?P<nick>\\S+):\\s+(?P<rack>\\S*)\\s+\\(challenge\\)\\s+\\+(?P<bonus>\\d+"
    ")\\s+(?P<cumul>\\d+)";
const char *exchange_regex = ">(?P<nick>\\S+):\\s+(?P<rack>\\S+)\\s+-(?P<"
                             "exchanged>\\S+)\\s+\\+0\\s+(?P<cumul>\\d+)";
const char *end_rack_points_regex =
    ">(?P<nick>\\S+):\\s+\\((?P<rack>\\S+)\\)\\s+\\+(?P<score>\\d+)\\s+(?P<"
    "cumul>-?\\d+)";
const char *time_penalty_regex =
    ">(?P<nick>\\S+):\\s+(?P<rack>\\S*)\\s+\\(time\\)\\s+\\-(?P<penalty>\\d+)"
    "\\s+(?P<cumul>-?\\d+)";
const char *pts_lost_for_last_rack_regex =
    ">(?P<nick>\\S+):\\s+(?P<rack>\\S+)\\s+\\((?P<rack>\\S+)\\)\\s+\\-(?P<"
    "penalty>\\d+)\\s+(?P<cumul>-?\\d+)";
const char *incomplete_regex = "#incomplete.*";
const char *tile_declaration_regex =
    "#tile (?P<uppercase>\\S+)\\s+(?P<lowercase>\\S+)";

TokenRegexPair *create_token_regex_pair(gcg_token_t token,
                                        const char *regex_string) {
  TokenRegexPair *token_regex_pair = malloc(sizeof(TokenRegexPair));
  token_regex_pair->token = token;
  int regex_compilation_result =
      regcomp(&token_regex_pair->regex, regex_string, 0);
  if (regex_compilation_result) {
    fprintf(stderr, "Could not compile regex\n");
    abort();
  }
  return token_regex_pair
}

TokenRegexPair *destroy_token_regex_pair(TokenRegexPair *token_regex_pair) {
  free(token_regex_pair);
}

GCGParser *create_gcg_parser(const char *input_gcg_string,
                             GameHistory *game_history) {
  GCGParser *gcg_parser = malloc(sizeof(GCGParser));
  gcg_parser->input_gcg_string = input_gcg_string;
  gcg_parser->utf8_gcg_string = input_gcg_string;
  gcg_parser->current_gcg_char_index = 0;
  gcg_parser->at_end_of_gcg = false;

  // All tokens have associated regexes except for the
  // undefined token.
  gcg_parser->token_regex_pairs =
      malloc(sizeof(TokenRegexPair) * (NUMBER_OF_TOKENS));
  int token_regex_pair_index = 0;
  token_regex_pairs[token_regex_pair_index++] =
      create_token_regex_pair(PLAYER_TOKEN, player_regex);

  return gcg_parser;
}

void destroy_gcg_parser(GCGParser *gcg_parser) {
  if (gcg_parser->utf8_gcg_string != gcg_parser->input_gcg_string) {
    free(gcg_parser->utf8_gcg_string);
  }
  for (int i = 0; i < (NUMBER_OF_TOKENS); i++) {
    destroy_token_regex_pair(gcg_parser->token_regex_pairs[i]);
  }
  free(gcg_parser->token_regex_pairs);
  free(gcg_parser);
}

void utf8_encode(const unsigned char *input, unsigned char *output) {
  const unsigned char *in = input;
  unsigned char *out = output;

  while (*in) {
    if (*in < 128) {
      *out++ = *in++;
    } else {
      *out++ = 0xC2 + (*in > 0xBF);
      *out++ = (*in++ & 0x3F) + 0x80;
    }
  }
}

gcg_parse_status_t write_next_gcg_line_to_buffer(GCGParser *gcg_parser) {
  int buffer_index = 0;
  gcg_parse_status_t gcg_parse_status = GCG_PARSE_STATUS_SUCCESS;
  while (gcg_parser->input_gcg_string[gcg_parser->current_gcg_char_index] !=
             '\n' &&
         gcg_parser->input_gcg_string[gcg_parser->current_gcg_char_index] !=
             '\r') {
    if (buffer_index > MAX_GCG_LINE_LENGTH) {
      gcg_parse_status = GCG_PARSE_STATUS_LINE_OVERFLOW;
      break;
    } else if (gcg_parser
                   ->input_gcg_string[gcg_parser->current_gcg_char_index] ==
               '\0') {
      gcg_parser->at_end_of_gcg = true;
      break;
    }
    gcg_parser->gcg_line_buffer[buffer_index] =
        input_gcg_string[gcg_parser->current_gcg_char_index];
    gcg_parser->current_gcg_char_index++;
    buffer_index++;
  }
  return gcg_parse_status;
}

bool at_end_of_gcg(GCGParser *gcg_parser) { return false; }

gcg_parse_status_t handle_encoding(GCGParser *gcg_parser) {
  gcg_parse_status_t gcg_parse_status = GCG_PARSE_STATUS_SUCCESS;

  // TODO: maybe make a string copy func
  // char encoding_string[6];
  // int matching_group_start_index = group_array[1].rm_so;
  // int matching_group_end_index = group_array[1].rm_eo;
  // int length_of_matching_group =
  //     matching_group_end_index - matching_group_start_index;
  // strncpy(encoding_string, gcg_line_buffer + matching_group_start_index,
  //         length_of_matching_group);
  // encoding_string[length_of_matching_group] = '\0';

  // if (!strcmp("utf-8", encoding_string) || strcmp("utf8", encoding_string)) {
  //   gcg_encoding = GCG_ENCODING_UTF8;
  // } else {
  //   gcg_encoding = GCG_ENCODING_UNKNOWN;
  // }
  // first_line_is_encoding = true;

  return gcg_parse_status;
}

gcg_parse_status_t find_matching_gcg_token(GCGParser *gcg_parser) {
  for (int i = 0; i < NUMBER_OF_TOKENS; i++) {
    int regexec_result = regexec(&gcg_parser->token_regex_pairs[i]->regex,
                                 gcg_parser->gcg_line_buffer, (MAX_GROUPS),
                                 gcg_parser->group_array, 0);
    if (!regexec_result) {
      gcg_parser->token = gcg_parser->token_regex_pairs[i]->token;
      return GCG_PARSE_STATUS_SUCCESS;
    } else if (regexec_result != REG_NOMATCH) {
      // char msgbuf[100];
      // regerror(regexec_result, &encoding_token_regex_pair->regex, msgbuf,
      //          sizeof(msgbuf));
      // fprintf(stderr, "Regex match failed: %s\n", msgbuf);
      // exit(1);
      return GCG_PARSE_STATUS_REGEX_FAILURE;
    }
  }
  return GCG_PARSE_STATUS_NO_MATCHING_TOKEN;
}

gcg_parse_status_t parse_next_gcg_line(GCGParser *gcg_parser) {
  gcg_parse_status_t gcg_parse_status =
      write_next_gcg_line_to_buffer(gcg_parser);
  if (gcg_parse_status != GCG_PARSE_STATUS_SUCCESS) {
    return gcg_parse_status;
  }
  gcg_parse_status = find_matching_gcg_token(GCGParser * gcg_parser);
  if (gcg_parse_status != GCG_PARSE_STATUS_SUCCESS) {
    return gcg_parse_status;
  }
  switch (gcg_parser->token) {
  case ENCODING_TOKEN:
    handle_encoding(gcg_parser);
    break;
  default:
    break;
  }
  return gcg_parse_status;
}

gcg_parse_status_t parse_gcg_with_parser(GCGParser *gcg_parser) {
  gcg_parse_status_t gcg_parse_status = GCG_PARSE_STATUS_SUCCESS;
  while (gcg_parse_status == GCG_PARSE_STATUS_SUCCESS &&
         !at_end_of_gcg(GCGParser * gcg_parser)) {
    gcg_parse_status = parse_next_gcg_line(GCGParser * gcg_parser);
  }
  return gcg_parse_status;
}

gcg_parse_status_t parse_gcg(const char *input_gcg_string,
                             GameHistory *game_history) {

  if (input_gcg_string == NULL || !strcmp(input_gcg_string, "")) {
    return GCG_PARSE_STATUS_GCG_EMPTY;
  }
  GCGParser *gcg_parser = create_gcg_parser(input_gcg_string, game_history);
  gcg_parse_status_t gcg_parse_status =
      parse_gcg_with_parser(GCGParser * gcg_parser);
  destroy_gcg_parser(gcg_parser);
  return gcg_parse_status;
}