#include "pat_test.h"

#include "../src/def/board_defs.h"
#include "../src/def/cross_set_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/pat_defs.h"
#include "../src/def/players_data_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/bonus_square.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/kwg.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/pat.h"
#include "../src/ent/player.h"
#include "../src/ent/players_data.h"
#include "../src/ent/rack.h"
#include "../src/ent/static_eval.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Creates a temporary data directory with a strategy/ subdirectory and
// returns the path (owned by the caller).
static char *create_temp_pat_data_dir(void) {
  char tmp_template[] = "/tmp/magpie_pat_XXXXXX";
  const char *tmp_dir = mkdtemp(tmp_template);
  assert(tmp_dir);
  char *strategy_dir = get_formatted_string("%s/strategy", tmp_dir);
  assert(mkdir(strategy_dir, 0755) == 0);
  free(strategy_dir);
  return string_duplicate(tmp_dir);
}

// A zeroed PATWeights with its lexicon tables prepared from the game's
// data, ready for evaluation (an unprepared model is refused; see
// PATWeights.prepared).
static PATWeights *pat_test_create_prepared(const char *name,
                                            const Game *game) {
  PATWeights *pat = pat_create_zeroed(name);
  pat_prepare_hook_flex(pat, player_get_kwg(game_get_player(game, 0)),
                        game_get_ld(game));
  return pat;
}

static void write_pat_file_contents(const char *data_dir, const char *pat_name,
                                    const char *contents) {
  ErrorStack *error_stack = error_stack_create();
  char *filename =
      get_formatted_string("%s/strategy/%s.pat", data_dir, pat_name);
  write_string_to_file(filename, "w", contents, error_stack);
  assert(error_stack_is_empty(error_stack));
  free(filename);
  error_stack_destroy(error_stack);
}

static void current_pat_header(char *buf, size_t buf_size) {
  snprintf(buf, buf_size, "%s%d", PAT_MAGIC_PREFIX, PAT_VERSION);
}

static void assert_pat_create_fails(const char *data_dir, const char *pat_name,
                                    const char *contents) {
  write_pat_file_contents(data_dir, pat_name, contents);
  ErrorStack *error_stack = error_stack_create();
  const PATWeights *pat = pat_create(data_dir, pat_name, error_stack);
  assert(!pat);
  assert(!error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
}

static void test_pat_feature_names(void) {
  char name_buffer[64];
  pat_feature_name(PAT_FEATURE_HOOK_START, name_buffer, sizeof(name_buffer));
  assert(strings_equal(name_buffer, "hook_d1"));
  pat_feature_name(PAT_FEATURE_FLOAT_FLEX_START, name_buffer,
                   sizeof(name_buffer));
  assert(strings_equal(name_buffer, "float_flex_d1"));
  pat_feature_name(PAT_FEATURE_FLOAT_SCORE_START + 1, name_buffer,
                   sizeof(name_buffer));
  assert(strings_equal(name_buffer, "float_score_d2"));
  pat_feature_name(PAT_FEATURE_TT_FLOATER, name_buffer, sizeof(name_buffer));
  assert(strings_equal(name_buffer, "tt_floater"));
  pat_feature_name(PAT_FEATURE_TT_HOOK_ONLY, name_buffer, sizeof(name_buffer));
  assert(strings_equal(name_buffer, "tt_hook_only"));
  pat_feature_name(PAT_FEATURE_DLS_HOOK_START, name_buffer,
                   sizeof(name_buffer));
  assert(strings_equal(name_buffer, "dls_hook_d1"));
  pat_feature_name(PAT_FEATURE_DLS_FLOAT_SCORE_START + 1, name_buffer,
                   sizeof(name_buffer));
  assert(strings_equal(name_buffer, "dls_float_score_d2"));
}

// A version 1 file predates the double letter square channels: it has no
// rows for them at all, not zero-valued ones. Confirms they read back as
// zero and every other feature still round-trips through the gap.
static void test_pat_version1_has_no_dls(const char *data_dir) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_string(sb, "magpie_pat_v1\n");
  char feature_name[64];
  for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
       feature_index++) {
    if ((feature_index >= PAT_FEATURE_DLS_HOOK_START &&
         feature_index < PAT_FEATURE_QWS_HOOK_START) ||
        (feature_index >= PAT_FEATURE_HOOK_SCALED_START &&
         feature_index < PAT_FEATURE_DWS_HOOK_START)) {
      continue;
    }
    pat_feature_name(feature_index, feature_name, sizeof(feature_name));
    string_builder_add_formatted_string(sb, "%s,%d\n", feature_name,
                                        -(feature_index + 1));
  }
  write_pat_file_contents(data_dir, "v1_no_dls", string_builder_peek(sb));
  string_builder_destroy(sb);

  ErrorStack *error_stack = error_stack_create();
  PATWeights *loaded = pat_create(data_dir, "v1_no_dls", error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(loaded);
  for (int feature_index = PAT_FEATURE_DLS_HOOK_START;
       feature_index < PAT_FEATURE_QWS_HOOK_START; feature_index++) {
    assert(pat_get_weight(loaded, feature_index) == 0);
  }
  for (int feature_index = PAT_FEATURE_HOOK_SCALED_START;
       feature_index < PAT_FEATURE_DWS_HOOK_START; feature_index++) {
    assert(pat_get_weight(loaded, feature_index) == 0);
  }
  assert(pat_get_weight(loaded, PAT_FEATURE_HOOK_START) == -1);
  assert(pat_get_weight(loaded, PAT_FEATURE_QWS_HOOK_START) ==
         -(PAT_FEATURE_QWS_HOOK_START + 1));
  error_stack_destroy(error_stack);
  pat_destroy(loaded);
}

// A version 2 file predates the hypergeometric-scaled channels: it has DLS
// rows (added in version 2) but no rows for the scaled channels added in
// version 3. Confirms they read back as zero and every other feature still
// round-trips through the gap.
static void test_pat_version2_has_no_scaled_channels(const char *data_dir) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_string(sb, "magpie_pat_v2\n");
  char feature_name[64];
  for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
       feature_index++) {
    if (feature_index >= PAT_FEATURE_HOOK_SCALED_START &&
        feature_index < PAT_FEATURE_DWS_HOOK_START) {
      continue;
    }
    pat_feature_name(feature_index, feature_name, sizeof(feature_name));
    string_builder_add_formatted_string(sb, "%s,%d\n", feature_name,
                                        -(feature_index + 1));
  }
  write_pat_file_contents(data_dir, "v2_no_scaled", string_builder_peek(sb));
  string_builder_destroy(sb);

  ErrorStack *error_stack = error_stack_create();
  PATWeights *loaded = pat_create(data_dir, "v2_no_scaled", error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(loaded);
  for (int feature_index = PAT_FEATURE_HOOK_SCALED_START;
       feature_index < PAT_FEATURE_DWS_HOOK_START; feature_index++) {
    assert(pat_get_weight(loaded, feature_index) == 0);
  }
  assert(pat_get_weight(loaded, PAT_FEATURE_HOOK_START) == -1);
  assert(pat_get_weight(loaded, PAT_FEATURE_DLS_HOOK_START) ==
         -(PAT_FEATURE_DLS_HOOK_START + 1));
  assert(pat_get_weight(loaded, PAT_FEATURE_DWS_HOOK_START) ==
         -(PAT_FEATURE_DWS_HOOK_START + 1));
  error_stack_destroy(error_stack);
  pat_destroy(loaded);
}

static void test_pat_round_trip(const char *data_dir) {
  PATWeights *pat = pat_create_zeroed("round_trip");
  assert(strings_equal(pat_get_name(pat), "round_trip"));
  assert(pat_get_mutation_counter(pat) == 0);
  for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
       feature_index++) {
    assert(pat_get_weight(pat, feature_index) == 0);
    pat_set_weight(pat, feature_index, -100 * (feature_index + 1));
  }
  pat_bump_mutation_counter(pat);
  assert(pat_get_mutation_counter(pat) == 1);

  ErrorStack *error_stack = error_stack_create();
  pat_write(pat, data_dir, "round_trip", error_stack);
  assert(error_stack_is_empty(error_stack));

  PATWeights *loaded = pat_create(data_dir, "round_trip", error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(loaded);
  assert(strings_equal(pat_get_name(loaded), "round_trip"));
  for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
       feature_index++) {
    assert(pat_get_weight(loaded, feature_index) ==
           pat_get_weight(pat, feature_index));
  }
  error_stack_destroy(error_stack);
  pat_destroy(loaded);
  pat_destroy(pat);
}

static void test_pat_invalid_files(const char *data_dir) {
  char header[32];
  current_pat_header(header, sizeof(header));
  // Malformed header: no version suffix at all
  assert_pat_create_fails(data_dir, "no_version", "magpie_pat_v\nhook_d2,-1\n");
  // Malformed header: non-numeric version suffix
  assert_pat_create_fails(data_dir, "junk_version",
                          "magpie_pat_vfoo\nhook_d2,-1\n");
  // Unsupported (too old) version
  assert_pat_create_fails(data_dir, "old_version",
                          "magpie_pat_v0\nhook_d2,-1\n");
  // Positive weight
  char *contents = get_formatted_string("%s\nhook_d1,1\n", header);
  assert_pat_create_fails(data_dir, "positive_weight", contents);
  free(contents);
  // Missing comma
  contents = get_formatted_string("%s\nhook_d1 -1\n", header);
  assert_pat_create_fails(data_dir, "missing_comma", contents);
  free(contents);
  // Wrong feature name
  contents = get_formatted_string("%s\nnot_a_feature,-1\n", header);
  assert_pat_create_fails(data_dir, "wrong_name", contents);
  free(contents);
  // Too few rows
  contents = get_formatted_string("%s\nhook_d1,-1\n# a comment\n\n", header);
  assert_pat_create_fails(data_dir, "too_few_rows", contents);
  free(contents);
}

static void test_pat_comments_and_blank_lines(const char *data_dir) {
  char header[32];
  current_pat_header(header, sizeof(header));
  StringBuilder *sb = string_builder_create();
  string_builder_add_formatted_string(sb, "%s\n# leading comment\n\n", header);
  char feature_name[64];
  for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
       feature_index++) {
    pat_feature_name(feature_index, feature_name, sizeof(feature_name));
    string_builder_add_formatted_string(sb, "%s,%d\n# comment %d\n",
                                        feature_name, -feature_index,
                                        feature_index);
  }
  write_pat_file_contents(data_dir, "commented", string_builder_peek(sb));
  string_builder_destroy(sb);

  ErrorStack *error_stack = error_stack_create();
  PATWeights *loaded = pat_create(data_dir, "commented", error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(loaded);
  for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
       feature_index++) {
    assert(pat_get_weight(loaded, feature_index) == -feature_index);
  }
  error_stack_destroy(error_stack);
  pat_destroy(loaded);
}

// A board with the word TREE reading down column F (rows 12-15), so the
// final E sits on row 15 (the bottom TWS row) as a floater between the TWS
// at 15A and 15H, enabling a triple-triple through it.
#define PAT_FLOATER_CGP_CMD                                                    \
  "cgp 15/15/15/15/15/15/15/15/15/15/15/5T9/5R9/5E9/5E9 AB/CD 0/0 0"

static void set_single_tile_move(Move *move, MachineLetter ml, int row,
                                 int col) {
  MachineLetter strip[1];
  strip[0] = ml;
  move_set_all_except_equity(move, strip, 0, 0, 0, row, col, 1,
                             BOARD_HORIZONTAL_DIRECTION,
                             GAME_EVENT_TILE_PLACEMENT_MOVE);
}

