#include "gcg_tokenizer.h"

#include <stdbool.h>
#include <string.h>

// A GCG line is one token: a pragma such as "#player1 jo Jo Smith" or an
// event such as ">jo: ABC 8D CAB +10 10". Each kind is a literal prefix
// ("#player", ">") followed by a tail grammar, written out above each tail
// matcher below as the POSIX extended regular expression it implements.
//
// These grammars once were those regular expressions, and the tokenizer keeps
// their exact behavior in the C locale, quirks included:
//
// - Nothing is anchored. A kind matches at the leftmost position in the line
//   where its prefix starts a full match; anything may come before it, and
//   anything may follow its last field.
// - The kinds are tried in the order of gcg_token_grammars, and the first one
//   that matches anywhere wins.
// - A space is one of the six bytes ' ', \t, \n, \v, \f and \r, and a word is
//   a maximal run of other bytes, the bytes of non-ASCII characters included.
//   Digits and letters are ASCII only, and a graphic byte is one of '!' to
//   '~'.
// - '.' matches any byte, so ".*" and ".+" run to the end of the line.
// - Every field that a run of spaces follows ends where its word ends, so the
//   fields of a line are its words. Where the regular expressions could split
//   a run of spaces two ways, they gave the earlier element all the spaces it
//   could take: "[[:space:]]+(.+)" captures what follows the run, or only the
//   run's last space when the line ends in it, and an empty
//   "([^[:space:]]*)" between two runs sits before the run's last space.

typedef bool (*gcg_tail_matcher_t)(const char *line, int pos,
                                   GCGCapture *captures);

typedef struct GCGTokenGrammar {
  gcg_token_t token;
  const char *prefix;
  gcg_tail_matcher_t match_tail;
} GCGTokenGrammar;

// [[:space:]]
static bool is_space(char byte) {
  return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\v' ||
         byte == '\f' || byte == '\r';
}

// [[:digit:]]
static bool is_digit(char byte) { return byte >= '0' && byte <= '9'; }

// [[:alnum:]]
static bool is_alnum(char byte) {
  return is_digit(byte) || (byte >= 'A' && byte <= 'Z') ||
         (byte >= 'a' && byte <= 'z');
}

// [[:graph:]]
static bool is_graph(char byte) { return byte >= '!' && byte <= '~'; }

static int skip_spaces(const char *line, int pos) {
  while (is_space(line[pos])) {
    pos++;
  }
  return pos;
}

static int skip_word(const char *line, int pos) {
  while (line[pos] != '\0' && !is_space(line[pos])) {
    pos++;
  }
  return pos;
}

static void set_capture(GCGCapture *capture, int start, int end) {
  capture->start = start;
  capture->end = end;
}

static int capture_length(const GCGCapture *capture) {
  return capture->end - capture->start;
}

// A field followed by [[:space:]]+: the word at *pos, which spaces must
// follow. Moves *pos past them to the next word.
static bool take_word_then_spaces(const char *line, int *pos,
                                  GCGCapture *word) {
  const int word_end = skip_word(line, *pos);
  if (word_end == *pos || !is_space(line[word_end])) {
    return false;
  }
  set_capture(word, *pos, word_end);
  *pos = skip_spaces(line, word_end);
  return true;
}

// A last field ([^[:space:]]+): the word at pos, which may end the line.
static bool take_last_word(const char *line, int pos, GCGCapture *word) {
  const int word_end = skip_word(line, pos);
  set_capture(word, pos, word_end);
  return word_end > pos;
}

// (.*) at the end of a pattern: the rest of the line.
static void take_rest(const char *line, int pos, GCGCapture *rest) {
  set_capture(rest, pos, (int)strlen(line));
}

static bool word_equals(const char *line, const GCGCapture *word,
                        const char *literal) {
  const int literal_length = (int)strlen(literal);
  return capture_length(word) == literal_length &&
         strncmp(line + word->start, literal, literal_length) == 0;
}

static bool word_is_alnum(const char *line, const GCGCapture *word) {
  for (int byte_idx = word->start; byte_idx < word->end; byte_idx++) {
    if (!is_alnum(line[byte_idx])) {
      return false;
    }
  }
  return true;
}

// A word that is `sign` followed by one or more digits.
static bool word_is_signed_number(const char *line, const GCGCapture *word,
                                  char sign) {
  if (capture_length(word) < 2 || line[word->start] != sign) {
    return false;
  }
  for (int byte_idx = word->start + 1; byte_idx < word->end; byte_idx++) {
    if (!is_digit(line[byte_idx])) {
      return false;
    }
  }
  return true;
}

