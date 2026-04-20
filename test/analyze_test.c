#include "analyze_test.h"

#include "../src/impl/config.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *SINGLE_GCG_PATH = "testdata/gcgs/success.gcg";
static const char *SINGLE_REPORT_PATH = "testdata/gcgs/success_report.txt";
// When a game is loaded from a string or file without an explicit filename,
// the report path is derived from player nicknames.
static const char *PLAYER_NAMES_REPORT_PATH = "Tim-vs-Josh_report.txt";

// Minimal GCG with a two-player game header for constructing inline GCGs.
// Player names: "Tim" (nick "Tim") and "Josh" (nick "Josh").
static const char *MINIMAL_GCG_HEADER = "#character-encoding UTF-8\n"
                                        "#player1 Tim Tim\n"
                                        "#player2 Josh Josh\n";

// Creates a temp directory and writes a single GCG file into it for directory
// tests. Returns the directory path (caller must free). The file is removed by
// the caller.
static char *make_temp_gcg_dir(void) {
  const char *tmp_dir = "/tmp/magpie_analyze_test_dir";
  (void)mkdir(tmp_dir, 0755);

  const char *gcg_content = "#character-encoding UTF-8\n"
                            "#player1 Tim Tim\n"
                            "#player2 Josh Josh\n"
                            ">Tim: AEITW 8D WAITE +24 24\n"
                            ">Josh: DEEFINO 7C DEFO +22 22\n";
  char *gcg_path = get_formatted_string("%s/test_game.gcg", tmp_dir);
  ErrorStack *es = error_stack_create();
  write_string_to_file(gcg_path, "w", gcg_content, es);
  assert(error_stack_is_empty(es));
  error_stack_destroy(es);
  free(gcg_path);
  return string_duplicate(tmp_dir);
}

static void remove_temp_gcg_dir(const char *dir_path) {
  char *gcg_path = get_formatted_string("%s/test_game.gcg", dir_path);
  char *report_path = get_formatted_string("%s/test_game_report.txt", dir_path);
  (void)remove(gcg_path);
  (void)remove(report_path);
  free(gcg_path);
  free(report_path);
  (void)rmdir(dir_path);
}

// PATH D1: single GCG file as argument (CSV output, plies=0).
static void test_analyze_single_file(void) {
  Config *config = config_create_or_die("set -lex CSW21 -plies 0");
  (void)remove(SINGLE_REPORT_PATH);
  char *cmd = get_formatted_string("analyze %s", SINGLE_GCG_PATH);
  assert_config_exec_status(config, cmd, ERROR_STATUS_SUCCESS);
  free(cmd);
  char *report = get_string_from_file_or_die(SINGLE_REPORT_PATH);
  assert(has_substring(
      report, "turn,player,rack,actual,best,equity_lost,win_pct_lost"));
  free(report);
  remove_or_die(SINGLE_REPORT_PATH);
  config_destroy(config);
}

// PATH A: 0-arg analyze with a game already loaded (CSV output).
static void test_analyze_zero_args(void) {
  Config *config = config_create_or_die("set -lex CSW21 -plies 0");
  load_game_history_with_gcg(config, "success");
  (void)remove(PLAYER_NAMES_REPORT_PATH);
  assert_config_exec_status(config, "analyze", ERROR_STATUS_SUCCESS);
  (void)remove(PLAYER_NAMES_REPORT_PATH);
  config_destroy(config);
}

// PATH A + human readable: 0-arg with -hr true produces a report with summary.
static void test_analyze_zero_args_human_readable(void) {
  Config *config = config_create_or_die("set -lex CSW21 -plies 0 -hr true");
  load_game_history_with_gcg_string(config, MINIMAL_GCG_HEADER,
                                    ">Tim: AEITW 8D WAITE +24 24\n"
                                    ">Josh: DEEFINO 7C DEFO +22 22\n");
  (void)remove(PLAYER_NAMES_REPORT_PATH);
  assert_config_exec_status(config, "analyze", ERROR_STATUS_SUCCESS);
  char *report = get_string_from_file_or_die(PLAYER_NAMES_REPORT_PATH);
  assert(has_substring(report, "=== Game Summary: Tim ==="));
  assert(has_substring(report, "=== Game Summary: Josh ==="));
  free(report);
  remove_or_die(PLAYER_NAMES_REPORT_PATH);
  config_destroy(config);
}

// PATH D2: 1-arg is a player name (game already loaded).
static void test_analyze_player_filter_loaded_game(void) {
  Config *config = config_create_or_die("set -lex CSW21 -plies 0");
  load_game_history_with_gcg(config, "success");
  // "Josh" is a player nickname in success.gcg; with new logic, only Josh is
  // analyzed. "UnknownExtra" is silently ignored (at least one name matched).
  assert_config_exec_status(config, "analyze Josh", ERROR_STATUS_SUCCESS);
  (void)remove(PLAYER_NAMES_REPORT_PATH);
  config_destroy(config);
}

