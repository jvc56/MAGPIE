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
  char *current_rack_info_string = get_formatted_string(
      "info infercurrrack %llu\n", (long long unsigned int)current_rack_index);
  print_to_file(thread_control, current_rack_info_string);
  free(current_rack_info_string);
}

void print_ucgi_inference_total_racks_evaluated(uint64_t total_racks_evaluated,
                                                ThreadControl *thread_control) {
  char *total_racks_info_string =
      get_formatted_string("info infertotalracks %llu\n",
                           (long long unsigned int)total_racks_evaluated);
  print_to_file(thread_control, total_racks_info_string);
  free(total_racks_info_string);
}

void string_builder_add_ucgi_leave_rack(LeaveRack *leave_rack, int index,
                                        uint64_t total_draws,
                                        LetterDistribution *letter_distribution,
                                        int is_exchange,
                                        StringBuilder *ucgi_string_builder) {
  if (is_exchange) {
    string_builder_add_rack(leave_rack->leave, letter_distribution,
                            ucgi_string_builder);
    string_builder_add_formatted_string(
        ucgi_string_builder, "%-3d %-6.2f %-6d %0.2f\n", index + 1,
        ((double)leave_rack->draws / total_draws) * 100, leave_rack->draws,
        leave_rack->equity);
  } else {
    string_builder_add_rack(leave_rack->leave, letter_distribution,
                            ucgi_string_builder);
    string_builder_add_spaces(ucgi_string_builder, 1, 0);
    string_builder_add_rack(leave_rack->exchanged, letter_distribution,
                            ucgi_string_builder);
    string_builder_add_formatted_string(
        ucgi_string_builder, "%-3d %-6.2f %-6d\n", index + 1,
        ((double)leave_rack->draws / total_draws) * 100, leave_rack->draws);
  }
}

void string_builder_ucgi_add_letter_minimum(
    InferenceRecord *record, Rack *rack, Rack *bag_as_rack, uint8_t letter,
    int minimum, int number_of_tiles_played_or_exchanged,
    StringBuilder *ucgi_string_builder) {
  int draw_subtotal = get_subtotal_sum_with_minimum(
      record, letter, minimum, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW);
  int leave_subtotal = get_subtotal_sum_with_minimum(
      record, letter, minimum, INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE);
  double inference_probability =
      ((double)draw_subtotal) / (double)get_weight(record->equity_values);
  double random_probability = get_probability_for_random_minimum_draw(
      bag_as_rack, rack, letter, minimum, number_of_tiles_played_or_exchanged);
  string_builder_add_formatted_string(
      ucgi_string_builder, " %f %f %d %d", inference_probability * 100,
      random_probability * 100, draw_subtotal, leave_subtotal);
}

void string_builder_ucgi_add_letter_line(
    Game *game, InferenceRecord *record, Rack *rack, Rack *bag_as_rack,
    Stat *letter_stat, uint8_t letter, int number_of_tiles_played_or_exchanged,
    const char *inference_record_type, StringBuilder *ucgi_string_builder) {
  get_stat_for_letter(record, letter_stat, letter);
  string_builder_add_formatted_string(ucgi_string_builder, "infertile %s ",
                                      inference_record_type, 0);
  string_builder_add_user_visible_letter(game->gen->letter_distribution, letter,
                                         0, ucgi_string_builder);
  string_builder_add_formatted_string(ucgi_string_builder, " %f %f",
                                      get_mean(letter_stat),
                                      get_stdev(letter_stat));

  for (int i = 1; i <= (RACK_SIZE); i++) {
    string_builder_ucgi_add_letter_minimum(record, rack, bag_as_rack, letter, i,
                                           number_of_tiles_played_or_exchanged,
                                           ucgi_string_builder);
  }
  string_builder_add_string(ucgi_string_builder, "\n", 0);
}