// The last field of every event, the cumulative score: (-?[[:digit:]]+), an
// optional minus and all the digits after it. Anything may follow it.
static bool take_cumulative_score(const char *line, int pos,
                                  GCGCapture *captures, int capture_idx) {
  int digits_start = pos;
  if (line[digits_start] == '-') {
    digits_start++;
  }
  int digits_end = digits_start;
  while (is_digit(line[digits_end])) {
    digits_end++;
  }
  if (digits_end == digits_start) {
    return false;
  }
  set_capture(&captures[capture_idx], pos, digits_end);
  captures[0].end = digits_end;
  return true;
}

// The start of every event after its '>': ([^[:space:]]+):[[:space:]]+.
// The colon must precede a space, so it ends the word, and the nickname is
// the rest of the word. Moves *pos to the word after the spaces.
static bool take_event_nickname(const char *line, int *pos,
                                GCGCapture *captures) {
  const int word_end = skip_word(line, *pos);
  if (word_end - *pos < 2 || line[word_end - 1] != ':' ||
      !is_space(line[word_end])) {
    return false;
  }
  set_capture(&captures[1], *pos, word_end - 1);
  *pos = skip_spaces(line, word_end);
  return true;
}

// ------------------------------- Pragmas --------------------------------

// #player: ([1-2])[[:space:]]+([^[:space:]]+)[[:space:]]+(.+)
static bool match_player_tail(const char *line, int pos, GCGCapture *captures) {
  if ((line[pos] != '1' && line[pos] != '2') || !is_space(line[pos + 1])) {
    return false;
  }
  set_capture(&captures[1], pos, pos + 1);
  pos = skip_spaces(line, pos + 1);
  if (!take_word_then_spaces(line, &pos, &captures[2])) {
    return false;
  }
  // (.+) takes the rest after the spaces, or the last space when nothing
  // follows them and [[:space:]]+ can spare one.
  const int line_length = (int)strlen(line);
  if (pos < line_length) {
    set_capture(&captures[3], pos, line_length);
  } else if (pos - captures[2].end >= 2) {
    set_capture(&captures[3], line_length - 1, line_length);
  } else {
    return false;
  }
  captures[0].end = line_length;
  return true;
}

// #title, #description: [[:space:]]*(.*)
static bool match_spaces_then_rest_tail(const char *line, int pos,
                                        GCGCapture *captures) {
  take_rest(line, skip_spaces(line, pos), &captures[1]);
  captures[0].end = captures[1].end;
  return true;
}

// #id: [[:space:]]*([^[:space:]]+)[[:space:]]+([^[:space:]]+)
static bool match_id_tail(const char *line, int pos, GCGCapture *captures) {
  pos = skip_spaces(line, pos);
  if (!take_word_then_spaces(line, &pos, &captures[1]) ||
      !take_last_word(line, pos, &captures[2])) {
    return false;
  }
  captures[0].end = captures[2].end;
  return true;
}

// "#rack1 ", "#rack2 ": ([^[:space:]]+)
static bool match_word_tail(const char *line, int pos, GCGCapture *captures) {
  if (!take_last_word(line, pos, &captures[1])) {
    return false;
  }
  captures[0].end = captures[1].end;
  return true;
}

// "#character-encoding ": ([[:graph:]]+)
static bool match_graph_tail(const char *line, int pos, GCGCapture *captures) {
  int graph_end = pos;
  while (is_graph(line[graph_end])) {
    graph_end++;
  }
  if (graph_end == pos) {
    return false;
  }
  set_capture(&captures[1], pos, graph_end);
  captures[0].end = graph_end;
  return true;
}

// "#note ", "#cgp ", "#lexicon ": (.+)
static bool match_nonempty_rest_tail(const char *line, int pos,
                                     GCGCapture *captures) {
  take_rest(line, pos, &captures[1]);
  captures[0].end = captures[1].end;
  return capture_length(&captures[1]) > 0;
}

// "#game-type ", "#tile-set ", "#board-layout ", "#tile-distribution ": (.*)
static bool match_rest_tail(const char *line, int pos, GCGCapture *captures) {
  take_rest(line, pos, &captures[1]);
  captures[0].end = captures[1].end;
  return true;
}