static void test_pat_extract_features_floater_board(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 15");
  load_and_exec_config_or_die(config, PAT_FLOATER_CGP_CMD);
  const Game *game = config_get_game(config);
  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  const Square *lanes = board_get_readonly_lanes(board, 0);

  int32_t features[PAT_NUM_FEATURES];
  pat_extract_features(lanes, ld, NULL, NULL, RACK_SIZE, features);

  // The floater E at (14,5) is two empties from the TWS at (14,7) and five
  // empties from the TWS at (14,0); E scores one point.
  assert(features[PAT_FEATURE_FLOAT_SCORE_START + 1] == 1);
  assert(features[PAT_FEATURE_FLOAT_SCORE_START + 4] == 1);
  const int32_t float_flex_d2 = features[PAT_FEATURE_FLOAT_FLEX_START + 1];
  const int32_t float_flex_d5 = features[PAT_FEATURE_FLOAT_FLEX_START + 4];
  assert(float_flex_d2 > 0);
  assert(float_flex_d5 > 0);
  // The triple-triple span (14,0)-(14,7) is counted from both endpoint TWS
  // squares, each contributing 1 plus its scan's floater flex.
  assert(features[PAT_FEATURE_TT_FLOATER] == 2 + float_flex_d2 + float_flex_d5);
  assert(features[PAT_FEATURE_TT_HOOK_ONLY] == 0);
  // No empty TWS-lane square is perpendicular-adjacent to a tile, so there
  // is no hook access anywhere.
  for (int bin = 0; bin < PAT_HOOK_BIN_COUNT; bin++) {
    assert(features[PAT_FEATURE_HOOK_START + bin] == 0);
  }
  // No other floater bins are populated.
  for (int bin = 0; bin < PAT_FLOATER_BIN_COUNT; bin++) {
    if (bin == 1 || bin == 4) {
      continue;
    }
    assert(features[PAT_FEATURE_FLOAT_FLEX_START + bin] == 0);
    assert(features[PAT_FEATURE_FLOAT_SCORE_START + bin] == 0);
  }
  config_destroy(config);
}

// A route needing more fresh tiles than the opponent currently holds cannot
// be played regardless of which letters would complete it, so the scan's
// reach must cap at the opponent's real rack size, not always at RACK_SIZE.
// Reuses the floater board from test_pat_extract_features_floater_board:
// the E floater is 2 empties from the near TWS and 5 from the far one.
// NARCEIN across row 8 from D8: nothing precedes NARCEIN in CSW21, so the
// floater route from the A8 triple (three tiles away) is dead under the
// real extension set; NARCEINE and NARCEINS exist, so the route from O8
// (five tiles away) admits exactly E and S; the two vertical routes
// through the E (seven tiles from H1 and H15) admit every letter. Under
// the legacy semantics every one of those runs counts all 91 unseen
// non-blank tiles (100 minus NARCEIN's 7 minus 2 blanks; no rack given).
static void test_pat_lexicon_floaters(const char *data_dir) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 15");
  load_and_exec_config_or_die(
      config,
      "cgp 15/15/15/15/15/15/15/3NARCEIN5/15/15/15/15/15/15/15 / 0/0 0");
  const Game *game = config_get_game(config);
  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  const Square *lanes = board_get_readonly_lanes(board, 0);
  const int unseen_nonblank = 100 - 7 - 2;
  const int unseen_e_and_s = 11 + 4;

  PATWeights *pat = pat_create_zeroed("lexicon_floaters");
  assert(!pat_get_lexicon_floaters(pat));
  int32_t legacy_features[PAT_NUM_FEATURES];
  pat_extract_features(lanes, ld, NULL, pat, RACK_SIZE, legacy_features);
  int32_t null_pat_features[PAT_NUM_FEATURES];
  pat_extract_features(lanes, ld, NULL, NULL, RACK_SIZE, null_pat_features);
  assert(legacy_features[PAT_FEATURE_FLOAT_FLEX_START + 2] == unseen_nonblank);
  assert(legacy_features[PAT_FEATURE_FLOAT_FLEX_START + 4] == unseen_nonblank);
  assert(legacy_features[PAT_FEATURE_FLOAT_FLEX_START + 6] ==
         2 * unseen_nonblank);
  for (int bin = 0; bin < PAT_HOOK_BIN_COUNT; bin++) {
    assert(null_pat_features[PAT_FEATURE_FLOAT_FLEX_START + bin] ==
           legacy_features[PAT_FEATURE_FLOAT_FLEX_START + bin]);
  }

  pat_set_lexicon_floaters(pat, true);
  int32_t lexicon_features[PAT_NUM_FEATURES];
  pat_extract_features(lanes, ld, NULL, pat, RACK_SIZE, lexicon_features);
  assert(lexicon_features[PAT_FEATURE_FLOAT_FLEX_START + 2] == 0);
  assert(lexicon_features[PAT_FEATURE_FLOAT_FLEX_START + 4] == unseen_e_and_s);
  assert(lexicon_features[PAT_FEATURE_FLOAT_FLEX_START + 6] ==
         2 * unseen_nonblank);
  // Only the floater flexibility channels (and their scaled variants)
  // read the extension set, and the premium-combination channels count a
  // floater route only when that flexibility is nonzero; everything else
  // is identical.
  for (int feature_index = 0; feature_index < PAT_FEATURE_LM_SPAN_START;
       feature_index++) {
    const bool is_float_flex =
        (feature_index >= PAT_FEATURE_FLOAT_FLEX_START &&
         feature_index < PAT_FEATURE_FLOAT_FLEX_START + PAT_HOOK_BIN_COUNT) ||
        (feature_index >= PAT_FEATURE_FLOAT_FLEX_SCALED_START &&
         feature_index <
             PAT_FEATURE_FLOAT_FLEX_SCALED_START + PAT_HOOK_BIN_COUNT);
    if (!is_float_flex) {
      assert(lexicon_features[feature_index] == legacy_features[feature_index]);
    }
  }

  // The flag round-trips through the file, and a file without the row
  // reads as legacy.
  ErrorStack *error_stack = error_stack_create();
  pat_write(pat, data_dir, "lexicon_floaters", error_stack);
  assert(error_stack_is_empty(error_stack));
  PATWeights *loaded = pat_create(data_dir, "lexicon_floaters", error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(pat_get_lexicon_floaters(loaded));
  pat_destroy(loaded);
  error_stack_destroy(error_stack);
  char header[32];
  current_pat_header(header, sizeof(header));
  char *contents =
      get_formatted_string("%s\nlexicon_floaters,2\nhook_d1,-1\n", header);
  assert_pat_create_fails(data_dir, "lexicon_floaters_bad", contents);
  free(contents);
  pat_destroy(pat);
  config_destroy(config);
}

// A lone J three empties below the H1 triple (H4): the only route to H1
// is a four-tile play running H1 -> H4, so the J is that word's LAST
// letter and almost nothing four letters long ends in J (HADJ, HAJJ).
// A lone J three empties above the H15 triple (H12): the word runs H12 ->
// H15 and starts with J, which hundreds of four-letter words do. The
// unsigned tables pool both ends, so they call the two boards the same
// threat; the signed tables do not. Every other channel is identical
// between the two semantics.
static void test_pat_signed_through(const char *data_dir) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 15");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/7J7/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  const Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  const KWG *kwg = player_get_kwg(game_get_player(game, 0));
  PATWeights *pat = pat_create_zeroed("signed_through");
  pat_prepare_hook_flex(pat, kwg, ld);
  const MachineLetter j = ld_hl_to_ml(ld, "J");
  const MachineLetter y = ld_hl_to_ml(ld, "Y");
  for (int span = 4; span <= 8; span += 4) {
    printf("through_count (8*log2(1+words)) span %d: J first %d last %d "
           "pooled %d; Y first %d last %d pooled %d\n",
           span, pat_get_through_count_end(pat, 0, j, span),
           pat_get_through_count_end(pat, 1, j, span),
           pat_get_through_count(pat, j, span),
           pat_get_through_count_end(pat, 0, y, span),
           pat_get_through_count_end(pat, 1, y, span),
           pat_get_through_count(pat, y, span));
  }
  // Table sanity: the unsigned entry pools both ends.
  assert(pat_get_through_count_end(pat, 0, j, 4) >
         pat_get_through_count_end(pat, 1, j, 4));
  assert(pat_get_through_count(pat, j, 4) >=
         pat_get_through_count_end(pat, 0, j, 4));

  int32_t unsigned_below[PAT_NUM_FEATURES];
  int32_t signed_below[PAT_NUM_FEATURES];
  assert(!pat_get_signed_through(pat));
  pat_extract_features(board_get_readonly_lanes(game_get_board(game), 0), ld,
                       NULL, pat, RACK_SIZE, unsigned_below);
  pat_set_signed_through(pat, true);
  pat_extract_features(board_get_readonly_lanes(game_get_board(game), 0), ld,
                       NULL, pat, RACK_SIZE, signed_below);
  const int through_count_d3 = PAT_FEATURE_FLOAT_THROUGH_COUNT_START + 2;
  const int through_score_d3 = PAT_FEATURE_FLOAT_THROUGH_SCORE_START + 2;
  assert(unsigned_below[through_count_d3] > 0);
  assert(signed_below[through_count_d3] ==
         pat_get_through_count_end(pat, 1, j, 4));
  assert(unsigned_below[through_count_d3] == pat_get_through_count(pat, j, 4));
  assert(signed_below[through_count_d3] < unsigned_below[through_count_d3]);
  for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
       feature_index++) {
    if (feature_index != through_count_d3 &&
        feature_index != through_score_d3) {
      assert(signed_below[feature_index] == unsigned_below[feature_index]);
    }
  }

  // J above the H15 triple: the word starts with it.
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/7J7/15/15/15 / 0/0 0");
  int32_t signed_above[PAT_NUM_FEATURES];
  pat_extract_features(board_get_readonly_lanes(game_get_board(game), 0), ld,
                       NULL, pat, RACK_SIZE, signed_above);
  assert(signed_above[through_count_d3] ==
         pat_get_through_count_end(pat, 0, j, 4));
  assert(signed_above[through_count_d3] > signed_below[through_count_d3]);

  // Round trip, and a file without the row reads as unsigned.
  ErrorStack *error_stack = error_stack_create();
  pat_write(pat, data_dir, "signed_through", error_stack);
  assert(error_stack_is_empty(error_stack));
  PATWeights *loaded = pat_create(data_dir, "signed_through", error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(pat_get_signed_through(loaded));
  pat_destroy(loaded);
  error_stack_destroy(error_stack);
  char header[32];
  current_pat_header(header, sizeof(header));
  char *contents =
      get_formatted_string("%s\nsigned_through,7\nhook_d1,-1\n", header);
  assert_pat_create_fails(data_dir, "signed_through_bad", contents);
  free(contents);
  pat_destroy(pat);
  config_destroy(config);
}

// A version 3 file has no hook-score rows: they read as zero and the rows
// after them land on the right features.
static void test_pat_version3_has_no_hook_score_channels(const char *data_dir) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_string(sb, "magpie_pat_v3\n");
  char feature_name[64];
  for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
       feature_index++) {
    if (feature_index >= PAT_FEATURE_HOOK_SCORE_START &&
        feature_index < PAT_FEATURE_DWS_HOOK_START) {
      continue;
    }
    pat_feature_name(feature_index, feature_name, sizeof(feature_name));
    string_builder_add_formatted_string(sb, "%s,%d\n", feature_name,
                                        -(feature_index + 1));
  }
  write_pat_file_contents(data_dir, "v3_no_hook_score",
                          string_builder_peek(sb));
  string_builder_destroy(sb);
  ErrorStack *error_stack = error_stack_create();
  PATWeights *loaded = pat_create(data_dir, "v3_no_hook_score", error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(loaded);
  for (int feature_index = PAT_FEATURE_HOOK_SCORE_START;
       feature_index < PAT_FEATURE_DWS_HOOK_START; feature_index++) {
    assert(pat_get_weight(loaded, feature_index) == 0);
  }
  assert(pat_get_weight(loaded, PAT_FEATURE_FLOAT_FLEX_SCALED_START) ==
         -(PAT_FEATURE_FLOAT_FLEX_SCALED_START + 1));
  assert(pat_get_weight(loaded, PAT_FEATURE_DWS_HOOK_START) ==
         -(PAT_FEATURE_DWS_HOOK_START + 1));
  assert(pat_get_weight(loaded, PAT_NUM_FEATURES - 1) == -PAT_NUM_FEATURES);
  error_stack_destroy(error_stack);
  pat_destroy(loaded);
}

