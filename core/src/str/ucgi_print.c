#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "../def/inference_defs.h"
#include "../def/rack_defs.h"
#include "../def/stats_defs.h"

#include "../util/util.h"

#include "autoplay_results.h"
#include "game.h"
#include "inference.h"
#include "leave_rack.h"
#include "log.h"
#include "movegen.h"
#include "simmer.h"
#include "stats.h"
#include "thread_control.h"
#include "timer.h"
#include "ucgi_formats.h"

// Inference

void print_ucgi_inference_current_rack(uint64_t current_rack_index,
                                       ThreadControl *thread_control) {
  char *current_rack_info_string = get_formatted_string(
      "info infercurrrack %llu\n", (long long unsigned int)current_rack_index);
  print_to_outfile(thread_control, current_rack_info_string);
  free(current_rack_info_string);
}

void print_ucgi_inference_total_racks_evaluated(uint64_t total_racks_evaluated,
                                                ThreadControl *thread_control) {
  char *total_racks_info_string =
      get_formatted_string("info infertotalracks %llu\n",
                           (long long unsigned int)total_racks_evaluated);
  print_to_outfile(thread_control, total_racks_info_string);
  free(total_racks_info_string);
}

void string_builder_add_ucgi_leave_rack(
    const LeaveRack *leave_rack, const LetterDistribution *letter_distribution,
    StringBuilder *ucgi_string_builder, int index, uint64_t total_draws,
    bool is_exchange) {
  Rack *leave_rack_leave = leave_rack_get_leave(leave_rack);
  Rack *leave_rack_exchanged = leave_rack_get_exchanged(leave_rack);
  int draws = leave_rack_get_draws(leave_rack);
  double equity = leave_rack_get_equity(leave_rack);
  if (!is_exchange) {
    string_builder_add_rack(leave_rack_leave, letter_distribution,
                            ucgi_string_builder);
    string_builder_add_formatted_string(
        ucgi_string_builder, " %-3d %-6.2f %-6d %0.2f\n", index + 1,
        ((double)draws / total_draws) * 100, draws, equity);
  } else {
    string_builder_add_rack(leave_rack_leave, letter_distribution,
                            ucgi_string_builder);
    string_builder_add_spaces(ucgi_string_builder, 1);
    string_builder_add_rack(leave_rack_exchanged, letter_distribution,
                            ucgi_string_builder);
    string_builder_add_formatted_string(
        ucgi_string_builder, "%-3d %-6.2f %-6d\n", index + 1,
        ((double)draws / total_draws) * 100, draws);
  }
}

void string_builder_ucgi_add_letter_minimum(
    const InferenceRecord *record, const Rack *rack, const Rack *bag_as_rack,
    StringBuilder *ucgi_string_builder, uint8_t letter, int minimum,
    int number_of_tiles_played_or_exchanged) {
  int draw_subtotal = get_subtotal_sum_with_minimum(
      record, letter, minimum, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW);
  int leave_subtotal = get_subtotal_sum_with_minimum(
      record, letter, minimum, INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE);
  double inference_probability =
      ((double)draw_subtotal) /
      (double)get_weight(inference_record_get_equity_values(record));
  double random_probability = get_probability_for_random_minimum_draw(
      bag_as_rack, rack, letter, minimum, number_of_tiles_played_or_exchanged);
  string_builder_add_formatted_string(
      ucgi_string_builder, " %f %f %d %d", inference_probability * 100,
      random_probability * 100, draw_subtotal, leave_subtotal);
}

