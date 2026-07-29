#include "analysis_progress_test.h"

#include "../src/def/bai_defs.h"
#include "../src/def/config_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/analysis_progress.h"
#include "../src/ent/analysis_trace.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/move.h"
#include "../src/ent/sim_args.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/validated_move.h"
#include "../src/ent/win_pct.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/peg.h"
#include "../src/impl/play_chooser.h"
#include "../src/impl/simmer.h"
#include "../src/util/io_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *const PROGRESS_ENDGAME_CGP =
    "cgp 5U4OHMIC/5N3WREATH/5T4FAX2/5i3B1VIA1/5N3L1E3/5G2VELDT2/"
    "5E3S5/5DREKS1F3/8YELL3/4ABASER1U3/4GYM3ZO3/WAITE5OR2J/"
    "10OI2A/3QUOIT1PINNER/4RENEGADE2P AIINOOU/CDIOST? 392/450 0 "
    "-lex CSW24";

static const char *const PROGRESS_PEG_CGP =
    "cgp 7C6D/7H4LAR/7I2P1ALA/7VOGUE1AG/6RERAN2M1/7S1BY2O1/"
    "8OY2Id1/5JEUX3NEW/3C1U2O3A1E/3O1M6N1B/3ZIP2OAK1E2/"
    "2TI1sTIFLERS2/2WED5F1T2/1HIDEOUT7/VEG1N2IDOL4 "
    "AEINRST/AEINRST 372/369 0 -lex CSW24";

static bool trace_find_event(AnalysisTrace *trace, analysis_mode_t mode,
                             analysis_event_t event,
                             AnalysisProgressEvent *found_event) {
  const size_t count = analysis_trace_get_count(trace);
  for (size_t event_idx = 0; event_idx < count; event_idx++) {
    AnalysisProgressEvent candidate;
    assert(analysis_trace_get_event(trace, event_idx, &candidate));
    if (candidate.mode == mode && candidate.event == event) {
      if (found_event != NULL) {
        *found_event = candidate;
      }
      return true;
    }
  }
  return false;
}

static void test_trace_storage_and_export(void) {
  AnalysisTrace *trace = analysis_trace_create(2);
  AnalysisProgressListener listener = {
      .callback = analysis_trace_record,
      .user_data = trace,
      .run_id = 17,
      .parent_run_id = 9,
  };
  AnalysisProgressEvent start =
      analysis_progress_event_create(ANALYSIS_MODE_SIM, ANALYSIS_EVENT_START);
  start.candidates_total = 3;
  analysis_progress_emit(&listener, &start);
  AnalysisProgressEvent checkpoint = analysis_progress_event_create(
      ANALYSIS_MODE_SIM, ANALYSIS_EVENT_CHECKPOINT);
  checkpoint.iterations = 10;
  checkpoint.work_units = 10;
  analysis_progress_emit(&listener, &checkpoint);
  AnalysisProgressEvent finish =
      analysis_progress_event_create(ANALYSIS_MODE_SIM, ANALYSIS_EVENT_FINISH);
  finish.status = ANALYSIS_STATUS_COMPLETED;
  analysis_progress_emit(&listener, &finish);

  assert(analysis_trace_get_count(trace) == 2);
  assert(analysis_trace_get_dropped(trace) == 1);
  AnalysisProgressEvent recorded;
  assert(analysis_trace_get_event(trace, 0, &recorded));
  assert(recorded.schema_version == ANALYSIS_PROGRESS_SCHEMA_VERSION);
  assert(recorded.sequence == 1);
  assert(recorded.run_id == 17);
  assert(recorded.parent_run_id == 9);
  assert(recorded.elapsed_ns == 0);
  assert(recorded.candidates_total == 3);
  assert(analysis_trace_get_event(trace, 1, &recorded));
  assert(recorded.sequence == 2);
  assert(recorded.iterations == 10);
  assert(!analysis_trace_get_event(trace, 2, &recorded));

  FILE *stream = tmpfile();
  assert(stream != NULL);
  assert(analysis_trace_write_tsv(trace, stream));
  rewind(stream);
  char header[512];
  const char *read_result = fgets(header, sizeof(header), stream);
  assert(read_result != NULL);
  assert(strstr(header, "schema_version\tsequence\trun_id\tparent_run_id") !=
         NULL);
  assert(strstr(header, "\twork_units\tnodes\titerations\tscenarios\t") !=
         NULL);
  fclose(stream);

  analysis_trace_reset(trace);
  assert(analysis_trace_get_count(trace) == 0);
  assert(analysis_trace_get_dropped(trace) == 0);
  analysis_trace_destroy(trace);
}

