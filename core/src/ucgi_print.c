#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "autoplay.h"
#include "game.h"
#include "infer.h"
#include "leave_rack.h"
#include "log.h"
#include "sim.h"
#include "stats.h"
#include "thread_control.h"
#include "ucgi_formats.h"
#include "util.h"

// Inference

void print_ucgi_inference_current_rack(uint64_t current_rack_index,
                                       ThreadControl *thread_control) {
  char info_output[40];
  info_output[0] = '\0';
  sprintf(info_output, "info infercurrrack %llu\n",
          (long long unsigned int)current_rack_index);
  print_to_file(thread_control, info_output);
}

void print_ucgi_inference_total_racks_evaluated(uint64_t total_racks_evaluated,
                                                ThreadControl *thread_control) {
  char info_output[40];
  info_output[0] = '\0';
  sprintf(info_output, "info infertotalracks %llu\n",
          (long long unsigned int)total_racks_evaluated);
  print_to_file(thread_control, info_output);
}

void ucgi_write_leave_rack(char **buffer, LeaveRack *leave_rack, int index,
                           uint64_t total_draws,
                           LetterDistribution *letter_distribution,
                           int is_exchange) {
  char leave_string[(RACK_SIZE)] = "";
  write_rack_to_end_of_buffer(leave_string, letter_distribution,
                              leave_rack->leave);
  if (!is_exchange) {
    *buffer +=
        sprintf(*buffer, "infercommon %d %s %f %d %f\n", index + 1,
                leave_string, ((double)leave_rack->draws / total_draws) * 100,
                leave_rack->draws, leave_rack->equity);
  } else {
    char exchanged_string[(RACK_SIZE)] = "";
    write_rack_to_end_of_buffer(exchanged_string, letter_distribution,
                                leave_rack->exchanged);
    *buffer += sprintf(*buffer, "inferleave %d %s %s %f %d\n", index + 1,
                       leave_string, exchanged_string,
                       ((double)leave_rack->draws / total_draws) * 100,
                       leave_rack->draws);
  }
}

void ucgi_write_letter_minimum(InferenceRecord *record, Rack *rack,
                               Rack *bag_as_rack, char **inference_string,
                               uint8_t letter, int minimum,
                               int number_of_tiles_played_or_exchanged) {
  int draw_subtotal = get_subtotal_sum_with_minimum(
      record, letter, minimum, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW);
  int leave_subtotal = get_subtotal_sum_with_minimum(
      record, letter, minimum, INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE);
  double inference_probability =
      ((double)draw_subtotal) / (double)get_weight(record->equity_values);
  double random_probability = get_probability_for_random_minimum_draw(
      bag_as_rack, rack, letter, minimum, number_of_tiles_played_or_exchanged);
  *inference_string +=
      sprintf(*inference_string, " %f %f %d %d", inference_probability * 100,
              random_probability * 100, draw_subtotal, leave_subtotal);
}

void ucgi_write_letter_line(Game *game, InferenceRecord *record, Rack *rack,
                            Rack *bag_as_rack, Stat *letter_stat,
                            char **inference_string, uint8_t letter,
                            int number_of_tiles_played_or_exchanged,
                            const char *inference_record_type) {
  get_stat_for_letter(record, letter_stat, letter);
  char readable_letter[MAX_LETTER_CHAR_LENGTH];
  machine_letter_to_human_readable_letter(game->gen->letter_distribution,
                                          letter, readable_letter);

  *inference_string +=
      sprintf(*inference_string, "infertile %s %s %f %f", inference_record_type,
              readable_letter, get_mean(letter_stat), get_stdev(letter_stat));

  for (int i = 1; i <= (RACK_SIZE); i++) {
    ucgi_write_letter_minimum(record, rack, bag_as_rack, inference_string,
                              letter, i, number_of_tiles_played_or_exchanged);
  }
  *inference_string += sprintf(*inference_string, "\n");
}

