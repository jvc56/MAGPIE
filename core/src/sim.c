#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "gameplay.h"
#include "rack.h"
#include "sim.h"
#include "stats.h"
#include "util.h"

Simmer *create_simmer(Config *config, Game *game) {
  Simmer *simmer = malloc(sizeof(Simmer));
  simmer->game = game;
  simmer->threads = 1;
  simmer->simming = 0;
  simmer->win_pcts = config->win_pcts;
  return simmer;
}

void add_score_stat(SimmedPlay *sp, int score, int is_bingo, int ply) {
  // remember the mutex locking later.
  push(sp->score_stat[ply], (double)score, 1);
  push(sp->bingo_stat[ply], (double)is_bingo, 1);
}

void add_equity_stat(SimmedPlay *sp, int initial_spread, int spread, float leftover) {
  push(sp->equity_stat, (double)(spread - initial_spread) + (double)(leftover), 1);
  push(sp->leftover_stat, (double)leftover, 1);
}

void add_winpct_stat(SimmedPlay *sp, WinPct *wp, int spread,
                     float leftover, int game_end_reason, int tiles_unseen,
                     int plies_are_odd) {

  double wpct = 0.0;
  if (game_end_reason != GAME_END_REASON_NONE) {
    // the game ended; use the actual result.
    if (spread == 0) {
      wpct = 0.5;
    } else if (spread > 0) {
      wpct = 1.0;
    }
  } else {
    int spread_plus_leftover = spread + round_to_nearest_int((double)leftover);
    // for an even-ply sim, it is our opponent's turn at the end of the sim.
    // the table is calculated from our perspective, so flip the spread.
    // i.e. if we are winning by 20 pts at the end of the sim, and our opponent
    // is on turn, we want to look up -20 as the spread, and then flip the win %
    // as well.
    if (!plies_are_odd) {
      spread_plus_leftover = -spread_plus_leftover;
    }
    wpct = (double)win_pct(wp, spread_plus_leftover, tiles_unseen);
    if (!plies_are_odd) {
      // see above comment regarding flipping win%
      wpct = 1.0 - wpct;
    }
  }
  push(sp->win_pct_stat, wpct, 1);
}

void make_game_copies(Simmer *simmer) {
  simmer->game_copies = malloc((sizeof(Game)) * simmer->threads);
  simmer->rack_placeholders = malloc((sizeof(Rack)) * simmer->threads);
  for (int i = 0; i < simmer->threads; i++) {
    simmer->game_copies[i] = copy_game(simmer->game);
    set_backup_mode(simmer->game_copies[i], BACKUP_MODE_SIMULATION);
    simmer->rack_placeholders[i] = create_rack(simmer->game->gen->letter_distribution->size);
  }
}

// this does all the reset work.
// note: this does not check that num_plays and the actual number of plays
// are the same. Caller should make sure we are not going to have a buffer
// overrun here.
void prepare_simmer(Simmer *simmer, int plies, Move **plays, int num_plays,
                    Rack *known_opp_rack) {
  simmer->max_plies = plies;
  make_game_copies(simmer);
  if (known_opp_rack != NULL) {
    simmer->known_opp_rack = copy_rack(known_opp_rack);
  } else {
    simmer->known_opp_rack = NULL;
  }
  simmer->simmed_plays = malloc((sizeof(SimmedPlay)) * num_plays);
  simmer->num_simmed_plays = num_plays;

  for (int i = 0; i < num_plays; i++) {
    SimmedPlay *sp = malloc(sizeof(SimmedPlay));
    sp->move = create_move();
    copy_move(plays[i], sp->move);

    sp->score_stat = malloc(sizeof(Stat *) * plies);
    sp->bingo_stat = malloc(sizeof(Stat *) * plies);
    sp->equity_stat = create_stat();
    sp->leftover_stat = create_stat();
    sp->win_pct_stat = create_stat();
    for (int j = 0; j < plies; j++) {
      sp->score_stat[j] = create_stat();
      sp->bingo_stat[j] = create_stat();
    }
    sp->ignore = 0;
    simmer->simmed_plays[i] = sp;
  }

  Game *gc = simmer->game_copies[0];
  simmer->initial_player = gc->player_on_turn_index;
  simmer->initial_spread = gc->players[gc->player_on_turn_index]->score -
                           gc->players[1 - gc->player_on_turn_index]->score;
}

void simulate(Simmer *simmer) {
  // assume that prepare_simmer has already been called.
  simmer->simming = 1;
}

Move *best_equity_play(Game *game) {
  StrategyParams *sp = game->players[game->player_on_turn_index]->strategy_params;
  int recorder_type = sp->play_recorder_type;
  sp->play_recorder_type = PLAY_RECORDER_TYPE_TOP_EQUITY;
  reset_move_list(game->gen->move_list);
  generate_moves(game->gen, game->players[game->player_on_turn_index],
                 game->players[1 - game->player_on_turn_index]->rack,
                 game->gen->bag->last_tile_index + 1 >= RACK_SIZE);
  // restore old recorder type
  sp->play_recorder_type = recorder_type;
  return game->gen->move_list->moves[0];
}

