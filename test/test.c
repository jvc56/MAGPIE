#include "../src/def/board_defs.h"
#include "../src/impl/exec.h"
#include "../src/util/io_util.h"
#include "alias_method_test.h"
#include "alphabet_test.h"
#include "autoplay_test.h"
#include "bag_test.h"
#include "bai_test.h"
#include "benchmark_endgame_test.h"
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
#include "game_test.h"
#include "gameplay_test.h"
#include "gcg_test.h"
#include "heat_map_test.h"
#include "infer_cmp_test.h"
#include "infer_test.h"
#include "klv_test.h"
#include "kwg_alpha_test.h"
#include "kwg_maker_test.h"
#include "leave_map_test.h"
#include "leaves_test.h"
#include "letter_distribution_test.h"
#include "load_gcg_test.h"
#include "math_util_test.h"
#include "move_gen_test.h"
#include "move_test.h"
#include "players_data_test.h"
#include "rack_list_test.h"
#include "rack_test.h"
#include "random_variable_test.h"
#include "shadow_test.h"
#include "sim_test.h"
#include "stats_test.h"
#include "string_util_test.h"
#include "transposition_table_test.h"
#include "validated_move_test.h"
#include "wasm_api_test.h"
#include "win_pct_test.h"
#include "wmp_maker_test.h"
#include "wmp_move_gen_test.h"
#include "wmp_test.h"
#include "word_prune_test.h"
#include "word_test.h"
#include "zobrist_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    {"rv", test_random_variable},
    {"am", test_alias_method},
    {"hm", test_heat_map},
    {"sim", test_sim},
    {"math", test_math_util},
    {"bai", test_bai},
    {"command", test_command},
    {"gcg", test_gcg},
    {"autoplay", test_autoplay},
    {"wasm", test_wasm_api},
    {"words", test_words},
    {"wordprune", test_word_prune},
    {"kwgmaker", test_kwg_maker},
    {"cgp", test_cgp},
    {"rl", test_rack_list},
    {"ch", test_checkpoint},
    {"klv", test_klv},
    {"cv", test_convert},
    {"cd", test_create_data},
    {"wmp", test_wmp},
    {"wmpmaker", test_wmp_maker},
    {"wmg", test_wmp_move_gen},
    {"winpct", test_win_pct},
    {"endgame", test_endgame},
    {"benchend", test_benchmark_endgame},
    {"benchendmg", test_benchmark_endgame_multi_ply},
    {"zobrist", test_zobrist},
    {"tt", test_transposition_table},
    {"load", test_load_gcg},
    {NULL, NULL} // Sentinel value to mark end of array
};

// Tests that only run when explicitly requested (not included in run_all)
static TestEntry on_demand_test_table[] = {
    {"infercmp", test_infer_cmp},
    {NULL, NULL} // Sentinel value to mark end of array
};

void run_all(void) {
  for (int i = 0; test_table[i].name != NULL; ++i) {
    printf("Running test %s\n", test_table[i].name);
    test_table[i].func();
  }
}

void run_test(const char *subtest) {
  for (int i = 0; test_table[i].name != NULL; ++i) {
    if (strcmp(subtest, test_table[i].name) == 0) {
      test_table[i].func();
      return;
    }
  }
  // Check on-demand tests (not run by run_all)
  for (int i = 0; on_demand_test_table[i].name != NULL; ++i) {
    if (strcmp(subtest, on_demand_test_table[i].name) == 0) {
      on_demand_test_table[i].func();
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

  // NOLINTNEXTLINE(misc-redundant-expression)
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