static void test_pat_version4_has_no_lm_channels(const char *data_dir) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_string(sb, "magpie_pat_v4\n");
  char feature_name[64];
  for (int feature_index = 0; feature_index < PAT_FEATURE_LM_SPAN_START;
       feature_index++) {
    pat_feature_name(feature_index, feature_name, sizeof(feature_name));
    string_builder_add_formatted_string(sb, "%s,%d\n", feature_name,
                                        -(feature_index + 1));
  }
  write_pat_file_contents(data_dir, "v4_no_lm", string_builder_peek(sb));
  string_builder_destroy(sb);
  ErrorStack *error_stack = error_stack_create();
  PATWeights *loaded = pat_create(data_dir, "v4_no_lm", error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(loaded);
  for (int feature_index = PAT_FEATURE_LM_SPAN_START;
       feature_index < PAT_NUM_FEATURES; feature_index++) {
    assert(pat_get_weight(loaded, feature_index) == 0);
  }
  assert(pat_get_weight(loaded, PAT_FEATURE_LM_SPAN_START - 1) ==
         -PAT_FEATURE_LM_SPAN_START);
  // Rewritten, it carries the rows and reads back the same.
  pat_write(loaded, data_dir, "v4_no_lm_rewritten", error_stack);
  assert(error_stack_is_empty(error_stack));
  PATWeights *rewritten =
      pat_create(data_dir, "v4_no_lm_rewritten", error_stack);
  assert(error_stack_is_empty(error_stack));
  for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
       feature_index++) {
    assert(pat_get_weight(rewritten, feature_index) ==
           pat_get_weight(loaded, feature_index));
  }
  pat_destroy(rewritten);
  error_stack_destroy(error_stack);
  pat_destroy(loaded);
}