static void test_sim_progress_is_observation_only(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 score -s2 score -r1 all -r2 all -numplays 3 "
      "-threads 4");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  load_and_exec_config_or_die(config, "rack AEIQRST");
  load_and_exec_config_or_die(config, "gen");

  ErrorStack *error_stack = error_stack_create();
  WinPct *win_pcts =
      win_pct_create(DEFAULT_TEST_DATA_PATH, DEFAULT_WIN_PCT, error_stack);
  assert(error_stack_is_empty(error_stack));
  ThreadControl *thread_control = config_get_thread_control(config);
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  const MoveList *move_list = config_get_move_list(config);
  const int num_candidates = move_list_get_count(move_list);
  assert(num_candidates == 3);

  SimArgs baseline_args = {0};
  sim_args_fill(
      /*num_plies=*/2, move_list, num_candidates,
      /*known_opp_rack=*/NULL, win_pcts, /*inference_results=*/NULL,
      thread_control, config_get_game(config),
      /*sim_with_inference=*/false, /*use_heat_map=*/false,
      /*num_threads=*/4, /*print_interval=*/0,
      /*max_num_display_plays=*/num_candidates,
      /*max_num_display_plies=*/2, /*seed=*/42,
      /*max_iterations=*/60, /*min_play_iterations=*/5,
      /*scond=*/0.0, BAI_THRESHOLD_NONE, /*time_limit_seconds=*/0.0,
      BAI_SAMPLING_RULE_ROUND_ROBIN, /*cutoff=*/0.0,
      /*utility_w_winpct=*/1.0, /*utility_w_spread=*/0.0,
      /*utility_spread_scale=*/100.0, /*inference_args=*/NULL, &baseline_args);

  SimResults *baseline_results =
      sim_results_create(convert_user_cutoff_to_cutoff(0.005));
  simulate_without_ctx(&baseline_args, baseline_results, error_stack);
  assert(error_stack_is_empty(error_stack));

  AnalysisTrace *trace = analysis_trace_create(128);
  SimArgs traced_args = baseline_args;
  traced_args.progress_listener = (AnalysisProgressListener){
      .callback = analysis_trace_record,
      .user_data = trace,
      .run_id = 101,
      .checkpoint_interval = 10,
  };
  SimResults *traced_results =
      sim_results_create(convert_user_cutoff_to_cutoff(0.005));
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  simulate_without_ctx(&traced_args, traced_results, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert_sim_results_equal(baseline_results, traced_results);

  AnalysisProgressEvent finish;
  assert(
      trace_find_event(trace, ANALYSIS_MODE_SIM, ANALYSIS_EVENT_START, NULL));
  assert(trace_find_event(trace, ANALYSIS_MODE_SIM, ANALYSIS_EVENT_CHECKPOINT,
                          NULL));
  assert(trace_find_event(trace, ANALYSIS_MODE_SIM, ANALYSIS_EVENT_FINISH,
                          &finish));
  assert(finish.status == ANALYSIS_STATUS_COMPLETED);
  assert(finish.run_id == 101);
  assert(finish.iterations == sim_results_get_iteration_count(traced_results));
  assert(finish.nodes == sim_results_get_node_count(traced_results));
  assert(finish.work_units == finish.iterations);

  analysis_trace_destroy(trace);
  sim_results_destroy(traced_results);
  sim_results_destroy(baseline_results);
  win_pct_destroy(win_pcts);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

static void test_endgame_progress_is_observation_only(void) {
  Config *config = config_create_or_die(
      "set -s1 score -s2 score -threads 1 -eplies 1 -ttfraction 0");
  load_and_exec_config_or_die(config, PROGRESS_ENDGAME_CGP);
  ThreadControl *thread_control = config_get_thread_control(config);
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);

  EndgameArgs baseline_args = {
      .thread_control = thread_control,
      .game = config_get_game(config),
      .plies = 1,
      .tt_fraction_of_mem = 0,
      .initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
      .num_threads = 1,
      .use_heuristics = true,
      .num_top_moves = 1,
      .forced_pass_bypass = true,
      .seed = 42,
  };
  ErrorStack *error_stack = error_stack_create();
  EndgameCtx *baseline_ctx = NULL;
  EndgameResults *baseline_results = endgame_results_create();
  endgame_solve(&baseline_ctx, &baseline_args, baseline_results, error_stack);
  assert(error_stack_is_empty(error_stack));
  const int baseline_value =
      endgame_results_get_value(baseline_results, ENDGAME_RESULT_BEST);
  const int baseline_depth =
      endgame_results_get_depth(baseline_results, ENDGAME_RESULT_BEST);
  const uint64_t baseline_move =
      endgame_results_get_pvline(baseline_results, ENDGAME_RESULT_BEST)
          ->moves[0]
          .tiny_move;

  AnalysisTrace *trace = analysis_trace_create(4096);
  EndgameArgs traced_args = baseline_args;
  traced_args.progress_listener = (AnalysisProgressListener){
      .callback = analysis_trace_record,
      .user_data = trace,
      .run_id = 202,
  };
  EndgameCtx *traced_ctx = NULL;
  EndgameResults *traced_results = endgame_results_create();
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  endgame_solve(&traced_ctx, &traced_args, traced_results, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(endgame_results_get_value(traced_results, ENDGAME_RESULT_BEST) ==
         baseline_value);
  assert(endgame_results_get_depth(traced_results, ENDGAME_RESULT_BEST) ==
         baseline_depth);
  assert(endgame_results_get_pvline(traced_results, ENDGAME_RESULT_BEST)
             ->moves[0]
             .tiny_move == baseline_move);

  AnalysisProgressEvent finish;
  assert(trace_find_event(trace, ANALYSIS_MODE_ENDGAME, ANALYSIS_EVENT_START,
                          NULL));
  assert(trace_find_event(trace, ANALYSIS_MODE_ENDGAME,
                          ANALYSIS_EVENT_CANDIDATE_SET, NULL));
  assert(trace_find_event(trace, ANALYSIS_MODE_ENDGAME,
                          ANALYSIS_EVENT_CANDIDATE_DONE, NULL));
  assert(trace_find_event(trace, ANALYSIS_MODE_ENDGAME,
                          ANALYSIS_EVENT_DEPTH_DONE, NULL));
  assert(trace_find_event(trace, ANALYSIS_MODE_ENDGAME, ANALYSIS_EVENT_FINISH,
                          &finish));
  assert(finish.status == ANALYSIS_STATUS_COMPLETED);
  assert(finish.run_id == 202);
  assert(finish.nodes > 0);
  assert(finish.work_units == finish.nodes);
  assert(finish.item_id == baseline_move);

  analysis_trace_destroy(trace);
  endgame_ctx_destroy(traced_ctx);
  endgame_ctx_destroy(baseline_ctx);
  endgame_results_destroy(traced_results);
  endgame_results_destroy(baseline_results);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

static void test_peg_progress_is_observation_only(void) {
  Config *config = config_create_or_die("set -threads 4 -s1 score -s2 score");
  load_and_exec_config_or_die(config, PROGRESS_PEG_CGP);
  Game *game = config_get_game(config);
  ThreadControl *thread_control = config_get_thread_control(config);
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  ErrorStack *error_stack = error_stack_create();
  ValidatedMoves *validated_moves = validated_moves_create(
      game, game_get_player_on_turn_index(game), "pass",
      /*allow_phonies=*/false, /*allow_playthrough=*/true, error_stack);
  assert(error_stack_is_empty(error_stack));
  const Move *candidate = validated_moves_get_move(validated_moves, 0);
  const Move *only_moves[1] = {candidate};

  PegArgs baseline_args = {
      .game = game,
      .thread_control = thread_control,
      .num_threads = 4,
      .only_moves = only_moves,
      .n_only_moves = 1,
      .opp_model = PEG_OPP_RATIONAL,
  };
  PegResult baseline_result = {0};
  peg_solve(&baseline_args, &baseline_result, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(baseline_result.n_top_cands == 1);

  AnalysisTrace *trace = analysis_trace_create(128);
  PegArgs traced_args = baseline_args;
  traced_args.progress_listener = (AnalysisProgressListener){
      .callback = analysis_trace_record,
      .user_data = trace,
      .run_id = 303,
  };
  PegResult traced_result = {0};
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  peg_solve(&traced_args, &traced_result, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(traced_result.n_top_cands == baseline_result.n_top_cands);
  assert(move_get_fingerprint(&traced_result.best_move) ==
         move_get_fingerprint(&baseline_result.best_move));
  assert(within_epsilon(traced_result.best_win, baseline_result.best_win));
  assert(
      within_epsilon(traced_result.best_spread, baseline_result.best_spread));
  assert(traced_result.top_cands[0].n_scenarios ==
         baseline_result.top_cands[0].n_scenarios);

  AnalysisProgressEvent finish;
  assert(
      trace_find_event(trace, ANALYSIS_MODE_PEG, ANALYSIS_EVENT_START, NULL));
  assert(trace_find_event(trace, ANALYSIS_MODE_PEG,
                          ANALYSIS_EVENT_CANDIDATE_SET, NULL));
  assert(trace_find_event(trace, ANALYSIS_MODE_PEG,
                          ANALYSIS_EVENT_CANDIDATE_DONE, NULL));
  assert(trace_find_event(trace, ANALYSIS_MODE_PEG, ANALYSIS_EVENT_CHECKPOINT,
                          NULL));
  assert(trace_find_event(trace, ANALYSIS_MODE_PEG, ANALYSIS_EVENT_FINISH,
                          &finish));
  assert(finish.status == ANALYSIS_STATUS_COMPLETED);
  assert(finish.run_id == 303);
  assert(finish.scenarios == (uint64_t)traced_result.top_cands[0].n_scenarios);
  assert(finish.work_units == finish.scenarios);

  analysis_trace_destroy(trace);
  peg_result_destroy(&traced_result);
  peg_result_destroy(&baseline_result);
  validated_moves_destroy(validated_moves);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

static void test_play_chooser_progress_hierarchy(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -threads 1");
  load_and_exec_config_or_die(config, "cgp " OPENING_CGP);
  Game *game = config_get_game(config);
  ErrorStack *error_stack = error_stack_create();

  const PlayChooserStrategy baseline_strategy = {
      .pre_endgame_eval = PLAY_CHOOSER_EVAL_STATIC,
      .endgame_eval = PLAY_CHOOSER_EVAL_STATIC,
      .fixed_seconds_per_move = 1.0,
      .num_threads = 1,
      .seed = 42,
  };
  PlayChooser *baseline_chooser = play_chooser_create(&baseline_strategy);
  Move baseline_move;
  play_chooser_choose_move(baseline_chooser, game, &baseline_move, error_stack);
  assert(error_stack_is_empty(error_stack));

  AnalysisTrace *trace = analysis_trace_create(16);
  PlayChooserStrategy traced_strategy = baseline_strategy;
  traced_strategy.progress_listener = (AnalysisProgressListener){
      .callback = analysis_trace_record,
      .user_data = trace,
  };
  PlayChooser *traced_chooser = play_chooser_create(&traced_strategy);
  Move traced_move;
  play_chooser_choose_move(traced_chooser, game, &traced_move, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert_moves_are_equal(&baseline_move, &traced_move);
  assert(analysis_trace_get_count(trace) == 4);

  AnalysisProgressEvent decision_start;
  AnalysisProgressEvent static_start;
  AnalysisProgressEvent static_finish;
  AnalysisProgressEvent decision_finish;
  assert(analysis_trace_get_event(trace, 0, &decision_start));
  assert(analysis_trace_get_event(trace, 1, &static_start));
  assert(analysis_trace_get_event(trace, 2, &static_finish));
  assert(analysis_trace_get_event(trace, 3, &decision_finish));
  assert(decision_start.mode == ANALYSIS_MODE_PLAY_CHOOSER);
  assert(decision_start.event == ANALYSIS_EVENT_START);
  assert(decision_start.parent_run_id == 0);
  assert(static_start.mode == ANALYSIS_MODE_STATIC);
  assert(static_start.event == ANALYSIS_EVENT_START);
  assert(static_start.parent_run_id == decision_start.run_id);
  assert(static_start.run_id != decision_start.run_id);
  assert(static_finish.mode == ANALYSIS_MODE_STATIC);
  assert(static_finish.event == ANALYSIS_EVENT_FINISH);
  assert(static_finish.run_id == static_start.run_id);
  assert(static_finish.item_id == move_get_fingerprint(&traced_move));
  assert(decision_finish.mode == ANALYSIS_MODE_PLAY_CHOOSER);
  assert(decision_finish.event == ANALYSIS_EVENT_FINISH);
  assert(decision_finish.run_id == decision_start.run_id);
  assert(decision_finish.item_id == move_get_fingerprint(&traced_move));

  play_chooser_destroy(traced_chooser);
  play_chooser_destroy(baseline_chooser);
  analysis_trace_destroy(trace);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

void test_analysis_progress(void) {
  test_trace_storage_and_export();
  test_sim_progress_is_observation_only();
  test_endgame_progress_is_observation_only();
  test_peg_progress_is_observation_only();
  test_play_chooser_progress_hierarchy();
}
