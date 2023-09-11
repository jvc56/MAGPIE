#include <regex.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game_history.h"
#include "log.h"
#include "string_builder.h"
#include "util.h"

#define MAX_GCG_FILE_SIZE 100000
#define MAX_GCG_LINE_LENGTH 512
#define MAX_GROUPS 7

typedef enum {
  GCG_ENCODING_ISO_8859_1,
  GCG_ENCODING_UTF8,
} gcg_encoding_t;

typedef enum {
  GCG_PARSE_STATUS_SUCCESS,
  GCG_PARSE_STATUS_DUPLICATE_NAMES,
  GCG_PARSE_STATUS_DUPLICATE_NICKNAMES,
  GCG_PARSE_STATUS_PRAGMA_PRECEDENT_EVENT,
  GCG_PARSE_STATUS_ENCODING_WRONG_PLACE,
  GCG_PARSE_STATUS_PLAYER_NOT_SUPPORTED,
  GCG_PARSE_STATUS_PLAYER_DOES_NOT_EXIST,
  GCG_PARSE_STATUS_PLAYER_NUMBER_REDUNDANT,
  GCG_PARSE_STATUS_LINE_OVERFLOW,
  GCG_PARSE_STATUS_UNSUPPORTED_CHARACTER_ENCODING,
  GCG_PARSE_STATUS_GCG_EMPTY,
  GCG_PARSE_STATUS_RACK_OVERFLOW,
  GCG_PARSE_STATUS_NOTE_PRECEDENT_EVENT,
  GCG_PARSE_STATUS_NO_MATCHING_TOKEN,
  GCG_PARSE_STATUS_PHONY_TILES_RETURNED_WITHOUT_PLAY,
  GCG_PARSE_STATUS_LAST_RACK_PENALTY_MALFORMED,
} gcg_parse_status_t;

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
  GCG_GAME_BOARD_TOKEN,
  GCG_BOARD_LAYOUT_TOKEN,
  GCG_TILE_DISTRIBUTION_NAME_TOKEN,
  GCG_CONTINUATION_TOKEN,
  GCG_INCOMPLETE_TOKEN,
  GCG_TILE_DECLARATION_TOKEN,
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
  bool converted_to_utf8;
  char regmatch_t matching_groups[(MAX_GROUPS)];
  StringBuilder *note_builder;
  gcg_token_t previous_token;
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
const char *lexicon_name_regex = "#lexicon_name (?P<lexicon_name>.+)";
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
    log_fatal("Could not compile regex: %s", regex_string);
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
  gcg_parser->note_builder = create_string_builder();
  gcg_parser->token_regex_pairs =
      malloc(sizeof(TokenRegexPair) * (NUMBER_OF_TOKENS));
  int token_regex_pair_index = 0;
  token_regex_pairs[token_regex_pair_index++] =
      create_token_regex_pair(GCG_PLAYER_TOKEN, player_regex);

  return gcg_parser;
}