// A vertical AT on D2-D3, worked by hand against the standard board's
// premium layout (D1 and L1 double letters on row 1; F2/J2 triple letters
// on row 2; G3/I3, A4/H4, C7 and D8 double letters):
//   - D1 hooks ?AT from both row-1 triples: four tiles from A1, five from
//     H1, and the D1 double letter is in the span both ways: lm_span_d4
//     and lm_span_d5 each get 3 x (2 - 1).
//   - No triple lane has a letter multiplier only in an extension.
//   - Double-word lanes never cover a letter multiplier on the way to a
//     contact here, but six routes could extend onto one: B2's row
//     floater at D2 (two tiles, past the run to the F2 triple letter:
//     2 x 2), C3's row floater at D3 (one tile, on to G3: 2 x 1), C3
//     itself as a column hook (?T; one tile, down to C7: 2 x 1), C3's
//     column hook at C2 (?A; two tiles, past C3 down to C7: 2 x 1), the
//     D4 hook itself on row 4 (AT?; one tile, out to A4: 2 x 1), and
//     D4's column floater through TA (one tile, on to D1 or down to D8:
//     2 x 1). So dws_lm_ext_d1 = 8 and dws_lm_ext_d2 = 6.
//   Covering D1 with a C (CAT) spends the double letter: both triple
//   spans go to zero while the double-word extensions are unchanged.
static void test_pat_lm_channels(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 15");
  load_and_exec_config_or_die(
      config, "cgp 15/3A11/3T11/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  PATWeights *pat = pat_test_create_prepared("lm", game);
  int32_t features[PAT_NUM_FEATURES];
  pat_extract_features(board_get_readonly_lanes(game_get_board(game), 0), ld,
                       NULL, pat, RACK_SIZE, features);
  for (int bin = 0; bin < PAT_HOOK_BIN_COUNT; bin++) {
    assert(features[PAT_FEATURE_LM_SPAN_START + bin] ==
           ((bin == 3 || bin == 4) ? 3 : 0));
    assert(features[PAT_FEATURE_LM_EXT_START + bin] == 0);
    assert(features[PAT_FEATURE_DWS_LM_SPAN_START + bin] == 0);
    assert(features[PAT_FEATURE_DWS_LM_EXT_START + bin] == (bin == 0   ? 8
                                                            : bin == 1 ? 6
                                                                       : 0));
  }
  // The same through the lexicon floater semantics: every route here
  // has real extensions, so nothing changes.
  pat_set_lexicon_floaters(pat, true);
  int32_t lexicon_features[PAT_NUM_FEATURES];
  pat_extract_features(board_get_readonly_lanes(game_get_board(game), 0), ld,
                       NULL, pat, RACK_SIZE, lexicon_features);
  for (int feature_index = PAT_FEATURE_LM_SPAN_START;
       feature_index < PAT_NUM_FEATURES; feature_index++) {
    assert(lexicon_features[feature_index] == features[feature_index]);
  }

  load_and_exec_config_or_die(
      config, "cgp 3C11/3A11/3T11/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  game = config_get_game(config);
  pat_extract_features(board_get_readonly_lanes(game_get_board(game), 0), ld,
                       NULL, pat, RACK_SIZE, features);
  for (int bin = 0; bin < PAT_HOOK_BIN_COUNT; bin++) {
    assert(features[PAT_FEATURE_LM_SPAN_START + bin] == 0);
    assert(features[PAT_FEATURE_LM_EXT_START + bin] == 0);
    assert(features[PAT_FEATURE_DWS_LM_SPAN_START + bin] == 0);
    assert(features[PAT_FEATURE_DWS_LM_EXT_START + bin] == (bin == 0   ? 8
                                                            : bin == 1 ? 6
                                                                       : 0));
  }

  // The runtime overlay sees the same thing: evaluating CAT's C from the
  // AT position as a move must match the post-move board.
  pat_destroy(pat);
  config_destroy(config);
}

// AT on D2-E2: the only hook squares on any triple lane are D1 (a DLS,
// four tiles from A1, five from H1) and E1 (plain, five from A1, four from
// H1), each hooking a one-letter word. The hook-score channel at each
// distance must be the two squares' exposures, computed here from the
// board's own cross sets: unseen count times (word multiplier x hooked
// word's score + letter multiplier x tile score x (word multiplier + 3)),
// summed and divided by PAT_HOOK_SCORE_SCALE.
static void test_pat_hook_score_channel(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 15");
  load_and_exec_config_or_die(
      config, "cgp 15/3AT10/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  const Game *game = config_get_game(config);
  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  const Square *lanes = board_get_readonly_lanes(board, 0);
  PATWeights *pat = pat_test_create_prepared("hook_score", game);
  int32_t features[PAT_NUM_FEATURES];
  pat_extract_features(lanes, ld, NULL, pat, RACK_SIZE, features);

  const Square *row1 =
      board_get_row_cache(lanes, 0, BOARD_HORIZONTAL_DIRECTION);
  int expected[2] = {0, 0};
  int expected_flex[2] = {0, 0};
  const int cols[2] = {3, 4};
  for (int k = 0; k < 2; k++) {
    const Square *square = &row1[cols[k]];
    const uint64_t cross_set = square_get_cross_set(square);
    assert(cross_set != 0 && cross_set != TRIVIAL_CROSS_SET);
    const int cross_score = equity_to_int(square_get_cross_score(square));
    assert(cross_score == 1);
    const BonusSquare bonus = square_get_bonus_square(square);
    const int lm = bonus_square_get_letter_multiplier(bonus);
    const int wm = bonus_square_get_word_multiplier(bonus);
    assert(lm == (k == 0 ? 2 : 1) && wm == 1);
    long exposure = 0;
    for (int ml = 1; ml < ld_get_size(ld); ml++) {
      if (!(cross_set & ((uint64_t)1 << ml))) {
        continue;
      }
      int unseen = ld_get_dist(ld, ml);
      if (ml == ld_hl_to_ml(ld, "A") || ml == ld_hl_to_ml(ld, "T")) {
        unseen--;
      }
      expected_flex[k] += unseen;
      exposure +=
          (long)unseen * (wm * cross_score +
                          lm * equity_to_int(ld_get_score(ld, ml)) * (wm + 3));
    }
    expected[k] = (int)(exposure / PAT_HOOK_SCORE_SCALE);
    assert(expected[k] > 0);
  }
  // D1 is d4 from A1 and d5 from H1; E1 is d5 from A1 and d4 from H1.
  assert(features[PAT_FEATURE_HOOK_START + 3] ==
         expected_flex[0] + expected_flex[1]);
  assert(features[PAT_FEATURE_HOOK_START + 4] ==
         expected_flex[0] + expected_flex[1]);
  assert(features[PAT_FEATURE_HOOK_SCORE_START + 3] ==
         expected[0] + expected[1]);
  assert(features[PAT_FEATURE_HOOK_SCORE_START + 4] ==
         expected[0] + expected[1]);
  for (int bin = 0; bin < PAT_HOOK_BIN_COUNT; bin++) {
    if (bin != 3 && bin != 4) {
      assert(features[PAT_FEATURE_HOOK_SCORE_START + bin] == 0);
    }
  }
  // The DLS doubles E1's per-letter exposure relative to a plain square
  // only through the letter term, so D1 (fewer admissible letters, T
  // hooks A/I/U-style plurals aside) is not simply twice E1; just check
  // both are positive and the channel is nonzero exactly where hooks are.
  printf("hook_score: D1 exposure %d (flex %d), E1 exposure %d (flex %d)\n",
         expected[0], expected_flex[0], expected[1], expected_flex[1]);
  pat_destroy(pat);
  config_destroy(config);
}

// Transposes the board part of a CGP (rows become columns), leaving the
// racks and scores alone: the standard board is symmetric about its main
// diagonal, so every PAT feature must be identical on the transposed
// position (each channel already sums both lane directions).
static char *pat_test_transpose_cgp(const char *cgp) {
  char grid[BOARD_DIM][BOARD_DIM];
  const char *p = cgp;
  for (int row = 0; row < BOARD_DIM; row++) {
    int col = 0;
    while (*p != '/' && *p != ' ') {
      if (*p >= '0' && *p <= '9') {
        int n = 0;
        while (*p >= '0' && *p <= '9') {
          n = n * 10 + (*p - '0');
          p++;
        }
        for (int k = 0; k < n; k++) {
          grid[row][col++] = 0;
        }
      } else {
        grid[row][col++] = *p;
        p++;
      }
    }
    assert(col == BOARD_DIM);
    if (*p == '/') {
      p++;
    }
  }
  const char *rest = p; // " RACK1/RACK2 S1/S2 N"
  StringBuilder *sb = string_builder_create();
  for (int col = 0; col < BOARD_DIM; col++) {
    int empties = 0;
    for (int row = 0; row < BOARD_DIM; row++) {
      const char c = grid[row][col];
      if (c == 0) {
        empties++;
      } else {
        if (empties > 0) {
          string_builder_add_formatted_string(sb, "%d", empties);
          empties = 0;
        }
        string_builder_add_char(sb, c);
      }
    }
    if (empties > 0) {
      string_builder_add_formatted_string(sb, "%d", empties);
    }
    if (col < BOARD_DIM - 1) {
      string_builder_add_char(sb, '/');
    }
  }
  string_builder_add_string(sb, rest);
  char *out = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  return out;
}

static void test_pat_transposition_invariance(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *game = config_get_game(config);
  PATWeights *pat = pat_test_create_prepared("transpose", game);
  pat_set_lexicon_floaters(pat, true);
  pat_set_signed_through(pat, true);
  MoveList *move_list = move_list_create(1);
  int32_t features[PAT_NUM_FEATURES];
  int32_t transposed_features[PAT_NUM_FEATURES];
  int positions = 0;
  for (int attempt = 0; attempt < 30; attempt++) {
    game_reset(game);
    game_seed(game, 5000000ULL + (uint64_t)attempt);
    draw_starting_racks(game);
    const int plies = 1 + attempt % 14;
    for (int ply = 0; ply < plies; ply++) {
      play_move(get_top_equity_move(game, move_list), game, NULL);
      if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
        break;
      }
    }
    if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
      continue;
    }
    char *cgp = game_get_cgp(game, true);
    const Rack *rack = player_get_rack(game_get_player(game, 0));
    Rack rack_copy_0;
    rack_copy(&rack_copy_0, rack);
    pat_extract_features(board_get_readonly_lanes(game_get_board(game), 0),
                         game_get_ld(game), &rack_copy_0, pat, RACK_SIZE,
                         features);
    char *transposed = pat_test_transpose_cgp(cgp);
    char *cmd = get_formatted_string("cgp %s", transposed);
    load_and_exec_config_or_die(config, cmd);
    pat_extract_features(board_get_readonly_lanes(game_get_board(game), 0),
                         game_get_ld(game), &rack_copy_0, pat, RACK_SIZE,
                         transposed_features);
    for (int f = 0; f < PAT_NUM_FEATURES; f++) {
      if (features[f] != transposed_features[f]) {
        char name[64];
        pat_feature_name(f, name, sizeof(name));
        printf("transposition mismatch at %s: %d vs %d\n  %s\n", name,
               features[f], transposed_features[f], cgp);
        assert(false);
      }
    }
    free(cmd);
    free(transposed);
    free(cgp);
    positions++;
  }
  printf("PAT transposition invariance: %d positions\n", positions);
  assert(positions >= 25);
  move_list_destroy(move_list);
  pat_destroy(pat);
  config_destroy(config);
}

// A blank in a floater run reaches whatever its letter reaches (through
// tables, extension sets) but scores nothing: NARCEIN with a blank E must
// match NARCEIN with a real E on every channel except the floater score
// channels, which lose exactly the E's one point per route through it.
static void test_pat_blank_floater(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 15");
  load_and_exec_config_or_die(
      config,
      "cgp 15/15/15/15/15/15/15/3NARCEIN5/15/15/15/15/15/15/15 / 0/0 0");
  Game *game = config_get_game(config);
  PATWeights *pat = pat_test_create_prepared("blank_floater", game);
  pat_set_lexicon_floaters(pat, true);
  pat_set_signed_through(pat, true);
  int32_t real_features[PAT_NUM_FEATURES];
  pat_extract_features(board_get_readonly_lanes(game_get_board(game), 0),
                       game_get_ld(game), NULL, pat, RACK_SIZE, real_features);
  load_and_exec_config_or_die(
      config,
      "cgp 15/15/15/15/15/15/15/3NARCeIN5/15/15/15/15/15/15/15 / 0/0 0");
  int32_t blank_features[PAT_NUM_FEATURES];
  pat_extract_features(board_get_readonly_lanes(game_get_board(game), 0),
                       game_get_ld(game), NULL, pat, RACK_SIZE, blank_features);
  int score_diffs = 0;
  for (int f = 0; f < PAT_NUM_FEATURES; f++) {
    const bool is_float_score =
        f >= PAT_FEATURE_FLOAT_SCORE_START &&
        f < PAT_FEATURE_FLOAT_SCORE_START + PAT_FLOATER_BIN_COUNT;
    if (is_float_score) {
      // The E is worth one point; the blank is worth none. Unseen counts
      // also shift one E back into the pool and one blank out, which the
      // flexibility channels see, so only the score channels are pinned.
      if (real_features[f] != blank_features[f]) {
        assert(real_features[f] - blank_features[f] >= 1);
        score_diffs++;
      }
    }
  }
  assert(score_diffs > 0);
  // Same routes: the through channels key on the unblanked letter.
  for (int bin = 0; bin < PAT_FLOATER_BIN_COUNT; bin++) {
    assert(real_features[PAT_FEATURE_FLOAT_THROUGH_COUNT_START + bin] ==
           blank_features[PAT_FEATURE_FLOAT_THROUGH_COUNT_START + bin]);
    assert(real_features[PAT_FEATURE_FLOAT_THROUGH_SCORE_START + bin] ==
           blank_features[PAT_FEATURE_FLOAT_THROUGH_SCORE_START + bin]);
  }
  pat_destroy(pat);
  config_destroy(config);
}

// Enumerates words of the given length whose first (word_end 0) or last
// (word_end 1) letters are key: the slow reference for the run-keyed
// through tables.
static void pat_test_count_key_words(const KWG *kwg, uint32_t node_index,
                                     int depth, int target_length,
                                     const MachineLetter *key, int key_len,
                                     int word_end, MachineLetter *word,
                                     long *count, long *rest_score_sum,
                                     const LetterDistribution *ld) {
  if (node_index == 0) {
    return;
  }
  for (uint32_t index = node_index;; index++) {
    const uint32_t node = kwg_node(kwg, index);
    word[depth] = (MachineLetter)kwg_node_tile(node);
    const int length = depth + 1;
    if (length == target_length && kwg_node_accepts(node)) {
      bool match = true;
      for (int k = 0; k < key_len; k++) {
        const int pos = (word_end == 0) ? k : length - key_len + k;
        if (word[pos] != key[k]) {
          match = false;
        }
      }
      if (match) {
        (*count)++;
        int rest = 0;
        for (int k = 0; k < length; k++) {
          const bool in_key =
              (word_end == 0) ? (k < key_len) : (k >= length - key_len);
          if (!in_key) {
            rest += equity_to_int(ld_get_score(ld, word[k]));
          }
        }
        *rest_score_sum += rest;
      }
    }
    if (length < target_length) {
      pat_test_count_key_words(kwg, kwg_node_arc_index(node), length,
                               target_length, key, key_len, word_end, word,
                               count, rest_score_sum, ld);
    }
    if (kwg_node_is_end(node)) {
      break;
    }
  }
}

// The run-keyed through tables against a direct enumeration of the
// lexicon, for the runs the audit used, and the single-letter keys against
// the signed per-letter table they must reproduce.
static void test_pat_run_through_table(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 15");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  const Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  const KWG *kwg = player_get_kwg(game_get_player(game, 0));
  PATWeights *pat = pat_test_create_prepared("run_through", game);
  const char *runs[] = {"E", "AT", "QI", "ION", "TIO"};
  int checked = 0;
  for (int r = 0; r < 5; r++) {
    const int key_len = (int)strlen(runs[r]);
    MachineLetter key[3];
    for (int k = 0; k < key_len; k++) {
      char one[2] = {runs[r][k], 0};
      key[k] = ld_hl_to_ml(ld, one);
    }
    for (int length = key_len + 2; length <= key_len + 6; length += 2) {
      for (int word_end = 0; word_end < 2; word_end++) {
        long count = 0;
        long rest = 0;
        MachineLetter word[16];
        pat_test_count_key_words(kwg, kwg_get_dawg_root_node_index(kwg), 0,
                                 length, key, key_len, word_end, word, &count,
                                 &rest, ld);
        const int expected_count = (int)(8.0 * log2(1.0 + count) + 0.5);
        const int expected_score =
            count > 0 ? (int)((double)rest / count + 0.5) : 0;
        const int table_count =
            pat_get_run_through_count(pat, word_end, key, key_len, length);
        const int table_score =
            pat_get_run_through_score(pat, word_end, key, key_len, length);
        if (table_count != expected_count || table_score != expected_score) {
          printf("run_through mismatch: %s len %d end %d: table %d/%d, "
                 "enumeration %d/%d (%ld words)\n",
                 runs[r], length, word_end, table_count, table_score,
                 expected_count, expected_score, count);
          assert(false);
        }
        if (key_len == 1) {
          assert(table_count ==
                 pat_get_through_count_end(pat, word_end, key[0], length));
          assert(table_score ==
                 pat_get_through_score_end(pat, word_end, key[0], length));
        }
        checked++;
      }
    }
  }
  printf("run_through table: %d (key, length, end) cells match the "
         "enumeration\n",
         checked);
  // The audit's headline: QI cannot end a six-letter word, NARCEIN (keyed
  // by its far three letters) is nothing at all.
  MachineLetter qi[2] = {ld_hl_to_ml(ld, "Q"), ld_hl_to_ml(ld, "I")};
  assert(pat_get_run_through_count(pat, 1, qi, 2, 6) == 0);
  assert(pat_get_run_through_count(pat, 0, qi, 2, 6) > 0);
  pat_destroy(pat);
  config_destroy(config);
}

// The scan under run_through on constructed boards. AT on B8-C8 with the
// A8 triple one square to its left: the word runs A8 -> AT, AT is a
// suffix, distance 1, word length 3 (?AT: BAT, CAT, ...). QI on N8-O8 is
// keyed as a prefix from the O8 triple... no: O8 is the last square, so
// the run before it, at distance 1 with the word running QI -> O8, QI is
// a prefix of a three-letter word (QIS): nonzero. QI on B8-C8 from A8: a
// suffix of a three-letter word ending in QI: none. A blank Q keys as Q.
static void test_pat_run_through_scan(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 15");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/1AT12/15/15/15/15/15/15/15 / 0/0 0");
  const Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  PATWeights *pat = pat_test_create_prepared("run_scan", game);
  pat_set_lexicon_floaters(pat, true);
  pat_set_signed_through(pat, true);
  int32_t features[PAT_NUM_FEATURES];
  int32_t run_features[PAT_NUM_FEATURES];
  pat_extract_features(board_get_readonly_lanes(game_get_board(game), 0), ld,
                       NULL, pat, RACK_SIZE, features);
  pat_set_run_through(pat, true);
  pat_extract_features(board_get_readonly_lanes(game_get_board(game), 0), ld,
                       NULL, pat, RACK_SIZE, run_features);
  MachineLetter at[2] = {ld_hl_to_ml(ld, "A"), ld_hl_to_ml(ld, "T")};
  // From A8 (side +1): distance 1, suffix AT, length 3. From O8 (side -1):
  // O8 N8 ... D8 are 12 empties, more than a rack: no route.
  assert(run_features[PAT_FEATURE_FLOAT_THROUGH_COUNT_START] ==
         pat_get_run_through_count(pat, 1, at, 2, 3));
  assert(run_features[PAT_FEATURE_FLOAT_THROUGH_SCORE_START] ==
         pat_get_run_through_score(pat, 1, at, 2, 3));
  assert(run_features[PAT_FEATURE_FLOAT_THROUGH_COUNT_START] > 0);
  // The per-tile sum claimed more (A's and T's single-letter statistics
  // added), and every other channel is untouched by the flag.
  assert(features[PAT_FEATURE_FLOAT_THROUGH_COUNT_START] >
         run_features[PAT_FEATURE_FLOAT_THROUGH_COUNT_START]);
  for (int f = 0; f < PAT_NUM_FEATURES; f++) {
    const bool is_through =
        (f >= PAT_FEATURE_FLOAT_THROUGH_SCORE_START &&
         f < PAT_FEATURE_FLOAT_THROUGH_COUNT_START + PAT_FLOATER_BIN_COUNT);
    if (!is_through) {
      assert(features[f] == run_features[f]);
    }
  }
  // QI before the O8 triple at distance 1: a prefix of a three-letter
  // word (QIS exists); QI after A8: a suffix of a three-letter word (none).
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/1QI10N1/15/15/15/15/15/15/15 / 0/0 0");
  pat_extract_features(board_get_readonly_lanes(game_get_board(game), 0), ld,
                       NULL, pat, RACK_SIZE, run_features);
  MachineLetter qi[2] = {ld_hl_to_ml(ld, "Q"), ld_hl_to_ml(ld, "I")};
  MachineLetter n[1] = {ld_hl_to_ml(ld, "N")};
  // d1 bin: QI from A8 (suffix, length 3: 0) plus N from O8 (prefix,
  // length 3... O8 is at index 14, N at 13: distance 1, word N + O8 =
  // length 2? The empties between N and the triple: just O8, so d = 1 and
  // the word is two letters starting with N).
  assert(pat_get_run_through_count(pat, 1, qi, 2, 3) == 0);
  assert(run_features[PAT_FEATURE_FLOAT_THROUGH_COUNT_START] ==
         pat_get_run_through_count(pat, 0, n, 1, 2));
  // A blank Q keys as Q.
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/1qI12/15/15/15/15/15/15/15 / 0/0 0");
  int32_t blank_features[PAT_NUM_FEATURES];
  pat_extract_features(board_get_readonly_lanes(game_get_board(game), 0), ld,
                       NULL, pat, RACK_SIZE, blank_features);
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/1QI12/15/15/15/15/15/15/15 / 0/0 0");
  pat_extract_features(board_get_readonly_lanes(game_get_board(game), 0), ld,
                       NULL, pat, RACK_SIZE, run_features);
  for (int bin = 0; bin < PAT_FLOATER_BIN_COUNT; bin++) {
    assert(blank_features[PAT_FEATURE_FLOAT_THROUGH_COUNT_START + bin] ==
           run_features[PAT_FEATURE_FLOAT_THROUGH_COUNT_START + bin]);
  }
  pat_destroy(pat);
  config_destroy(config);
}

