#include "peg_string.h"

#include "../ent/board.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"
#include "../impl/peg.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "move_string.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static char *peg_build_outcomes_string(const PegResult *result) {
  StringBuilder *sb = string_builder_create();
  const char *group_labels[3] = {"W:", "T:", "L:"};
  for (int group_idx = 0; group_idx < 3; group_idx++) {
    bool first_in_group = true;
    for (int scen_idx = 0; scen_idx < result->n_per_scenario; scen_idx++) {
      const PegPerScenario *sc = &result->per_scenario[scen_idx];
      const bool is_win = sc->mover_total > 0;
      const bool is_tie = sc->mover_total == 0;
      bool matches;
      if (group_idx == 0) {
        matches = is_win;
      } else if (group_idx == 1) {
        matches = is_tie;
      } else {
        matches = !is_win && !is_tie;
      }
      if (!matches) {
        continue;
      }
      if (first_in_group) {
        if (string_builder_length(sb) > 0) {
          string_builder_add_string(sb, " ");
        }
        string_builder_add_string(sb, group_labels[group_idx]);
        first_in_group = false;
      }
      string_builder_add_formatted_string(sb, " [%s]%s", sc->drawn,
                                          sc->remaining);
    }
  }
  char *out = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  return out;
}

char *peg_result_get_string(const PegResult *result, const Game *game,
                            bool show_outcomes) {
  StringBuilder *sb = string_builder_create();
  if (result->last_completed_stage < 0 || result->n_top_cands == 0) {
    string_builder_add_string(sb, "no PEG results.\n");
    char *out = string_builder_dump(sb, NULL);
    string_builder_destroy(sb);
    return out;
  }

  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);

  string_builder_add_formatted_string(
      sb, "PEG (last completed stage %d): %d candidates, %.2fs\n",
      result->last_completed_stage, result->n_top_cands,
      ctimer_elapsed_seconds(&result->timer));

  const bool have_outcomes = show_outcomes && result->n_per_scenario > 0;
  const int num_cols = have_outcomes ? 6 : 5;
  const int num_rows = result->n_top_cands + 1;

  StringGrid *sg = string_grid_create(num_rows, num_cols, 2);

  string_grid_set_cell(sg, 0, 0, string_duplicate("rank"));
  string_grid_set_cell(sg, 0, 1, string_duplicate("move"));
  string_grid_set_cell(sg, 0, 2, string_duplicate("wins"));
  string_grid_set_cell(sg, 0, 3, string_duplicate("win%"));
  string_grid_set_cell(sg, 0, 4, string_duplicate("spread"));
  if (have_outcomes) {
    string_grid_set_cell(sg, 0, 5, string_duplicate("outcomes"));
  }

  for (int cand_idx = 0; cand_idx < result->n_top_cands; cand_idx++) {
    const PegRankedCand *cand = &result->top_cands[cand_idx];
    const int row = cand_idx + 1;

    string_grid_set_cell(sg, row, 0, get_formatted_string("%d", cand_idx + 1));

    StringBuilder *move_sb = string_builder_create();
    string_builder_add_move(move_sb, board, &cand->move, ld, false);
    string_grid_set_cell(sg, row, 1, string_builder_dump(move_sb, NULL));
    string_builder_destroy(move_sb);

    const double wins = cand->win_pct * (double)cand->weight_sum;
    string_grid_set_cell(sg, row, 2, get_formatted_string("%.1f", wins));
    string_grid_set_cell(sg, row, 3,
                         get_formatted_string("%.1f", 100.0 * cand->win_pct));
    string_grid_set_cell(sg, row, 4,
                         get_formatted_string("%+.1f", cand->mean_spread));

    if (have_outcomes) {
      if (cand_idx == 0) {
        string_grid_set_cell(sg, row, 5, peg_build_outcomes_string(result));
      } else {
        string_grid_set_cell(sg, row, 5, string_duplicate(""));
      }
    }
  }

  string_builder_add_string_grid(sb, sg, false);
  string_grid_destroy(sg);

  char *out = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  return out;
}