void string_builder_ucgi_add_inference_record(
    InferenceRecord *record, Game *game, Rack *rack, Rack *bag_as_rack,
    Stat *letter_stat, int number_of_tiles_played_or_exchanged,
    const char *inference_record_type, StringBuilder *ucgi_string_builder) {
  uint64_t total_draws = get_weight(record->equity_values);
  uint64_t total_leaves = get_cardinality(record->equity_values);

  string_builder_add_formatted_string(
      ucgi_string_builder,
      "infertotaldraws %s %llu\n"
      "inferuniqueleaves %s %llu\n"
      "inferleaveavg %s %f\n"
      "inferleavestdev %s %f\n",
      inference_record_type, total_draws, inference_record_type, total_leaves,
      inference_record_type, get_mean(record->equity_values),
      inference_record_type, get_stdev(record->equity_values));
  for (int i = 0; i < (int)game->gen->letter_distribution->size; i++) {
    string_builder_ucgi_add_letter_line(
        game, record, rack, bag_as_rack, letter_stat, i,
        number_of_tiles_played_or_exchanged, inference_record_type,
        ucgi_string_builder);
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

  StringBuilder *ucgi_string_builder = create_string_builder();
  string_builder_ucgi_add_inference_record(
      inference->leave_record, game, inference->leave, inference->bag_as_rack,
      letter_stat, number_of_tiles_played_or_exchanged, "leave",
      ucgi_string_builder);
  InferenceRecord *common_leaves_record = inference->leave_record;
  if (is_exchange) {
    common_leaves_record = inference->rack_record;
    Rack *unknown_exchange_rack = create_rack(inference->leave->array_size);
    string_builder_ucgi_add_inference_record(
        inference->exchanged_record, game, unknown_exchange_rack,
        inference->bag_as_rack, letter_stat,
        inference->number_of_tiles_exchanged, "exch", ucgi_string_builder);
    destroy_rack(unknown_exchange_rack);
    string_builder_ucgi_add_inference_record(
        inference->rack_record, game, inference->leave, inference->bag_as_rack,
        letter_stat, 0, "rack", ucgi_string_builder);
  }
  destroy_stat(letter_stat);

  // Get the list of most common leaves
  int number_of_common_leaves = inference->leave_rack_list->count;
  sort_leave_racks(inference->leave_rack_list);
  for (int common_leave_index = 0; common_leave_index < number_of_common_leaves;
       common_leave_index++) {
    LeaveRack *leave_rack =
        inference->leave_rack_list->leave_racks[common_leave_index];
    string_builder_add_ucgi_leave_rack(
        leave_rack, common_leave_index,
        get_weight(common_leaves_record->equity_values),
        game->gen->letter_distribution, is_exchange, ucgi_string_builder);
  }
  print_to_file(thread_control, string_builder_peek(ucgi_string_builder));
  destroy_string_builder(ucgi_string_builder);
}

// Sim

char *ucgi_static_moves(Game *game, int nmoves) {
  StringBuilder *moves_string_builder = create_string_builder();
  for (int i = 0; i < nmoves; i++) {
    string_builder_add_string(moves_string_builder, "info currmove ", 0);
    string_builder_add_ucgi_move(
        game->gen->move_list->moves[i], game->gen->board,
        game->gen->letter_distribution, moves_string_builder);

    string_builder_add_formatted_string(moves_string_builder,
                                        "sc %d eq %.3f it 0\n",
                                        game->gen->move_list->moves[i]->score,
                                        game->gen->move_list->moves[i]->equity);
  }
  string_builder_add_string(moves_string_builder, "bestmove ", 0);
  string_builder_add_ucgi_move(game->gen->move_list->moves[0], game->gen->board,
                               game->gen->letter_distribution,
                               moves_string_builder);
  string_builder_add_string(moves_string_builder, "\n", 0);
  char *ucgi_static_moves_string = string_builder_dump(moves_string_builder, 0);
  destroy_string_builder(moves_string_builder);
  return ucgi_static_moves_string;
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
  StringBuilder *sim_stats_string_builder = create_string_builder();
  for (int i = 0; i < simmer->num_simmed_plays; i++) {
    SimmedPlay *play = simmer->simmed_plays[i];
    double wp_mean = play->win_pct_stat->mean * 100.0;
    double wp_se = get_standard_error(play->win_pct_stat, STATS_Z99) * 100.0;

    double eq_mean = play->equity_stat->mean;
    double eq_se = get_standard_error(play->equity_stat, STATS_Z99);
    uint64_t niters = play->equity_stat->cardinality;

    string_builder_add_string(sim_stats_string_builder, "info currmove ", 0);
    string_builder_add_ucgi_move(play->move, game->gen->board,
                                 game->gen->letter_distribution,
                                 sim_stats_string_builder);
    int ignore = play->ignore;
    string_builder_add_formatted_string(
        sim_stats_string_builder,
        " sc %d wp %.3f wpe %.3f eq %.3f eqe %.3f it %llu "
        "ig %d ",
        play->move->score, wp_mean, wp_se, eq_mean, eq_se,
        // need cast for WASM:
        (long long unsigned int)niters, ignore);
    for (int i = 0; i < simmer->max_plies; i++) {
      string_builder_add_formatted_string(
          sim_stats_string_builder,
          "ply%d-scm %.3f ply%d-scd %.3f ply%d-bp %.3f ", i + 1,
          play->score_stat[i]->mean, i + 1, get_stdev(play->score_stat[i]),
          i + 1, play->bingo_stat[i]->mean * 100.0);
    }
    string_builder_add_string(sim_stats_string_builder, "\n", 0);
  }

  if (best_known_play) {
    string_builder_add_string(sim_stats_string_builder, "bestmove ", 0);
  } else {
    string_builder_add_string(sim_stats_string_builder, "bestsofar ", 0);
  }
  SimmedPlay *play = simmer->simmed_plays[0];
  string_builder_add_ucgi_move(play->move, game->gen->board,
                               game->gen->letter_distribution,
                               sim_stats_string_builder);
  string_builder_add_formatted_string(sim_stats_string_builder,
                                      "\ninfo nps %f\n", nps);
  char *sim_stats_string = string_builder_dump(sim_stats_string_builder, 0);
  destroy_string_builder(sim_stats_string_builder);
  return sim_stats_string;
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