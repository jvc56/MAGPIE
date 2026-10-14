#include "gcg_tokenizer_diff_test.h"

#include "../src/compat/cdir.h"
#include "../src/compat/cfile.h"
#include "../src/impl/gcg_tokenizer.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "gcg_regex.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Differential test for the GCG tokenizer: runs it and the POSIX regular
// expressions it replaced on the same lines, and requires the same token
// kind and the same captures from both. The regular expressions below are
// the ones src/impl/gcg.c used, verbatim. Windows has no <regex.h>, so there
// the test has nothing to compare against and does nothing.

#ifdef GCG_REGEX_AVAILABLE

enum {
  GCG_DIFF_MAX_REPORTED_MISMATCHES = 50,
  GCG_DIFF_RANDOM_LINES = 100000,
  GCG_DIFF_MUTATED_TEMPLATE_LINES = 100000,
  GCG_DIFF_MAX_RANDOM_PIECES = 14,
  GCG_DIFF_MAX_MUTATIONS = 3,
  GCG_DIFF_MIN_GCG_FILES = 90,
};

typedef struct GCGRegexPattern {
  gcg_token_t token;
  const char *regex_string;
} GCGRegexPattern;

// The token kinds and patterns of gcg.c, in its order.
static const GCGRegexPattern gcg_regex_patterns[] = {
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
    {GCG_CGP_TOKEN, "#cgp (.+)"},
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

enum {
  NUMBER_OF_GCG_REGEX_PATTERNS =
      sizeof(gcg_regex_patterns) / sizeof(gcg_regex_patterns[0]),
};

// Hand-written lines: every token kind, well formed and malformed, with odd
// whitespace, signs, CR line endings, and non-ASCII (UTF-8 and ISO-8859-1)
// tile letters. Each is also mutated byte by byte.
static const char *const gcg_diff_edge_cases[] = {
    // Blank and junk lines
    "",
    " ",
    "  ",
    "\t",
    "\r",
    " \t\v\f\r",
    "random text",
    "#",
    "##",
    ">",
    ">:",
    "> ",
    ">: ",
    "#unknown pragma",
    "#Player1 jo Jo",
    // #player
    "#player1 jo Jo Smith",
    "#player2 cesar Cesar Del Solar",
    "#player1 jo Jo",
    "#player1 jo",
    "#player1 jo ",
    "#player1 jo  ",
    "#player1 jo   ",
    "#player1 jo \t",
    "#player1 jo\tJo\tSmith",
    "#player1\tjo\tJo",
    "#player1  jo   Jo  Smith  ",
    "#player1 jo Jo\r",
    "#player1 jo \r",
    "#player1 jo  \r",
    "#player1 jo Jo\v",
    "#player1 a\vb\fc",
    "#player3 jo Jo",
    "#player0 jo Jo",
    "#player12 jo Jo",
    "#player jo Jo",
    "#player1jo Jo Smith",
    "#player1",
    "#player1 ",
    "#player1  ",
    "#player1 jo Jo #player2 ce Ce",
    "#player1 #player2 a b",
    " #player1 jo Jo",
    "x#player1 jo Jo",
    "##player1 jo Jo",
    "#player1 \xc3\xa9t\xc3\xa9 \xc3\x89t\xc3\xa9 Smith",
    "#player1 \xe9t\xe9 \xc9t\xe9",
    "#player2 \xe4\xbd\xa0\xe5\xa5\xbd \xe4\xbd\xa0",
    // #title and #description
    "#title",
    "#title ",
    "#title   ",
    "#title My Game",
    "#title   spaced   title   ",
    "#titleNoSpace",
    "#title\tTabbed",
    "#title\r",
    "#title \xc3\xa9",
    "note #title inside",
    "#description Created with MAGPIE",
    "#description",
    "#description    ",
    "#descriptions are here",
    "#description\t\tx\r",
    // #id
    "#id io.woogles abc123",
    "#id io.woogles",
    "#id io.woogles ",
    "#id",
    "#id ",
    "#id  a  b  c",
    "#idauth uid",
    "#id\ta\tb",
    "#id a b\r",
    "#id a \r",
    "#identity crisis",
    "#id \xc3\xa9 \xc3\xa9",
    // #rack1 and #rack2
    "#rack1 ABC",
    "#rack1 AB?",
    "#rack1 ab?",
    "#rack1  ABC",
    "#rack1\tABC",
    "#rack1 ",
    "#rack1",
    "#rack1 ABC DEF",
    "#rack1 ABC\r",
    "#rack1 \r",
    "#rack2 XYZ",
    "#rack3 XYZ",
    "#rack2 \xc3\x91OL",
    "#rack2 [CH]AB",
    "#rack12 ABC",
    // #character-encoding
    "#character-encoding UTF-8",
    "#character-encoding utf8",
    "#character-encoding ISO-8859-1",
    "#character-encoding iso 8859-1",
    "#character-encoding  UTF-8",
    "#character-encoding\tUTF-8",
    "#character-encoding ",
    "#character-encoding",
    "#character-encoding UTF-8\r",
    "#character-encoding UTF-8 trailing",
    "#character-encoding \xc3\xa9",
    "#character-encoding UTF\xc3\xa9-8",
    "#character-encoding \x7f",
    "#character-encoding \x01UTF-8",
    "#character-encoding !~",
    "\xef\xbb\xbf#character-encoding UTF-8",
    // #note, #cgp, #lexicon
    "#note hello world",
    "#note ",
    "#note",
    "#note  ",
    "#note\thello",
    "#notes are here",
    "#note >jo: ABC 8D CAB +10 10",
    "#note see #title x",
    "#note see #player1 a b",
    "#note \r",
    "#note caf\xc3\xa9",
    "#note caf\xe9",
    "#cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 ABC/ 0/0 0",
    "#cgp ",
    "#cgp",
    "#cgp  x",
    "#lexicon CSW21",
    "#lexicon ",
    "#lexicon",
    "#lexiconCSW21",
    "#lexicon CSW21\r",
    // #game-type, #tile-set, #board-layout, #tile-distribution
    "#game-type classic",
    "#game-type wordsmog",
    "#game-type ",
    "#game-type",
    "#game-type\tclassic",
    "#tile-set english",
    "#tile-set ",
    "#tile-set",
    "#board-layout CrosswordGame",
    "#board-layout SuperCrosswordGame\r",
    "#board-layout ",
    "#board-layout",
    "#tile-distribution english",
    "#tile-distribution ",
    "#tile-distribution",
    // Tile placement moves
    ">jo: ABCDEFG 8D CAB +10 10",
    ">jo: ABCDEFG 8d cab +10 10",
    ">jo: ABC?EFG H8 C?B +14 24",
    ">jo: ABCDEFG 8D C.B +10 10",
    ">jo: ABCDEFG 8D CAB +10 -10",
    ">jo: ABCDEFG 8D CAB -10 10",
    ">jo: ABCDEFG 8D CAB +-10 10",
    ">jo: ABCDEFG 8D CAB 10 10",
    ">jo: ABCDEFG 8D CAB +10",
    ">jo: ABCDEFG 8D CAB +10 ",
    ">jo: ABCDEFG 8D CAB +10 +10",
    ">jo: ABCDEFG 8D CAB +10 --10",
    ">jo: ABCDEFG 8D CAB +10 -",
    ">jo: ABCDEFG 8D CAB +10 10abc",
    ">jo: ABCDEFG 8D CAB +10 10 trailing",
    ">jo: ABCDEFG 8D CAB +10 10\r",
    ">jo: ABCDEFG 8D CAB +10 10 ",
    ">jo: ABCDEFG 8D CAB +10 10 # comment",
    ">jo:  ABCDEFG   8D\tCAB \t +10\t\t10",
    ">jo:ABCDEFG 8D CAB +10 10",
    ">jo ABCDEFG 8D CAB +10 10",
    ">: ABCDEFG 8D CAB +10 10",
    "> jo: ABCDEFG 8D CAB +10 10",
    ">>jo: ABCDEFG 8D CAB +10 10",
    "x>jo: ABCDEFG 8D CAB +10 10",
    ">jo:: ABCDEFG 8D CAB +10 10",
    ">j:o: ABCDEFG 8D CAB +10 10",
    ">jo: ABCDEFG 8-D CAB +10 10",
    ">jo: ABCDEFG 8D CAB +1a 10",
    ">jo: ABCDEFG 8D CAB + 10",
    ">jo: ABCDEFG 8D CAB +10 10 >ce: A 8D A +1 1",
    ">jo: ABC 8D >ce: ABCDEFG 8D CAB +10 10",
    ">jo: ABCDEFG 8D\r CAB +10 10",
    ">jo: \xc3\x87\xc3\x91 8D \xc3\x87 +10 10",
    // A non-ASCII position: \x38 is '8', spelled so that it does not extend
    // the \xa9 escape.
    ">jo: ABC \xc3\xa9\x38 CAB +10 10",
    ">jo: \xc7\xd1 8D \xc7 +10 10",
    ">jo: ABCDEFG 8D CAB +010 0010",
    ">jo: [CH]AB 8D [CH]A +10 10",
    ">jo: L\xc2\xb7LAB 8D L\xc2\xb7LA +10 10",
    ">jo: ABCDEFG 8D CAB +10 99999999999999999999",
    ">\xc3\xa9t\xc3\xa9: ABCDEFG 8D CAB +10 10",
    ">jo: ABCDEFG 8D CAB +10 10\v",
    // Phony tiles returned
    ">jo: ABCDEFG -- -10 0",
    ">jo: ABCDEFG -- -10 -5",
    ">jo: ABCDEFG -- +10 0",
    ">jo: ABCDEFG -- 10 0",
    ">jo: ABCDEFG - -10 0",
    ">jo: ABCDEFG --- -10 0",
    ">jo: ABCDEFG -- - 0",
    ">jo: ABCDEFG -- -10",
    ">jo: ABCDEFG --  -10   0",
    ">jo: -- -10 0",
    ">jo: ABCDEFG -- -10 0\r",
    // Passes
    ">jo: ABCDEFG - +0 10",
    ">jo: ABCDEFG - +0 -10",
    ">jo: ABCDEFG - +1 10",
    ">jo: ABCDEFG - +00 10",
    ">jo: ABCDEFG - 0 10",
    ">jo: ABCDEFG -  +0  10",
    ">jo: - +0 10",
    ">jo: ABCDEFG - +0",
    ">jo: ABCDEFG - +0 x",
    ">jo: ABCDEFG - +0 10\r",
    ">jo: ABCDEFG\t-\t+0\t10",
    // Challenge bonuses
    ">jo: ABCDEFG (challenge) +5 100",
    ">jo:  (challenge) +5 100",
    ">jo:   (challenge) +5 100",
    ">jo: (challenge) +5 100",
    ">jo:\t\t(challenge) +5 100",
    ">jo: ABC (challenge) 5 100",
    ">jo: ABC (challenge) -5 100",
    ">jo: ABC (challenge) +5 -100",
    ">jo: ABC (Challenge) +5 100",
    ">jo: ABC (challenge)+5 100",
    ">jo: ABC (challenge) +5",
    ">jo: (challenge) (challenge) +5 100",
    ">jo:  (challenge) (challenge) +5 100",
    ">jo: ABC (challenge) +5 100\r",
    ">jo: \xc3\xa9 (challenge) +5 100",
    ">jo: ABC  (challenge)  +5  100",
    // Exchanges
    ">jo: ABCDEFG -ABC +0 10",
    ">jo: ABCDEFG -? +0 10",
    ">jo: ABCDEFG -abc +0 10",
    ">jo: ABCDEFG -- +0 10",
    ">jo: ABCDEFG -ABC +1 10",
    ">jo: ABCDEFG -ABC +0 -10",
    ">jo: ABCDEFG ABC +0 10",
    ">jo: ABCDEFG -7 +0 10",
    ">jo: -ABC +0 10",
    ">jo: ABCDEFG -\xc3\x91 +0 10",
    ">jo: ABCDEFG -ABC +0 10\r",
    ">jo: ABCDEFG -ABC +0",
    // End rack points
    ">jo: (ABC) +10 300",
    ">jo: (ABC) +10 -300",
    ">jo: (ABC) 10 300",
    ">jo: (ABC) -10 300",
    ">jo: () +10 300",
    ">jo: (A) +2 300",
    ">jo: (ABC +10 300",
    ">jo: ABC) +10 300",
    ">jo: ((ABC)) +10 300",
    ">jo: (A)B) +10 300",
    ">jo:  (ABC)  +10  300",
    ">jo: (abc?) +10 300",
    ">jo: ABC (ABC) +10 300",
    ">jo: (\xc3\x91) +20 300\r",
    // Time penalties
    ">jo: ABC (time) -10 300",
    ">jo:  (time) -10 300",
    ">jo: (time) -10 300",
    ">jo: ABC (time) +10 300",
    ">jo: ABC (time) 10 300",
    ">jo: ABC (time) -10 -300",
    ">jo: ABC (Time) -10 300",
    ">jo:\t \t(time)\t-10\t300",
    ">jo: ABC (time) -10",
    // End rack penalties
    ">jo: ABC (ABC) -10 300",
    ">jo: ABC (ABC) +10 300",
    ">jo: ABC (ABC) -10 -300",
    ">jo: ABC () -10 300",
    ">jo: ABC (ABC) -10",
    ">jo: ABC (ABC)-10 300",
    ">jo: ABC (ABC) - 300",
    ">jo: (ABC) (ABC) -10 300",
    ">jo: ABC (ABC) -10 300\r",
};

enum {
  NUMBER_OF_GCG_DIFF_EDGE_CASES =
      sizeof(gcg_diff_edge_cases) / sizeof(gcg_diff_edge_cases[0]),
};

// Pieces that random lines are built from and that mutations insert.
static const char *const gcg_diff_pieces[] = {
    ">",
    "#",
    ":",
    " ",
    "  ",
    "\t",
    "\r",
    "\v",
    "\f",
    "jo",
    "jo:",
    "ABC",
    "8D",
    "H8",
    "?",
    ".",
    "+",
    "-",
    "+0",
    "+10",
    "-10",
    "10",
    "0",
    "--",
    "(",
    ")",
    "(challenge)",
    "(time)",
    "(ABC)",
    "\xc3\xa9",
    "\xe9",
    "x",
    "1",
    "2",
    "UTF-8",
    "#player1",
    "#player2",
    "#title",
    "#description",
    "#id",
    "#rack1 ",
    "#rack2 ",
    "#character-encoding ",
    "#note ",
    "#cgp ",
    "#lexicon ",
    "#game-type ",
    "#tile-set ",
    "#board-layout ",
    "#tile-distribution ",
};

enum {
  NUMBER_OF_GCG_DIFF_PIECES =
      sizeof(gcg_diff_pieces) / sizeof(gcg_diff_pieces[0]),
};

// Bytes that single-byte replacements put at every position of an edge case.
static const char gcg_diff_replacement_bytes[] = " \t\r:>#-+()0?Aa\xc3";

typedef struct GCGDiffState {
  regex_t regexes[NUMBER_OF_GCG_REGEX_PATTERNS];
  uint64_t random_state;
  int64_t lines_compared;
  int64_t file_lines_compared;
  int files_compared;
  int mismatches;
  int64_t file_line_token_counts[NUMBER_OF_GCG_TOKENS];
} GCGDiffState;

static void print_escaped_line(const char *line) {
  for (const char *byte_ptr = line; *byte_ptr != '\0'; byte_ptr++) {
    const unsigned char byte = (unsigned char)*byte_ptr;
    if (byte < 0x20 || byte >= 0x7f || byte == '\\') {
      printf("\\x%02x", byte);
    } else {
      putchar(byte);
    }
  }
}

static void report_mismatch(GCGDiffState *state, const char *line,
                            const char *what, gcg_token_t token) {
  state->mismatches++;
  if (state->mismatches > GCG_DIFF_MAX_REPORTED_MISMATCHES) {
    return;
  }
  printf("gcgdiff mismatch (%s, token %d): \"", what, (int)token);
  print_escaped_line(line);
  printf("\"\n");
}

static bool captures_equal(const regmatch_t *regex_groups,
                           const GCGCapture *captures) {
  for (int group_idx = 0; group_idx < GCG_MAX_CAPTURES; group_idx++) {
    if ((int)regex_groups[group_idx].rm_so != captures[group_idx].start ||
        (int)regex_groups[group_idx].rm_eo != captures[group_idx].end) {
      return false;
    }
  }
  return true;
}

// Compares each token kind on its own, then the kind the line tokenizes as.
static gcg_token_t compare_line(GCGDiffState *state, const char *line) {
  state->lines_compared++;
  gcg_token_t first_regex_token = GCG_UNKNOWN_TOKEN;
  regmatch_t first_regex_groups[GCG_MAX_CAPTURES];
  for (int pattern_idx = 0; pattern_idx < NUMBER_OF_GCG_REGEX_PATTERNS;
       pattern_idx++) {
    const gcg_token_t token = gcg_regex_patterns[pattern_idx].token;
    regmatch_t regex_groups[GCG_MAX_CAPTURES];
    const int regexec_result = regexec(&state->regexes[pattern_idx], line,
                                       GCG_MAX_CAPTURES, regex_groups, 0);
    assert(regexec_result == 0 || regexec_result == REG_NOMATCH);
    const bool regex_matched = regexec_result == 0;
    GCGCapture captures[GCG_MAX_CAPTURES];
    const bool tokenizer_matched =
        gcg_line_matches_token(line, token, captures);
    if (regex_matched != tokenizer_matched) {
      report_mismatch(state, line,
                      regex_matched ? "only regex matches"
                                    : "only tokenizer matches",
                      token);
      continue;
    }
    if (!regex_matched) {
      continue;
    }
    if (!captures_equal(regex_groups, captures)) {
      report_mismatch(state, line, "captures differ", token);
    }
    if (first_regex_token == GCG_UNKNOWN_TOKEN) {
      first_regex_token = token;
      memcpy(first_regex_groups, regex_groups, sizeof(first_regex_groups));
    }
  }
  GCGCapture captures[GCG_MAX_CAPTURES];
  const gcg_token_t tokenizer_token = gcg_tokenize_line(line, captures);
  if (tokenizer_token != first_regex_token) {
    report_mismatch(state, line, "token kinds differ", tokenizer_token);
  } else if (tokenizer_token != GCG_UNKNOWN_TOKEN &&
             !captures_equal(first_regex_groups, captures)) {
    report_mismatch(state, line, "line captures differ", tokenizer_token);
  }
  return first_regex_token;
}

// Compares the line, its prefixes, and the line with each byte deleted, each
// space turned into a tab or doubled, and with bytes added at either end.
static void compare_line_and_mutations(GCGDiffState *state, const char *line,
                                       bool replace_each_byte) {
  const size_t length = strlen(line);
  char *buffer = malloc_or_die(length + 3);
  for (size_t prefix_length = 0; prefix_length <= length; prefix_length++) {
    memcpy(buffer, line, prefix_length);
    buffer[prefix_length] = '\0';
    compare_line(state, buffer);
  }
  for (size_t byte_idx = 0; byte_idx < length; byte_idx++) {
    memcpy(buffer, line, byte_idx);
    memcpy(buffer + byte_idx, line + byte_idx + 1, length - byte_idx);
    compare_line(state, buffer);
    if (line[byte_idx] == ' ') {
      memcpy(buffer, line, length + 1);
      buffer[byte_idx] = '\t';
      compare_line(state, buffer);
      memcpy(buffer, line, byte_idx + 1);
      memcpy(buffer + byte_idx + 1, line + byte_idx, length - byte_idx + 1);
      compare_line(state, buffer);
    }
    if (replace_each_byte) {
      for (size_t replacement_idx = 0;
           replacement_idx < sizeof(gcg_diff_replacement_bytes) - 1;
           replacement_idx++) {
        memcpy(buffer, line, length + 1);
        buffer[byte_idx] = gcg_diff_replacement_bytes[replacement_idx];
        compare_line(state, buffer);
      }
    }
  }
  const char *const affixes[] = {" ", "\r", "x", ">", "#"};
  for (size_t affix_idx = 0; affix_idx < sizeof(affixes) / sizeof(affixes[0]);
       affix_idx++) {
    memcpy(buffer, line, length);
    buffer[length] = affixes[affix_idx][0];
    buffer[length + 1] = '\0';
    compare_line(state, buffer);
    buffer[0] = affixes[affix_idx][0];
    memcpy(buffer + 1, line, length + 1);
    compare_line(state, buffer);
  }
  free(buffer);
}

static void compare_text(GCGDiffState *state, const char *text) {
  StringSplitter *lines = split_string(text, '\n', false);
  const int number_of_lines = string_splitter_get_number_of_items(lines);
  for (int line_idx = 0; line_idx < number_of_lines; line_idx++) {
    const char *line = string_splitter_get_item(lines, line_idx);
    state->file_line_token_counts[compare_line(state, line)]++;
    state->file_lines_compared++;
    compare_line_and_mutations(state, line, false);
  }
  string_splitter_destroy(lines);
}

// Each line of the file as read, and as gcg.c decodes it from ISO-8859-1.
static void compare_gcg_file(GCGDiffState *state, const char *path) {
  ErrorStack *error_stack = error_stack_create();
  char *contents = get_string_from_file(path, error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  compare_text(state, contents);
  char *decoded = iso_8859_1_to_utf8(contents);
  if (!strings_equal(decoded, contents)) {
    compare_text(state, decoded);
  }
  free(decoded);
  free(contents);
  state->files_compared++;
}

static void compare_gcg_files_in_dir(GCGDiffState *state,
                                     const char *dir_path) {
  CDir *dir = cdir_open(dir_path);
  if (!dir) {
    log_fatal("gcgdiff: cannot open directory %s", dir_path);
  }
  const char *name = cdir_next(dir);
  while (name) {
    char *path = get_formatted_string("%s/%s", dir_path, name);
    if (cfile_is_directory(path)) {
      compare_gcg_files_in_dir(state, path);
    } else if (has_suffix(".gcg", name)) {
      compare_gcg_file(state, path);
    }
    free(path);
    name = cdir_next(dir);
  }
  cdir_close(dir);
}

static uint64_t next_random(GCGDiffState *state) {
  // xorshift64
  state->random_state ^= state->random_state << 13;
  state->random_state ^= state->random_state >> 7;
  state->random_state ^= state->random_state << 17;
  return state->random_state;
}

static int random_below(GCGDiffState *state, int bound) {
  return (int)(next_random(state) % (uint64_t)bound);
}

// Lines made of random pieces.
static void compare_random_lines(GCGDiffState *state) {
  StringBuilder *line_builder = string_builder_create();
  for (int line_idx = 0; line_idx < GCG_DIFF_RANDOM_LINES; line_idx++) {
    string_builder_clear(line_builder);
    const int number_of_pieces =
        1 + random_below(state, GCG_DIFF_MAX_RANDOM_PIECES);
    for (int piece_idx = 0; piece_idx < number_of_pieces; piece_idx++) {
      string_builder_add_string(
          line_builder,
          gcg_diff_pieces[random_below(state, NUMBER_OF_GCG_DIFF_PIECES)]);
    }
    compare_line(state, string_builder_peek(line_builder));
  }
  string_builder_destroy(line_builder);
}

// Edge cases with a few random edits: a piece inserted, a span of bytes
// deleted, or a span of bytes repeated.
static void compare_mutated_edge_cases(GCGDiffState *state) {
  for (int line_idx = 0; line_idx < GCG_DIFF_MUTATED_TEMPLATE_LINES;
       line_idx++) {
    char *line = string_duplicate(gcg_diff_edge_cases[random_below(
        state, NUMBER_OF_GCG_DIFF_EDGE_CASES)]);
    const int number_of_mutations =
        1 + random_below(state, GCG_DIFF_MAX_MUTATIONS);
    for (int mutation_idx = 0; mutation_idx < number_of_mutations;
         mutation_idx++) {
      const int length = (int)strlen(line);
      const int pos = random_below(state, length + 1);
      const int span = random_below(state, length - pos + 1);
      char *edited = NULL;
      switch (random_below(state, 3)) {
      case 0:
        edited = get_formatted_string(
            "%.*s%s%s", pos, line,
            gcg_diff_pieces[random_below(state, NUMBER_OF_GCG_DIFF_PIECES)],
            line + pos);
        break;
      case 1:
        edited = get_formatted_string("%.*s%s", pos, line, line + pos + span);
        break;
      default:
        edited = get_formatted_string("%.*s%s", pos + span, line, line + pos);
        break;
      }
      free(line);
      line = edited;
    }
    compare_line(state, line);
    free(line);
  }
}

void test_gcg_tokenizer_diff(void) {
  GCGDiffState *state = calloc_or_die(1, sizeof(GCGDiffState));
  state->random_state = 0x9e3779b97f4a7c15ULL;
  for (int pattern_idx = 0; pattern_idx < NUMBER_OF_GCG_REGEX_PATTERNS;
       pattern_idx++) {
    const int regcomp_result =
        regcomp(&state->regexes[pattern_idx],
                gcg_regex_patterns[pattern_idx].regex_string, REG_EXTENDED);
    assert(regcomp_result == 0);
  }

  compare_gcg_files_in_dir(state, "testdata");
  compare_gcg_files_in_dir(state, "test/fixtures");
  const int64_t file_lines_compared = state->file_lines_compared;
  const int64_t file_and_mutation_lines_compared = state->lines_compared;

  for (int case_idx = 0; case_idx < NUMBER_OF_GCG_DIFF_EDGE_CASES; case_idx++) {
    compare_line(state, gcg_diff_edge_cases[case_idx]);
    compare_line_and_mutations(state, gcg_diff_edge_cases[case_idx], true);
  }
  const int64_t edge_case_lines_compared =
      state->lines_compared - file_and_mutation_lines_compared;

  compare_random_lines(state);
  compare_mutated_edge_cases(state);

  printf("gcgdiff: %d GCG files, %lld file lines (%lld with their "
         "mutations), %d edge cases (%lld with their mutations), %d random "
         "lines, %d randomly edited edge cases: %lld lines compared, %d "
         "mismatches\n",
         state->files_compared, (long long)file_lines_compared,
         (long long)file_and_mutation_lines_compared,
         NUMBER_OF_GCG_DIFF_EDGE_CASES, (long long)edge_case_lines_compared,
         GCG_DIFF_RANDOM_LINES, GCG_DIFF_MUTATED_TEMPLATE_LINES,
         (long long)state->lines_compared, state->mismatches);
  printf("gcgdiff: file lines by token kind:");
  for (int token_idx = 0; token_idx < NUMBER_OF_GCG_TOKENS; token_idx++) {
    printf(" %d:%lld", token_idx,
           (long long)state->file_line_token_counts[token_idx]);
  }
  printf("\n");

  assert(state->files_compared >= GCG_DIFF_MIN_GCG_FILES);
  assert(state->mismatches == 0);
  for (int pattern_idx = 0; pattern_idx < NUMBER_OF_GCG_REGEX_PATTERNS;
       pattern_idx++) {
    regfree(&state->regexes[pattern_idx]);
  }
  free(state);
}

#else

void test_gcg_tokenizer_diff(void) {}

#endif
