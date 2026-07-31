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
#include "../src/impl/move_gen.h"
#include "../src/impl/peg.h"
#include "../src/impl/peg_time_manager.h"
#include "../src/impl/play_chooser.h"
#include "../src/impl/simmer.h"
#include "../src/util/io_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const PROGRESS_ENDGAME_CGP =
    "cgp 5U4OHMIC/5N3WREATH/5T4FAX2/5i3B1VIA1/5N3L1E3/5G2VELDT2/"
    "5E3S5/5DREKS1F3/8YELL3/4ABASER1U3/4GYM3ZO3/WAITE5OR2J/"
    "10OI2A/3QUOIT1PINNER/4RENEGADE2P AIINOOU/CDIOST? 392/450 0 "
    "-lex CSW24";

static const char *const PROGRESS_SMALL_ENDGAME_CGP =
    "cgp AVOWS2g7/3OIK1O1T5/2VOX2L1E5/3E3D1E5/3DELATING4/5AGONY5/"
    "4Q2NU5R/CH1FABLERS4E/R1JEDI2E5E/O3IO1B6N/UR1ASPIRINS3A/"
    "PUTT1L1A2HEMIC/IN3A1IF5T/ED3Y1ZAG5/R6EMU5 TW?/O 453/455 0 "
    "-lex CSW24";

static const char *const PROGRESS_MEDIUM_ENDGAME_CGP =
    "cgp 6CWM6/5FOO7/1EVEJAR5V2/U3A7E2/N1I1FE4D1I2/"
    "D1N1AX2SMUGLY1/E1C2PANTON1I2/N1E2OYE2KANZU/I1P2R6GI1/"
    "EWT2T7T1/DROLEST1Q4I1/1AR4BAILIEs1/1T6I6/1HALlOAED6/"
    "5UGH7 BRSS/EOOR 360/531 0 -lex CSW24";

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

typedef struct SimSampleTestTrace {
  atomic_uint_fast64_t count;
  SimSampleEvent events[60];
} SimSampleTestTrace;

static void sim_sample_test_record(const SimSampleEvent *event,
                                   void *user_data) {
  SimSampleTestTrace *trace = user_data;
  const uint64_t index = atomic_fetch_add(&trace->count, 1);
  assert(index < sizeof(trace->events) / sizeof(trace->events[0]));
  trace->events[index] = *event;
}

static int compare_uint64(const void *a, const void *b) {
  const uint64_t av = *(const uint64_t *)a;
  const uint64_t bv = *(const uint64_t *)b;
  return (av > bv) - (av < bv);
}

static int count_tabs(const char *text) {
  int count = 0;
  for (; *text != '\0'; text++) {
    if (*text == '\t') {
      count++;
    }
  }
  return count;
}

typedef struct EndgameAdmissionTestState {
  atomic_int calls;
  EndgameDepthAdmissionRequest request;
  bool valid;
  bool safe_to_enforce;
  bool should_start;
} EndgameAdmissionTestState;