static void test_pat_scan_reach_capped_by_opponent_rack_size(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 15");
  load_and_exec_config_or_die(config, PAT_FLOATER_CGP_CMD);
  const Game *game = config_get_game(config);
  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  const Square *lanes = board_get_readonly_lanes(board, 0);

  int32_t full_reach_features[PAT_NUM_FEATURES];
  pat_extract_features(lanes, ld, NULL, NULL, RACK_SIZE, full_reach_features);
  const int32_t float_flex_d2 =
      full_reach_features[PAT_FEATURE_FLOAT_FLEX_START + 1];
  assert(full_reach_features[PAT_FEATURE_FLOAT_SCORE_START + 4] == 1);

  // With the opponent down to 3 tiles, the d=5 route to the far TWS is
  // impossible: that floater bin must vanish, while the d=2 route (still
  // <= 3) survives unchanged.
  int32_t capped_features[PAT_NUM_FEATURES];
  pat_extract_features(lanes, ld, NULL, NULL, 3, capped_features);
  assert(capped_features[PAT_FEATURE_FLOAT_SCORE_START + 1] == 1);
  assert(capped_features[PAT_FEATURE_FLOAT_FLEX_START + 1] == float_flex_d2);
  assert(capped_features[PAT_FEATURE_FLOAT_SCORE_START + 4] == 0);
  assert(capped_features[PAT_FEATURE_FLOAT_FLEX_START + 4] == 0);
  // The triple-triple span needed both endpoints live; losing the far one
  // drops it to a plain hook-only reach from the near TWS, not a floater.
  assert(capped_features[PAT_FEATURE_TT_FLOATER] == 0);

  config_destroy(config);
}

// placement_adjustment (the legacy per-square opening penalty; see
// update_opening_penalty) must skip whichever axis a live PAT class already
// prices, and only that axis, so the crude fixed penalty and a trained PAT
// weight for the same square never both apply.
static void test_pat_opening_penalty_gating(void) {
  Config *config = config_create_or_die("set -lex CSW21 -numplays 5");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  const Game *game = config_get_game(config);
  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  const Square *lanes = board_get_readonly_lanes(board, 0);

  Equity word_penalties[BOARD_DIM * 2] = {0};
  Equity letter_penalties[BOARD_DIM * 2] = {0};
  word_penalties[3] = -700;
  letter_penalties[3] = -350;

  const MachineLetter e_ml = ld_hl_to_ml(ld, "E");
  Move move;
  set_single_tile_move(&move, e_ml, 7, 3);

  // No PAT context: both axes apply, matching PAT-off behavior exactly.
  assert(placement_adjustment(ld, &move, word_penalties, letter_penalties,
                              pat_eval_ctx_active_classes(NULL)) == -1050);
  PATEvalContext disabled_ctx;
  pat_eval_context_disable(&disabled_ctx);
  assert(placement_adjustment(ld, &move, word_penalties, letter_penalties,
                              pat_eval_ctx_active_classes(&disabled_ctx)) ==
         -1050);

  // Only a letter-multiplier class (DLS) weighted: the letter axis is
  // suppressed, the word axis is untouched.
  PATWeights *dls_only = pat_test_create_prepared("dls_only_gate", game);
  pat_set_weight(dls_only, PAT_FEATURE_DLS_HOOK_START, -5);
  PATEvalContext ctx;
  pat_eval_context_load(&ctx, dls_only, lanes, ld, NULL, PAT_CLASS_MASK_ALL,
                        RACK_SIZE);
  const uint32_t dls_active = pat_eval_ctx_active_classes(&ctx);
  assert(dls_active & PAT_CLASS_MASK_LETTER_MULT);
  assert(!(dls_active & PAT_CLASS_MASK_WORD_MULT));
  assert(placement_adjustment(ld, &move, word_penalties, letter_penalties,
                              dls_active) == -700);
  pat_destroy(dls_only);

  // Only a word-multiplier class (TWS) weighted: the word axis is
  // suppressed, the letter axis is untouched.
  PATWeights *tws_only = pat_test_create_prepared("tws_only_gate", game);
  pat_set_weight(tws_only, PAT_FEATURE_HOOK_START, -5);
  pat_eval_context_load(&ctx, tws_only, lanes, ld, NULL, PAT_CLASS_MASK_ALL,
                        RACK_SIZE);
  const uint32_t tws_active = pat_eval_ctx_active_classes(&ctx);
  assert(tws_active & PAT_CLASS_MASK_WORD_MULT);
  assert(!(tws_active & PAT_CLASS_MASK_LETTER_MULT));
  assert(placement_adjustment(ld, &move, word_penalties, letter_penalties,
                              tws_active) == -350);
  pat_destroy(tws_only);

  // A runtime mask excluding DLS even though the file has real DLS weights
  // must not suppress the legacy penalty: gating follows what actually
  // applies, not what the file merely contains.
  PATWeights *dls_weighted =
      pat_test_create_prepared("dls_masked_off_gate", game);
  pat_set_weight(dls_weighted, PAT_FEATURE_DLS_HOOK_START, -5);
  pat_eval_context_load(&ctx, dls_weighted, lanes, ld, NULL,
                        PAT_CLASS_MASK_TWS_ONLY, RACK_SIZE);
  const uint32_t masked_active = pat_eval_ctx_active_classes(&ctx);
  assert(!(masked_active & PAT_CLASS_MASK_LETTER_MULT));
  assert(placement_adjustment(ld, &move, word_penalties, letter_penalties,
                              masked_active) == -1050);
  pat_destroy(dls_weighted);

  config_destroy(config);
}

static void test_pat_move_penalty(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 15");
  load_and_exec_config_or_die(config, PAT_FLOATER_CGP_CMD);
  const Game *game = config_get_game(config);
  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  const Square *lanes = board_get_readonly_lanes(board, 0);
  int32_t features[PAT_NUM_FEATURES];
  pat_extract_features(lanes, ld, NULL, NULL, RACK_SIZE, features);
  const int32_t float_flex_d5 = features[PAT_FEATURE_FLOAT_FLEX_START + 4];

  PATWeights *pat = pat_test_create_prepared("penalty_test", game);
  pat_set_weight(pat, PAT_FEATURE_TT_FLOATER, -1000);
  pat_set_weight(pat, PAT_FEATURE_FLOAT_FLEX_START + 1, -100);
  pat_set_weight(pat, PAT_FEATURE_FLOAT_FLEX_START + 4, -100);

  PATEvalContext pat_eval_ctx;
  pat_eval_context_load(&pat_eval_ctx, pat, lanes, ld, NULL, PAT_CLASS_MASK_ALL,
                        RACK_SIZE);
  const Equity expected_pre = -1000 * features[PAT_FEATURE_TT_FLOATER] -
                              100 * features[PAT_FEATURE_FLOAT_FLEX_START + 1] -
                              100 * float_flex_d5;
  assert(pat_eval_ctx.pre_penalty == expected_pre);
  assert(pat_eval_ctx.pre_penalty < 0);

  const MachineLetter x_ml = ld_hl_to_ml(ld, "X");

  // A move far from every TWS lane changes nothing: its term is the
  // position baseline.
  Move far_move;
  set_single_tile_move(&far_move, x_ml, 5, 10);
  assert(pat_eval_move_penalty(&pat_eval_ctx, &far_move, NULL) ==
         pat_eval_ctx.pre_penalty);

  // An exchange also carries the baseline.
  Move exchange_move;
  MachineLetter exchange_strip[1] = {x_ml};
  move_set_all_except_equity(&exchange_move, exchange_strip, 0, 0, 0, 0, 0, 1,
                             BOARD_HORIZONTAL_DIRECTION, GAME_EVENT_EXCHANGE);
  assert(pat_eval_move_penalty(&pat_eval_ctx, &exchange_move, NULL) ==
         pat_eval_ctx.pre_penalty);

  // Covering the TWS at (14,7) kills both triple-triple counts and the
  // d = 2 floater unit; only the d = 5 floater from (14,0) survives (the
  // fresh tile becomes a d = 6 floater, whose bins are unweighted here and
  // whose flex approximation is zero with an unprepared hook_flex table).
  Move block_move;
  set_single_tile_move(&block_move, x_ml, 14, 7);
  const Equity block_penalty =
      pat_eval_move_penalty(&pat_eval_ctx, &block_move, NULL);
  assert(block_penalty == -100 * float_flex_d5);
  assert(block_penalty > pat_eval_ctx.pre_penalty);
  assert(block_penalty <= 0);

  // With zero weights everything is zero.
  PATWeights *zero_pat = pat_test_create_prepared("zero_test", game);
  PATEvalContext zero_ctx;
  pat_eval_context_load(&zero_ctx, zero_pat, lanes, ld, NULL,
                        PAT_CLASS_MASK_ALL, RACK_SIZE);
  assert(zero_ctx.pre_penalty == 0);
  assert(pat_eval_move_penalty(&zero_ctx, &block_move, NULL) == 0);
  assert(pat_eval_move_penalty(&zero_ctx, &far_move, NULL) == 0);

  // A disabled or NULL context is exactly zero.
  PATEvalContext disabled_ctx;
  pat_eval_context_disable(&disabled_ctx);
  assert(pat_eval_move_penalty(&disabled_ctx, &block_move, NULL) == 0);
  assert(pat_eval_move_penalty(NULL, &block_move, NULL) == 0);

  pat_destroy(zero_pat);
  pat_destroy(pat);
  config_destroy(config);
}

// Regression test for a bug where pat_scan_unit had no double letter square
// case and fell through to the triple word square channel bases, so double
// letter features silently landed on hook_d*/float_score_d* instead of
// dls_hook_d*/dls_float_score_d*. A weight on dls_hook_d1 alone should
// still produce a nonzero penalty on a board with open double letter
// squares; under the bug the feature would never reach that channel and
// the penalty would stay exactly zero.
static void test_pat_dls_features_land_in_dls_channels(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 15");
  // Places Q directly above the double letter square at (7, 3) (0-indexed),
  // giving it a real, non-trivial cross set (only I completes QI): hooks
  // cannot exist on a truly empty board (there is nothing to hook onto), so
  // this is the minimum board state that can ever produce a nonzero hook
  // feature.
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/3Q11/15/15/15/15/15/15/15/15 / 0/0 0");
  const Game *game = config_get_game(config);
  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  const Square *lanes = board_get_readonly_lanes(board, 0);

  PATWeights *pat = pat_test_create_prepared("dls_only", game);
  pat_set_weight(pat, PAT_FEATURE_DLS_HOOK_START, -1000);

  PATEvalContext ctx;
  pat_eval_context_load(&ctx, pat, lanes, ld, NULL, PAT_CLASS_MASK_ALL,
                        RACK_SIZE);
  int num_dls = 0;
  for (int tws_idx = 0; tws_idx < ctx.num_tws; tws_idx++) {
    if (ctx.tws_classes[tws_idx] == PAT_PREMIUM_DLS) {
      num_dls++;
    }
  }
  assert(num_dls > 0);
  assert(ctx.pre_penalty < 0);

  pat_destroy(pat);
  config_destroy(config);
}

