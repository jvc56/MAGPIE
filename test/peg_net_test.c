#include "peg_net_test.h"

#include "../src/compat/cfile.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "peg_judge.h"
#include "peg_net_data_test.h"
#include "peg_net_eval_test.h"
#include <stdbool.h>
#include <stdlib.h>

// The environment variables' names before the harness left value_net_test.c's
// VNT_ prefix behind. A run that still sets one stops rather than silently
// running with the default.
static void peg_net_check_renamed_env(void) {
  static const char *const renamed[][2] = {
      {"VNT_EBESTREPLY", "PEG_NET_BEST_REPLY"},
      {"VNT_EXACT_CAP_S", "PEG_EXACT_CAP_S"},
      {"VNT_PEG2_MAX_STAGE", "PEG2_PICK_MAX_STAGE"},
      {"VNT_PEG2_BUDGET_S", "PEG2_PICK_BUDGET_S"},
      {"VNT_PEG_TRACE", "PEG_PICK_TRACE"},
      {"VNT_PEG2_RANKS", "PEG2_CANDIDATE_LIST"},
      {"VNT_PEG2_ONLY", "PEG2_EVAL_ONLY"},
      {"VNT_PEG2_NO_CACHE", "PEG_WORLD_NO_CACHE"},
      {"VNT_PEG_NEST_DEPTH", "PEG3_EVAL_NEST_DEPTH"},
      {"VNT_PEG_THREADS", "PEG3_EVAL_THREADS"},
      {"VNT_ORACLE_THREADS", "PEG_JUDGE_THREADS"},
      {"VNT_ORACLE_BAG", "PEG_JUDGE_BAG"},
      {"VNT_ORACLE_SAMPLES", "PEG_JUDGE_SAMPLES"},
      {"VNT_ORACLE_SEED", "PEG_JUDGE_SEED"},
      {"VNT_SIM_BAG", "PEG_SIM_BAG"},
      {"VNT_SIM_THREADS", "PEG_SIM_THREADS"},
      {"VNT_SIM_SEED", "PEG_SIM_SEED"},
      {"VNT_SIM_ARMS", "PEG_SIM_ARMS"},
      {"VNT_SIM_EG_PLIES", "PEG_SIM_EG_PLIES"},
      {"VNT_SIM_CHECK", "PEG_SIM_CHECK"},
      {"VNT_PB_MAX_STAGE", "PEG_BUDGET_MAX_STAGE"},
      {"VNT_PB_THREADS", "PEG_BUDGET_THREADS"},
      {"VNT_PB_ARMS", "PEG_BUDGET_ARMS"},
      {"VNT_POS_BAG", "PEG_POSITIONS_BAG"},
  };
  for (size_t idx = 0; idx < sizeof(renamed) / sizeof(renamed[0]); idx++) {
    if (getenv(renamed[idx][0]) != NULL) {
      log_fatal("%s is now %s", renamed[idx][0], renamed[idx][1]);
    }
  }
}

void peg_net_test_run_spec(const char *spec) {
  peg_net_check_renamed_env();
  StringSplitter *fields = split_string(spec, ':', true);
  const char *mode = string_splitter_get_item(fields, 0);
  if (strings_equal(mode, "peg1data")) {
    peg1_labels_mode(fields);
  } else if (strings_equal(mode, "peg1roots")) {
    peg1_roots_mode(fields);
  } else if (strings_equal(mode, "peg1play")) {
    peg1_play_mode(fields);
  } else if (strings_equal(mode, "peg1feat2")) {
    peg1_features_v2_mode(fields);
  } else if (strings_equal(mode, "peg1pos")) {
    peg1_positions_mode(fields);
  } else if (strings_equal(mode, "peg1deep")) {
    peg1_deep_labels_mode(fields);
  } else if (strings_equal(mode, "peg1util")) {
    peg1_utility_net_eval_mode(fields);
  } else if (strings_equal(mode, "peg1pred")) {
    peg1_utility_net_predict_mode(fields);
  } else if (strings_equal(mode, "peg1games3")) {
    peg1_game_pairs_mode(fields);
  } else if (strings_equal(mode, "peg1greedy")) {
    peg1_greedy_seed_eval_mode(fields);
  } else if (strings_equal(mode, "peg1exact")) {
    peg1_exact_eval_mode(fields);
  } else if (strings_equal(mode, "peg2pos")) {
    peg2_positions_mode(fields);
  } else if (strings_equal(mode, "peg2data")) {
    peg2_labels_mode(fields);
  } else if (strings_equal(mode, "peg2eval")) {
    peg2_exact_eval_mode(fields);
  } else if (strings_equal(mode, "peg2feat")) {
    peg2_features_mode(fields);
  } else if (strings_equal(mode, "peg2pick")) {
    peg2_pick_eval_mode(fields);
  } else if (strings_equal(mode, "peg2wide")) {
    peg2_wide_pick_eval_mode(fields);
  } else if (strings_equal(mode, "peg3pos")) {
    peg3_positions_mode(fields);
  } else if (strings_equal(mode, "peg3eval")) {
    peg3_exact_eval_mode(fields);
  } else if (strings_equal(mode, "pegsim")) {
    peg_sim_mode(fields);
  } else if (strings_equal(mode, "pegsimbai")) {
    peg_sim_bai_replay_mode(fields);
  } else if (strings_equal(mode, "pegnpos")) {
    peg_positions_mode(fields);
  } else if (strings_equal(mode, "peg3oracle")) {
    peg_judge_mode(fields);
  } else if (strings_equal(mode, "pegbudget")) {
    peg_budget_mode(fields);
  } else if (strings_equal(mode, "pegrank")) {
    peg_budget_pick_ranks_mode(fields);
  } else {
    log_fatal("unknown pegnet spec: %s", spec);
  }
  string_splitter_destroy(fields);
}