void destroy_gcg_parser(GCGParser *gcg_parser) {
  if (gcg_parser->converted_to_utf8) {
    free(gcg_parser->utf8_gcg_string);
  }
  for (int i = 0; i < (NUMBER_OF_TOKENS); i++) {
    destroy_token_regex_pair(gcg_parser->token_regex_pairs[i]);
  }
  destroy_string_builder(gcg_parser->note_builder);
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

gcg_parse_status_t load_next_gcg_line(GCGParser *gcg_parser) {
  int buffer_index = 0;
  gcg_parse_status_t gcg_parse_status = GCG_PARSE_STATUS_SUCCESS;
  char *gcg_string = gcg_parser->input_gcg_string;
  if (gcg_parser->converted_to_utf8) {
    gcg_string = gcg_parser->utf8_gcg_string;
  }
  while (gcg_string[gcg_parser->current_gcg_char_index] != '\n' &&
         gcg_string[gcg_parser->current_gcg_char_index] != '\r') {
    if (buffer_index > MAX_GCG_LINE_LENGTH) {
      gcg_parse_status = GCG_PARSE_STATUS_LINE_OVERFLOW;
      break;
    } else if (gcg_string[gcg_parser->current_gcg_char_index] == '\0') {
      gcg_parser->at_end_of_gcg = true;
      break;
    }
    gcg_parser->gcg_line_buffer[buffer_index] =
        gcg_string[gcg_parser->current_gcg_char_index];
    gcg_parser->current_gcg_char_index++;
    buffer_index++;
  }
  return gcg_parse_status;
}

gcg_parse_status_t handle_encoding(GCGParser *gcg_parser) {
  gcg_parse_status_t gcg_parse_status = GCG_PARSE_STATUS_SUCCESS;

  // Find encoding token
  TokenRegexPair *encoding_token_regex_pair = NULL;
  for (int i = 0; i < NUMBER_OF_TOKENS; i++) {
    if (gcg_parser->token_regex_pairs[i]->token == GCG_ENCODING_TOKEN) {
      encoding_token_regex_pair = gcg_parser->token_regex_pairs[i];
      break;
    }
  }
  if (encoding_token_regex_pair == NULL) {
    log_fatal("Encoding token not found\n");
  }

  gcg_parse_status_t gcg_parse_status = load_next_gcg_line(gcg_parser);
  if (gcg_parse_status != GCG_PARSE_STATUS_SUCCESS) {
    return gcg_parse_status;
  }

  // ISO_8859-1 is the default encoding
  gcg_encoding_t gcg_encoding = GCG_ENCODING_ISO_8859_1;

  int regexec_result =
      regexec(&encoding_token_regex_pair->regex, gcg_parser->gcg_line_buffer,
              (MAX_GROUPS), gcg_parser->matching_groups, 0);
  if (!regexec_result) {
    char encoding_string[MAX_GCG_LINE_LENGTH] = '\0';
    copy_substring(gcg_parser->gcg_line_buffer,
                   gcg_parser->matching_groups[1].rm_so,
                   gcg_parser->matching_groups[1].rm_eo, encoding_string);

    if (!strcmp("utf-8", encoding_string) || strcmp("utf8", encoding_string)) {
      gcg_encoding = GCG_ENCODING_UTF8;
    } else {
      return GCG_PARSE_STATUS_UNSUPPORTED_CHARACTER_ENCODING;
    }
  } else if (regexec_result != REG_NOMATCH) {
    char msgbuf[100];
    regerror(regexec_result, &gcg_parser->token_regex_pairs[i]->regex, msgbuf,
             sizeof(msgbuf));
    log_fatal("Regex match failed for encoding token: %s\n", msgbuf);
  }

  if (gcg_encoding == GCG_ENCODING_ISO_8859_1) {
    // Convert to utf8.
    gcg_parser->utf8_gcg_string =
        malloc(sizeof(char) * 2 * strlen(gcg_parser->input_gcg_string));
    utf8_encode(gcg_parser->input_gcg_string +
                    gcg_parser->current_gcg_char_index,
                gcg_parser->utf8_gcg_string);
    gcg_parser->converted_to_utf8 = true;
  }
  gcg_parser->current_gcg_char_index = 0;
  return gcg_parse_status;
}

gcg_token_t find_matching_gcg_token(GCGParser *gcg_parser) {
  for (int i = 0; i < NUMBER_OF_TOKENS; i++) {
    int regexec_result = regexec(&gcg_parser->token_regex_pairs[i]->regex,
                                 gcg_parser->gcg_line_buffer, (MAX_GROUPS),
                                 gcg_parser->matching_groups, 0);
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

char *get_matching_group_as_string(GCGParser *gcg_parser, int group_index) {}

void copy_matching_group_to_string(GCGParser *gcg_parser, int group_index,
                                   char *dest) {}

int get_player_index(GCGParser *gcg_parser, int group_index) { return -1; }

void copy_score_to_game_event(GCGParser *gcg_parser, GameEvent *game_event,
                              int group_index) {
  game_event->move->score =
      strtol(gcg_parser->gcg_line_buffer +
                 gcg_parser->matching_groups[group_index].rm_so,
             NULL, 10);
}

void copy_cumulative_score_to_game_event(GCGParser *gcg_parser,
                                         GameEvent *game_event,
                                         int group_index) {
  game_event->cumulative_score =
      strtol(gcg_parser->gcg_line_buffer +
                 gcg_parser->matching_groups[group_index].rm_so,
             NULL, 10);
}

void copy_played_tiles_to_game_event(GCGParser *gcg_parser,
                                     GameEvent *game_event, int group_index) {
  char played_tiles_string[MAX_CHAR_PLAY_SIZE] = "";
  for (int i = gcg_parser->matching_groups[4].rm_so;
       i <= gcg_parser->matching_groups[4].rm_eo; i++) {
    played_tiles_string[i] = gcg_parser->gcg_line_buffer[i];
  }
  game_event->move->tiles_length =
      str_to_machine_letters(game_history->letter_distribution,
                             played_tiles_string, game_event->move->tiles);
  // Calculate tiles played
  game_event->move->tiles_played = 0;
  for (int i = 0; i < game_event->move->tiles_length; i++) {
    if (game_event->move->tiles[i] != PLAYED_THROUGH_MARKER) {
      game_event->move->tiles_played++;
    }
  }
}

Rack *get_rack_from_matching(GCGParser *gcg_parser, int group_index) {}

void copy_position_to_game_event(GCGParser *gcg_parser, GameEvent *game_event,
                                 int group_index) {
  bool previous_char_was_digit = false;
  for (int i = gcg_parser->matching_groups[group_index].rm_so;
       i <= gcg_parser->matching_groups[group_index].rm_eo; i++) {
    char position_char = gcg_parser->gcg_line_buffer[i];
    if (position_char >= 48 && position_char <= 57) {
      if (i == 0) {
        game_event->move->vertical = 0;
      }
      previous_char_was_digit = true;
      game_event->move->row_start =
          game_event->move->row_start * 10 + (position_char - 48);
    } else if (position_char >= 65 && position_char <= 90) {
      if (i == 0) {
        game_event->move->vertical = 1;
      }
      game_event->move->row_start = position_char - 65;
    } else {
      return GCG_PARSE_STATUS_RACK_OVERFLOW;
    }
  }
}

gcg_parse_status_t parse_next_gcg_line(GCGParser *gcg_parser) {
  gcg_parse_status_t gcg_parse_status = load_next_gcg_line(gcg_parser);
  if (gcg_parse_status != GCG_PARSE_STATUS_SUCCESS) {
    return gcg_parse_status;
  }
  GameHistory *game_history = gcg_parser->game_history;
  gcg_token_t token = find_matching_gcg_token(gcg_parser);
  // Perform logic with previous token here because it
  // is set.
  if (gcg_parser->previous_token == GCG_NOTE_TOKEN && token != GCG_NOTE_TOKEN) {
    game_history->events[game_history->number_of_events - 1]->note =
        string_builder_dump(gcg_parser->note_builder);
    game_history.string_builder_clear(gcg_parser->note_builder);
  }
  gcg_parser->previous_token = token;
  switch (token) {
  case GCG_PLAYER_TOKEN:
    if (game_history->number_of_events > 0) {
      return GCG_PARSE_STATUS_PRAGMA_PRECEDENT_EVENT;
    }
    int player_index = get_player_index(gcg_parser, 1);
    if (player_index != 0 && player_index != 1) {
      return GCG_PARSE_STATUS_PLAYER_NOT_SUPPORTED;
    }
    if (game_history->players[player_index]) {
      return GCG_PARSE_STATUS_PLAYER_NUMBER_REDUNDANT;
    }
    char *player_name = get_matching_group_as_string(gcg_parser, 2);
    char *player_nickname = get_matching_group_as_string(gcg_parser, 3);
    if (game_history->players[1 - player_index] &&
        !strcmp(player_name, game_history->players[1 - player_index]->name)) {
      return GCG_PARSE_STATUS_DUPLICATE_NAMES;
    }
    if (game_history->players[1 - player_index] &&
        !strcmp(player_nickname,
                game_history->players[1 - player_index]->nickname)) {
      return GCG_PARSE_STATUS_DUPLICATE_NICKNAMES;
    }
    game_history->players[player_index] =
        create_game_history_player(player_index, player_name, player_nickname);
    free(player_name);
    free(player_nickname);
    break;
  case GCG_TITLE_TOKEN:
    if (game_history->number_of_events > 0) {
      return GCG_PARSE_STATUS_PRAGMA_PRECEDENT_EVENT;
    }
    game_history->title = get_matching_group_as_string(gcg_parser, 1);
    break;
  case GCG_DESCRIPTION_TOKEN:
    if (game_history->number_of_events > 0) {
      return GCG_PARSE_STATUS_PRAGMA_PRECEDENT_EVENT;
    }
    game_history->description = get_matching_group_as_string(gcg_parser, 1);
    break;
  case GCG_ID_TOKEN:
    if (game_history->number_of_events > 0) {
      return GCG_PARSE_STATUS_PRAGMA_PRECEDENT_EVENT;
    }
    game_history->id_auth = get_matching_group_as_string(gcg_parser, 1);
    game_history->uid = get_matching_group_as_string(gcg_parser, 2);
    break;
  case GCG_RACK1_TOKEN:
    int char_rack_length = gcg_parser->matching_groups[1].rm_eo -
                           gcg_parser->matching_groups[1].rm_so;
    if (char_rack_length > MAX_CHAR_RACK_SIZE) {
      return GCG_PARSE_STATUS_RACK_OVERFLOW;
    }
    game_history->players[0]->last_known_rack =
        get_rack_from_matching(gcg_parser, 1);
    break;
  case GCG_RACK2_TOKEN:
    int char_rack_length = gcg_parser->matching_groups[1].rm_eo -
                           gcg_parser->matching_groups[1].rm_so;
    if (char_rack_length > MAX_CHAR_RACK_SIZE) {
      return GCG_PARSE_STATUS_RACK_OVERFLOW;
    }
    game_history->players[1]->last_known_rack =
        get_rack_from_matching(gcg_parser, 1);
    break;
  case GCG_ENCODING_TOKEN:
    return GCG_PARSE_STATUS_ENCODING_WRONG_PLACE;
    break;
  case GCG_MOVE_TOKEN:
    int player_index = get_player_index(gcg_parser, 1);
    if (player_index < 0) {
      return GCG_PARSE_STATUS_PLAYER_DOES_NOT_EXIST;
    }

    GameEvent *game_event = create_game_event(game_history);

    // Write the move rack
    game_event->event_type = GAME_EVENT_TILE_PLACEMENT_MOVE;
    game_event->move = create_move();

    // Rack
    game_event->rack = get_rack_from_matching(gcg_parser, 2);

    // Position
    copy_position_to_game_event(gcg_parser, game_event, 3);

    // Played tiles
    copy_played_tiles_to_game_event(gcg_parser, game_event, 4);

    // Score
    copy_score_to_game_event(gcg_parser, game_event, 5);

    // Cumulative score
    copy_cumulative_score_to_game_event(gcg_parser, game_event, 6);

    break;
  case GCG_NOTE_TOKEN:
    if (game_history->number_of_events == 0) {
      return GCG_PARSE_STATUS_NOTE_PRECEDENT_EVENT;
    }
    char *note = get_matching_group_as_string(gcg_parser, 1);
    string_builder_add_string(gcg_parser->note_builder, note, strlen(note));
    free(note);
    break;
  case GCG_LEXICON_TOKEN:
    if (game_history->number_of_events > 0) {
      return GCG_PARSE_STATUS_PRAGMA_PRECEDENT_EVENT;
    }
    game_history->lexicon_name = get_matching_group_as_string(gcg_parser, 1);
    break;
  case GCG_BOARD_LAYOUT_TOKEN:
    if (game_history->number_of_events > 0) {
      return GCG_PARSE_STATUS_PRAGMA_PRECEDENT_EVENT;
    }
    char *board_layout_string = get_matching_group_as_string(gcg_parser, 1);
    game_history->board_layout =
        board_layout_string_to_board_layout(board_layout_string);
    free(board_layout_string);
    break;
  case GCG_TILE_DISTRIBUTION_NAME_TOKEN:
    if (game_history->number_of_events > 0) {
      return GCG_PARSE_STATUS_PRAGMA_PRECEDENT_EVENT;
    }
    game_history->letter_distribution_name =
        get_matching_group_as_string(gcg_parser, 1);
    game_history->letter_distribution =
        load_letter_distribution(game_history->letter_distribution,
                                 game_history->letter_distribution_name);
    break;
  case GCG_GAME_TYPE_TOKEN:
    if (game_history->number_of_events > 0) {
      return GCG_PARSE_STATUS_PRAGMA_PRECEDENT_EVENT;
    }
    game_history->variant = get_matching_group_as_string(gcg_parser, 1);
    break;
  case GCG_PHONY_TILES_RETURNED_TOKEN:
    int player_index = get_player_index(gcg_parser, 1);
    if (player_index < 0) {
      return GCG_PARSE_STATUS_PLAYER_DOES_NOT_EXIST;
    }

    if (game_history->number_of_events == 0 ||
        game_history->events[game_history->number_of_events - 1]->event_type !=
            GAME_EVENT_TILE_PLACEMENT_MOVE) {
      return GCG_PARSE_STATUS_PHONY_TILES_RETURNED_WITHOUT_PLAY;
    }

    GameEvent *game_event = create_game_event(game_history);

    game_event->event_type = GAME_EVENT_PHONY_TILES_RETURNED;
    game_event->rack = get_rack_from_matching(gcg_parser, 2);
    copy_cumulative_score_to_game_event(gcg_parser, game_event, 4);

    break;
  case GCG_TIME_PENALTY_TOKEN:
    int player_index = get_player_index(gcg_parser, 1);
    if (player_index < 0) {
      return GCG_PARSE_STATUS_PLAYER_DOES_NOT_EXIST;
    }
    GameEvent *game_event = create_game_event(game_history);
    game_event->event_type = GAME_EVENT_TIME_PENALTY;
    game_event->rack = get_rack_from_matching(gcg_parser, 2);
    copy_cumulative_score_to_game_event(gcg_parser, game_event, 4);
    break;
  case GCG_LAST_RACK_PENALTY_TOKEN:
    int player_index = get_player_index(gcg_parser, 1);
    if (player_index < 0) {
      return GCG_PARSE_STATUS_PLAYER_DOES_NOT_EXIST;
    }
    GameEvent *game_event = create_game_event(game_history);
    game_event->event_type = GAME_EVENT_END_RACK_PENALTY;
    game_event->rack = get_rack_from_matching(gcg_parser, 2);

    Rack *should_be_identical_rack = get_rack_from_matching(gcg_parser, 3);

    bool racks_are_identical = true;
    for (int i = 0; i < game_event->rack->array_size; i++) {
      should_be_identical_rack->array[i] -= game_event->rack[i];
      if (should_be_identical_rack->array[i] != 0) {
        racks_are_identical = false;
        break;
      }
    }

    destroy_rack(should_be_identical_rack);

    if (!racks_are_identical) {
      return GCG_PARSE_STATUS_LAST_RACK_PENALTY_MALFORMED;
    }

    copy_cumulative_score_to_game_event(gcg_parser, game_event, 5);

    break;
  case GCG_PASS_TOKEN:
    int player_index = get_player_index(gcg_parser, 1);
    if (player_index < 0) {
      return GCG_PARSE_STATUS_PLAYER_DOES_NOT_EXIST;
    }
    GameEvent *game_event = create_game_event(game_history);
    game_event->event_type = GAME_EVENT_PASS;
    game_event->rack = get_rack_from_matching(gcg_parser, 2);
    copy_cumulative_score_to_game_event(gcg_parser, game_event, 3);
    break;
  case GCG_CHALLENGE_BONUS_TOKEN:
  case GCG_END_RACK_POINTS_TOKEN:
    int player_index = get_player_index(gcg_parser, 1);
    if (player_index < 0) {
      return GCG_PARSE_STATUS_PLAYER_DOES_NOT_EXIST;
    }
    GameEvent *game_event = create_game_event(game_history);
    game_event->rack = get_rack_from_matching(gcg_parser, 2);
    copy_cumulative_score_to_game_event(gcg_parser, game_event, 4);
    if (token == GCG_CHALLENGE_BONUS_TOKEN) {
      game_event->event_type = GAME_EVENT_CHALLENGE_BONUS;
    } else if (token == GCG_END_RACK_POINTS_TOKEN) {
      game_event->event_type = GAME_EVENT_END_RACK_POINTS;
    }
    break;
  case GCG_EXCHANGE_TOKEN:
    int player_index = get_player_index(gcg_parser, 1);
    if (player_index < 0) {
      return GCG_PARSE_STATUS_PLAYER_DOES_NOT_EXIST;
    }
    GameEvent *game_event = create_game_event(game_history);
    game_event->event_type = GAME_EVENT_EXCHANGE;
    game_event->rack = get_rack_from_matching(gcg_parser, 2);
    copy_exchanged_to_game_event(gcg_parser, game_event, 3);
    copy_cumulative_score_to_game_event(gcg_parser, game_event, 4);
    break;
  case GCG_UNKNOWN_TOKEN:
    if (gcg_parser->previous_token == GCG_NOTE_TOKEN) {
      // Assume this is the continuation of a note
      string_builder_add_string(gcg_parser->note_builder,
                                gcg_parser->gcg_line_buffer,
                                strlen(gcg_parser->gcg_line_buffer));
    }
    if (!contains_all_whitespace(gcg_parser->gcg_line_buffer)) {
      return GCG_PARSE_STATUS_NO_MATCHING_TOKEN;
    }
  default:
    log_fatal("Unhandled token");
  }
  return gcg_parse_status;
}

gcg_parse_status_t parse_gcg_with_parser(GCGParser *gcg_parser) {
  gcg_parse_status_t gcg_parse_status = handle_encoding(gcg_parser);
  while (gcg_parse_status == GCG_PARSE_STATUS_SUCCESS &&
         !gcg_parser->at_end_of_gcg) {
    gcg_parse_status = parse_next_gcg_line(gcg_parser);
  }
  return gcg_parse_status;
}

gcg_parse_status_t parse_gcg_string(const char *input_gcg_string,
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

gcg_parse_status_t parse_gcg(const char *gcg_filename,
                             GameHistory *game_history) {
  FILE *gcg_file_handle = fopen(gcg_filename, "r");
  if (gcg_file_handle == NULL) {
    log_fatal("Error opening file: %s\n", gcg_filename);
  }

  // Get the file size by seeking to the end and then back to the beginning
  fseek(gcg_file_handle, 0, SEEK_END);
  long file_size = ftell(gcg_file_handle);
  fseek(gcg_file_handle, 0, SEEK_SET);

  if (file_size > MAX_GCG_FILE_SIZE) {
    fclose(gcg_file_handle);
    log_fatal("File size exceeds maximum allowed size of %d bytes.\n",
              MAX_GCG_FILE_SIZE);
  }

  // Allocate memory for the result string
  char *gcg_string = (char *)malloc(file_size + 1); // +1 for null terminator
  if (gcg_string == NULL) {
    fclose(gcg_file_handle);
    log_fatal("Memory allocation error.\n");
  }

  // Read the file content into the gcg_string string
  size_t bytesRead = fread(gcg_string, 1, file_size, gcg_file_handle);
  if (bytesRead != (size_t)file_size) {
    fclose(gcg_file_handle);
    free(gcg_string);
    log_fatal("Error reading file: %s\n", gcg_filename);
  }

  gcg_string[file_size] = '\0'; // Null-terminate the string
  fclose(gcg_file_handle);

  // At this point, the file has been successfully read into 'gcg_string'
  // You can use 'gcg_string' as needed, and don't forget to free the memory
  // when done.
  gcg_parse_status_t gcg_parse_status = parse_gcg(gcg_string, game_history);
  free(gcg_string);
  return gcg_parse_status;
}