// -------------------------------- Events --------------------------------
//
// Each event starts with '>', the nickname and a colon (take_event_nickname)
// and ends with the cumulative score (take_cumulative_score). The words
// between them are the event's fields.

// Tile placement move:
// >nickname: rack position tiles +score cumulative
// >([^[:space:]]+):[[:space:]]+([^[:space:]]+)[[:space:]]+([[:alnum:]]+)
// [[:space:]]+([^[:space:]]+)[[:space:]]+[+]([[:digit:]]+)[[:space:]]+
// (-?[[:digit:]]+)
static bool match_move_tail(const char *line, int pos, GCGCapture *captures) {
  GCGCapture score;
  if (!take_event_nickname(line, &pos, captures) ||
      !take_word_then_spaces(line, &pos, &captures[2]) ||
      !take_word_then_spaces(line, &pos, &captures[3]) ||
      !word_is_alnum(line, &captures[3]) ||
      !take_word_then_spaces(line, &pos, &captures[4]) ||
      !take_word_then_spaces(line, &pos, &score) ||
      !word_is_signed_number(line, &score, '+')) {
    return false;
  }
  // The score capture leaves out the '+'.
  set_capture(&captures[5], score.start + 1, score.end);
  return take_cumulative_score(line, pos, captures, 6);
}

// Phony tiles returned:
// >nickname: rack -- -score cumulative
// >([^[:space:]]+):[[:space:]]+([^[:space:]]+)[[:space:]]+--[[:space:]]+
// -([[:digit:]]+)[[:space:]]+(-?[[:digit:]]+)
static bool match_phony_tiles_returned_tail(const char *line, int pos,
                                            GCGCapture *captures) {
  GCGCapture dashes;
  GCGCapture score;
  if (!take_event_nickname(line, &pos, captures) ||
      !take_word_then_spaces(line, &pos, &captures[2]) ||
      !take_word_then_spaces(line, &pos, &dashes) ||
      !word_equals(line, &dashes, "--") ||
      !take_word_then_spaces(line, &pos, &score) ||
      !word_is_signed_number(line, &score, '-')) {
    return false;
  }
  // The score capture leaves out the '-'.
  set_capture(&captures[3], score.start + 1, score.end);
  return take_cumulative_score(line, pos, captures, 4);
}

// Pass:
// >nickname: rack - +0 cumulative
// >([^[:space:]]+):[[:space:]]+([^[:space:]]+)[[:space:]]+-[[:space:]]+
// \+0[[:space:]]+(-?[[:digit:]]+)
static bool match_pass_tail(const char *line, int pos, GCGCapture *captures) {
  GCGCapture dash;
  GCGCapture zero;
  if (!take_event_nickname(line, &pos, captures) ||
      !take_word_then_spaces(line, &pos, &captures[2]) ||
      !take_word_then_spaces(line, &pos, &dash) ||
      !word_equals(line, &dash, "-") ||
      !take_word_then_spaces(line, &pos, &zero) ||
      !word_equals(line, &zero, "+0")) {
    return false;
  }
  return take_cumulative_score(line, pos, captures, 3);
}

// Exchange:
// >nickname: rack -tiles +0 cumulative
// >([^[:space:]]+):[[:space:]]+([^[:space:]]+)[[:space:]]+-([^[:space:]]+)
// [[:space:]]+\+0[[:space:]]+(-?[[:digit:]]+)
static bool match_exchange_tail(const char *line, int pos,
                                GCGCapture *captures) {
  GCGCapture exchanged;
  GCGCapture zero;
  if (!take_event_nickname(line, &pos, captures) ||
      !take_word_then_spaces(line, &pos, &captures[2]) ||
      !take_word_then_spaces(line, &pos, &exchanged) ||
      capture_length(&exchanged) < 2 || line[exchanged.start] != '-' ||
      !take_word_then_spaces(line, &pos, &zero) ||
      !word_equals(line, &zero, "+0")) {
    return false;
  }
  // The exchanged tiles capture leaves out the '-'.
  set_capture(&captures[3], exchanged.start + 1, exchanged.end);
  return take_cumulative_score(line, pos, captures, 4);
}

// A word that is '(' + one or more bytes + ')'. Captures what is inside.
static bool take_parenthesized_word(const char *line, int *pos,
                                    GCGCapture *inside) {
  GCGCapture word;
  if (!take_word_then_spaces(line, pos, &word) || capture_length(&word) < 3 ||
      line[word.start] != '(' || line[word.end - 1] != ')') {
    return false;
  }
  set_capture(inside, word.start + 1, word.end - 1);
  return true;
}