static EndgameDepthAdmissionDecision
test_endgame_depth_admission(const EndgameDepthAdmissionRequest *request,
                             void *user_data) {
  EndgameAdmissionTestState *state = user_data;
  state->request = *request;
  atomic_fetch_add(&state->calls, 1);
  return (EndgameDepthAdmissionDecision){
      .valid = state->valid,
      .safe_to_enforce = state->safe_to_enforce,
      .should_start = state->should_start,
      .expected_nodes = 100,
      .completion_bound_nodes = 200,
      .completion_confidence = 0.99,
  };
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
  assert(recorded.cpu_ns == 0);
  assert(recorded.candidates_total == 3);
  assert(analysis_trace_get_event(trace, 1, &recorded));
  assert(recorded.sequence == 2);
  assert(recorded.iterations == 10);
  assert(!analysis_trace_get_event(trace, 2, &recorded));

  FILE *stream = tmpfile();
  assert(stream != NULL);
  assert(analysis_trace_write_tsv(trace, stream));
  rewind(stream);
  char header[2048];
  const char *read_result = fgets(header, sizeof(header), stream);
  assert(read_result != NULL);
  assert(strstr(header, "schema_version\tsequence\trun_id\tparent_run_id") !=
         NULL);
  assert(strstr(header, "\telapsed_ns\tcpu_ns\tbudget_seconds\tworkers\t") !=
         NULL);
  assert(strstr(header, "\twork_units\tnodes\titerations\tscenarios\t") !=
         NULL);
  assert(strstr(header,
                "\texpected_next_seconds\tcompletion_bound_seconds\t") != NULL);
  assert(strstr(header,
                "\texpected_next_nodes\tcompletion_bound_nodes\t"
                "expected_next_scenarios\tcompletion_bound_scenarios\t") !=
         NULL);
  assert(
      strstr(header, "\thas_time_manager_plan\texpected_regret_reduction\t") !=
      NULL);
  char row[2048];
  read_result = fgets(row, sizeof(row), stream);
  assert(read_result != NULL);
  assert(count_tabs(row) == count_tabs(header));
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
  SimSampleTestTrace sample_trace = {0};
  SimArgs traced_args = baseline_args;
  traced_args.progress_listener = (AnalysisProgressListener){
      .callback = analysis_trace_record,
      .user_data = trace,
      .run_id = 101,
      .checkpoint_interval = 10,
  };
  traced_args.sample_listener = (SimSampleListener){
      .callback = sim_sample_test_record,
      .user_data = &sample_trace,
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

  assert(atomic_load(&sample_trace.count) == 60);
  uint64_t seeds[3][60];
  int seed_counts[3] = {0};
  for (size_t index = 0; index < 60; index++) {
    const SimSampleEvent *event = &sample_trace.events[index];
    assert(event->play_index >= 0 && event->play_index < 3);
    assert(isfinite(event->utility));
    assert(event->win_pct >= 0.0 && event->win_pct <= 1.0);
    assert(isfinite(event->spread));
    const int play = event->play_index;
    assert(seed_counts[play] < 60);
    seeds[play][seed_counts[play]++] = event->scenario_seed;
  }
  for (int play = 0; play < 3; play++) {
    assert(seed_counts[play] == 20);
    qsort(seeds[play], (size_t)seed_counts[play], sizeof(seeds[play][0]),
          compare_uint64);
  }
  for (int play = 1; play < 3; play++) {
    assert(memcmp(seeds[0], seeds[play],
                  (size_t)seed_counts[0] * sizeof(seeds[0][0])) == 0);
  }

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
  assert(!trace_find_event(trace, ANALYSIS_MODE_ENDGAME,
                           ANALYSIS_EVENT_ADMISSION, NULL));
  assert(trace_find_event(trace, ANALYSIS_MODE_ENDGAME, ANALYSIS_EVENT_FINISH,
                          &finish));
  assert(finish.status == ANALYSIS_STATUS_COMPLETED);
  assert(finish.run_id == 202);
  assert(finish.nodes > 0);
  assert(finish.work_units == finish.nodes);
  assert(finish.item_id == baseline_move);

  // The opt-in work ceiling uses the same periodic check as the deadline and
  // reports why the solve stopped. A one-thread solve can overshoot by less
  // than one 1024-node check interval; it must still publish a fallback move.
  analysis_trace_reset(trace);
  EndgameArgs limited_args = traced_args;
  limited_args.plies = MAX_SEARCH_DEPTH;
  limited_args.node_limit = 1024;
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  endgame_solve(&traced_ctx, &limited_args, traced_results, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(trace_find_event(trace, ANALYSIS_MODE_ENDGAME, ANALYSIS_EVENT_FINISH,
                          &finish));
  assert(finish.status == ANALYSIS_STATUS_WORK_LIMIT);
  assert(finish.nodes >= limited_args.node_limit);
  assert(finish.nodes < limited_args.node_limit + 1024);
  assert(endgame_results_get_pvline(traced_results, ENDGAME_RESULT_BEST)
             ->num_moves > 0);

  // ABDADA workers start at jittered depths. The progress stream must publish
  // whichever worker advances the globally usable result, not only thread 0
  // (which may still be searching depth 1 when another finishes depth 4).
  load_and_exec_config_or_die(config, PROGRESS_SMALL_ENDGAME_CGP);
  analysis_trace_reset(trace);
  EndgameArgs parallel_args = traced_args;
  parallel_args.game = config_get_game(config);
  parallel_args.plies = 4;
  parallel_args.num_threads = 2;
  parallel_args.node_limit = 0;
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  endgame_solve(&traced_ctx, &parallel_args, traced_results, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(trace_find_event(trace, ANALYSIS_MODE_ENDGAME, ANALYSIS_EVENT_FINISH,
                          &finish));
  int last_depth = 0;
  uint64_t last_nodes = 0;
  for (size_t event_idx = 0; event_idx < analysis_trace_get_count(trace);
       event_idx++) {
    AnalysisProgressEvent event;
    assert(analysis_trace_get_event(trace, event_idx, &event));
    if (event.mode != ANALYSIS_MODE_ENDGAME ||
        event.event != ANALYSIS_EVENT_DEPTH_DONE) {
      continue;
    }
    assert(event.depth > last_depth);
    assert(event.nodes >= last_nodes);
    assert(event.candidates_completed == event.candidates_total);
    last_depth = event.depth;
    last_nodes = event.nodes;
  }
  assert(last_depth == finish.depth);
  assert(last_depth == 4);

  // Shadow admission records a real whole-depth recommendation but cannot
  // change the search result. Use one thread so scheduling is identical to
  // the no-callback baseline.
  analysis_trace_reset(trace);
  EndgameArgs admission_baseline_args = traced_args;
  admission_baseline_args.game = config_get_game(config);
  admission_baseline_args.plies = 3;
  admission_baseline_args.num_threads = 1;
  admission_baseline_args.node_limit = 0;
  admission_baseline_args.progress_listener = (AnalysisProgressListener){0};
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  endgame_solve(&baseline_ctx, &admission_baseline_args, baseline_results,
                error_stack);
  assert(error_stack_is_empty(error_stack));
  const int admission_baseline_value =
      endgame_results_get_value(baseline_results, ENDGAME_RESULT_BEST);
  const uint64_t admission_baseline_move =
      endgame_results_get_pvline(baseline_results, ENDGAME_RESULT_BEST)
          ->moves[0]
          .tiny_move;

  EndgameAdmissionTestState admission_state = {
      .valid = true,
      .safe_to_enforce = true,
      .should_start = false,
  };
  EndgameArgs shadow_args = admission_baseline_args;
  shadow_args.progress_listener = traced_args.progress_listener;
  shadow_args.depth_admission_callback = test_endgame_depth_admission;
  shadow_args.depth_admission_callback_data = &admission_state;
  shadow_args.depth_admission_min_depth = 2;
  shadow_args.enforce_depth_admission = false;
  shadow_args.has_player_clock = true;
  shadow_args.player_clock_seconds_at_start = 50.0;
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  endgame_solve(&traced_ctx, &shadow_args, traced_results, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(atomic_load(&admission_state.calls) == 1);
  assert(admission_state.request.current.depth == 2);
  assert(admission_state.request.next_depth == 3);
  assert(admission_state.request.has_previous);
  assert(admission_state.request.previous.depth == 1);
  assert(admission_state.request.has_player_clock);
  assert(admission_state.request.player_clock_remaining_seconds > 0.0);
  assert(admission_state.request.player_clock_remaining_seconds <= 50.0);
  assert(endgame_results_get_depth(traced_results, ENDGAME_RESULT_BEST) == 3);
  assert(endgame_results_get_value(traced_results, ENDGAME_RESULT_BEST) ==
         admission_baseline_value);
  assert(endgame_results_get_pvline(traced_results, ENDGAME_RESULT_BEST)
             ->moves[0]
             .tiny_move == admission_baseline_move);
  AnalysisProgressEvent admission;
  assert(trace_find_event(trace, ANALYSIS_MODE_ENDGAME,
                          ANALYSIS_EVENT_ADMISSION, &admission));
  assert(admission.phase == 2);
  assert(admission.depth == 3);
  assert(admission.expected_next_work_units == 100);
  assert(admission.completion_bound_work_units == 200);
  assert(admission.expected_next_nodes == 100);
  assert(admission.completion_bound_nodes == 200);
  assert(within_epsilon(admission.completion_confidence, 0.99));
  assert(admission.admission == ANALYSIS_ADMISSION_REFUSE);
  assert(admission.admission_safe_to_enforce);
  assert(!admission.admission_enforced);
  assert(admission.player_rack_tiles >= 0);
  assert(admission.opponent_rack_tiles >= 0);
  assert(
      within_epsilon(admission.clock_seconds_remaining,
                     admission_state.request.player_clock_remaining_seconds));

  // Enforced admission fails closed at the completed boundary. Even with
  // multiple ABDADA workers, none may start depth 3 after the refusal.
  analysis_trace_reset(trace);
  atomic_store(&admission_state.calls, 0);
  EndgameArgs enforced_args = shadow_args;
  enforced_args.num_threads = 2;
  enforced_args.enforce_depth_admission = true;
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  endgame_solve(&traced_ctx, &enforced_args, traced_results, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(atomic_load(&admission_state.calls) == 1);
  assert(endgame_results_get_depth(traced_results, ENDGAME_RESULT_BEST) == 2);
  assert(trace_find_event(trace, ANALYSIS_MODE_ENDGAME, ANALYSIS_EVENT_FINISH,
                          &finish));
  assert(finish.status == ANALYSIS_STATUS_TIME_LIMIT);
  for (size_t event_idx = 0; event_idx < analysis_trace_get_count(trace);
       event_idx++) {
    AnalysisProgressEvent event;
    assert(analysis_trace_get_event(trace, event_idx, &event));
    assert(event.event != ANALYSIS_EVENT_DEPTH_DONE || event.depth < 3);
  }

  // A valid prediction from the wrong solver topology is useful in shadow
  // mode, but never authorizes enforced search. This is distinct from a
  // malformed prediction: the trace retains the prediction for drift audits
  // while the depth gate fails closed.
  analysis_trace_reset(trace);
  atomic_store(&admission_state.calls, 0);
  admission_state.valid = true;
  admission_state.safe_to_enforce = false;
  admission_state.should_start = true;
  enforced_args.num_threads = 2;
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  endgame_solve(&traced_ctx, &enforced_args, traced_results, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(atomic_load(&admission_state.calls) == 1);
  assert(endgame_results_get_depth(traced_results, ENDGAME_RESULT_BEST) == 2);
  assert(trace_find_event(trace, ANALYSIS_MODE_ENDGAME,
                          ANALYSIS_EVENT_ADMISSION, &admission));
  assert(admission.admission == ANALYSIS_ADMISSION_REFUSE);
  assert(!admission.admission_safe_to_enforce);
  assert(admission.admission_enforced);
  assert(admission.expected_next_work_units == 100);
  assert(admission.completion_bound_work_units == 200);

  // A model that says "start" without a valid whole-depth bound is not
  // permission. Enforced mode must fail closed at the same boundary.
  analysis_trace_reset(trace);
  atomic_store(&admission_state.calls, 0);
  admission_state.valid = false;
  admission_state.safe_to_enforce = true;
  admission_state.should_start = true;
  enforced_args.num_threads = 1;
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  endgame_solve(&traced_ctx, &enforced_args, traced_results, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(atomic_load(&admission_state.calls) == 1);
  assert(endgame_results_get_depth(traced_results, ENDGAME_RESULT_BEST) == 2);
  assert(trace_find_event(trace, ANALYSIS_MODE_ENDGAME,
                          ANALYSIS_EVENT_ADMISSION, &admission));
  assert(admission.admission == ANALYSIS_ADMISSION_REFUSE);
  assert(!admission.admission_safe_to_enforce);
  assert(admission.admission_enforced);
  assert(admission.expected_next_work_units == 0);
  assert(admission.completion_bound_work_units == 0);

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
  MoveList *generated_moves = move_list_create(1);
  const MoveGenArgs movegen_args = {
      .game = game,
      .move_record_type = MOVE_RECORD_BEST,
      .move_sort_type = MOVE_SORT_EQUITY,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .move_list = generated_moves,
  };
  generate_moves(&movegen_args);
  assert(move_list_get_count(generated_moves) == 1);
  const Move *only_moves[2] = {candidate,
                               move_list_get_move(generated_moves, 0)};

  PegArgs baseline_args = {
      .game = game,
      .thread_control = thread_control,
      .num_threads = 4,
      .max_stage = 1,
      .only_moves = only_moves,
      .n_only_moves = 2,
      .opp_model = PEG_OPP_RATIONAL,
  };
  PegResult baseline_result = {0};
  peg_solve(&baseline_args, &baseline_result, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(baseline_result.n_top_cands == 2);

  AnalysisTrace *trace = analysis_trace_create(128);
  PegArgs traced_args = baseline_args;
  const PegTimeManagerCalibration *peg_calibration =
      peg_time_manager_default_calibration(
          PEG_TIME_MANAGER_BOUNDARY_FIRST_TWO_2PLY, 1);
  PegTimeManagerPolicy peg_time_manager = {
      .clock =
          {
              .remaining_seconds = 100.0,
              .turns_remaining = 5,
              .minimum_completion_confidence = 0.99,
          },
      .fixed_expected_regret_reduction = 0.01,
  };
  assert(peg_time_manager_reference_cost_models(
      peg_calibration, 1.0, 1.0, &peg_time_manager.expected_cost_model,
      &peg_time_manager.deadline_cost_model));
  traced_args.progress_listener = (AnalysisProgressListener){
      .callback = analysis_trace_record,
      .user_data = trace,
      .run_id = 303,
  };
  traced_args.time_manager_policy = &peg_time_manager;
  traced_args.has_player_clock = true;
  traced_args.player_clock_seconds_at_start = 100.0;
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
  AnalysisProgressEvent admission;
  assert(trace_find_event(trace, ANALYSIS_MODE_PEG, ANALYSIS_EVENT_ADMISSION,
                          &admission));
  assert(admission.admission == ANALYSIS_ADMISSION_REFUSE);
  assert(!admission.admission_safe_to_enforce);
  assert(!admission.admission_enforced);
  assert(admission.has_time_manager_plan);
  assert(admission.expected_next_candidates == 2);
  assert(admission.completion_bound_candidates == 2);
  assert(admission.completion_confidence < 0.99);
  assert(trace_find_event(trace, ANALYSIS_MODE_PEG, ANALYSIS_EVENT_FINISH,
                          &finish));
  assert(finish.status == ANALYSIS_STATUS_COMPLETED);
  assert(finish.run_id == 303);
  uint64_t final_stage_nodes = 0;
  for (int cand_idx = 0; cand_idx < traced_result.n_top_cands; cand_idx++) {
    final_stage_nodes += traced_result.top_cands[cand_idx].endgame_nodes;
  }
  assert(final_stage_nodes > 0);
  assert(finish.nodes == final_stage_nodes);
  assert(finish.work_units == finish.scenarios);

  // Enforced mode changes the dispatcher topology: first two together, then
  // one completed candidate at a time. Cap at eight here so the test does not
  // depend on the position's winner-stability history.
  MoveList *wave_move_list = move_list_create(16);
  const MoveGenArgs wave_movegen_args = {
      .game = game,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .move_list = wave_move_list,
  };
  generate_moves(&wave_movegen_args);
  assert(move_list_get_count(wave_move_list) >= 10);
  const Move *wave_moves[10];
  for (int move_idx = 0; move_idx < 10; move_idx++) {
    wave_moves[move_idx] = move_list_get_move(wave_move_list, move_idx);
  }
  PegTimeManagerPolicy enforced_policy = peg_time_manager;
  enforced_policy.clock.minimum_completion_confidence = 0.95;
  enforced_policy.regret_callback = peg_time_manager_default_regret_reduction;
  enforced_policy.completion_bound_multiplier = 1.5;
  enforced_policy.allow_provisional_enforcement = true;
  enforced_policy.minimum_2ply_candidates = 8;
  enforced_policy.stability_patience_candidates = 4;
  enforced_policy.maximum_2ply_candidates = 8;
  PegArgs wave_args = baseline_args;
  wave_args.only_moves = wave_moves;
  wave_args.n_only_moves = 10;
  wave_args.time_budget_seconds = 100.0;
  wave_args.progress_listener = (AnalysisProgressListener){
      .callback = analysis_trace_record,
      .user_data = trace,
      .run_id = 304,
  };
  wave_args.time_manager_policy = &enforced_policy;
  wave_args.enforce_time_manager = true;
  wave_args.has_player_clock = true;
  wave_args.player_clock_seconds_at_start = 100.0;
  analysis_trace_reset(trace);
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  PegResult wave_result = {0};
  peg_solve(&wave_args, &wave_result, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(wave_result.last_completed_stage == 1);
  assert(wave_result.n_top_cands == 8);
  assert(wave_result.last_stage_partial);
  assert(wave_result.stopped_by_time_manager);
  assert(wave_result.time_manager_admitted_chunks == 7);
  assert(wave_result.time_manager_false_starts == 0);
  assert(trace_find_event(trace, ANALYSIS_MODE_PEG, ANALYSIS_EVENT_FINISH,
                          &finish));
  assert(finish.status == ANALYSIS_STATUS_COMPLETED);
  peg_result_destroy(&wave_result);
  move_list_destroy(wave_move_list);

  analysis_trace_destroy(trace);
  peg_result_destroy(&traced_result);
  peg_result_destroy(&baseline_result);
  move_list_destroy(generated_moves);
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

static void test_play_chooser_endgame_admission_shadow(void) {
  Config *config =
      config_create_or_die("set -lex CSW24 -s1 score -s2 score -threads 1");
  load_and_exec_config_or_die(config, PROGRESS_MEDIUM_ENDGAME_CGP);
  Game *game = config_get_game(config);
  ErrorStack *error_stack = error_stack_create();

  const PlayChooserStrategy baseline_strategy = {
      .pre_endgame_eval = PLAY_CHOOSER_EVAL_STATIC,
      .endgame_eval = PLAY_CHOOSER_EVAL_ENDGAME,
      .endgame_plies = 4,
      .fixed_seconds_per_move = 5.0,
      .num_threads = 1,
      .seed = 77,
  };
  PlayChooser *baseline_chooser = play_chooser_create(&baseline_strategy);
  Move baseline_move;
  play_chooser_choose_move(baseline_chooser, game, &baseline_move, error_stack);
  assert(error_stack_is_empty(error_stack));

  AnalysisTrace *trace = analysis_trace_create(8192);
  PlayChooserStrategy shadow_strategy = baseline_strategy;
  shadow_strategy.endgame_admission_shadow = true;
  shadow_strategy.progress_listener = (AnalysisProgressListener){
      .callback = analysis_trace_record,
      .user_data = trace,
  };
  PlayChooser *shadow_chooser = play_chooser_create(&shadow_strategy);
  Move shadow_move;
  play_chooser_choose_move(shadow_chooser, game, &shadow_move, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert_moves_are_equal(&baseline_move, &shadow_move);

  AnalysisProgressEvent admission;
  assert(trace_find_event(trace, ANALYSIS_MODE_ENDGAME,
                          ANALYSIS_EVENT_ADMISSION, &admission));
  assert(admission.phase == 3);
  assert(admission.depth == 4);
  assert(admission.expected_next_work_units > 0);
  assert(admission.completion_bound_work_units >=
         admission.expected_next_work_units);
  assert(isfinite(admission.expected_next_seconds));
  assert(admission.completion_bound_seconds >= admission.expected_next_seconds);
  // This test deliberately uses one worker and PlayChooser's external shared
  // TT, while the frozen artifact was calibrated at 10 workers/private 5% TT.
  assert(!admission.admission_safe_to_enforce);
  assert(!admission.admission_enforced);

  play_chooser_destroy(shadow_chooser);
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
  test_play_chooser_endgame_admission_shadow();
}