// A Q directly above an open TWS makes it hooky (CSW21's only completion is
// QI), giving both the raw and hypergeometric-scaled hook channels a real,
// nonzero value to compare on an otherwise near-empty board, where the
// unseen pool is far larger than RACK_SIZE.
static void test_pat_hook_scaled_channel(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 15");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/Q14/15/15/15/15/15/15/15/15 / 0/0 0");
  const Game *game = config_get_game(config);
  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  const Square *lanes = board_get_readonly_lanes(board, 0);

  int32_t features[PAT_NUM_FEATURES];
  pat_extract_features(lanes, ld, NULL, NULL, RACK_SIZE, features);

  const int32_t raw_hook = features[PAT_FEATURE_HOOK_START];
  const int32_t scaled_hook = features[PAT_FEATURE_HOOK_SCALED_START];
  assert(raw_hook > 0);
  // The unseen pool on a near-empty board is far larger than RACK_SIZE, so
  // the hypergeometric fraction is well under 1: the scaled channel must be
  // strictly smaller than the raw count, never equal to or exceeding it,
  // and never negative.
  assert(scaled_hook >= 0);
  assert(scaled_hook < raw_hook);
  // Every other hook bin (no hook there) and DWS/TLS/DLS/QWS/QLS's own
  // hook channels (TWS-only feature) stay at zero in both forms.
  for (int bin = 1; bin < PAT_HOOK_BIN_COUNT; bin++) {
    assert(features[PAT_FEATURE_HOOK_START + bin] == 0);
    assert(features[PAT_FEATURE_HOOK_SCALED_START + bin] == 0);
  }

  config_destroy(config);
}

// The bound argument for pat_eval_move_penalty_bound depends on the
// discount staying in [0, 1] for every value a PATWeights could ever
// carry, not just the ones a file happens to pass through pat_parse_contents
// (a search over candidate discounts would set this directly).
static void test_pat_own_asset_discount_clamped(void) {
  PATWeights *pat = pat_create_zeroed("clamp_test");
  pat_set_own_asset_discount(pat, -0.5);
  assert(pat_get_own_asset_discount(pat) == 0.0);
  pat_set_own_asset_discount(pat, 1.5);
  assert(pat_get_own_asset_discount(pat) == 1.0);
  pat_set_own_asset_discount(pat, 0.5);
  assert(pat_get_own_asset_discount(pat) == 0.5);
  pat_destroy(pat);
}

// A move played far from every premium square still credits a unit its own
// leave could exploit, entirely through unit_hook_letters -- the point of
// the feature (Q above the open TWS at (7,0) makes CSW21's QI its only
// hook letter, so a leave holding an I should get credit; one that
// doesn't, or a discount of 0, should not).
static void test_pat_own_asset_discount(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 15");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/Q14/15/15/15/15/15/15/15/15 / 0/0 0");
  const Game *game = config_get_game(config);
  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  const Square *lanes = board_get_readonly_lanes(board, 0);
  const int ld_size = ld_get_size(ld);

  const MachineLetter x_ml = ld_hl_to_ml(ld, "X");
  const MachineLetter i_ml = ld_hl_to_ml(ld, "I");

  Move far_move;
  set_single_tile_move(&far_move, x_ml, 5, 10);

  Rack *leave_with_i = rack_create(ld_size);
  rack_add_letter(leave_with_i, i_ml);
  Rack *leave_without_i = rack_create(ld_size);
  rack_add_letter(leave_without_i, x_ml);
  Rack *leave_with_blank = rack_create(ld_size);
  rack_add_letter(leave_with_blank, BLANK_MACHINE_LETTER);

  PATWeights *pat = pat_test_create_prepared("own_asset_test", game);
  pat_set_weight(pat, PAT_FEATURE_HOOK_START, -1000);
  pat_set_own_asset_discount(pat, 0.5);

  PATEvalContext ctx;
  pat_eval_context_load(&ctx, pat, lanes, ld, NULL, PAT_CLASS_MASK_ALL,
                        RACK_SIZE);
  assert(ctx.pre_penalty < 0);
  // Never touches (7,0) or its halo, so with no leave (or one that cannot
  // exploit the hook) the term is exactly the baseline, same as before this
  // feature existed.
  assert(pat_eval_move_penalty(&ctx, &far_move, NULL) == ctx.pre_penalty);
  assert(pat_eval_move_penalty(&ctx, &far_move, leave_without_i) ==
         ctx.pre_penalty);
  // Holding the I this exact hook needs earns credit even though the move
  // never went near (7,0): half the baseline penalty, per the 0.5 discount.
  const Equity credited_i =
      pat_eval_move_penalty(&ctx, &far_move, leave_with_i);
  assert(credited_i == (Equity)lround((double)ctx.pre_penalty * 0.5));
  assert(credited_i > ctx.pre_penalty);
  assert(credited_i <= 0);
  // A blank stands in for whatever the hook needs, so it earns the same
  // credit even without holding an I specifically.
  assert(pat_eval_move_penalty(&ctx, &far_move, leave_with_blank) ==
         credited_i);
  // The bound must stay a real upper bound: at least as generous as the
  // exact value once the leave is known.
  assert(pat_eval_move_penalty_bound(&ctx, &far_move, leave_with_i) >=
         credited_i);
  assert(pat_eval_move_penalty_bound(&ctx, &far_move, NULL) >= ctx.pre_penalty);

  // lane_penalty_bound is position-level, computed before any specific
  // move's leave is known, so it must assume the worst case: any unit
  // reachable through a letter the player's rack currently holds could end
  // up in that move's leave and earn credit. A rack holding an I must
  // widen even a lane nowhere near (7,0) all the way to 0 (the only
  // nonzero unit, now assumed creditable everywhere); a rack that cannot
  // supply the hook's letter at all must leave that lane's bound exactly
  // where it already was.
  Rack *rack_with_i = rack_create(ld_size);
  rack_add_letter(rack_with_i, i_ml);
  PATEvalContext ctx_with_i;
  pat_eval_context_load(&ctx_with_i, pat, lanes, ld, rack_with_i,
                        PAT_CLASS_MASK_ALL, RACK_SIZE);
  assert(pat_eval_lane_penalty_bound(&ctx_with_i, BOARD_HORIZONTAL_DIRECTION,
                                     5) == 0);

  Rack *rack_without_i = rack_create(ld_size);
  rack_add_letter(rack_without_i, x_ml);
  PATEvalContext ctx_without_i;
  pat_eval_context_load(&ctx_without_i, pat, lanes, ld, rack_without_i,
                        PAT_CLASS_MASK_ALL, RACK_SIZE);
  assert(pat_eval_lane_penalty_bound(&ctx_without_i, BOARD_HORIZONTAL_DIRECTION,
                                     5) == ctx_without_i.pre_penalty);
  rack_destroy(rack_with_i);
  rack_destroy(rack_without_i);

  // With no discount configured (the default every earlier file already
  // has), the same leave earns nothing: byte-for-byte today's behavior.
  PATWeights *no_discount_pat =
      pat_test_create_prepared("no_discount_test", game);
  pat_set_weight(no_discount_pat, PAT_FEATURE_HOOK_START, -1000);
  PATEvalContext no_discount_ctx;
  pat_eval_context_load(&no_discount_ctx, no_discount_pat, lanes, ld, NULL,
                        PAT_CLASS_MASK_ALL, RACK_SIZE);
  assert(pat_eval_move_penalty(&no_discount_ctx, &far_move, leave_with_i) ==
         no_discount_ctx.pre_penalty);

  // A move that covers the hooky square itself destroys the only route
  // this unit had. Eligibility must come from the fresh, post-move scan,
  // not the stale baseline reverse index: an I in the leave (what the now-
  // gone hook needed) must not resurrect a discount on a unit whose real,
  // rescanned contribution is already gone.
  Move cover_move;
  set_single_tile_move(&cover_move, x_ml, 7, 0);
  const Equity covered_penalty =
      pat_eval_move_penalty(&ctx, &cover_move, leave_with_i);
  assert(covered_penalty == pat_eval_move_penalty(&ctx, &cover_move, NULL));

  rack_destroy(leave_with_i);
  rack_destroy(leave_without_i);
  rack_destroy(leave_with_blank);
  pat_destroy(pat);
  pat_destroy(no_discount_pat);
  config_destroy(config);
}

static void test_pat_unweighted_units_dropped(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 15");
  load_and_exec_config_or_die(config, PAT_FLOATER_CGP_CMD);
  const Game *game = config_get_game(config);
  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  const Square *lanes = board_get_readonly_lanes(board, 0);

  // Weights on triple-word channels only: the double-word, triple-letter
  // and window units can never charge anything, so the evaluation context
  // leaves them out while the all-units context keeps them.
  PATWeights *pat = pat_test_create_prepared("drop_test", game);
  pat_set_weight(pat, PAT_FEATURE_FLOAT_SCORE_START + 1, -500);
  pat_set_weight(pat, PAT_FEATURE_HOOK_START, -20);

  PATEvalContext pruned_ctx;
  pat_eval_context_load(&pruned_ctx, pat, lanes, ld, NULL, PAT_CLASS_MASK_ALL,
                        RACK_SIZE);
  PATEvalContext full_ctx;
  pat_eval_context_load_all_units(&full_ctx, pat, lanes, ld, NULL, RACK_SIZE);
  assert(pruned_ctx.num_tws > 0);
  assert(full_ctx.num_tws > pruned_ctx.num_tws);
  assert(full_ctx.num_dd > 0);
  assert(pruned_ctx.num_dd == 0);
  for (int tws_idx = 0; tws_idx < pruned_ctx.num_tws; tws_idx++) {
    assert(pruned_ctx.tws_classes[tws_idx] == PAT_PREMIUM_TWS);
  }

  // Dropping them changes nothing the engine reads: the baseline, every
  // lane bound, and the penalty and bound of a tile on every empty square.
  assert(pruned_ctx.pre_penalty == full_ctx.pre_penalty);
  assert(pruned_ctx.pre_penalty < 0);
  for (int dir = 0; dir < 2; dir++) {
    for (int lane = 0; lane < BOARD_DIM; lane++) {
      assert(pruned_ctx.lane_penalty_bound[dir][lane] ==
             full_ctx.lane_penalty_bound[dir][lane]);
    }
  }
  const MachineLetter z_ml = ld_hl_to_ml(ld, "Z");
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      if (board_get_letter(board, row, col) != ALPHABET_EMPTY_SQUARE_MARKER) {
        continue;
      }
      Move move;
      set_single_tile_move(&move, z_ml, row, col);
      assert(pat_eval_move_penalty(&pruned_ctx, &move, NULL) ==
             pat_eval_move_penalty(&full_ctx, &move, NULL));
      assert(pat_eval_move_penalty_bound(&pruned_ctx, &move, NULL) ==
             pat_eval_move_penalty_bound(&full_ctx, &move, NULL));
    }
  }

  // A weight on a double-word channel brings those squares back, and one
  // on a double-double channel brings the windows back.
  pat_set_weight(pat, PAT_FEATURE_DWS_HOOK_START, -10);
  pat_eval_context_load(&pruned_ctx, pat, lanes, ld, NULL, PAT_CLASS_MASK_ALL,
                        RACK_SIZE);
  int num_dws = 0;
  for (int tws_idx = 0; tws_idx < pruned_ctx.num_tws; tws_idx++) {
    if (pruned_ctx.tws_classes[tws_idx] == PAT_PREMIUM_DWS) {
      num_dws++;
    }
  }
  assert(num_dws > 0);
  assert(pruned_ctx.num_dd == 0);
  // The standard board also has triple-double and triple-triple windows in
  // the higher tiers, so a double-double weight brings back exactly the
  // tier-0 windows and no others.
  pat_set_weight(pat, PAT_FEATURE_DD_TILES_SAVED, -10);
  pat_eval_context_load(&pruned_ctx, pat, lanes, ld, NULL, PAT_CLASS_MASK_ALL,
                        RACK_SIZE);
  int num_tier0 = 0;
  for (int dd_idx = 0; dd_idx < full_ctx.num_dd; dd_idx++) {
    if (full_ctx.dd_tiers[dd_idx] == 0) {
      num_tier0++;
    }
  }
  assert(num_tier0 > 0);
  assert(pruned_ctx.num_dd == num_tier0);
  for (int dd_idx = 0; dd_idx < pruned_ctx.num_dd; dd_idx++) {
    assert(pruned_ctx.dd_tiers[dd_idx] == 0);
  }

  pat_destroy(pat);
  config_destroy(config);
}

