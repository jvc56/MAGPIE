#include "peg_poll_test.h"

#include "../src/compat/cpthread.h"
#include "../src/compat/cpthread_defs.h"
#include "../src/compat/ctime.h"
#include "../src/compat/memory_info.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/peg_defs.h"
#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/impl/config.h"
#include "../src/impl/peg.h"
#include "../src/str/move_string.h"
#include "../src/str/peg_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_platform.h"
#include "test_util.h"
#include <assert.h>
#include <locale.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A 2-in-bag PEG position (from notes/peg_positions/random_2peg.txt). Stage 0
// runs greedy playouts over the full candidate field, so the poll's leaderboard
// fills in quickly; a short time budget keeps the whole test ~1-2s wall.
static const char *const PEG_POLL_CGP =
    "cgp 4WILI3JANE/4A3AQUAE2/2C1N3K2Y3/2RUG3E2G3/2O1L3B1REV2/2U1EL2I1EEL2/"
    "2T1SI2A1S1Y2/R1O2PWNS1T4/E1N2I4A4/AH1MED4T4/GI1OPE4E4/IT1ZO2VOUDON2/N3D10/"
    "I3E10/COIFS10 BDFHMX?/AAORTT? 420/412 0 -lex CSW24";

typedef struct PollerArgs {
  PegPoll *poll;
  atomic_bool stop;
  int reads;
  int max_entries_seen;
  int max_stage_seen;
  uint64_t max_version_seen;
  int draw_reads; // reads that caught a candidate's draws
} PollerArgs;

// A candidate's draws agree with its stage: one per scenario job, the finished
// ones counted as solved, every running one listed, and a done one's orderings
// won and tied within all it had.
static void assert_draws_consistent(const PegPollSnapshot *snap) {
  const int n_draws = snap->draws_queued + snap->draws_running +
                      snap->draws_done + snap->draws_stopped;
  assert(snap->n_draw_rows <= PEG_POLL_MAX_DRAW_ROWS);
  if (n_draws == 0) {
    assert(snap->n_draw_rows == 0);
    return;
  }
  assert(snap->n_stage_history > 0);
  const PegStageSnapshot *stage =
      &snap->stage_history[snap->n_stage_history - 1];
  assert(n_draws == stage->scenarios_total);
  assert(snap->draws_done + snap->draws_stopped == stage->scenarios_done);
  int running_rows = 0;
  for (int row_idx = 0; row_idx < snap->n_draw_rows; row_idx++) {
    const PegDrawStatus *draw = &snap->draw_rows[row_idx];
    assert(draw->state != PEG_DRAW_QUEUED);
    assert(draw->start_ns != 0);
    if (draw->state == PEG_DRAW_RUNNING) {
      running_rows++;
    } else {
      assert(draw->end_ns >= draw->start_ns);
    }
    if (draw->state == PEG_DRAW_DONE) {
      assert(draw->win_count + draw->tie_count <= draw->weight_sum);
    }
  }
  assert(running_rows == snap->draws_running);
  assert(snap->draws_done_wins + snap->draws_done_ties <=
         snap->draws_done_weight);
}

// Polls the live leaderboard ~1 kHz, like a render loop would, until the solve
// reports done (or the caller asks it to stop). Records what it observed so the
// test can assert the poll actually advanced while peg_solve ran on the main
// thread.
static void *poller_main(void *arg) {
  PollerArgs *poller = (PollerArgs *)arg;
  PegPollSnapshot snap;
  while (!atomic_load(&poller->stop)) {
    peg_poll_read(poller->poll, &snap);
    poller->reads++;
    if (snap.n_entries > poller->max_entries_seen) {
      poller->max_entries_seen = snap.n_entries;
    }
    if (snap.stage > poller->max_stage_seen) {
      poller->max_stage_seen = snap.stage;
    }
    if (snap.version > poller->max_version_seen) {
      poller->max_version_seen = snap.version;
    }
    assert_draws_consistent(&snap);
    if (snap.n_draw_rows > 0) {
      poller->draw_reads++;
    }
    if (snap.done) {
      break;
    }
    ctime_nap(0.001);
  }
  return NULL;
}

