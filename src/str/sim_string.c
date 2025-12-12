
#include "../def/equity_defs.h"
#include "../def/game_history_defs.h"
#include "../ent/bai_result.h"
#include "../ent/board.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/rack.h"
#include "../ent/sim_results.h"
#include "../ent/thread_control.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "move_string.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

bool string_builder_add_sim_stats_with_display_lock(
    StringBuilder *sb, const Game *game, const SimResults *sim_results,
    int max_num_display_plays, bool use_ucgi_format) {
  const int num_simmed_plays = sim_results_get_number_of_plays(sim_results);
  int num_display_plays = num_simmed_plays;
  if (num_display_plays > max_num_display_plays) {
    num_display_plays = max_num_display_plays;
  }
  int num_rows = num_display_plays;
  if (!use_ucgi_format) {
    // +1 for header
    num_rows += 1;
  }
  const int num_plies = sim_results_get_num_plies(sim_results);
  const int num_cols = 9 + num_plies * 2;
  StringGrid *sg = string_grid_create(num_rows, num_cols, 1);

  int curr_row = 0;
  int curr_col = 0;
  if (!use_ucgi_format) {
    string_grid_set_cell(sg, curr_row, curr_col++, string_duplicate(""));
    string_grid_set_cell(sg, curr_row, curr_col++, string_duplicate("Move"));
    string_grid_set_cell(sg, curr_row, curr_col++, string_duplicate("Leave"));
    string_grid_set_cell(sg, curr_row, curr_col++, string_duplicate("Score"));
    string_grid_set_cell(sg, curr_row, curr_col++, string_duplicate("Ig"));
    string_grid_set_cell(sg, curr_row, curr_col++, string_duplicate("Win%"));
    string_grid_set_cell(sg, curr_row, curr_col++, string_duplicate("Eq"));
    string_grid_set_cell(sg, curr_row, curr_col++, string_duplicate("StEq"));
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
  BAIResult *bai_result = sim_results_get_bai_result(sim_results);
  const bai_result_status_t bai_result_status =
      bai_result_get_status(bai_result);
  for (int i = 0; i < num_display_plays; i++) {
    curr_col = 0;
    const SimmedPlay *sp = sim_results_get_display_simmed_play(sim_results, i);
    const Move *move = simmed_play_get_move(sp);
    string_builder_add_move(move_sb, board, move, ld, false);

    string_grid_set_cell(sg, curr_row, curr_col++,
                         get_formatted_string("%d:", i + 1));

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

    if (bai_result_status == BAI_RESULT_STATUS_THRESHOLD && i > 0 &&
        sim_results_plays_are_similar(sim_results, 0, i)) {
      string_grid_set_cell(sg, curr_row, curr_col, string_duplicate("X"));
    }
    curr_col++;

    const Stat *win_pct_stat = simmed_play_get_win_pct_stat(sp);
    string_grid_set_cell(
        sg, curr_row, curr_col++,
        get_formatted_string("%.2f", stat_get_mean(win_pct_stat) * 100));

    string_grid_set_cell(
        sg, curr_row, curr_col++,
        get_formatted_string("%.2f",
                             stat_get_mean(simmed_play_get_equity_stat(sp))));

    double move_equity;
    if (move_get_type(move) == GAME_EVENT_PASS) {
      move_equity = EQUITY_PASS_DISPLAY_DOUBLE;
    } else {
      move_equity = equity_to_double(move_get_equity(move));
    }
    string_grid_set_cell(sg, curr_row, curr_col++,
                         get_formatted_string("%.2f", move_equity));

    string_grid_set_cell(
        sg, curr_row, curr_col++,
        get_formatted_string("%lu", stat_get_num_samples(win_pct_stat)));

    for (int j = 0; j < num_plies; j++) {
      const PlyInfo *sp_ply_info = simmed_play_get_ply_info(sp, j);
      string_grid_set_cell(
          sg, curr_row, curr_col++,
          get_formatted_string("%.2f", stat_get_mean(sp_ply_info->score_stat)));
      string_grid_set_cell(
          sg, curr_row, curr_col++,
          get_formatted_string("%.2f",
                               stat_get_mean(sp_ply_info->bingo_stat) * 100.0));
    }
    curr_row++;
  }
  string_builder_destroy(move_sb);
  string_builder_add_string_grid(sb, sg, false);
  string_grid_destroy(sg);

  string_builder_add_formatted_string(sb, "\nShowing %d of %d simmed plays\n",
                                      num_display_plays, num_simmed_plays);

  StringGrid *summary_sg = string_grid_create(3, 2, 1);

  curr_row = 0;

  string_grid_set_cell(summary_sg, curr_row, 0, string_duplicate("Iters:"));
  string_grid_set_cell(
      summary_sg, curr_row, 1,
      get_formatted_string("%lu",
                           sim_results_get_iteration_count(sim_results)));
  curr_row++;

  string_grid_set_cell(summary_sg, curr_row, 0, string_duplicate("Time:"));
  string_grid_set_cell(
      summary_sg, curr_row, 1,
      get_formatted_string("%.2f seconds",
                           bai_result_get_elapsed_seconds(bai_result)));
  curr_row++;

  string_grid_set_cell(summary_sg, curr_row, 0, string_duplicate("Status:"));

  char *status_str = NULL;
  switch (bai_result_status) {
  case BAI_RESULT_STATUS_THRESHOLD:
    status_str =
        get_formatted_string("Finished (statistical threshold achieved)\n");
    break;
  case BAI_RESULT_STATUS_SAMPLE_LIMIT:
    status_str = get_formatted_string("Finished (max iterations reached)\n");
    break;
  case BAI_RESULT_STATUS_TIMEOUT:
    status_str = get_formatted_string("Finished (time limit exceeded)\n");
    break;
  case BAI_RESULT_STATUS_USER_INTERRUPT:
    status_str = get_formatted_string("Finished (user interrupt)\n");
    break;
  case BAI_RESULT_STATUS_NONE:
    status_str = get_formatted_string("Running\n");
    break;
  }

  string_grid_set_cell(summary_sg, curr_row, 1, status_str);
  string_builder_add_string_grid(sb, summary_sg, false);
  string_grid_destroy(summary_sg);

  return true;
}

void string_builder_add_sim_stats(StringBuilder *sb, const Game *game,
                                  SimResults *sim_results,
                                  int max_num_display_plays,
                                  bool use_ucgi_format) {
  // Only locks on success
  bool sim_stats_ready =
      sim_results_lock_and_sort_display_simmed_plays(sim_results);
  if (!sim_stats_ready) {
    string_builder_add_string(sb, "sim results not yet available\n");
    return;
  }
  string_builder_add_sim_stats_with_display_lock(
      sb, game, sim_results, max_num_display_plays, use_ucgi_format);
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
                             int max_num_display_plays, bool use_ucgi_format) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_sim_stats(sb, game, sim_results, max_num_display_plays,
                               use_ucgi_format);
  char *str = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  return str;
}

void sim_results_print(ThreadControl *thread_control, const Game *game,
                       SimResults *sim_results, int max_num_display_plays,
                       bool use_ucgi_format) {
  char *sim_stats_string = sim_results_get_string(
      game, sim_results, max_num_display_plays, use_ucgi_format);
  thread_control_print(thread_control, sim_stats_string);
  free(sim_stats_string);
}