static void test_pat_opening_and_hook_flex(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 15");
  load_and_exec_config_or_die(config, PAT_FLOATER_CGP_CMD);
  const Game *game = config_get_game(config);
  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  const Player *player = game_get_player(game, 0);
  const Square *lanes = board_get_readonly_lanes(board, 0);

  PATWeights *pat = pat_create_zeroed("opening_test");
  pat_set_weight(pat, PAT_FEATURE_FLOAT_SCORE_START + 1, -500);
  pat_prepare_hook_flex(pat, player_get_kwg(player), ld);
  const MachineLetter z_ml = ld_hl_to_ml(ld, "Z");
  const MachineLetter e_ml = ld_hl_to_ml(ld, "E");
  // Two-letter-word flexibility is real for common letters.
  assert(pat_get_hook_flex(pat, e_ml) > 0);
  assert(pat_get_hook_flex(pat, z_ml) > 0);
  assert(pat_get_hook_flex(pat, e_ml) > pat_get_hook_flex(pat, z_ml));

  PATEvalContext pat_eval_ctx;
  pat_eval_context_load(&pat_eval_ctx, pat, lanes, ld, NULL, PAT_CLASS_MASK_ALL,
                        RACK_SIZE);
  // Baseline: the floater E is a 1-point tile two empties from (14,7).
  assert(pat_eval_ctx.pre_penalty == -500);

  // Placing a Z at (14,12) creates a fresh 10-point floater two empties
  // from the TWS at (14,14): the opening play is penalized.
  Move open_move;
  set_single_tile_move(&open_move, z_ml, 14, 12);
  const Equity open_penalty =
      pat_eval_move_penalty(&pat_eval_ctx, &open_move, NULL);
  assert(open_penalty == -500 * (1 + 10));
  assert(open_penalty < pat_eval_ctx.pre_penalty);

  // The same fresh floater's flexibility is approximated with the
  // hook_flex table when the flex bin is weighted.
  pat_set_weight(pat, PAT_FEATURE_FLOAT_SCORE_START + 1, 0);
  pat_set_weight(pat, PAT_FEATURE_FLOAT_FLEX_START + 1, -10);
  pat_eval_context_load(&pat_eval_ctx, pat, lanes, ld, NULL, PAT_CLASS_MASK_ALL,
                        RACK_SIZE);
  const Equity flex_penalty =
      pat_eval_move_penalty(&pat_eval_ctx, &open_move, NULL);
  // Baseline has the board floater E at d = 2; the move adds the fresh Z
  // floater at d = 2 with hook_flex[Z] flexibility.
  assert(flex_penalty ==
         pat_eval_ctx.pre_penalty - 10 * pat_get_hook_flex(pat, z_ml));

  pat_destroy(pat);
  config_destroy(config);
}

// A candidate must get the same PAT contribution through every
// generation path: best-move recording, exhaustive recording (what the
// simmer and the play chooser use), within-margin recording, and each of
// those with WMP on and off. Heavy weights on every channel with both
// floater semantics on, so any path that drops or misreads the term
// shows up as a different equity for the same move.
static void test_pat_path_parity(void) {
  const char *set_cmd =
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1 ";
  Config *config_wmp = config_create_or_die(set_cmd);
  load_and_exec_config_or_die(config_wmp, "set -wmp true");
  Config *config_no_wmp = config_create_or_die(set_cmd);
  load_and_exec_config_or_die(config_no_wmp, "set -wmp false");
  Config *configs[2] = {config_wmp, config_no_wmp};
  PATWeights *pats[2];
  for (int c = 0; c < 2; c++) {
    PATWeights *pat = pat_create_zeroed("path_parity");
    for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
         feature_index++) {
      pat_set_weight(pat, feature_index, -40 - 3 * (feature_index % 7));
    }
    pat_set_combine_gamma(pat, 0.5);
    pat_set_lexicon_floaters(pat, true);
    pat_set_signed_through(pat, true);
    players_data_set_data(config_get_players_data(configs[c]),
                          PLAYERS_DATA_TYPE_PAT, 0, pat);
    players_data_set_data(config_get_players_data(configs[c]),
                          PLAYERS_DATA_TYPE_PAT, 1, pat);
    players_data_set_is_shared(config_get_players_data(configs[c]),
                               PLAYERS_DATA_TYPE_PAT, true);
    pats[c] = pat;
  }
  load_and_exec_config_or_die(
      config_wmp, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  load_and_exec_config_or_die(
      config_no_wmp,
      "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *game_wmp = config_get_game(config_wmp);
  Game *game_no_wmp = config_get_game(config_no_wmp);
  for (int c = 0; c < 2; c++) {
    Game *game = config_get_game(configs[c]);
    pat_prepare_hook_flex(pats[c], player_get_kwg(game_get_player(game, 0)),
                          game_get_ld(game));
    assert(player_get_pat(game_get_player(game, 0)) == pats[c]);
  }

  MoveList *best_list = move_list_create(1);
  MoveList *all_list_wmp = move_list_create(3000);
  MoveList *all_list_no_wmp = move_list_create(3000);
  MoveList *within_list = move_list_create(3000);
  PATEvalContext *parity_ctx = malloc_or_die(sizeof(PATEvalContext));
  double *parity_row = malloc_or_die(sizeof(double) * PAT_NUM_FEATURES);
  int positions_checked = 0;
  int moves_checked = 0;
  int rows_checked = 0;
  for (int attempt = 0; attempt < 40; attempt++) {
    // Positions from seeded self-play in the WMP config, mirrored into
    // the other config through their CGP.
    game_reset(game_wmp);
    game_seed(game_wmp, 4000000ULL + (uint64_t)attempt);
    draw_starting_racks(game_wmp);
    // The setup plays run without the heavy weights (under which passing
    // beats every opening play and the game ends scoreless); they are
    // restored for the checks.
    player_set_pat(game_get_player(game_wmp, 0), NULL);
    player_set_pat(game_get_player(game_wmp, 1), NULL);
    const int plies = attempt % 12;
    for (int ply = 0; ply < plies; ply++) {
      play_move(get_top_equity_move(game_wmp, best_list), game_wmp, NULL);
      if (game_get_game_end_reason(game_wmp) != GAME_END_REASON_NONE) {
        break;
      }
    }
    player_set_pat(game_get_player(game_wmp, 0), pats[0]);
    player_set_pat(game_get_player(game_wmp, 1), pats[0]);
    if (game_get_game_end_reason(game_wmp) != GAME_END_REASON_NONE ||
        bag_get_letters(game_get_bag(game_wmp)) == 0) {
      continue;
    }
    char *cgp = game_get_cgp(game_wmp, true);
    char *cgp_cmd = get_formatted_string("cgp %s", cgp);
    load_and_exec_config_or_die(config_no_wmp, cgp_cmd);
    free(cgp_cmd);
    free(cgp);
    positions_checked++;

    Game *games[2] = {game_wmp, game_no_wmp};
    MoveList *all_lists[2] = {all_list_wmp, all_list_no_wmp};
    for (int c = 0; c < 2; c++) {
      const MoveGenArgs all_args = {
          .game = games[c],
          .move_list = all_lists[c],
          .move_record_type = MOVE_RECORD_ALL,
          .move_sort_type = MOVE_SORT_EQUITY,
          .override_kwg = NULL,
          .eq_margin_movegen = 0,
          .target_equity = EQUITY_MAX_VALUE,
          .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      };
      generate_moves(&all_args);
      move_list_sort_moves(all_lists[c]);
      // Best-move recording: same move, same equity as the exhaustive top.
      const Move *best = get_top_equity_move(games[c], best_list);
      const Move *all_top = move_list_get_move(all_lists[c], 0);
      assert(move_get_equity(best) == move_get_equity(all_top));
      assert(compare_moves_without_equity(best, all_top, true) == -1);
      // Within-margin recording: every move it keeps has the exhaustive
      // list's equity for that move.
      const MoveGenArgs within_args = {
          .game = games[c],
          .move_list = within_list,
          .move_record_type = MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST,
          .move_sort_type = MOVE_SORT_EQUITY,
          .override_kwg = NULL,
          .eq_margin_movegen = int_to_equity(12),
          .target_equity = EQUITY_MAX_VALUE,
          .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      };
      generate_moves(&within_args);
      move_list_sort_moves(within_list);
      const int num_within = move_list_get_count(within_list);
      const int num_all = move_list_get_count(all_lists[c]);
      assert(num_within > 0);
      for (int w = 0; w < num_within; w++) {
        const Move *within_move = move_list_get_move(within_list, w);
        bool found = false;
        for (int a = 0; a < num_all; a++) {
          const Move *all_move = move_list_get_move(all_lists[c], a);
          if (compare_moves_without_equity(within_move, all_move, true) == -1) {
            assert(move_get_equity(within_move) == move_get_equity(all_move));
            found = true;
            break;
          }
        }
        assert(found);
        moves_checked++;
      }
    }
    // The overlay training row dotted with the weights must reproduce
    // the runtime term for the same move (the same units, overlay and
    // combination), to milli-equity rounding.
    {
      const int mover_index = game_get_player_on_turn_index(game_wmp);
      const Player *mover = game_get_player(game_wmp, mover_index);
      const int csi = board_get_cross_set_index(
          game_get_data_is_shared(game_wmp, PLAYERS_DATA_TYPE_KWG),
          mover_index);
      pat_eval_context_load(
          parity_ctx, pats[0],
          board_get_readonly_lanes(game_get_board(game_wmp), csi),
          game_get_ld(game_wmp), player_get_rack(mover), PAT_CLASS_MASK_ALL,
          rack_get_total_letters(
              player_get_rack(game_get_player(game_wmp, 1 - mover_index))));
      pat_eval_context_set_kwg(parity_ctx, player_get_kwg(mover));
      const int num_top = move_list_get_count(all_list_wmp);
      for (int i = 0; i < num_top && i < 20; i++) {
        const Move *move = move_list_get_move(all_list_wmp, i);
        Rack leave;
        get_leave_for_move(move, game_wmp, &leave);
        const double runtime =
            equity_to_double(pat_eval_move_penalty(parity_ctx, move, &leave));
        pat_extract_move_features_combined(parity_ctx, move, parity_row);
        double dot = 0.0;
        for (int f = 0; f < PAT_NUM_FEATURES; f++) {
          dot += equity_to_double(pat_get_weight(pats[0], f)) * parity_row[f];
        }
        assert(fabs(dot - runtime) < 0.002);
        rows_checked++;
      }
    }
    // WMP on and off: identical exhaustive lists, move for move.
    const int num_all = move_list_get_count(all_list_wmp);
    assert(num_all == move_list_get_count(all_list_no_wmp));
    for (int a = 0; a < num_all; a++) {
      const Move *m1 = move_list_get_move(all_list_wmp, a);
      const Move *m2 = move_list_get_move(all_list_no_wmp, a);
      assert(compare_moves_without_equity(m1, m2, true) == -1);
      assert(move_get_equity(m1) == move_get_equity(m2));
    }
  }
  printf("PAT path parity: %d positions, %d within-margin moves, %d overlay "
         "training rows checked\n",
         positions_checked, moves_checked, rows_checked);
  assert(positions_checked >= 30);
  assert(rows_checked >= 300);
  free(parity_ctx);
  free(parity_row);
  move_list_destroy(best_list);
  move_list_destroy(all_list_wmp);
  move_list_destroy(all_list_no_wmp);
  move_list_destroy(within_list);
  config_destroy(config_wmp);
  config_destroy(config_no_wmp);
}

// Stage scaling: a file's stage rows multiply the applied term (and every
// bound on it) by a nonnegative factor chosen from the pre-move bag
// count. Checked end to end through move generation against the same
// weights unscaled and against no PAT at all: at an early position with
// early scale 0.5 every move's equity is the no-PAT equity plus half the
// unscaled term (rounded); at a mid position with mid scale 1 the lists
// are the unscaled ones; at a late position with late scale 0 they are
// the no-PAT ones, best move included, which also exercises the scaled
// pruning bounds.
static void test_pat_stage_scale(const char *data_dir) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1 "
      "-wmp true");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *game = config_get_game(config);
  PATWeights *unscaled = pat_create_zeroed("stage_unscaled");
  PATWeights *scaled = pat_create_zeroed("stage_scaled");
  for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
       feature_index++) {
    pat_set_weight(unscaled, feature_index, -40 - 3 * (feature_index % 7));
    pat_set_weight(scaled, feature_index, -40 - 3 * (feature_index % 7));
  }
  PATWeights *pats[2] = {unscaled, scaled};
  for (int i = 0; i < 2; i++) {
    pat_set_combine_gamma(pats[i], 0.5);
    pat_set_lexicon_floaters(pats[i], true);
    pat_set_signed_through(pats[i], true);
    pat_prepare_hook_flex(pats[i], player_get_kwg(game_get_player(game, 0)),
                          game_get_ld(game));
  }
  pat_set_stage_scale(scaled, PAT_STAGE_EARLY, 0.5);
  pat_set_stage_scale(scaled, PAT_STAGE_MID, 1.0);
  pat_set_stage_scale(scaled, PAT_STAGE_LATE, 0.0);

  // The rows round-trip, and a negative one is rejected.
  ErrorStack *error_stack = error_stack_create();
  pat_write(scaled, data_dir, "stage_scaled", error_stack);
  assert(error_stack_is_empty(error_stack));
  PATWeights *loaded = pat_create(data_dir, "stage_scaled", error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(pat_get_stage_scale(loaded, PAT_STAGE_EARLY) == 0.5);
  assert(pat_get_stage_scale(loaded, PAT_STAGE_MID) == 1.0);
  assert(pat_get_stage_scale(loaded, PAT_STAGE_LATE) == 0.0);
  pat_destroy(loaded);
  error_stack_destroy(error_stack);
  {
    char header[64];
    current_pat_header(header, sizeof(header));
    char *contents = get_formatted_string("%s\nstage_scale_mid,-0.5\n", header);
    assert_pat_create_fails(data_dir, "stage_negative", contents);
    free(contents);
  }
  assert(pat_stage_for_bag(86) == PAT_STAGE_EARLY);
  assert(pat_stage_for_bag(PAT_STAGE_EARLY_MIN_BAG) == PAT_STAGE_EARLY);
  assert(pat_stage_for_bag(PAT_STAGE_EARLY_MIN_BAG - 1) == PAT_STAGE_MID);
  assert(pat_stage_for_bag(PAT_STAGE_MID_MIN_BAG) == PAT_STAGE_MID);
  assert(pat_stage_for_bag(PAT_STAGE_MID_MIN_BAG - 1) == PAT_STAGE_LATE);
  assert(pat_stage_for_bag(1) == PAT_STAGE_LATE);

  MoveList *setup_list = move_list_create(1);
  MoveList *lists[3];
  for (int i = 0; i < 3; i++) {
    lists[i] = move_list_create(3000);
  }
  static const int bag_targets[3] = {70, 45, 20};
  int positions = 0;
  int moves_checked = 0;
  for (int attempt = 0; attempt < 12; attempt++) {
    const int stage = attempt % 3;
    game_reset(game);
    game_seed(game, 5000000ULL + (uint64_t)attempt);
    draw_starting_racks(game);
    player_set_pat(game_get_player(game, 0), NULL);
    player_set_pat(game_get_player(game, 1), NULL);
    while (bag_get_letters(game_get_bag(game)) > bag_targets[stage] &&
           game_get_game_end_reason(game) == GAME_END_REASON_NONE) {
      play_move(get_top_equity_move(game, setup_list), game, NULL);
    }
    if (game_get_game_end_reason(game) != GAME_END_REASON_NONE ||
        bag_get_letters(game_get_bag(game)) == 0) {
      continue;
    }
    assert(pat_stage_for_bag(bag_get_letters(game_get_bag(game))) == stage);
    positions++;
    // 0: no PAT, 1: unscaled, 2: scaled.
    const PATWeights *choices[3] = {NULL, unscaled, scaled};
    for (int i = 0; i < 3; i++) {
      player_set_pat(game_get_player(game, 0), choices[i]);
      player_set_pat(game_get_player(game, 1), choices[i]);
      const MoveGenArgs args = {
          .game = game,
          .move_list = lists[i],
          .move_record_type = MOVE_RECORD_ALL,
          .move_sort_type = MOVE_SORT_EQUITY,
          .override_kwg = NULL,
          .eq_margin_movegen = 0,
          .target_equity = EQUITY_MAX_VALUE,
          .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      };
      generate_moves(&args);
      move_list_sort_moves(lists[i]);
      // Best-move recording (the pruned path) agrees with the exhaustive
      // top under the same weights.
      const Move *best = get_top_equity_move(game, setup_list);
      assert(move_get_equity(best) ==
             move_get_equity(move_list_get_move(lists[i], 0)));
    }
    const int num = move_list_get_count(lists[0]);
    assert(num == move_list_get_count(lists[1]));
    assert(num == move_list_get_count(lists[2]));
    if (stage != PAT_STAGE_EARLY) {
      // Mid: the scaled list is the unscaled one; late: the no-PAT one.
      // Move for move in sorted order, equities included.
      MoveList *expected_list = (stage == PAT_STAGE_MID) ? lists[1] : lists[0];
      for (int a = 0; a < num; a++) {
        const Move *expected = move_list_get_move(expected_list, a);
        const Move *with_scale = move_list_get_move(lists[2], a);
        assert(compare_moves_without_equity(expected, with_scale, true) == -1);
        assert(move_get_equity(expected) == move_get_equity(with_scale));
        moves_checked++;
      }
      continue;
    }
    // Early: half the term. The generator keeps one blank designation per
    // word and square and which one wins can depend on the term, so only
    // moves present in all three lists are compared, and nearly all are.
    int matched = 0;
    for (int a = 0; a < num; a++) {
      const Move *with_scale = move_list_get_move(lists[2], a);
      const Move *none = NULL;
      const Move *plain = NULL;
      for (int b = 0; b < num && (!none || !plain); b++) {
        if (!none &&
            compare_moves_without_equity(
                with_scale, move_list_get_move(lists[0], b), true) == -1) {
          none = move_list_get_move(lists[0], b);
        }
        if (!plain &&
            compare_moves_without_equity(
                with_scale, move_list_get_move(lists[1], b), true) == -1) {
          plain = move_list_get_move(lists[1], b);
        }
      }
      if (!none || !plain) {
        continue;
      }
      const Equity term = move_get_equity(plain) - move_get_equity(none);
      assert(term <= 0);
      assert(move_get_equity(with_scale) ==
             move_get_equity(none) + (Equity)lround(0.5 * term));
      matched++;
      moves_checked++;
    }
    assert(matched * 10 >= num * 9);
  }
  assert(positions >= 9);
  assert(moves_checked > 1000);
  for (int i = 0; i < 3; i++) {
    move_list_destroy(lists[i]);
  }
  move_list_destroy(setup_list);
  player_set_pat(game_get_player(game, 0), NULL);
  player_set_pat(game_get_player(game, 1), NULL);
  pat_destroy(unscaled);
  pat_destroy(scaled);
  config_destroy(config);
}