void string_builder_ucgi_add_letter_line(
    const Game *game, const InferenceRecord *record, const Rack *rack,
    const Rack *bag_as_rack, StringBuilder *ucgi_string_builder,
    Stat *letter_stat, uint8_t letter, int number_of_tiles_played_or_exchanged,
    const char *inference_record_type) {

  LetterDistribution *ld = gen_get_ld(game_get_gen(game));

  get_stat_for_letter(record, letter_stat, letter);
  string_builder_add_formatted_string(ucgi_string_builder, "infertile %s ",
                                      inference_record_type, 0);
  string_builder_add_user_visible_letter(ld, ucgi_string_builder, letter);
  string_builder_add_formatted_string(ucgi_string_builder, " %f %f",
                                      get_mean(letter_stat),
                                      get_stdev(letter_stat));

  for (int i = 1; i <= (RACK_SIZE); i++) {
    string_builder_ucgi_add_letter_minimum(record, rack, bag_as_rack,
                                           ucgi_string_builder, letter, i,
                                           number_of_tiles_played_or_exchanged);
  }
  string_builder_add_string(ucgi_string_builder, "\n");
}

void string_builder_ucgi_add_inference_record(
    const InferenceRecord *record, const Game *game, const Rack *rack,
    const Rack *bag_as_rack, StringBuilder *ucgi_string_builder,
    Stat *letter_stat, int number_of_tiles_played_or_exchanged,
    const char *inference_record_type) {
  Stat *equity_values = inference_record_get_equity_values(record);
  LetterDistribution *ld = gen_get_ld(game_get_gen(game));

  uint64_t total_draws = get_weight(equity_values);
  uint64_t total_leaves = get_cardinality(equity_values);

  string_builder_add_formatted_string(
      ucgi_string_builder,
      "infertotaldraws %s %llu\n"
      "inferuniqueleaves %s %llu\n"
      "inferleaveavg %s %f\n"
      "inferleavestdev %s %f\n",
      inference_record_type, total_draws, inference_record_type, total_leaves,
      inference_record_type, get_mean(equity_values), inference_record_type,
      get_stdev(equity_values));
  for (int i = 0; i < (int)letter_distribution_get_size(ld); i++) {
    string_builder_ucgi_add_letter_line(
        game, record, rack, bag_as_rack, ucgi_string_builder, letter_stat, i,
        number_of_tiles_played_or_exchanged, inference_record_type);
  }
}

void print_ucgi_inference(const Inference *inference,
                          ThreadControl *thread_control) {
  bool is_exchange = inference_get_number_of_tiles_exchanged(inference) > 0;
  int number_of_tiles_played_or_exchanged =
      inference_get_number_of_tiles_exchanged(inference);
  if (number_of_tiles_played_or_exchanged == 0) {
    number_of_tiles_played_or_exchanged =
        (RACK_SIZE)-inference_get_initial_tiles_to_infer(inference);
  }
  const Game *game = inference_get_game(inference);
  LetterDistribution *ld = gen_get_ld(game_get_gen(game));

  // Create a transient stat to use the stat functions
  Stat *letter_stat = create_stat();

  StringBuilder *ucgi_string_builder = create_string_builder();
  string_builder_ucgi_add_inference_record(
      inference_get_leave_record(inference), game,
      inference_get_leave(inference), inference_get_bag_as_rack(inference),
      ucgi_string_builder, letter_stat, number_of_tiles_played_or_exchanged,
      "leave");
  const InferenceRecord *common_leaves_record =
      inference_get_leave_record(inference);
  if (is_exchange) {
    common_leaves_record = inference_get_rack_record(inference);
    Rack *unknown_exchange_rack =
        create_rack(get_array_size(inference_get_leave(inference)));
    string_builder_ucgi_add_inference_record(
        inference_get_exchanged_record(inference), game, unknown_exchange_rack,
        inference_get_bag_as_rack(inference), ucgi_string_builder, letter_stat,
        inference_get_number_of_tiles_exchanged(inference), "exch");
    destroy_rack(unknown_exchange_rack);
    string_builder_ucgi_add_inference_record(
        inference_get_rack_record(inference), game,
        inference_get_leave(inference), inference_get_bag_as_rack(inference),
        ucgi_string_builder, letter_stat, 0, "rack");
  }
  destroy_stat(letter_stat);

  // Get the list of most common leaves
  int number_of_common_leaves = inference_get_leave_rack_list(inference)->count;
  sort_leave_racks(inference_get_leave_rack_list(inference));
  Stat *common_leave_equity_values =
      inference_record_get_equity_values(common_leaves_record);
  for (int common_leave_index = 0; common_leave_index < number_of_common_leaves;
       common_leave_index++) {
    const LeaveRack *leave_rack = inference_get_leave_rack_list(inference)
                                      ->leave_racks[common_leave_index];
    string_builder_add_ucgi_leave_rack(
        leave_rack, ld, ucgi_string_builder, common_leave_index,
        get_weight(common_leave_equity_values), is_exchange);
  }
  print_to_outfile(thread_control, string_builder_peek(ucgi_string_builder));
  destroy_string_builder(ucgi_string_builder);
}