// The status text of a 1-in-bag exhaustive solve whose first candidate,
// `move`, is part way through its draws: the summary, the candidate and its
// time, each draw's state and endgame, and the greedy ranking labelled as
// such; then the same with too many draws to list the finished ones; then
// with candidates solved at once, one per thread.
static void test_peg_status_string(const Game *game, const Move *move) {
  const int64_t now_ns = 1000LL * 1000 * 1000 * 1000;
  const int64_t second_ns = 1000LL * 1000 * 1000;
  PegPollSnapshot *snap = calloc_or_die(1, sizeof(*snap));
  snap->stage = 1;
  snap->n_stage_history = 2;
  snap->stage_history[0].fidelity_plies = 0;
  snap->stage_history[0].field_size = 168;
  snap->stage_history[0].cands_done = 168;
  snap->stage_history[0].start_ns = now_ns - 312 * second_ns;
  snap->stage_history[0].end_ns = now_ns - 311 * second_ns;
  snap->stage_history[0].best_win_pct = 1.0;
  snap->stage_history[1].fidelity_plies = PEG_EXHAUSTIVE_PLIES;
  snap->stage_history[1].field_size = 168;
  snap->stage_history[1].start_ns = now_ns - 311 * second_ns;
  snap->stage_history[1].best_win_pct = -1.0;
  snap->stage_history[1].scenarios_total = 4;
  snap->stage_history[1].scenarios_done = 2;
  snap->n_baseline_entries = 1;
  snap->baseline_fidelity = 0;
  snap->baseline_entries[0].move = *move;
  snap->baseline_entries[0].win_pct = 1.0;
  snap->baseline_entries[0].weight_sum = 8;
  snap->baseline_entries[0].win_count = 8;
  snap->currently_evaluating_move_idx = 0;
  snap->eval_start_ns = now_ns - 311 * second_ns;
  snap->evaluating_move = *move;
  snap->draws_done = 2;
  snap->draws_running = 2;
  snap->draws_done_weight = 2;
  snap->draws_done_wins = 1;
  snap->n_draw_rows = 4;
  // Machine letters 1 and 5 are A and E.
  const MachineLetter tiles[] = {1, 5, 9, 15};
  for (int row_idx = 0; row_idx < 4; row_idx++) {
    PegDrawStatus *draw = &snap->draw_rows[row_idx];
    draw->drawn[0] = tiles[row_idx];
    draw->n_drawn = 1;
    draw->start_ns = now_ns - 311 * second_ns;
    draw->weight_sum = 1;
  }
  snap->draw_rows[0].state = PEG_DRAW_DONE;
  snap->draw_rows[0].end_ns = now_ns - 310 * second_ns;
  snap->draw_rows[0].win_count = 1;
  snap->draw_rows[0].spread_unknown = true;
  snap->draw_rows[1].state = PEG_DRAW_DONE;
  snap->draw_rows[1].end_ns = now_ns - 300 * second_ns;
  snap->draw_rows[1].mean_spread = -12.5;
  snap->draw_rows[2].state = PEG_DRAW_RUNNING;
  snap->draw_rows[2].in_endgame = true;
  snap->draw_rows[2].depth = 23;
  snap->draw_rows[2].root_moves_done = 5;
  snap->draw_rows[2].root_moves_total = 14;
  snap->draw_rows[2].nodes = 1234567890ULL;
  snap->draw_rows[2].helpers = 3;
  snap->draw_rows[3].state = PEG_DRAW_RUNNING;
  // A pass draws nothing: only the bag it leaves tells its draws apart.
  snap->draw_rows[3].n_drawn = 0;
  snap->draw_rows[3].bag[0] = 5;
  snap->draw_rows[3].n_bag = 1;

  StringBuilder *move_sb = string_builder_create();
  string_builder_add_move(move_sb, game_get_board(game), move,
                          game_get_ld(game), false);
  char *solving = get_formatted_string("\nCurrently working on %s\nDraw ",
                                       string_builder_peek(move_sb));
  string_builder_destroy(move_sb);

  char *status = peg_snapshot_get_status_string(snap, game, now_ns, NULL, 0);
  assert(has_prefix("Status          312.0s (running)\n", status));
  assert(has_substring(status, "\nProgress        0 of 168 (0%)\n"));
  assert(has_substring(status, "\nBest Move       "));
  assert(has_substring(status, " unique draws)\nBest Move       "));
  assert(has_substring(status, "100.0% (greedy)\n"));
  assert(has_prefix("Status          312.0s (running)\n"
                    "  Greedy        1.0s (finished)\n"
                    "  End           311.0s (running)\n"
                    "Progress        0 of 168 (0%)\n"
                    "  Finished      0 (0.0%)\n"
                    "  Cut           0 (0.0%)\n"
                    "  Pending       168 (100.0%)\n"
                    "Draws           ",
                    status));
  assert(!has_substring(status, "Phase"));
  assert(!has_substring(status, "Stages"));
  assert(!has_substring(status, "Nodes"));
  assert(!has_substring(status, "Threads"));
  // The pass's draw leaves a tile in the bag: its thread works through
  // the opponent's replies, not one endgame.
  assert(has_substring(status, "nested"));
  assert(!has_substring(status, "Outcomes"));
  assert(!has_substring(status, "draws 2/4"));
  assert(has_substring(status, solving));
  assert(has_substring(status, "won"));
  assert(has_substring(status, "lost"));
  assert(has_substring(status, "-12.5"));
  assert(has_substring(status, "5/14"));
  assert(has_substring(status, "311.0s"));
  assert(has_substring(status, "\n-/E "));
  assert(has_substring(status, "\nGreedy ranking, until a candidate "
                               "finishes\nRank  Move"));
  assert(!has_substring(status, "Seed"));
  assert(!has_substring(status, "estimate"));
  free(status);

  // A time limit can leave every candidate unscored, which is not a -100%.
  snap->baseline_entries[0].win_pct = -1.0;
  status = peg_snapshot_get_status_string(snap, game, now_ns, NULL, 0);
  assert(has_substring(status, "  not scored\n"));
  assert(!has_substring(status, "-100.0"));
  free(status);
  snap->baseline_entries[0].win_pct = 1.0;

  // With outcomes, the ranking has an outcomes column: each candidate's
  // draws, here a win drawing A and a loss drawing E.
  PegPerScenario rows[2] = {0};
  (void)snprintf(rows[0].drawn, sizeof(rows[0].drawn), "A");
  rows[0].weight = 1;
  rows[0].mover_total = 10;
  (void)snprintf(rows[1].drawn, sizeof(rows[1].drawn), "E");
  rows[1].weight = 1;
  rows[1].mover_total = -10;
  PegCandOutcomes outcomes = {.move = *move, .rows = rows, .n_rows = 2};
  char *cell = peg_build_outcomes_string_rows(rows, 2);
  assert(cell[0] != '\0');
  status = peg_snapshot_get_status_string(snap, game, now_ns, &outcomes, 1);
  assert(has_substring(status, "Spread  Outcomes\n"));
  char *cell_line = get_formatted_string("  %s\n", cell);
  assert(has_substring(status, cell_line));
  free(cell_line);
  free(cell);
  free(status);

  // Past PEG_STATUS_MAX_FINISHED_DRAW_ROWS draws, the finished ones are
  // totalled, and only the running ones listed.
  snap->stage_history[1].scenarios_total = 40;
  snap->stage_history[1].scenarios_done = 20;
  snap->draws_queued = 18;
  snap->draws_done = 20;
  snap->draws_done_weight = 20;
  snap->draws_done_wins = 15;
  snap->draws_done_ties = 1;
  status = peg_snapshot_get_status_string(snap, game, now_ns, NULL, 0);
  assert(!has_substring(status, "queued"));
  assert(has_substring(status,
                       "20 finished draws: orderings won 15, tied 1, lost 4"));
  assert(!has_substring(status, "-12.5"));
  assert(has_substring(status, "5/14"));
  free(status);

  // Solved at once, each candidate's row gives its draws solved of all it
  // has, its time, and the draw its thread is solving with that endgame.
  snap->currently_evaluating_move_idx = -1;
  snap->eval_start_ns = 0;
  snap->cands_running = 2;
  snap->n_cand_rows = 2;
  snap->cand_rows[0].move = *move;
  snap->cand_rows[0].start_ns = now_ns - 5 * second_ns;
  snap->cand_rows[0].draws_done = 3;
  snap->cand_rows[0].draws_total = 8;
  snap->cand_rows[0].draw = snap->draw_rows[2];
  snap->cand_rows[1].move = *move;
  snap->cand_rows[1].start_ns = now_ns - second_ns;
  snap->cand_rows[1].draw.state = PEG_DRAW_QUEUED;
  status = peg_snapshot_get_status_string(snap, game, now_ns, NULL, 0);
  assert(has_substring(status, "\nCurrently working on 2 candidates:\nMove"));
  assert(has_substring(status, "3/8"));
  assert(has_substring(status, "5.0s"));
  // Its endgame is at depth 23, 5 of 14 root moves searched. The second
  // candidate has not started its first draw, so it has nothing to show.
  assert(has_substring(status, "5/14"));
  assert(!has_substring(status, "Threads"));
  assert(!has_substring(status, "starting"));
  assert(!has_substring(status, "between"));
  assert(!has_substring(status, "no endgame progress"));
  assert(!has_substring(status, "draws: 20 done"));
  free(status);
  // Once it has solved a draw, it is between draws.
  snap->cand_rows[1].draws_done = 2;
  status = peg_snapshot_get_status_string(snap, game, now_ns, NULL, 0);
  assert(has_substring(status, "between"));
  free(status);
  free(solving);
  free(snap);
}

