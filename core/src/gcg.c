#include <regex.h>
#include <stdbool.h>
#include <stdint.h>

#define MAX_GCG_LINE_LENGTH 256

typedef enum {
  UTF8,
} gcg_encoding_t;

typedef enum {
  DUPLICATE_NAMES,
  PRAGMA_PRECEDENT_EVENT,
  ENCODING_WRONG_PLACE,
  PLAYER_NOT_SUPPORTED,
  PLAYER_DOES_NOT_EXIST,
} gcg_error_t;

// A Token is an event in a GCG file.

typedef enum {
  UNDEFINED_TOKEN,
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
  NUMBER_OF_TOKENS
} gcg_token_t;

typedef struct TokenRegexPair {
  gcg_token_t token;
  regex_t regex;
} TokenRegexPair;

typedef struct GCGRegexes {
  TokenRegexPair **token_regex_pairs;
} GCGRegexes;

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

void init_token_regex_pair(TokenRegexPair *token_regex_pair, gcg_token_t token,
                           const char *regex_string) {
  token_regex_pair->token = token;
  int regex_compilation_result =
      regcomp(&token_regex_pair->regex, regex_string, 0);
  if (regex_compilation_result) {
    fprintf(stderr, "Could not compile regex\n");
    abort();
  }
}

TokenRegexPair *get_token_regex_pair_by_token(GCGRegexes *gcg_regexes,
                                              gcg_token_t token) {
  // Linear search is fine since this code does not
  // need to be performant.
  for (int i = 0; i < (NUMBER_OF_TOKENS - 1); i++) {
    if (gcg_regexes->token_regex_pairs[i]->token == token) {
      return gcg_regexes->token_regex_pairs[i];
    }
  }
  return NULL;
}

GCGRegexes *create_gcg_regexes() {
  GCGRegexes *gcg_regexes = malloc(sizeof(GCGRegexes));
  // All tokens have associated regexes except for the
  // undefined token.
  gcg_regexes->token_regex_pairs =
      malloc(sizeof(TokenRegexPair) * (NUMBER_OF_TOKENS - 1));
  init_token_regex_pair(token_regex_pairs[0], PLAYER_TOKEN, player_regex);
  return gcg_regexes;
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

int write_gcg_line_to_buffer(const char *input_gcg, char *buffer,
                             int *current_gcg_char_index) {
  while (input_gcg[current_gcg_char_index] != '\n' &&
         input_gcg[current_gcg_char_index] != '\r' &&
         input_gcg[current_gcg_char_index] != '\0') {
    buffer[current_gcg_char_index] = input_gcg[current_gcg_char_index];
    *current_gcg_char_index++;
    if (current_gcg_char_index > MAX_GCG_LINE_LENGTH) {
      return 1;
    }
  }
  return 0;
}

gcg_error_t parse_gcg(const char *input_gcg, GameHistory *game_history) {

  GCGRegexes *gcg_regexes = create_gcg_regexes();

  int current_gcg_char_index = 0;
  char line_buffer[MAX_GCG_LINE_LENGTH];

  write_gcg_line_to_buffer(input_gcg, line_buffer, &current_gcg_char_index);

  // Determine encoding
  // character-encoding matches
  //   if encoding is neither utf-8 or utf8: error
  //   else: use as is
  // no match
  //   assume iso8859_1 and attempt to convert

  /* Execute regular expression */
  TokenRegexPair *encoding_token_regex_pair =
      get_token_regex_pair_by_token(gcg_regexes, ENCODING_TOKEN);
  if (encoding_token_regex_pair == NULL) {
    fprintf(stderr, "encoding regex not found\n");
    abort();
  }
  int regexec_result =
      regexec(&encoding_token_regex_pair->regex, line_buffer, 0, NULL, 0);
  if (!regexec_result) {
    puts("Match");
  } else if (regexec_result == REG_NOMATCH) {
    puts("No match");
  } else {
    char msgbuf[100];
    regerror(regexec_result, &encoding_token_regex_pair->regex, msgbuf,
             sizeof(msgbuf));
    fprintf(stderr, "Regex match failed: %s\n", msgbuf);
    exit(1);
  }

  gcg_encoding_t gcg_encoding = get_encoding_or_first_line(input_gcg);
  char *gcg;
  if (gcg_encoding != UTF8) {
    // Convert gcg to UTF8
    gcg = (char *)malloc(sizeof(char) * 2 * strlen(gcg));
  } else {
    gcg = input_gcg;
  }

  // Do the stuff

  if (gcg_encoding != UTF8) {
    // Convert gcg to UTF8
    free(gcg);
  }
}