// Sim

char *ucgi_static_moves(const Game *game, int nmoves) {
  StringBuilder *moves_string_builder = create_string_builder();
  LetterDistribution *ld = gen_get_ld(game_get_gen(game));
  Generator *gen = game_get_gen(game);
  MoveList *move_list = gen_get_move_list(gen);
  Board *board = gen_get_board(gen);

  for (int i = 0; i < nmoves; i++) {
    Move *move = move_list_get_move(move_list, i);
    string_builder_add_string(moves_string_builder, "info currmove ");
    string_builder_add_ucgi_move(move, board, ld, moves_string_builder);

    string_builder_add_formatted_string(moves_string_builder,
                                        " sc %d eq %.3f it 0\n",
                                        get_score(move), get_equity(move));
  }
  string_builder_add_string(moves_string_builder, "bestmove ");
  string_builder_add_ucgi_move(move_list_get_move(move_list, 0), board, ld,
                               moves_string_builder);
  string_builder_add_string(moves_string_builder, "\n");
  char *ucgi_static_moves_string =
      string_builder_dump(moves_string_builder, NULL);
  destroy_string_builder(moves_string_builder);
  return ucgi_static_moves_string;
}

void print_ucgi_static_moves(const Game *game, int nmoves,
                             ThreadControl *thread_control) {
  char *starting_moves_string_pointer = ucgi_static_moves(game, nmoves);
  print_to_outfile(thread_control, starting_moves_string_pointer);
  free(starting_moves_string_pointer);
}

char *ucgi_sim_stats(const Game *game, Simmer *simmer,
                     ThreadControl *thread_control, bool best_known_play) {
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
  LetterDistribution *ld = gen_get_ld(game_get_gen(game));
  Generator *gen = game_get_gen(game);
  Board *board = gen_get_board(gen);
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
    bool ignore = is_simmed_play_ignore(play);
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

void print_ucgi_sim_stats(const Game *game, Simmer *simmer,
                          ThreadControl *thread_control, bool print_best_play) {
  char *starting_stats_string_pointer =
      ucgi_sim_stats(game, simmer, thread_control, print_best_play);
  print_to_outfile(thread_control, starting_stats_string_pointer);
  free(starting_stats_string_pointer);
}

// Autoplay

void print_ucgi_autoplay_results(const AutoplayResults *autoplay_results,
                                 ThreadControl *thread_control) {
  char *results_string = get_formatted_string(
      "autoplay %d %d %d %d %d %f %f %f %f\n",
      get_total_games(autoplay_results), get_p1_wins(autoplay_results),
      get_p1_losses(autoplay_results), get_p1_ties(autoplay_results),
      get_p1_firsts(autoplay_results), get_mean(get_p1_score(autoplay_results)),
      get_stdev(get_p1_score(autoplay_results)),
      get_mean(get_p2_score(autoplay_results)),
      get_stdev(get_p2_score(autoplay_results)));
  print_to_outfile(thread_control, results_string);
  free(results_string);
}