#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../def/exec_defs.h"

#include "../ent/board.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/sim_results.h"
#include "../ent/stats.h"
#include "../ent/thread_control.h"
#include "../ent/timer.h"

#include "move_string.h"

#include "../util/math_util.h"
#include "../util/string_util.h"

char *ucgi_sim_stats(Game *game, SimResults *sim_results,
                     ThreadControl *thread_control, bool best_known_play) {
  sim_results_lock_simmed_plays(sim_results);
  bool success = sim_results_sort_plays_by_win_rate(sim_results);
  sim_results_unlock_simmed_plays(sim_results);

  if (!success) {
    return string_duplicate(SEARCH_STATUS_STARTING);
  }

  ThreadControlIterCompletedOutput iter_completed_output;
  thread_control_get_iter_count_completed(thread_control,
                                          &iter_completed_output);

  int total_node_count = sim_results_get_node_count(sim_results);
  double nps = (double)total_node_count / iter_completed_output.time_elapsed;
  // No need to keep the mutex locked too long here. This is because this
  // function (ucgi_sim_stats) will only execute on a single thread.

  // info currmove h4.HADJI sc 40 wp 3.5 wpe 0.731 eq 7.2 eqe 0.812 it 12345
  // ig 0 ply1-scm 30 ply1-scd 3.7 ply1-bp 23 ply2-scm ...

  // sc - score, wp(e) - win perc
  // (error), eq(e) - equity (error) scm - mean of score, scd - stdev of
  // score, bp - bingo perc ig - this play has been cut-off
  const LetterDistribution *ld = game_get_ld(game);
  Board *board = game_get_board(game);
  StringBuilder *sim_stats_string_builder = string_builder_create();
  int number_of_simmed_plays = sim_results_get_number_of_plays(sim_results);
  double zval = sim_results_get_zval(sim_results);
  bool z_valid = is_z_valid(zval);
  for (int i = 0; i < number_of_simmed_plays; i++) {
    const SimmedPlay *play = sim_results_get_simmed_play(sim_results, i);
    Stat *win_pct_stat = simmed_play_get_win_pct_stat(play);
    double wp_mean = stat_get_mean(win_pct_stat) * 100.0;
    double wp_margin_of_error = 0.0;
    if (z_valid) {
      wp_margin_of_error = stat_get_margin_of_error(win_pct_stat, zval) * 100.0;
    }

    Stat *equity_stat = simmed_play_get_equity_stat(play);
    double eq_mean = stat_get_mean(equity_stat);
    double eq_se = 0;
    if (z_valid) {
      eq_se = stat_get_margin_of_error(equity_stat, zval);
    }

    uint64_t niters = stat_get_num_unique_samples(equity_stat);

    Move *move = simmed_play_get_move(play);
    string_builder_add_string(sim_stats_string_builder, "info currmove ");
    string_builder_add_ucgi_move(sim_stats_string_builder, move, board, ld);
    bool ignore = simmed_play_get_ignore(play);
    string_builder_add_formatted_string(
        sim_stats_string_builder,
        " sc %d wp %.3f wpe %.3f eq %.3f eqe %.3f it %llu "
        "ig %d ",
        move_get_score(move), wp_mean, wp_margin_of_error, eq_mean, eq_se,
        // need cast for WASM:
        (long long unsigned int)niters, ignore);
    for (int j = 0; j < sim_results_get_max_plies(sim_results); j++) {
      string_builder_add_formatted_string(
          sim_stats_string_builder,
          "ply%d-scm %.3f ply%d-scd %.3f ply%d-bp %.3f ", j + 1,
          stat_get_mean(simmed_play_get_score_stat(play, j)), j + 1,
          stat_get_stdev(simmed_play_get_score_stat(play, j)), j + 1,
          stat_get_mean(simmed_play_get_bingo_stat(play, j)) * 100.0);
    }
    string_builder_add_string(sim_stats_string_builder, "\n");
  }

  if (best_known_play) {
    string_builder_add_string(sim_stats_string_builder, "bestmove ");
  } else {
    string_builder_add_string(sim_stats_string_builder, "bestsofar ");
  }
  const SimmedPlay *play = sim_results_get_simmed_play(sim_results, 0);
  string_builder_add_ucgi_move(sim_stats_string_builder,
                               simmed_play_get_move(play), board, ld);
  string_builder_add_formatted_string(sim_stats_string_builder,
                                      "\ninfo nps %f\n", nps);
  char *sim_stats_string = string_builder_dump(sim_stats_string_builder, NULL);
  string_builder_destroy(sim_stats_string_builder);
  return sim_stats_string;
}

void print_ucgi_sim_stats(Game *game, SimResults *sim_results,
                          ThreadControl *thread_control, bool print_best_play) {
  char *sim_stats_string =
      ucgi_sim_stats(game, sim_results, thread_control, print_best_play);
  thread_control_print(thread_control, sim_stats_string);
  free(sim_stats_string);
}