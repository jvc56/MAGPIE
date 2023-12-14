#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "../def/inference_defs.h"
#include "../def/rack_defs.h"
#include "../def/stats_defs.h"

#include "../util/log.h"
#include "../util/util.h"

#include "../util/string_util.h"
#include "letter_distribution_string.h"
#include "move_string.h"
#include "rack_string.h"

#include "../ent/autoplay_results.h"
#include "../ent/game.h"
#include "../ent/inference.h"
#include "../ent/leave_rack.h"
#include "../ent/move_gen.h"
#include "../ent/simmer.h"
#include "../ent/stats.h"
#include "../ent/thread_control.h"
#include "../ent/timer.h"

char *ucgi_sim_stats(Game *game, Simmer *simmer, ThreadControl *thread_control,
                     bool best_known_play) {
  sort_plays_by_win_rate(simmer);

  Timer *timer = get_timer(thread_control);
  mtimer_stop(timer);

  double elapsed = mtimer_elapsed_seconds(timer);
  int total_node_count = simmer_get_node_count(simmer);
  double nps = (double)total_node_count / elapsed;
  // No need to keep the mutex locked too long here. This is because this
  // function (ucgi_sim_stats) will only execute on a single thread.

  // info currmove h4.HADJI sc 40 wp 3.5 wpe 0.731 eq 7.2 eqe 0.812 it 12345
  // ig 0 ply1-scm 30 ply1-scd 3.7 ply1-bp 23 ply2-scm ...

  // sc - score, wp(e) - win perc
  // (error), eq(e) - equity (error) scm - mean of score, scd - stdev of
  // score, bp - bingo perc ig - this play has been cut-off
  // FIXME: get better numbers
  LetterDistribution *ld = game_get_ld(game);
  Board *board = game_get_board(game);
  StringBuilder *sim_stats_string_builder = create_string_builder();
  int number_of_simmed_plays = simmer_get_number_of_plays(simmer);
  for (int i = 0; i < number_of_simmed_plays; i++) {
    const SimmedPlay *play = simmer_get_simmed_play(simmer, i);
    Stat *win_pct_stat = simmed_play_get_win_pct_stat(play);
    double wp_mean = get_mean(win_pct_stat) * 100.0;
    double wp_se = get_standard_error(win_pct_stat, STATS_Z99) * 100.0;

    Stat *equity_stat = simmed_play_get_equity_stat(play);
    double eq_mean = get_mean(equity_stat);
    double eq_se = get_standard_error(equity_stat, STATS_Z99);
    uint64_t niters = get_cardinality(equity_stat);

    Move *move = simmed_play_get_move(play);
    string_builder_add_string(sim_stats_string_builder, "info currmove ");
    string_builder_add_ucgi_move(move, board, ld, sim_stats_string_builder);
    bool ignore = simmed_play_get_ignore(play);
    string_builder_add_formatted_string(
        sim_stats_string_builder,
        " sc %d wp %.3f wpe %.3f eq %.3f eqe %.3f it %llu "
        "ig %d ",
        get_score(move), wp_mean, wp_se, eq_mean, eq_se,
        // need cast for WASM:
        (long long unsigned int)niters, ignore);
    for (int i = 0; i < simmer_get_max_plies(simmer); i++) {
      string_builder_add_formatted_string(
          sim_stats_string_builder,
          "ply%d-scm %.3f ply%d-scd %.3f ply%d-bp %.3f ", i + 1,
          get_mean(simmed_play_get_score_stat(play, i)), i + 1,
          get_stdev(simmed_play_get_score_stat(play, i)), i + 1,
          get_mean(simmed_play_get_bingo_stat(play, i)) * 100.0);
    }
    string_builder_add_string(sim_stats_string_builder, "\n");
  }

  if (best_known_play) {
    string_builder_add_string(sim_stats_string_builder, "bestmove ");
  } else {
    string_builder_add_string(sim_stats_string_builder, "bestsofar ");
  }
  const SimmedPlay *play = simmer_get_simmed_play(simmer, 0);
  string_builder_add_ucgi_move(simmed_play_get_move(play), board, ld,
                               sim_stats_string_builder);
  string_builder_add_formatted_string(sim_stats_string_builder,
                                      "\ninfo nps %f\n", nps);
  char *sim_stats_string = string_builder_dump(sim_stats_string_builder, NULL);
  destroy_string_builder(sim_stats_string_builder);
  return sim_stats_string;
}

void print_ucgi_sim_stats(Game *game, Simmer *simmer,
                          ThreadControl *thread_control, bool print_best_play) {
  char *starting_stats_string_pointer =
      ucgi_sim_stats(game, simmer, thread_control, print_best_play);
  print_to_outfile(thread_control, starting_stats_string_pointer);
  free(starting_stats_string_pointer);
}