#include "pat_test.h"

#include "../src/def/board_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/pat_defs.h"
#include "../src/def/players_data_defs.h"
#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/pat.h"
#include "../src/ent/player.h"
#include "../src/ent/players_data.h"
#include "../src/ent/rack.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
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
    if (feature_index >= PAT_FEATURE_DLS_HOOK_START &&
        feature_index < PAT_FEATURE_QWS_HOOK_START) {
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
  assert(pat_get_weight(loaded, PAT_FEATURE_HOOK_START) == -1);
  assert(pat_get_weight(loaded, PAT_FEATURE_QWS_HOOK_START) ==
         -(PAT_FEATURE_QWS_HOOK_START + 1));
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
  pat_extract_features(lanes, ld, NULL, NULL, features);

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

static void test_pat_move_penalty(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 15");
  load_and_exec_config_or_die(config, PAT_FLOATER_CGP_CMD);
  const Game *game = config_get_game(config);
  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  const Square *lanes = board_get_readonly_lanes(board, 0);
  int32_t features[PAT_NUM_FEATURES];
  pat_extract_features(lanes, ld, NULL, NULL, features);
  const int32_t float_flex_d5 = features[PAT_FEATURE_FLOAT_FLEX_START + 4];

  PATWeights *pat = pat_create_zeroed("penalty_test");
  pat_set_weight(pat, PAT_FEATURE_TT_FLOATER, -1000);
  pat_set_weight(pat, PAT_FEATURE_FLOAT_FLEX_START + 1, -100);
  pat_set_weight(pat, PAT_FEATURE_FLOAT_FLEX_START + 4, -100);

  PATEvalContext pat_eval_ctx;
  pat_eval_context_load(&pat_eval_ctx, pat, lanes, ld, NULL,
                        PAT_CLASS_MASK_ALL);
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
  assert(pat_eval_move_penalty(&pat_eval_ctx, &far_move) ==
         pat_eval_ctx.pre_penalty);

  // An exchange also carries the baseline.
  Move exchange_move;
  MachineLetter exchange_strip[1] = {x_ml};
  move_set_all_except_equity(&exchange_move, exchange_strip, 0, 0, 0, 0, 0, 1,
                             BOARD_HORIZONTAL_DIRECTION, GAME_EVENT_EXCHANGE);
  assert(pat_eval_move_penalty(&pat_eval_ctx, &exchange_move) ==
         pat_eval_ctx.pre_penalty);

  // Covering the TWS at (14,7) kills both triple-triple counts and the
  // d = 2 floater unit; only the d = 5 floater from (14,0) survives (the
  // fresh tile becomes a d = 6 floater, whose bins are unweighted here and
  // whose flex approximation is zero with an unprepared hook_flex table).
  Move block_move;
  set_single_tile_move(&block_move, x_ml, 14, 7);
  const Equity block_penalty =
      pat_eval_move_penalty(&pat_eval_ctx, &block_move);
  assert(block_penalty == -100 * float_flex_d5);
  assert(block_penalty > pat_eval_ctx.pre_penalty);
  assert(block_penalty <= 0);

  // With zero weights everything is zero.
  PATWeights *zero_pat = pat_create_zeroed("zero_test");
  PATEvalContext zero_ctx;
  pat_eval_context_load(&zero_ctx, zero_pat, lanes, ld, NULL,
                        PAT_CLASS_MASK_ALL);
  assert(zero_ctx.pre_penalty == 0);
  assert(pat_eval_move_penalty(&zero_ctx, &block_move) == 0);
  assert(pat_eval_move_penalty(&zero_ctx, &far_move) == 0);

  // A disabled or NULL context is exactly zero.
  PATEvalContext disabled_ctx;
  pat_eval_context_disable(&disabled_ctx);
  assert(pat_eval_move_penalty(&disabled_ctx, &block_move) == 0);
  assert(pat_eval_move_penalty(NULL, &block_move) == 0);

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
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  const Game *game = config_get_game(config);
  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  const Square *lanes = board_get_readonly_lanes(board, 0);

  PATWeights *pat = pat_create_zeroed("dls_only");
  pat_set_weight(pat, PAT_FEATURE_DLS_HOOK_START, -1000);

  PATEvalContext ctx;
  pat_eval_context_load(&ctx, pat, lanes, ld, NULL, PAT_CLASS_MASK_ALL);
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
  PATWeights *pat = pat_create_zeroed("drop_test");
  pat_set_weight(pat, PAT_FEATURE_FLOAT_SCORE_START + 1, -500);
  pat_set_weight(pat, PAT_FEATURE_HOOK_START, -20);

  PATEvalContext pruned_ctx;
  pat_eval_context_load(&pruned_ctx, pat, lanes, ld, NULL, PAT_CLASS_MASK_ALL);
  PATEvalContext full_ctx;
  pat_eval_context_load_all_units(&full_ctx, pat, lanes, ld, NULL);
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
      assert(pat_eval_move_penalty(&pruned_ctx, &move) ==
             pat_eval_move_penalty(&full_ctx, &move));
      assert(pat_eval_move_penalty_bound(&pruned_ctx, &move) ==
             pat_eval_move_penalty_bound(&full_ctx, &move));
    }
  }

  // A weight on a double-word channel brings those squares back, and one
  // on a double-double channel brings the windows back.
  pat_set_weight(pat, PAT_FEATURE_DWS_HOOK_START, -10);
  pat_eval_context_load(&pruned_ctx, pat, lanes, ld, NULL, PAT_CLASS_MASK_ALL);
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
  pat_eval_context_load(&pruned_ctx, pat, lanes, ld, NULL, PAT_CLASS_MASK_ALL);
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
  pat_eval_context_load(&pat_eval_ctx, pat, lanes, ld, NULL,
                        PAT_CLASS_MASK_ALL);
  // Baseline: the floater E is a 1-point tile two empties from (14,7).
  assert(pat_eval_ctx.pre_penalty == -500);

  // Placing a Z at (14,12) creates a fresh 10-point floater two empties
  // from the TWS at (14,14): the opening play is penalized.
  Move open_move;
  set_single_tile_move(&open_move, z_ml, 14, 12);
  const Equity open_penalty = pat_eval_move_penalty(&pat_eval_ctx, &open_move);
  assert(open_penalty == -500 * (1 + 10));
  assert(open_penalty < pat_eval_ctx.pre_penalty);

  // The same fresh floater's flexibility is approximated with the
  // hook_flex table when the flex bin is weighted.
  pat_set_weight(pat, PAT_FEATURE_FLOAT_SCORE_START + 1, 0);
  pat_set_weight(pat, PAT_FEATURE_FLOAT_FLEX_START + 1, -10);
  pat_eval_context_load(&pat_eval_ctx, pat, lanes, ld, NULL,
                        PAT_CLASS_MASK_ALL);
  const Equity flex_penalty = pat_eval_move_penalty(&pat_eval_ctx, &open_move);
  // Baseline has the board floater E at d = 2; the move adds the fresh Z
  // floater at d = 2 with hook_flex[Z] flexibility.
  assert(flex_penalty ==
         pat_eval_ctx.pre_penalty - 10 * pat_get_hook_flex(pat, z_ml));

  pat_destroy(pat);
  config_destroy(config);
}

static void test_pat_movegen_integration(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 5");
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
  PATWeights *heavy_pat = pat_create_zeroed("movegen_heavy");
  pat_set_weight(heavy_pat, PAT_FEATURE_TT_FLOATER, -2000);
  pat_set_weight(heavy_pat, PAT_FEATURE_FLOAT_SCORE_START, -500);
  pat_set_weight(heavy_pat, PAT_FEATURE_FLOAT_SCORE_START + 1, -500);
  players_data_set_data(config_get_players_data(config), PLAYERS_DATA_TYPE_PAT,
                        0, heavy_pat);
  player_update(config_get_players_data(config), game_get_player(game, 0));
  generate_moves_for_game(&move_gen_args);
  sorted_moves = sorted_move_list_create(move_list);
  assert(move_get_equity(sorted_moves->moves[0]) <= zero_weight_equities[0]);
  sorted_move_list_destroy(sorted_moves);

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
  test_pat_extract_features_floater_board();
  test_pat_move_penalty();
  test_pat_dls_features_land_in_dls_channels();
  test_pat_unweighted_units_dropped();
  test_pat_opening_and_hook_flex();
  test_pat_movegen_integration();
  free(data_dir);
}