// End rack points:
// >nickname: (rack) +points cumulative
// >([^[:space:]]+):[[:space:]]+\(([^[:space:]]+)\)[[:space:]]+
// (\+[[:digit:]]+)[[:space:]]+(-?[[:digit:]]+)
static bool match_end_rack_points_tail(const char *line, int pos,
                                       GCGCapture *captures) {
  if (!take_event_nickname(line, &pos, captures) ||
      !take_parenthesized_word(line, &pos, &captures[2]) ||
      !take_word_then_spaces(line, &pos, &captures[3]) ||
      !word_is_signed_number(line, &captures[3], '+')) {
    return false;
  }
  return take_cumulative_score(line, pos, captures, 4);
}

// End rack penalty:
// >nickname: rack (rack) -penalty cumulative
// >([^[:space:]]+):[[:space:]]+([^[:space:]]+)[[:space:]]+
// \(([^[:space:]]+)\)[[:space:]]+(\-[[:digit:]]+)[[:space:]]+
// (-?[[:digit:]]+)
static bool match_end_rack_penalty_tail(const char *line, int pos,
                                        GCGCapture *captures) {
  if (!take_event_nickname(line, &pos, captures) ||
      !take_word_then_spaces(line, &pos, &captures[2]) ||
      !take_parenthesized_word(line, &pos, &captures[3]) ||
      !take_word_then_spaces(line, &pos, &captures[4]) ||
      !word_is_signed_number(line, &captures[4], '-')) {
    return false;
  }
  return take_cumulative_score(line, pos, captures, 5);
}

// The fields after the rack of a challenge bonus or time penalty:
// keyword (sign digits) cumulative.
static bool match_score_adjustment_fields(const char *line, int pos,
                                          GCGCapture *captures,
                                          const char *keyword, char sign) {
  GCGCapture keyword_word;
  if (!take_word_then_spaces(line, &pos, &keyword_word) ||
      !word_equals(line, &keyword_word, keyword) ||
      !take_word_then_spaces(line, &pos, &captures[3]) ||
      !word_is_signed_number(line, &captures[3], sign)) {
    return false;
  }
  return take_cumulative_score(line, pos, captures, 4);
}

// An event whose rack, ([^[:space:]]*), may be empty. The rack is either the
// first word after the colon, or empty, which needs two spaces after the
// colon, one for each [[:space:]]+ around it; the empty capture then sits
// before the last of those spaces. At most one of the two can match, since
// the keyword cannot also be the signed number that follows it.
static bool match_score_adjustment_tail(const char *line, int pos,
                                        GCGCapture *captures,
                                        const char *keyword, char sign) {
  if (!take_event_nickname(line, &pos, captures)) {
    return false;
  }
  int after_rack = pos;
  if (take_word_then_spaces(line, &after_rack, &captures[2]) &&
      match_score_adjustment_fields(line, after_rack, captures, keyword,
                                    sign)) {
    return true;
  }
  const int colon_end = captures[1].end + 1;
  if (pos - colon_end < 2) {
    return false;
  }
  set_capture(&captures[2], pos - 1, pos - 1);
  return match_score_adjustment_fields(line, pos, captures, keyword, sign);
}

// Challenge bonus:
// >nickname: rack (challenge) +bonus cumulative
// >([^[:space:]]+):[[:space:]]+([^[:space:]]*)[[:space:]]+\(challenge\)
// [[:space:]]+(\+[[:digit:]]+)[[:space:]]+(-?[[:digit:]]+)
static bool match_challenge_bonus_tail(const char *line, int pos,
                                       GCGCapture *captures) {
  return match_score_adjustment_tail(line, pos, captures, "(challenge)", '+');
}

// Time penalty:
// >nickname: rack (time) -penalty cumulative
// >([^[:space:]]+):[[:space:]]+([^[:space:]]*)[[:space:]]+\(time\)
// [[:space:]]+(\-[[:digit:]]+)[[:space:]]+(-?[[:digit:]]+)
static bool match_time_penalty_tail(const char *line, int pos,
                                    GCGCapture *captures) {
  return match_score_adjustment_tail(line, pos, captures, "(time)", '-');
}