void ucgi_write_inference_record(char **buffer, InferenceRecord *record,
                                 Game *game, Rack *rack, Rack *bag_as_rack,
                                 Stat *letter_stat,
                                 int number_of_tiles_played_or_exchanged,
                                 const char *inference_record_type) {
  uint64_t total_draws = get_weight(record->equity_values);
  uint64_t total_leaves = get_cardinality(record->equity_values);
  *buffer +=
      sprintf(*buffer, "infertotaldraws %s %llu\n", inference_record_type,
              (long long unsigned int)total_draws);
  *buffer +=
      sprintf(*buffer, "inferuniqueleaves %s %llu\n", inference_record_type,
              (long long unsigned int)total_leaves);
  *buffer += sprintf(*buffer, "inferleaveavg %s %f\n", inference_record_type,
                     get_mean(record->equity_values));
  *buffer += sprintf(*buffer, "inferleavestdev %s %f\n", inference_record_type,
                     get_stdev(record->equity_values));
  for (int i = 0; i < (int)game->gen->letter_distribution->size; i++) {
    ucgi_write_letter_line(game, record, rack, bag_as_rack, letter_stat, buffer,
                           i, number_of_tiles_played_or_exchanged,
                           inference_record_type);
  }
}

void print_ucgi_inference(Inference *inference, ThreadControl *thread_control) {
  int is_exchange = inference->number_of_tiles_exchanged > 0;
  int number_of_tiles_played_or_exchanged =
      inference->number_of_tiles_exchanged;
  if (number_of_tiles_played_or_exchanged == 0) {
    number_of_tiles_played_or_exchanged =
        (RACK_SIZE)-inference->initial_tiles_to_infer;
  }
  Game *game = inference->game;

  // Create a transient stat to use the stat functions
  Stat *letter_stat = create_stat();

  int records_string_size = 10000;
  if (is_exchange) {
    records_string_size = 20000;
  }
  char *records_string = (char *)malloc(sizeof(char) * records_string_size);
  char *starting_records_string_pointer = records_string;
  records_string[0] = '\0';
  ucgi_write_inference_record(&records_string, inference->leave_record, game,
                              inference->leave, inference->bag_as_rack,
                              letter_stat, number_of_tiles_played_or_exchanged,
                              "leave");
  InferenceRecord *common_leaves_record = inference->leave_record;
  if (is_exchange) {
    common_leaves_record = inference->rack_record;
    Rack *unknown_exchange_rack = create_rack(inference->leave->array_size);
    ucgi_write_inference_record(&records_string, inference->exchanged_record,
                                game, unknown_exchange_rack,
                                inference->bag_as_rack, letter_stat,
                                inference->number_of_tiles_exchanged, "exch");
    destroy_rack(unknown_exchange_rack);
    ucgi_write_inference_record(&records_string, inference->rack_record, game,
                                inference->leave, inference->bag_as_rack,
                                letter_stat, 0, "rack");
  }
  destroy_stat(letter_stat);

  // Get the list of most common leaves
  int number_of_common_leaves = inference->leave_rack_list->count;
  sort_leave_racks(inference->leave_rack_list);
  for (int common_leave_index = 0; common_leave_index < number_of_common_leaves;
       common_leave_index++) {
    LeaveRack *leave_rack =
        inference->leave_rack_list->leave_racks[common_leave_index];
    ucgi_write_leave_rack(&records_string, leave_rack, common_leave_index,
                          get_weight(common_leaves_record->equity_values),
                          game->gen->letter_distribution, is_exchange);
  }
  print_to_file(thread_control, starting_records_string_pointer);
  free(starting_records_string_pointer);
}

// Sim

char *ucgi_static_moves(Game *game, int nmoves) {
  int moves_size = nmoves * sizeof(char) * 90;
  char *moves_string = (char *)malloc(moves_size);
  char *starting_moves_string_pointer = moves_string;
  moves_string[0] = '\0';
  for (int i = 0; i < nmoves; i++) {
    char move[30];
    store_move_ucgi(game->gen->move_list->moves[i], game->gen->board, move,
                    game->gen->letter_distribution);
    moves_string +=
        sprintf(moves_string, "info currmove %s sc %d eq %.3f it 0\n", move,
                game->gen->move_list->moves[i]->score,
                game->gen->move_list->moves[i]->equity);
  }
  char move[30];
  store_move_ucgi(game->gen->move_list->moves[0], game->gen->board, move,
                  game->gen->letter_distribution);
  sprintf(moves_string, "bestmove %s\n", move);
  return starting_moves_string_pointer;
}