// The live ranking of an endgame stage: its first ten rows, a "..." row,
// then a solved play ranked below them, which the top entries miss, at its
// rank; a cut play whose bound rounds to zero at one decimal; and how many
// more plays have finished.
static void test_peg_status_ranking(const Game *game, const Move *move) {
  const int64_t now_ns = 1000LL * 1000 * 1000 * 1000;
  PegPollSnapshot *snap = calloc_or_die(1, sizeof(*snap));
  snap->stage = 1;
  snap->fidelity_plies = 3;
  snap->n_stage_history = 2;
  snap->stage_history[0].field_size = 60;
  snap->stage_history[0].cands_done = 60;
  snap->stage_history[0].start_ns = now_ns - 10LL * 1000 * 1000 * 1000;
  snap->stage_history[0].end_ns = now_ns - 9LL * 1000 * 1000 * 1000;
  snap->stage_history[1].fidelity_plies = 3;
  snap->stage_history[1].field_size = 60;
  snap->stage_history[1].cands_done = 50;
  snap->stage_history[1].cands_cut = 48;
  snap->stage_history[1].start_ns = now_ns - 9LL * 1000 * 1000 * 1000;
  snap->n_entries = 12;
  for (int entry_idx = 0; entry_idx < snap->n_entries; entry_idx++) {
    PegRankedCand *entry = &snap->entries[entry_idx];
    entry->move = *move;
    entry->weight_sum = 72;
    entry->spread_unknown = true;
    entry->cut = entry_idx > 0;
    entry->win_pct = entry_idx > 0 ? 0.25 : 19.0 / 72.0;
    entry->win_count = entry_idx > 0 ? 0 : 19;
  }
  // 0.006%: one decimal would show "<=0.0".
  snap->entries[9].win_pct = 0.00006;
  snap->n_solved_rows = 2;
  snap->solved_rows[0] = snap->entries[0];
  snap->solved_ranks[0] = 1;
  snap->solved_rows[1] = snap->entries[0];
  snap->solved_rows[1].win_count = 10;
  snap->solved_rows[1].win_pct = 10.0 / 72.0;
  snap->solved_ranks[1] = 40;

  char *status = peg_snapshot_get_status_string(snap, game, now_ns, NULL, 0);
  assert(has_substring(status, "\nProgress        50 of 60 (83%)\n"
                               "  Finished      2 (3.3%)\n"
                               "  Cut           48 (80.0%)\n"
                               "  Pending       10 (16.7%)\n"
                               "Draws           "));
  assert(has_substring(status, "\n  10  "));
  assert(has_substring(status, "<=0.01"));
  assert(!has_substring(status, "<=0.0 "));
  assert(has_substring(status, "\n ...  "));
  assert(has_substring(status, "\n  40  "));
  assert(has_substring(status, "13.9"));
  assert(!has_substring(status, "\n  11  "));
  assert(has_substring(status, "      (39 more, all cut)\n"));
  free(status);

  // A solved play below the pinned ones is counted, not named.
  snap->stage_history[1].cands_cut = 47;
  status = peg_snapshot_get_status_string(snap, game, now_ns, NULL, 0);
  assert(has_substring(status, "      (39 more, including 1 solved)\n"));
  free(status);
  free(snap);
}