// In priority order.
static const GCGTokenGrammar gcg_token_grammars[] = {
    {GCG_PLAYER_TOKEN, "#" GCG_PLAYER_STRING, match_player_tail},
    {GCG_TITLE_TOKEN, "#" GCG_TITLE_STRING, match_spaces_then_rest_tail},
    {GCG_DESCRIPTION_TOKEN, "#" GCG_DESCRIPTION_STRING,
     match_spaces_then_rest_tail},
    {GCG_ID_TOKEN, "#" GCG_ID_STRING, match_id_tail},
    {GCG_RACK1_TOKEN, "#" GCG_RACK_STRING "1 ", match_word_tail},
    {GCG_RACK2_TOKEN, "#" GCG_RACK_STRING "2 ", match_word_tail},
    {GCG_ENCODING_TOKEN, "#" GCG_CHAR_ENCODING_STRING " ", match_graph_tail},
    {GCG_MOVE_TOKEN, ">", match_move_tail},
    {GCG_NOTE_TOKEN, "#" GCG_NOTE_STRING " ", match_nonempty_rest_tail},
    {GCG_CGP_TOKEN, "#" GCG_CGP_STRING " ", match_nonempty_rest_tail},
    {GCG_LEXICON_TOKEN, "#" GCG_LEXICON_STRING " ", match_nonempty_rest_tail},
    {GCG_PHONY_TILES_RETURNED_TOKEN, ">", match_phony_tiles_returned_tail},
    {GCG_PASS_TOKEN, ">", match_pass_tail},
    {GCG_CHALLENGE_BONUS_TOKEN, ">", match_challenge_bonus_tail},
    {GCG_EXCHANGE_TOKEN, ">", match_exchange_tail},
    {GCG_END_RACK_POINTS_TOKEN, ">", match_end_rack_points_tail},
    {GCG_TIME_PENALTY_TOKEN, ">", match_time_penalty_tail},
    {GCG_END_RACK_PENALTY_TOKEN, ">", match_end_rack_penalty_tail},
    {GCG_GAME_TYPE_TOKEN, "#" GCG_GAME_TYPE_STRING " ", match_rest_tail},
    {GCG_TILE_SET_TOKEN, "#" GCG_TILE_SET_STRING " ", match_rest_tail},
    {GCG_BOARD_LAYOUT_TOKEN, "#" GCG_BOARD_LAYOUT_STRING " ", match_rest_tail},
    {GCG_TILE_DISTRIBUTION_NAME_TOKEN, "#" GCG_TILE_DISTRIBUTION_STRING " ",
     match_rest_tail},
};

enum {
  NUMBER_OF_GCG_TOKEN_GRAMMARS =
      sizeof(gcg_token_grammars) / sizeof(gcg_token_grammars[0]),
};

// Tries the grammar at each occurrence of its prefix, leftmost first.
static bool line_matches_grammar(const char *line,
                                 const GCGTokenGrammar *grammar,
                                 GCGCapture captures[GCG_MAX_CAPTURES]) {
  const size_t prefix_length = strlen(grammar->prefix);
  for (int start = 0; line[start] != '\0'; start++) {
    if (strncmp(line + start, grammar->prefix, prefix_length) != 0) {
      continue;
    }
    for (int capture_idx = 0; capture_idx < GCG_MAX_CAPTURES; capture_idx++) {
      set_capture(&captures[capture_idx], -1, -1);
    }
    captures[0].start = start;
    if (grammar->match_tail(line, start + (int)prefix_length, captures)) {
      return true;
    }
  }
  return false;
}

bool gcg_line_matches_token(const char *line, gcg_token_t token,
                            GCGCapture captures[GCG_MAX_CAPTURES]) {
  for (int grammar_idx = 0; grammar_idx < NUMBER_OF_GCG_TOKEN_GRAMMARS;
       grammar_idx++) {
    if (gcg_token_grammars[grammar_idx].token == token) {
      return line_matches_grammar(line, &gcg_token_grammars[grammar_idx],
                                  captures);
    }
  }
  return false;
}

gcg_token_t gcg_tokenize_line(const char *line,
                              GCGCapture captures[GCG_MAX_CAPTURES]) {
  for (int grammar_idx = 0; grammar_idx < NUMBER_OF_GCG_TOKEN_GRAMMARS;
       grammar_idx++) {
    if (line_matches_grammar(line, &gcg_token_grammars[grammar_idx],
                             captures)) {
      return gcg_token_grammars[grammar_idx].token;
    }
  }
  return GCG_UNKNOWN_TOKEN;
}