void print_ucgi_static_moves(Game *game, int nmoves,
                             ThreadControl *thread_control) {
  char *starting_moves_string_pointer = ucgi_static_moves(game, nmoves);
  print_to_file(thread_control, starting_moves_string_pointer);
  free(starting_moves_string_pointer);
}

char *ucgi_sim_stats(Simmer *simmer, Game *game, int best_known_play) {
  pthread_mutex_lock(&simmer->simmed_plays_mutex);
  sort_plays_by_win_rate(simmer->simmed_plays, simmer->num_simmed_plays);
  pthread_mutex_unlock(&simmer->simmed_plays_mutex);

  struct timespec finish_time;
  double elapsed;

  clock_gettime(CLOCK_MONOTONIC, &finish_time);

  elapsed = (finish_time.tv_sec - simmer->thread_control->start_time.tv_sec);
  elapsed +=
      (finish_time.tv_nsec - simmer->thread_control->start_time.tv_nsec) /
      1000000000.0;
  int total_node_count = atomic_load(&simmer->node_count);
  double nps = (double)total_node_count / elapsed;
  // No need to keep the mutex locked too long here. This is because this
  // function (ucgi_sim_stats) will only execute on a single thread.

  // info currmove h4.HADJI sc 40 wp 3.5 wpe 0.731 eq 7.2 eqe 0.812 it 12345
  // ig 0 ply1-scm 30 ply1-scd 3.7 ply1-bp 23 ply2-scm ...

  // sc - score, wp(e) - win perc
  // (error), eq(e) - equity (error) scm - mean of score, scd - stdev of
  // score, bp - bingo perc ig - this play has been cut-off
  // FIXME: get better numbers
  int max_line_length = 120 + (simmer->max_plies * 50);
  int output_size =
      (simmer->num_simmed_plays + 2) * sizeof(char) * max_line_length;
  char *stats_string = (char *)malloc(output_size);
  char *starting_stats_string_pointer = stats_string;
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
    int ignore = play->ignore;
    stats_string += sprintf(
        stats_string,
        "info currmove %s sc %d wp %.3f wpe %.3f eq %.3f eqe %.3f it %llu "
        "ig %d ",
        move, play->move->score, wp_mean, wp_se, eq_mean, eq_se,
        // need cast for WASM:
        (long long unsigned int)niters, ignore);
    for (int i = 0; i < simmer->max_plies; i++) {
      // stats_string += sprintf(stats_string, "ply %d ", i + 1);
      stats_string += sprintf(
          stats_string, "ply%d-scm %.3f ply%d-scd %.3f ply%d-bp %.3f ", i + 1,
          play->score_stat[i]->mean, i + 1, get_stdev(play->score_stat[i]),
          i + 1, play->bingo_stat[i]->mean * 100.0);
    }
    stats_string += sprintf(stats_string, "\n");
  }
  char move[30];
  SimmedPlay *play = simmer->simmed_plays[0];
  store_move_ucgi(play->move, game->gen->board, move,
                  game->gen->letter_distribution);
  if (best_known_play) {
    stats_string += sprintf(stats_string, "bestmove %s\n", move);
  } else {
    stats_string += sprintf(stats_string, "bestsofar %s\n", move);
  }
  stats_string += sprintf(stats_string, "info nps %f\n", nps);
  return starting_stats_string_pointer;
}

void print_ucgi_sim_stats(Simmer *simmer, Game *game, int print_best_play) {
  char *starting_stats_string_pointer =
      ucgi_sim_stats(simmer, game, print_best_play);
  print_to_file(simmer->thread_control, starting_stats_string_pointer);
  free(starting_stats_string_pointer);
}

// Autoplay

void print_ucgi_autoplay_results(AutoplayResults *autoplay_results,
                                 ThreadControl *thread_control) {
  char results_string[300];
  sprintf(results_string, "autoplay %d %d %d %d %d %f %f %f %f\n",
          autoplay_results->total_games, autoplay_results->p1_wins,
          autoplay_results->p1_losses, autoplay_results->p1_ties,
          autoplay_results->p1_firsts, get_mean(autoplay_results->p1_score),
          get_stdev(autoplay_results->p1_score),
          get_mean(autoplay_results->p2_score),
          get_stdev(autoplay_results->p2_score));
  print_to_file(thread_control, results_string);
}