// PATH D2: 1-arg player name list with one unknown name — should still succeed
// because at least one name ("Tim") matches.
static void test_analyze_player_filter_partial_match(void) {
  Config *config = config_create_or_die("set -lex CSW21 -plies 0");
  load_game_history_with_gcg(config, "success");
  assert_config_exec_status(config, "analyze Tim,UnknownExtra",
                            ERROR_STATUS_SUCCESS);
  (void)remove(PLAYER_NAMES_REPORT_PATH);
  config_destroy(config);
}

// PATH C: 2-arg — explicit GCG source + player filter.
static void test_analyze_gcg_and_player(void) {
  Config *config = config_create_or_die("set -lex CSW21 -plies 0");
  (void)remove(SINGLE_REPORT_PATH);
  char *cmd = get_formatted_string("analyze %s Tim", SINGLE_GCG_PATH);
  assert_config_exec_status(config, cmd, ERROR_STATUS_SUCCESS);
  free(cmd);
  (void)remove(SINGLE_REPORT_PATH);
  config_destroy(config);
}

// PATH B1: directory argument with no player filter.
static void test_analyze_directory(void) {
  Config *config = config_create_or_die("set -lex CSW21 -plies 0");
  char *tmp_dir = make_temp_gcg_dir();
  char *cmd = get_formatted_string("analyze %s", tmp_dir);
  assert_config_exec_status(config, cmd, ERROR_STATUS_SUCCESS);
  free(cmd);
  remove_temp_gcg_dir(tmp_dir);
  free(tmp_dir);
  config_destroy(config);
}

// PATH B2: directory argument with player filter.
static void test_analyze_directory_with_player(void) {
  Config *config = config_create_or_die("set -lex CSW21 -plies 0");
  char *tmp_dir = make_temp_gcg_dir();
  char *cmd = get_formatted_string("analyze %s Tim", tmp_dir);
  assert_config_exec_status(config, cmd, ERROR_STATUS_SUCCESS);
  free(cmd);
  remove_temp_gcg_dir(tmp_dir);
  free(tmp_dir);
  config_destroy(config);
}

// Error: no lexicon loaded — GCG parse fails before we can check game data.
static void test_analyze_no_lexicon(void) {
  Config *config = config_create_default_test();
  assert_config_exec_status(config, "analyze testdata/gcgs/success.gcg",
                            ERROR_STATUS_GCG_PARSE_LEXICON_NOT_SPECIFIED);
  config_destroy(config);
}

// Error: 0-arg analyze with no game loaded.
static void test_analyze_no_game(void) {
  Config *config = config_create_or_die("set -lex CSW21 -plies 0");
  assert_config_exec_status(config, "analyze",
                            ERROR_STATUS_CONFIG_LOAD_GAME_HISTORY_ERROR);
  config_destroy(config);
}

// Error: player list where no name matches any player.
static void test_analyze_unknown_player(void) {
  Config *config = config_create_or_die("set -lex CSW21 -plies 0");
  load_game_history_with_gcg(config, "success");
  assert_config_exec_status(config, "analyze UnknownPlayer1,UnknownPlayer2",
                            ERROR_STATUS_CONFIG_LOAD_MALFORMED_PLAYER_NAME);
  config_destroy(config);
}

// Transposability: vertical opening on a symmetric board is treated as the
// horizontal equivalent so the actual play ranks as best.
static void test_analyze_vertical_opening_transposable(void) {
  Config *config = config_create_or_die("set -lex CSW21 -plies 0");
  load_game_history_with_gcg_string(config, MINIMAL_GCG_HEADER,
                                    ">Tim: QIRESIT H7 QI +22 22\n");
  (void)remove(PLAYER_NAMES_REPORT_PATH);
  assert_config_exec_status(config, "analyze", ERROR_STATUS_SUCCESS);
  char *report = get_string_from_file_or_die(PLAYER_NAMES_REPORT_PATH);
  assert(has_substring(report, "H7 QI,-,0.00"));
  free(report);
  remove_or_die(PLAYER_NAMES_REPORT_PATH);
  config_destroy(config);
}

void test_analyze(void) {
  test_analyze_single_file();
  test_analyze_zero_args();
  test_analyze_zero_args_human_readable();
  test_analyze_player_filter_loaded_game();
  test_analyze_player_filter_partial_match();
  test_analyze_gcg_and_player();
  test_analyze_directory();
  test_analyze_directory_with_player();
  test_analyze_no_lexicon();
  test_analyze_no_game();
  test_analyze_unknown_player();
  test_analyze_vertical_opening_transposable();
}

// Slow test: plies=2 simulation on a real GCG. Invoked only when explicitly
// requested (e.g., ./bin/magpie_test analyze_sim).
void test_analyze_sim(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -plies 2 -hr true -iter 50 -numplays 5");
  assert_config_exec_status(config, "analyze testdata/gcgs/success.gcg",
                            ERROR_STATUS_SUCCESS);
  config_destroy(config);
}