static void test_pat_movegen_integration(void) {
  // WMP on: its recording path precomputes score-plus-leave equity for
  // nonempty boards and once forgot to add the defense term for
  // MOVE_RECORD_ALL / WITHIN_X lists (best-move recording always had it).
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 5 "
      "-wmp true");
  // Install weights for player 0 before the game is created so the player
  // picks them up. Ownership transfers to players_data.
  PATWeights *pat = pat_create_zeroed("movegen_test");
  players_data_set_data(config_get_players_data(config), PLAYERS_DATA_TYPE_PAT,
                        0, pat);
  load_and_exec_config_or_die(config, PAT_FLOATER_CGP_CMD);
  Game *game = config_get_game(config);
  MoveList *move_list = move_list_create(5);
  const MoveGenArgs move_gen_args = {
      .game = game,
      .move_list = move_list,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  rack_set_to_string(game_get_ld(game),
                     player_get_rack(game_get_player(game, 0)), "QUIXOTE");

  // SortedMoveList aliases the MoveList's move pool, so snapshot the
  // equities before regenerating.
  Equity zero_weight_equities[5];
  generate_moves_for_game(&move_gen_args);
  SortedMoveList *sorted_moves = sorted_move_list_create(move_list);
  const int zero_weight_count = sorted_moves->count;
  for (int move_idx = 0; move_idx < zero_weight_count; move_idx++) {
    zero_weight_equities[move_idx] =
        move_get_equity(sorted_moves->moves[move_idx]);
  }
  sorted_move_list_destroy(sorted_moves);

  // Zero weights must not change anything relative to no weights at all.
  players_data_set_data(config_get_players_data(config), PLAYERS_DATA_TYPE_PAT,
                        0, NULL);
  player_update(config_get_players_data(config), game_get_player(game, 0));
  generate_moves_for_game(&move_gen_args);
  sorted_moves = sorted_move_list_create(move_list);
  assert(sorted_moves->count == zero_weight_count);
  for (int move_idx = 0; move_idx < zero_weight_count; move_idx++) {
    assert(move_get_equity(sorted_moves->moves[move_idx]) ==
           zero_weight_equities[move_idx]);
  }
  sorted_move_list_destroy(sorted_moves);

  // With a real penalty active, no move's equity can exceed the unweighted
  // best (every term is <= 0).
  PATWeights *heavy_pat = pat_test_create_prepared("movegen_heavy", game);
  pat_set_weight(heavy_pat, PAT_FEATURE_TT_FLOATER, -2000);
  pat_set_weight(heavy_pat, PAT_FEATURE_FLOAT_SCORE_START, -500);
  pat_set_weight(heavy_pat, PAT_FEATURE_FLOAT_SCORE_START + 1, -500);
  players_data_set_data(config_get_players_data(config), PLAYERS_DATA_TYPE_PAT,
                        0, heavy_pat);
  player_update(config_get_players_data(config), game_get_player(game, 0));
  generate_moves_for_game(&move_gen_args);
  sorted_moves = sorted_move_list_create(move_list);
  const Equity heavy_all_top = move_get_equity(sorted_moves->moves[0]);
  Move heavy_all_top_move;
  move_copy(&heavy_all_top_move, sorted_moves->moves[0]);
  sorted_move_list_destroy(sorted_moves);
  // The floater board's features fire for these channels, so the penalty
  // is real: strictly below, not merely not above.
  assert(heavy_all_top < zero_weight_equities[0]);
  // And the exhaustive list's top must be exactly what best-only recording
  // finds: same move, same equity, defense term included in both.
  generate_moves_for_game_override_record_type(&move_gen_args,
                                               MOVE_RECORD_BEST);
  const Move *heavy_best = move_list_get_move(move_list, 0);
  assert(move_get_equity(heavy_best) == heavy_all_top);
  assert(compare_moves_without_equity(heavy_best, &heavy_all_top_move, true) ==
         -1);

  move_list_destroy(move_list);
  config_destroy(config);
}

void test_pat(void) {
  char *data_dir = create_temp_pat_data_dir();
  test_pat_feature_names();
  test_pat_round_trip(data_dir);
  test_pat_invalid_files(data_dir);
  test_pat_comments_and_blank_lines(data_dir);
  test_pat_version1_has_no_dls(data_dir);
  test_pat_version2_has_no_scaled_channels(data_dir);
  test_pat_version3_has_no_hook_score_channels(data_dir);
  test_pat_version4_has_no_lm_channels(data_dir);
  test_pat_hook_score_channel();
  test_pat_lm_channels();
  test_pat_stage_scale(data_dir);
  test_pat_transposition_invariance();
  test_pat_blank_floater();
  test_pat_run_through_table();
  test_pat_run_through_scan();
  test_pat_extract_features_floater_board();
  test_pat_lexicon_floaters(data_dir);
  test_pat_signed_through(data_dir);
  test_pat_scan_reach_capped_by_opponent_rack_size();
  test_pat_move_penalty();
  test_pat_dls_features_land_in_dls_channels();
  test_pat_hook_scaled_channel();
  test_pat_own_asset_discount_clamped();
  test_pat_own_asset_discount();
  test_pat_opening_penalty_gating();
  test_pat_unweighted_units_dropped();
  test_pat_opening_and_hook_flex();
  test_pat_movegen_integration();
  test_pat_path_parity();
  free(data_dir);
}
