#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "sim.h"
#include "stats.h"
#include "ucgi_formats.h"

void print_to_file(FILE *outfile, const char *content,
                   pthread_mutex_t *print_output_mutex) {
  // Lock to print unconditionally even if we might not need
  // to for simplicity. The performance cost is negligible.
  pthread_mutex_lock(print_output_mutex);
  fprintf(outfile, "%s", content);
  fflush(outfile);
  pthread_mutex_unlock(print_output_mutex);
}

void print_ucgi_static_moves(Game *game, int nmoves, FILE *outfile,
                             pthread_mutex_t *print_output_mutex) {
  int moves_size = nmoves * sizeof(char) * 90;
  char *moves_string = (char *)malloc(moves_size);
  moves_string[0] = '\0';
  for (int i = 0; i < nmoves; i++) {
    char move[30];
    store_move_ucgi(game->gen->move_list->moves[i], game->gen->board, move,
                    game->gen->letter_distribution);
    sprintf(moves_string + strlen(moves_string),
            "info currmove %s sc %d eq %.3f it 0\n", move,
            game->gen->move_list->moves[i]->score,
            game->gen->move_list->moves[i]->equity);
  }
  print_to_file(outfile, moves_string, print_output_mutex);
  free(moves_string);
}

void print_ucgi_sim_stats(Simmer *simmer, Game *game, double nps,
                          int print_best_play) {
  pthread_mutex_lock(&simmer->simmed_plays_mutex);
  sort_plays_by_win_rate(simmer->simmed_plays, simmer->num_simmed_plays);
  pthread_mutex_unlock(&simmer->simmed_plays_mutex);
  // No need to keep the mutex locked too long here. This is because this
  // function (print_ucgi_stats_string) will only execute on a single thread.

  // info currmove h4.HADJI sc 40 wp 3.5 wpe 0.731 eq 7.2 eqe 0.812 it 12345 ig
  // 0 plies ply 1 scm 30 scd 3.7 bp 23 ply 2 ...

  // sc - score, wp(e) - win perc
  // (error), eq(e) - equity (error) scm - mean of score, scd - stdev of score,
  // bp - bingo perc ig - this play has been cut-off
  // plies ply 1 ... ply 2 ... ply 3 ...

  // FIXME: get better numbers
  int max_line_length = 120 + (simmer->max_plies * 50);
  int output_size =
      (simmer->num_simmed_plays + 2) * sizeof(char) * max_line_length;
  char *stats_string = (char *)malloc(output_size);
  stats_string[0] = '\0';
  for (int i = 0; i < simmer->num_simmed_plays; i++) {
    SimmedPlay *play = simmer->simmed_plays[i];
    char move[30];
    double wp_mean = play->win_pct_stat->mean * 100.0;
    double wp_se = get_standard_error(play->win_pct_stat, STATS_Z99) * 100.0;

    double eq_mean = play->equity_stat->mean;
    double eq_se = get_standard_error(play->equity_stat, STATS_Z99);
    uint64_t niters = play->equity_stat->cardinality;
    store_move_ucgi(play->move, game->gen->board, move,
                    game->gen->letter_distribution);

    sprintf(stats_string + strlen(stats_string),
            "info currmove %s sc %d wp %.3f wpe %.3f eq %.3f eqe %.3f it %lu "
            "ig %d ",
            move, play->move->score, wp_mean, wp_se, eq_mean, eq_se, niters,
            play->ignore);
    sprintf(stats_string + strlen(stats_string), "plies ");
    for (int i = 0; i < simmer->max_plies; i++) {
      sprintf(stats_string + strlen(stats_string), "ply %d ", i + 1);
      sprintf(stats_string + strlen(stats_string), "scm %.3f scd %.3f bp %.3f ",
              play->score_stat[i]->mean, get_stdev(play->score_stat[i]),
              play->bingo_stat[i]->mean * 100.0);
    }
    sprintf(stats_string + strlen(stats_string), "\n");
  }
  if (print_best_play) {
    char move[30];
    SimmedPlay *play = simmer->simmed_plays[0];
    store_move_ucgi(play->move, game->gen->board, move,
                    game->gen->letter_distribution);
    sprintf(stats_string + strlen(stats_string), "bestmove %s\n", move);
  }
  if (nps > 0) {
    sprintf(stats_string + strlen(stats_string), "info nps %f\n", nps);
  }
  print_to_file(simmer->outfile, stats_string, &simmer->print_output_mutex);
  free(stats_string);
}
