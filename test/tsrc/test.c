#include "../../src/impl/exec.h"

#include "../../src/util/log.h"
#include "../../src/util/string_util.h"

#include "alphabet_test.h"
#include "autoplay_test.h"
#include "bag_test.h"
#include "bit_rack_test.h"
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
#include "endgame_test.h"
#include "equity_adjustment_test.h"
#include "equity_test.h"
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
#include "transposition_table_test.h"
#include "validated_move_test.h"
#include "wasm_api_test.h"
#include "wmp_maker_test.h"
#include "wmp_test.h"
#include "word_prune_test.h"
#include "word_test.h"
#include "zobrist_test.h"

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
  test_bit_rack();
  test_board();
  test_board_layout_default();
  test_cross_set();
  test_equity();
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
  test_wmp_maker();
  test_wmp();
  test_file_handler();
  test_leave_list();
  test_convert();
  test_create_data();
  test_endgame();
  test_zobrist();
  test_transposition_table();
}

typedef void (*TestFunc)(void);

typedef struct {
  const char *name;
  TestFunc func;
} TestEntry;

static TestEntry test_table[] = {
    {"config", test_config},
    {"players", test_players_data},
    {"string", test_string_util},
    {"alpha", test_alphabet},
    {"ld", test_ld},
    {"l", test_leaves},
    {"leavemap", test_leave_map},
    {"kwg", test_kwg_alpha},
    {"bag", test_bag},
    {"rack", test_rack},
    {"bitrack", test_bit_rack},
    {"board", test_board},
    {"layout", test_board_layout_default},
    {"cs", test_cross_set},
    {"move", test_move},
    {"game", test_game},
    {"vm", test_validated_move},
    {"shadow", test_shadow},
    {"movegen", test_move_gen},
    {"eq", test_equity},
    {"eqadj", test_equity_adjustments},
    {"gameplay", test_gameplay},
    {"stats", test_stats},
    {"infer", test_infer},
    {"sim", test_sim},
    {"command", test_command},
    {"gcg", test_gcg},
    {"autoplay", test_autoplay},
    {"wasm", test_wasm_api},
    {"words", test_words},
    {"wordprune", test_word_prune},
    {"kwgmaker", test_kwg_maker},
    {"fh", test_file_handler},
    {"cgp", test_cgp},
    {"ll", test_leave_list},
    {"ch", test_checkpoint},
    {"klv", test_klv},
    {"cv", test_convert},
    {"cd", test_create_data},
    {"wmp", test_wmp},
    {"wmpmaker", test_wmp_maker},
    {"endgame", test_endgame},
    {"zobrist", test_zobrist},
    {"tt", test_transposition_table},
    {NULL, NULL} // Sentinel value to mark end of array
};

void run_test(const char *subtest) {
  for (int i = 0; test_table[i].name != NULL; ++i) {
    if (strcmp(subtest, test_table[i].name) == 0) {
      test_table[i].func();
      return;
    }
  }
  log_fatal("unrecognized test: %s\n", subtest);
}

void run_all_super(void) {
  test_bit_rack();
  test_board_layout_super();
}

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