void test_peg_poll(void) {
  log_set_level(LOG_FATAL);
  Config *config =
      config_create_or_die("set -lex CSW24 -threads 4 -s1 score -s2 score");
  load_and_exec_config_or_die(config, PEG_POLL_CGP);
  Game *game = config_get_game(config);

  PegPoll *poll = peg_poll_create();

  // Initial state before the solve: nothing published yet.
  PegPollSnapshot initial;
  peg_poll_read(poll, &initial);
  assert(initial.stage == -1);
  assert(initial.n_entries == 0);
  assert(!initial.done);

  PollerArgs poller;
  memset(&poller, 0, sizeof(poller));
  poller.poll = poll;
  atomic_init(&poller.stop, false);
  cpthread_t poller_thread;
  cpthread_create(&poller_thread, poller_main, &poller);

  static const int stage_top_k[] = {4, 2};
  PegArgs args;
  memset(&args, 0, sizeof(args));
  args.game = game;
  args.thread_control = config_get_thread_control(config);
  args.num_threads = 4;
  args.time_budget_seconds = 1.5; // short; the deadline gate bounds the solve
  args.stage_top_k = stage_top_k;
  args.num_stages = 2;
  args.poll = poll;

  PegResult result;
  memset(&result, 0, sizeof(result));
  ErrorStack *error_stack = error_stack_create();
  peg_solve(&args, &result, error_stack);
  assert(error_stack_is_empty(error_stack));

  atomic_store(&poller.stop, true);
  cpthread_join(poller_thread);

  // The poller ran concurrently and saw the leaderboard populate and the
  // version counter advance while peg_solve was working.
  assert(poller.reads > 0);
  assert(poller.max_entries_seen >= 1);
  assert(poller.max_version_seen >= 1);
  assert(poller.max_stage_seen >= 0);

  // Final poll: done, non-empty, and its top entry matches the solver's
  // published best move (the poll and PegResult are refreshed together at each
  // stage boundary, so they must agree at the end).
  PegPollSnapshot final;
  peg_poll_read(poll, &final);
  assert(final.done);
  assert(final.n_entries >= 1);
  char *json = peg_poll_get_json(poll, game);
  assert(has_substring(json, "\"done\":true"));
  assert(has_substring(json, "\"stages\":[{"));
  assert(has_substring(json, "\"entries\":[{"));
  char *expected_score = get_formatted_string(
      "\"score\":%d", equity_to_int(move_get_score(&final.entries[0].move)));
  assert(has_substring(json, expected_score));
  free(expected_score);
  assert(!has_substring(json, "nan"));
  assert(!has_substring(json, "inf"));
  assert(!has_substring(json, "\"seconds\":-"));
  free(json);

  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  StringBuilder *poll_best = string_builder_create();
  StringBuilder *solve_best = string_builder_create();
  string_builder_add_move(poll_best, board, &final.entries[0].move, ld, false);
  string_builder_add_move(solve_best, board, &result.best_move, ld, false);
  assert(strings_equal(string_builder_peek(poll_best),
                       string_builder_peek(solve_best)));

  // Draws belong to the candidate in progress, and none is once it is done.
  assert(final.n_draw_rows == 0);
  assert(final.draws_queued + final.draws_running + final.draws_done +
             final.draws_stopped ==
         0);

  printf("peg-poll: reads=%d draw_reads=%d max_entries=%d max_stage=%d "
         "versions=%llu best=%s win=%.1f\n",
         poller.reads, poller.draw_reads, poller.max_entries_seen,
         poller.max_stage_seen, (unsigned long long) final.version,
         string_builder_peek(poll_best), 100.0 * final.entries[0].win_pct);

  // Synthetic display snapshot: non-finite values and unusual distribution
  // glyphs must still serialize as valid JSON; malformed timing stays zero.
  PegPollSnapshot synthetic = {0};
  synthetic.n_entries = 1;
  synthetic.n_stage_history = 1;
  synthetic.entries[0].win_pct = NAN;
  synthetic.entries[0].mean_spread = INFINITY;
  synthetic.entries[0].move.move_type = GAME_EVENT_EXCHANGE;
  synthetic.entries[0].move.tiles_played = 1;
  synthetic.entries[0].move.tiles_length = 1;
  synthetic.entries[0].move.tiles[0] = 1;
  synthetic.stage_history[0].start_ns = 10;
  synthetic.stage_history[0].end_ns = 5;
  LetterDistribution *mutable_ld = config_get_ld(config);
  char original[MAX_LETTER_BYTE_LENGTH];
  memcpy(original, mutable_ld->ld_ml_to_hl[1], sizeof(original));
  memcpy(mutable_ld->ld_ml_to_hl[1], "\"\\\n", sizeof("\"\\\n"));
  json = peg_snapshot_get_json(&synthetic, game);
  memcpy(mutable_ld->ld_ml_to_hl[1], original, sizeof(original));
  assert(has_substring(json, "\\\""));
  assert(has_substring(json, "\\\\"));
  assert(has_substring(json, "\\u000a"));
  assert(has_substring(json, "\"win\":null"));
  assert(has_substring(json, "\"spread\":0.000000"));
  assert(has_substring(json, "\"seconds\":0.000"));
  free(json);
  if (test_platform_has_host_locales()) {
    char *saved_locale = string_duplicate(setlocale(LC_NUMERIC, NULL));
    if (setlocale(LC_NUMERIC, "de_DE.UTF-8")) {
      json = peg_snapshot_get_json(&synthetic, game);
      assert(has_substring(json, "\"spread\":0.000000"));
      assert(has_substring(json, "\"seconds\":0.000"));
      free(json);
    }
    const char *restored_locale = setlocale(LC_NUMERIC, saved_locale);
    assert(restored_locale);
    (void)restored_locale;
    free(saved_locale);
  } else {
    printf("peg-poll: skipped the locale case: no host locales here\n");
  }

  // A solve of a few candidates reaches the stages that solve one candidate
  // at a time, whose draws the poller checks while they run.
  Move few_moves[3];
  const Move *few[3];
  const int n_few = final.n_entries < 3 ? final.n_entries : 3;
  for (int move_idx = 0; move_idx < n_few; move_idx++) {
    few_moves[move_idx] = final.entries[move_idx].move;
    few[move_idx] = &few_moves[move_idx];
  }
  peg_poll_reset(poll);
  PollerArgs few_poller;
  memset(&few_poller, 0, sizeof(few_poller));
  few_poller.poll = poll;
  atomic_init(&few_poller.stop, false);
  cpthread_create(&poller_thread, poller_main, &few_poller);
  PegArgs few_args = args;
  few_args.only_moves = few;
  few_args.n_only_moves = n_few;
  few_args.time_budget_seconds = 3.0;
  PegResult few_result;
  memset(&few_result, 0, sizeof(few_result));
  peg_solve(&few_args, &few_result, error_stack);
  assert(error_stack_is_empty(error_stack));
  atomic_store(&few_poller.stop, true);
  cpthread_join(poller_thread);
  assert(few_poller.max_stage_seen >= 1);
  printf("peg-poll: few candidates: reads=%d draw_reads=%d max_stage=%d\n",
         few_poller.reads, few_poller.draw_reads, few_poller.max_stage_seen);
  peg_result_destroy(&few_result);

  test_peg_status_string(game, &final.entries[0].move);
  test_peg_status_ranking(game, &final.entries[0].move);

  string_builder_destroy(poll_best);
  string_builder_destroy(solve_best);
  if (search_memory_is_bounded()) {
    error_stack_reset(error_stack);
    args.tt_fraction_of_mem = 0.0000001;
    peg_result_destroy(&result);
    peg_solve(&args, &result, error_stack);
    assert(!error_stack_is_empty(error_stack));
    peg_poll_read(poll, &final);
    assert(final.done);
    error_stack_reset(error_stack);
  }
  error_stack_destroy(error_stack);
  peg_result_destroy(&result);
  peg_poll_destroy(poll);
  config_destroy(config);
}
