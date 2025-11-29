
#include "../ent/game.h"
#include "../ent/sim_results.h"
#include "../ent/thread_control.h"
#include "move_string.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

bool string_builder_add_sim_stats_with_display_lock(StringBuilder *sb,
                                                    const Game *game,
                                                    SimResults *sim_results,
                                                    bool use_ucgi_format) {
  const int number_of_simmed_plays =
      sim_results_get_number_of_plays(sim_results);
  int num_rows = number_of_simmed_plays;
  if (!use_ucgi_format) {
    // +1 for header
    num_rows += 1;
  }
  const int num_plies = sim_results_get_num_plies(sim_results);
  const int num_cols = 8 + num_plies * 2;
  StringGrid *sg = string_grid_create(num_rows, num_cols, 1);

  int curr_row = 0;
  int curr_col = 0;
  if (!use_ucgi_format) {
    string_grid_set_cell(sg, curr_row, curr_col++, string_duplicate(""));
    string_grid_set_cell(sg, curr_row, curr_col++, string_duplicate("Move"));
    string_grid_set_cell(sg, curr_row, curr_col++, string_duplicate("Leave"));
    string_grid_set_cell(sg, curr_row, curr_col++, string_duplicate("Score"));
    string_grid_set_cell(sg, curr_row, curr_col++,
                         string_duplicate("Static Eq"));
    string_grid_set_cell(sg, curr_row, curr_col++, string_duplicate("Equity"));
    string_grid_set_cell(sg, curr_row, curr_col++, string_duplicate("WinPct"));
    string_grid_set_cell(sg, curr_row, curr_col++, string_duplicate("Iters"));
    for (int j = 0; j < num_plies; j++) {
      string_grid_set_cell(sg, curr_row, curr_col++,
                           get_formatted_string("Ply%d-S", j + 1));
      string_grid_set_cell(sg, curr_row, curr_col++,
                           get_formatted_string("Ply%d-BP", j + 1));
    }
    curr_row++;
  }

  const LetterDistribution *ld = game_get_ld(game);
  const Board *board = game_get_board(game);
  StringBuilder *move_sb = string_builder_create();
  const Rack *rack = sim_results_get_rack(sim_results);
  const uint16_t rack_dist_size = rack_get_dist_size(rack);
  for (int i = 0; i < number_of_simmed_plays; i++) {
    curr_col = 0;
    const SimmedPlayDisplayInfo *sp_dinfo =
        sim_results_get_display_info(sim_results, i);
    const Move *move = &sp_dinfo->move;
    string_builder_add_move(move_sb, board, move, ld, false);

    string_grid_set_cell(sg, curr_row, curr_col++,
                         get_formatted_string("%d: ", i + 1));

    string_grid_set_cell(sg, curr_row, curr_col++,
                         string_builder_dump(move_sb, NULL));
    string_builder_clear(move_sb);

    // The rack from which the move is made should always
    // be set, but in case it isn't, skip leave display
    if (rack_dist_size > 0) {
      string_builder_add_move_leave(move_sb, rack, move, ld);
      string_grid_set_cell(sg, curr_row, curr_col++,
                           string_builder_dump(move_sb, NULL));
      string_builder_clear(move_sb);
    } else {
      curr_col++;
    }

    string_grid_set_cell(
        sg, curr_row, curr_col++,
        get_formatted_string("%d", equity_to_int(move_get_score(move))));

    string_grid_set_cell(
        sg, curr_row, curr_col++,
        get_formatted_string("%.2f", equity_to_double(move_get_equity(move))));

    string_grid_set_cell(sg, curr_row, curr_col++,
                         get_formatted_string("%.2f", sp_dinfo->equity_mean));

    string_grid_set_cell(
        sg, curr_row, curr_col++,
        get_formatted_string("%.2f", sp_dinfo->win_pct_mean * 100));

    string_grid_set_cell(sg, curr_row, curr_col++,
                         get_formatted_string("%lu", sp_dinfo->niters));

    for (int j = 0; j < num_plies; j++) {
      string_grid_set_cell(
          sg, curr_row, curr_col++,
          get_formatted_string("%.2f", sp_dinfo->score_means[j]));
      string_grid_set_cell(
          sg, curr_row, curr_col++,
          get_formatted_string("%.2f", sp_dinfo->bingo_means[j] * 100.0));
    }
    curr_row++;
  }
  string_builder_destroy(move_sb);
  string_builder_add_string_grid(sb, sg, false);
  string_grid_destroy(sg);
  return true;
}

void string_builder_add_sim_stats(StringBuilder *sb, const Game *game,
                                  SimResults *sim_results,
                                  bool use_ucgi_format) {
  // Only locks on success
  bool sim_stats_ready = sim_results_lock_and_sort_display_infos(sim_results);
  if (!sim_stats_ready) {
    string_builder_add_string(sb, "sim results not yet available\n");
    return;
  }
  string_builder_add_sim_stats_with_display_lock(sb, game, sim_results,
                                                 use_ucgi_format);
  if (use_ucgi_format) {
    string_builder_add_formatted_string(
        sb, "\ninfo nps %f\n",
        (double)sim_results_get_node_count(sim_results) /
            bai_result_get_elapsed_seconds(
                sim_results_get_bai_result(sim_results)));
  }
  sim_results_unlock_display_infos(sim_results);
}

char *sim_results_get_string(const Game *game, SimResults *sim_results,
                             bool use_ucgi_format) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_sim_stats(sb, game, sim_results, use_ucgi_format);
  char *str = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  return str;
}

void sim_results_print(ThreadControl *thread_control, const Game *game,
                       SimResults *sim_results, bool use_ucgi_format) {
  char *sim_stats_string =
      sim_results_get_string(game, sim_results, use_ucgi_format);
  thread_control_print(thread_control, sim_stats_string);
  free(sim_stats_string);
}