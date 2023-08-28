#include <regex.h>
#include <stdint.h>

enum {
  GCG_PARSE_ERROR_DUPLICATE_NAMES,
  GCG_PARSE_ERROR_PRAGMA_PRECEDENT_EVENT,
  GCG_PARSE_ERROR_ENCODING_WRONG_PLACE,
  GCG_PARSE_ERROR_PLAYER_NOT_SUPPORTED,
  GCG_PARSE_ERROR_PLAYER_DOES_NOT_EXIST,
};

// A Token is an event in a GCG file.
typedef uint8_t Token;

enum {
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
  TILE_DECLARATION_TOKEN
};

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
