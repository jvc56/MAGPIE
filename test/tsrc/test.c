#include "../../src/impl/exec.h"

#include "../../src/util/log.h"
#include "../../src/util/string_util.h"

#include "alphabet_test.h"
#include "autoplay_test.h"
#include "bag_test.h"
#include "board_layout_default_test.h"
#include "board_layout_super_test.h"
#include "board_test.h"
#include "cgp_test.h"
#include "checkpoint_test.h"
#include "command_test.h"
#include "config_test.h"
#include "convert_test.h"
#include "create_data_test.h"
#include "cross_set_test.h"
#include "equity_adjustment_test.h"
#include "file_handler_test.h"
#include "game_test.h"
#include "gameplay_test.h"
#include "gcg_test.h"
#include "infer_test.h"
#include "klv_test.h"
#include "kwg_alpha_test.h"
#include "kwg_maker_test.h"
#include "leave_list_test.h"
#include "leave_map_test.h"
#include "leaves_test.h"
#include "letter_distribution_test.h"
#include "move_gen_test.h"
#include "move_test.h"
#include "players_data_test.h"
#include "rack_test.h"
#include "shadow_test.h"
#include "sim_test.h"
#include "stats_test.h"
#include "string_util_test.h"
#include "validated_move_test.h"
#include "wasm_api_test.h"
#include "word_prune_test.h"
#include "word_test.h"

void run_all(void) {
  // Test the loading of the config
  test_players_data();
  test_config();

  // Test the readonly data first
  test_string_util();
  test_alphabet();
  test_ld();
  test_leaves();
  test_leave_map();
  test_kwg_alpha();
  test_checkpoint();

  // Now test the rest
  test_bag();
  test_rack();
  test_board();
  test_board_layout_default();
  test_cross_set();
  test_move();
  test_cgp();
  test_klv();
  test_game();
  test_validated_move();
  test_shadow();
  test_move_gen();
  test_equity_adjustments();
  test_gameplay();
  test_stats();
  test_infer();
  test_sim();
  test_command();
  test_gcg();
  test_autoplay();
  test_wasm_api();
  test_words();
  test_word_prune();
  test_kwg_maker();
  test_file_handler();
  test_leave_list();
  test_convert();
  test_create_data();
}

void run_test(const char *subtest) {
  if (strings_equal(subtest, "config")) {
    test_config();
  } else if (strings_equal(subtest, "players")) {
    test_players_data();
  } else if (strings_equal(subtest, "string")) {
    test_string_util();
  } else if (strings_equal(subtest, "alpha")) {
    test_alphabet();
  } else if (strings_equal(subtest, "ld")) {
    test_ld();
  } else if (strings_equal(subtest, "l")) {
    test_leaves();
  } else if (strings_equal(subtest, "leavemap")) {
    test_leave_map();
  } else if (strings_equal(subtest, "kwg")) {
    test_kwg_alpha();
  } else if (strings_equal(subtest, "bag")) {
    test_bag();
  } else if (strings_equal(subtest, "rack")) {
    test_rack();
  } else if (strings_equal(subtest, "board")) {
    test_board();
  } else if (strings_equal(subtest, "layout")) {
    test_board_layout_default();
  } else if (strings_equal(subtest, "cs")) {
    test_cross_set();
  } else if (strings_equal(subtest, "move")) {
    test_move();
  } else if (strings_equal(subtest, "game")) {
    test_game();
  } else if (strings_equal(subtest, "vm")) {
    test_validated_move();
  } else if (strings_equal(subtest, "shadow")) {
    test_shadow();
  } else if (strings_equal(subtest, "movegen")) {
    test_move_gen();
  } else if (strings_equal(subtest, "eq")) {
    test_equity_adjustments();
  } else if (strings_equal(subtest, "gameplay")) {
    test_gameplay();
  } else if (strings_equal(subtest, "stats")) {
    test_stats();
  } else if (strings_equal(subtest, "infer")) {
    test_infer();
  } else if (strings_equal(subtest, "sim")) {
    test_sim();
  } else if (strings_equal(subtest, "command")) {
    test_command();
  } else if (strings_equal(subtest, "gcg")) {
    test_gcg();
  } else if (strings_equal(subtest, "autoplay")) {
    test_autoplay();
  } else if (strings_equal(subtest, "wasm")) {
    test_wasm_api();
  } else if (strings_equal(subtest, "words")) {
    test_words();
  } else if (strings_equal(subtest, "wordprune")) {
    test_word_prune();
  } else if (strings_equal(subtest, "kwgmaker")) {
    test_kwg_maker();
  } else if (strings_equal(subtest, "fh")) {
    test_file_handler();
  } else if (strings_equal(subtest, "cgp")) {
    test_cgp();
  } else if (strings_equal(subtest, "ll")) {
    test_leave_list();
  } else if (strings_equal(subtest, "ch")) {
    test_checkpoint();
  } else if (strings_equal(subtest, "klv")) {
    test_klv();
  } else if (strings_equal(subtest, "cv")) {
    test_convert();
  } else if (strings_equal(subtest, "cd")) {
    test_create_data();
  } else {
    log_fatal("unrecognized test: %s\n", subtest);
  }
}

void run_all_super(void) { test_board_layout_super(); }

int main(int argc, char *argv[]) {
  log_set_level(LOG_WARN);

  if (BOARD_DIM == DEFAULT_BOARD_DIM) {
    if (argc == 1) {
      run_all();
    } else {
      for (int i = 1; i < argc; i++) {
        run_test(argv[i]);
      }
    }
  } else if (BOARD_DIM == DEFAULT_SUPER_BOARD_DIM) {
    if (argc > 1) {
      log_warn("Ignoring test arguments when testing default super board "
               "dimensions of %d.",
               DEFAULT_SUPER_BOARD_DIM);
    }
    run_all_super();
  } else {
    log_fatal(
        "Testing with unsupported board dimension of %d. Only %d and %d are "
        "supported for testing.",
        BOARD_DIM, DEFAULT_BOARD_DIM, DEFAULT_SUPER_BOARD_DIM);
  }

  caches_destroy();
}