void sim_single_iteration(Simmer *simmer, int plies, int thread) {
  Game *gc = simmer->game_copies[thread];
  Rack *rack_placeholder = simmer->rack_placeholders[thread];

  // set random rack for opponent (throw in rack, shuffle, draw new tiles).
  set_random_rack(gc, 1 - gc->player_on_turn_index, simmer->known_opp_rack);
  // need a new shuffle for every iteration:
  shuffle(gc->gen->bag);

  for (int i = 0; i < simmer->num_simmed_plays; i++) {
    if (simmer->simmed_plays[i]->ignore) {
      continue;
    }

    double leftover = 0.0;
    set_backup_mode(gc, BACKUP_MODE_SIMULATION);
    // play move
    play_move(gc, simmer->simmed_plays[i]->move);

    set_backup_mode(gc, BACKUP_MODE_OFF);
    // further plies will NOT be backed up.
    for (int ply = 0; ply < plies; ply++) {
      int onturn = gc->player_on_turn_index;
      if (gc->game_end_reason != GAME_END_REASON_NONE) {
        // game is over.
        break;
      }

      Move *best_play = best_equity_play(gc);
      copy_rack_into(rack_placeholder, gc->players[onturn]->rack);
      play_move(gc, best_play);

      char placeholder[80];
      store_move_description(best_play, placeholder, simmer->game->gen->letter_distribution);

      if (ply == plies - 2 || ply == plies - 1) {
        double this_leftover = get_leave_value_for_move(
            gc->players[0]->strategy_params->klv, best_play, rack_placeholder);
        if (onturn == simmer->initial_player) {
          leftover += this_leftover;
        } else {
          leftover -= this_leftover;
        }
      }
      add_score_stat(simmer->simmed_plays[i], best_play->score, best_play->tiles_played == 7, ply);
    }

    int spread = gc->players[simmer->initial_player]->score - gc->players[1 - simmer->initial_player]->score;
    add_equity_stat(simmer->simmed_plays[i], simmer->initial_spread, spread, leftover);
    add_winpct_stat(simmer->simmed_plays[i], simmer->win_pcts,
                    spread, leftover, gc->game_end_reason,
                    // number of tiles unseen to us: bag tiles + tiles on opp rack.
                    gc->gen->bag->last_tile_index + 1 + gc->players[1 - simmer->initial_player]->rack->number_of_letters,
                    plies % 2);
    // reset to first state. we only need to restore one backup.
    unplay_last_move(gc);
  }
}

int compare_simmed_plays(const void *a, const void *b) {
  const SimmedPlay *play_a = *(const SimmedPlay **)a;
  const SimmedPlay *play_b = *(const SimmedPlay **)b;

  // Compare the mean values of win_pct_stat
  double mean_a = play_a->win_pct_stat->mean;
  double mean_b = play_b->win_pct_stat->mean;

  if (mean_a > mean_b) {
    return -1;
  } else if (mean_a < mean_b) {
    return 1;
  } else {
    // If win_pct_stat->mean values are equal, compare equity_stat->mean
    double equity_mean_a = play_a->equity_stat->mean;
    double equity_mean_b = play_b->equity_stat->mean;

    if (equity_mean_a > equity_mean_b) {
      return -1;
    } else if (equity_mean_a < equity_mean_b) {
      return 1;
    } else {
      return 0;
    }
  }
}

void sort_plays_by_win_rate(SimmedPlay **simmed_plays, int num_simmed_plays) {
  qsort(simmed_plays, num_simmed_plays, sizeof(SimmedPlay *), compare_simmed_plays);
}

void print_sim_stats(Simmer *simmer) {
  sort_plays_by_win_rate(simmer->simmed_plays, simmer->num_simmed_plays);
  printf("%-20s%-9s%-16s%-16s\n", "Play", "Score", "Win%", "Equity");
  for (int i = 0; i < simmer->num_simmed_plays; i++) {
    SimmedPlay *play = simmer->simmed_plays[i];
    double wp_mean = play->win_pct_stat->mean * 100.0;
    double wp_se = get_standard_error(play->win_pct_stat, STATS_Z99) * 100.0;

    double eq_mean = play->equity_stat->mean;
    double eq_se = get_standard_error(play->equity_stat, STATS_Z99);

    char wp[20];
    char eq[20];
    sprintf(wp, "%.3f±%.3f", wp_mean, wp_se);
    sprintf(eq, "%.3f±%.3f", eq_mean, eq_se);

    const char *ignore = play->ignore ? "❌" : "";
    char placeholder[80];
    store_move_description(play->move, placeholder, simmer->game->gen->letter_distribution);
    printf("%-20s%-9d%-16s%-16s%s\n", placeholder, play->move->score, wp, eq, ignore);
  }
}

// destructors

void free_simmed_plays(Simmer *simmer) {
  for (int i = 0; i < simmer->num_simmed_plays; i++) {
    for (int j = 0; j < simmer->max_plies; j++) {
      destroy_stat(simmer->simmed_plays[i]->bingo_stat[j]);
      destroy_stat(simmer->simmed_plays[i]->score_stat[j]);
    }
    free(simmer->simmed_plays[i]->bingo_stat);
    free(simmer->simmed_plays[i]->score_stat);
    destroy_stat(simmer->simmed_plays[i]->equity_stat);
    destroy_stat(simmer->simmed_plays[i]->leftover_stat);
    destroy_stat(simmer->simmed_plays[i]->win_pct_stat);
    destroy_move(simmer->simmed_plays[i]->move);
    free(simmer->simmed_plays[i]);
  }
  free(simmer->simmed_plays);
}

void free_game_copies(Simmer *simmer) {
  for (int i = 0; i < simmer->threads; i++) {
    destroy_game(simmer->game_copies[i]);
  }
  free(simmer->game_copies);
}

void free_rack_placeholders(Simmer *simmer) {
  for (int i = 0; i < simmer->threads; i++) {
    destroy_rack(simmer->rack_placeholders[i]);
  }
  free(simmer->rack_placeholders);
}

void destroy_simmer(Simmer *simmer) {
  free_simmed_plays(simmer);
  if (simmer->known_opp_rack != NULL) {
    destroy_rack(simmer->known_opp_rack);
  }
  free_game_copies(simmer);
  free_rack_placeholders(simmer);
  free(simmer);
}
