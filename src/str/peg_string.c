#include "peg_string.h"

#include "../compat/ctime.h"
#include "../ent/board.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"
#include "../impl/peg.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "move_string.h"
#include <inttypes.h>
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

static void peg_append_stage_table(StringBuilder *sb,
                                   const PegStageSnapshot *history,
                                   int n_history) {
  const int64_t now_ns = ctimer_monotonic_ns();
  const int num_rows = n_history + 1;
  StringGrid *sg = string_grid_create(num_rows, 5, 2);
  string_grid_set_cell(sg, 0, 0, string_duplicate("stage"));
  string_grid_set_cell(sg, 0, 1, string_duplicate("fidelity"));
  string_grid_set_cell(sg, 0, 2, string_duplicate("cands"));
  string_grid_set_cell(sg, 0, 3, string_duplicate("best win%"));
  string_grid_set_cell(sg, 0, 4, string_duplicate("time"));

  for (int stage_idx = 0; stage_idx < n_history; stage_idx++) {
    const PegStageSnapshot *st = &history[stage_idx];
    const int row = stage_idx + 1;
    const bool is_current = (st->end_ns == 0);

    string_grid_set_cell(sg, row, 0, get_formatted_string("%d", stage_idx));

    if (st->fidelity_plies == 0) {
      string_grid_set_cell(sg, row, 1, string_duplicate("greedy"));
    } else {
      string_grid_set_cell(
          sg, row, 1, get_formatted_string("%d-ply eg", st->fidelity_plies));
    }

    if (is_current && st->field_size > 0) {
      const double pct = 100.0 * st->cands_done / st->field_size;
      string_grid_set_cell(sg, row, 2,
                           get_formatted_string("%d/%d (%.0f%%)",
                                                st->cands_done, st->field_size,
                                                pct));
    } else {
      string_grid_set_cell(
          sg, row, 2,
          get_formatted_string("%d/%d", st->cands_done, st->field_size));
    }

    if (st->best_win_pct >= 0.0) {
      string_grid_set_cell(
          sg, row, 3, get_formatted_string("%.1f%%", 100.0 * st->best_win_pct));
    } else {
      string_grid_set_cell(sg, row, 3, string_duplicate("-"));
    }

    const int64_t end_ns = (st->end_ns != 0) ? st->end_ns : now_ns;
    const double stage_secs = (double)(end_ns - st->start_ns) / 1e9;
    string_grid_set_cell(sg, row, 4, get_formatted_string("%.1fs", stage_secs));
  }

  string_builder_add_string_grid(sb, sg, false);
  string_grid_destroy(sg);
}

// Render the graded final ranking: every play that entered a halving stage,
// grouped by the deepest endgame fidelity it reached. Deepest tier first, a
// dashed separator between tiers, and the rank continuing across them. Shows
// the whole graded list (its size is the post-greedy cutoff, e.g. 32 by default
// or the entire field under -pegtopk all), so the row buffers are sized to it.
static void peg_append_graded_table(StringBuilder *sb, const PegResult *result,
                                    const Board *board,
                                    const LetterDistribution *ld) {
  const int total = result->n_graded;
  char **depth = malloc_or_die((size_t)total * sizeof(char *));
  char **rankc = malloc_or_die((size_t)total * sizeof(char *));
  char **movec = malloc_or_die((size_t)total * sizeof(char *));
  char **winsc = malloc_or_die((size_t)total * sizeof(char *));
  char **tiesc = malloc_or_die((size_t)total * sizeof(char *));
  char **winc = malloc_or_die((size_t)total * sizeof(char *));
  char **spreadc = malloc_or_die((size_t)total * sizeof(char *));
  char **timec = malloc_or_die((size_t)total * sizeof(char *));
  int *rowfid = malloc_or_die((size_t)total * sizeof(int));
  int n_rows = 0;

  // The labeled-ordering denominator (weight_sum) is the same for every play in
  // a position; the distinct-leaf count (n_scenarios) is the top play's. Both
  // are captured from the first (overall rank 1) row below.
  int64_t weighted_orderings = 0;
  int unique_orderings = 0;

  // The graded list is grouped by fidelity in ascending order, so walk the
  // contiguous blocks from the last (deepest) to the first (shallowest).
  int block_end = total;
  while (block_end > 0) {
    const int fid = result->graded_fidelity[block_end - 1];
    int block_start = block_end - 1;
    while (block_start > 0 && result->graded_fidelity[block_start - 1] == fid) {
      block_start--;
    }
    for (int i = block_start; i < block_end; i++) {
      const PegRankedCand *cand = &result->graded_cands[i];
      if (n_rows == 0) {
        weighted_orderings = cand->weight_sum;
        unique_orderings = cand->n_scenarios;
      }
      depth[n_rows] = get_formatted_string("%d-ply", fid);
      rankc[n_rows] = get_formatted_string("%d", n_rows + 1);
      StringBuilder *move_sb = string_builder_create();
      string_builder_add_move(move_sb, board, &cand->move, ld, false);
      movec[n_rows] = string_builder_dump(move_sb, NULL);
      string_builder_destroy(move_sb);
      winsc[n_rows] = get_formatted_string("%" PRId64, cand->win_count);
      tiesc[n_rows] = get_formatted_string("%" PRId64, cand->tie_count);
      winc[n_rows] = get_formatted_string("%.1f", 100.0 * cand->win_pct);
      spreadc[n_rows] = get_formatted_string("%+.1f", cand->mean_spread);
      timec[n_rows] = get_formatted_string("%.1fs", cand->eval_seconds);
      rowfid[n_rows] = fid;
      n_rows++;
    }
    block_end = block_start;
  }

  // Stats line: the wins/ties columns are integer counts of labeled bag
  // orderings out of `weighted_orderings`; `unique_orderings` is how many
  // distinct draw scenarios the top play actually solved.
  string_builder_add_formatted_string(
      sb, "%" PRId64 " weighted bag orderings (%d unique)\n",
      weighted_orderings, unique_orderings);

  size_t wd = string_length("depth");
  size_t wr = string_length("rank");
  size_t wm = string_length("move");
  size_t ww = string_length("wins");
  size_t wti = string_length("ties");
  size_t wp = string_length("win%");
  size_t wsp = string_length("spread");
  size_t wt = string_length("time");
  for (int i = 0; i < n_rows; i++) {
    wd = string_length(depth[i]) > wd ? string_length(depth[i]) : wd;
    wr = string_length(rankc[i]) > wr ? string_length(rankc[i]) : wr;
    wm = string_length(movec[i]) > wm ? string_length(movec[i]) : wm;
    ww = string_length(winsc[i]) > ww ? string_length(winsc[i]) : ww;
    wti = string_length(tiesc[i]) > wti ? string_length(tiesc[i]) : wti;
    wp = string_length(winc[i]) > wp ? string_length(winc[i]) : wp;
    wsp = string_length(spreadc[i]) > wsp ? string_length(spreadc[i]) : wsp;
    wt = string_length(timec[i]) > wt ? string_length(timec[i]) : wt;
  }
  const size_t total_w =
      wd + wr + wm + ww + wti + wp + wsp + wt + 14; // 7 * 2-space gaps

  string_builder_add_formatted_string(
      sb, "%-*s  %*s  %-*s  %*s  %*s  %*s  %*s  %*s\n", (int)wd, "depth",
      (int)wr, "rank", (int)wm, "move", (int)ww, "wins", (int)wti, "ties",
      (int)wp, "win%", (int)wsp, "spread", (int)wt, "time");
  for (int i = 0; i < n_rows; i++) {
    if (i > 0 && rowfid[i] != rowfid[i - 1]) {
      for (size_t k = 0; k < total_w; k++) {
        string_builder_add_char(sb, '-');
      }
      string_builder_add_char(sb, '\n');
    }
    string_builder_add_formatted_string(
        sb, "%-*s  %*s  %-*s  %*s  %*s  %*s  %*s  %*s\n", (int)wd, depth[i],
        (int)wr, rankc[i], (int)wm, movec[i], (int)ww, winsc[i], (int)wti,
        tiesc[i], (int)wp, winc[i], (int)wsp, spreadc[i], (int)wt, timec[i]);
    free(depth[i]);
    free(rankc[i]);
    free(movec[i]);
    free(winsc[i]);
    free(tiesc[i]);
    free(winc[i]);
    free(spreadc[i]);
    free(timec[i]);
  }
  free(depth);
  free(rankc);
  free(movec);
  free(winsc);
  free(tiesc);
  free(winc);
  free(spreadc);
  free(timec);
  free(rowfid);
}

// Append the flat candidate ranking for result->top_cands. Numeric columns are
// right-aligned; the move column is left-aligned. Columns are rank/move/win%/
// spread plus optional ones: wins and ties (integer labeled bag-ordering
// counts; losses are weight_sum - wins - ties) and time (per-candidate wall
// clock, live solves only). With show_stats, a "<P> weighted bag orderings (<U>
// unique)" line precedes the table. The live stream omits wins/ties and stats;
// the final view omits time. The caller appends the outcomes line (as in the
// graded view).
static void peg_append_flat_ranking(StringBuilder *sb, const PegResult *result,
                                    const Board *board,
                                    const LetterDistribution *ld,
                                    bool show_wins, bool show_time,
                                    bool show_stats, int min_move_width) {
  int col = 0;
  const int rank_col = col++;
  const int move_col = col++;
  const int wins_col = show_wins ? col++ : -1;
  const int ties_col = show_wins ? col++ : -1;
  const int winpct_col = col++;
  const int spread_col = col++;
  const int time_col = show_time ? col++ : -1;
  const int num_cols = col;
  const int num_rows = result->n_top_cands + 1;

  if (show_stats && result->n_top_cands > 0) {
    // weight_sum is the labeled ordered-draw denominator (constant across
    // plays); n_scenarios is the top play's distinct-scenario count.
    string_builder_add_formatted_string(
        sb, "%" PRId64 " weighted bag orderings (%d unique)\n",
        result->top_cands[0].weight_sum, result->top_cands[0].n_scenarios);
  }

  StringGrid *sg = string_grid_create(num_rows, num_cols, 2);
  // Right-align every numeric column; the move column stays left-aligned.
  string_grid_set_col_right_align(sg, rank_col, true);
  if (wins_col >= 0) {
    string_grid_set_col_right_align(sg, wins_col, true);
  }
  if (ties_col >= 0) {
    string_grid_set_col_right_align(sg, ties_col, true);
  }
  string_grid_set_col_right_align(sg, winpct_col, true);
  string_grid_set_col_right_align(sg, spread_col, true);
  if (time_col >= 0) {
    string_grid_set_col_right_align(sg, time_col, true);
  }

  string_grid_set_cell(sg, 0, rank_col, string_duplicate("rank"));
  string_grid_set_cell(sg, 0, move_col, string_duplicate("move"));
  if (wins_col >= 0) {
    string_grid_set_cell(sg, 0, wins_col, string_duplicate("wins"));
  }
  if (ties_col >= 0) {
    string_grid_set_cell(sg, 0, ties_col, string_duplicate("ties"));
  }
  string_grid_set_cell(sg, 0, winpct_col, string_duplicate("win%"));
  string_grid_set_cell(sg, 0, spread_col, string_duplicate("spread"));
  if (time_col >= 0) {
    string_grid_set_cell(sg, 0, time_col, string_duplicate("time"));
  }

  for (int cand_idx = 0; cand_idx < result->n_top_cands; cand_idx++) {
    const PegRankedCand *cand = &result->top_cands[cand_idx];
    const int row = cand_idx + 1;

    string_grid_set_cell(sg, row, rank_col,
                         get_formatted_string("%d", cand_idx + 1));

    StringBuilder *move_sb = string_builder_create();
    string_builder_add_move(move_sb, board, &cand->move, ld, false);
    char *move_str = string_builder_dump(move_sb, NULL);
    string_builder_destroy(move_sb);
    if (min_move_width > (int)string_length(move_str)) {
      // Left-pad to the stage's widest move so the column never grows
      // mid-solve.
      char *padded = get_formatted_string("%-*s", min_move_width, move_str);
      free(move_str);
      move_str = padded;
    }
    string_grid_set_cell(sg, row, move_col, move_str);

    if (wins_col >= 0) {
      string_grid_set_cell(sg, row, wins_col,
                           get_formatted_string("%" PRId64, cand->win_count));
    }
    if (ties_col >= 0) {
      string_grid_set_cell(sg, row, ties_col,
                           get_formatted_string("%" PRId64, cand->tie_count));
    }
    string_grid_set_cell(sg, row, winpct_col,
                         get_formatted_string("%.1f", 100.0 * cand->win_pct));
    string_grid_set_cell(sg, row, spread_col,
                         get_formatted_string("%+.1f", cand->mean_spread));

    if (time_col >= 0) {
      string_grid_set_cell(sg, row, time_col,
                           get_formatted_string("%.1fs", cand->eval_seconds));
    }
  }

  string_builder_add_string_grid(sb, sg, false);
  string_grid_destroy(sg);
}

char *peg_result_get_ranking_string(const PegResult *result, const Game *game,
                                    const Move *stage_moves, int n_stage_moves,
                                    int fidelity_plies, bool only_last_row) {
  StringBuilder *sb = string_builder_create();
  const int n = result->n_top_cands;
  if (n == 0) {
    char *empty = string_builder_dump(sb, NULL);
    string_builder_destroy(sb);
    return empty;
  }
  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);

  // Per-row cell strings.
  char **rankc = malloc_or_die((size_t)n * sizeof(char *));
  char **movec = malloc_or_die((size_t)n * sizeof(char *));
  char **winc = malloc_or_die((size_t)n * sizeof(char *));
  char **spreadc = malloc_or_die((size_t)n * sizeof(char *));
  char **timec = malloc_or_die((size_t)n * sizeof(char *));
  for (int i = 0; i < n; i++) {
    const PegRankedCand *cand = &result->top_cands[i];
    rankc[i] = get_formatted_string("%d", i + 1);
    StringBuilder *move_sb = string_builder_create();
    string_builder_add_move(move_sb, board, &cand->move, ld, false);
    movec[i] = string_builder_dump(move_sb, NULL);
    string_builder_destroy(move_sb);
    winc[i] = get_formatted_string("%.1f", 100.0 * cand->win_pct);
    spreadc[i] = get_formatted_string("%+.1f", cand->mean_spread);
    timec[i] = get_formatted_string("%.1fs", cand->eval_seconds);
  }

  char *depth_label = get_formatted_string("%d-ply", fidelity_plies);

  // Column widths. The move column is sized to the widest move in the whole
  // stage (not just the entries seen so far) so it does not grow as candidates
  // land; the rest are sized across all current entries.
  size_t wd = string_length("depth");
  wd = string_length(depth_label) > wd ? string_length(depth_label) : wd;
  size_t wr = string_length("rank");
  size_t wm = string_length("move");
  for (int i = 0; i < n_stage_moves; i++) {
    StringBuilder *move_sb = string_builder_create();
    string_builder_add_move(move_sb, board, &stage_moves[i], ld, false);
    const size_t w = string_length(string_builder_peek(move_sb));
    string_builder_destroy(move_sb);
    wm = w > wm ? w : wm;
  }
  size_t wp = string_length("win%");
  size_t wsp = string_length("spread");
  size_t wt = string_length("time");
  for (int i = 0; i < n; i++) {
    wr = string_length(rankc[i]) > wr ? string_length(rankc[i]) : wr;
    wm = string_length(movec[i]) > wm ? string_length(movec[i]) : wm;
    wp = string_length(winc[i]) > wp ? string_length(winc[i]) : wp;
    wsp = string_length(spreadc[i]) > wsp ? string_length(spreadc[i]) : wsp;
    wt = string_length(timec[i]) > wt ? string_length(timec[i]) : wt;
  }

  // Header, unless we are only appending the new bottom row.
  if (!only_last_row) {
    string_builder_add_formatted_string(sb, "%-*s  %*s  %-*s  %*s  %*s  %*s\n",
                                        (int)wd, "depth", (int)wr, "rank",
                                        (int)wm, "move", (int)wp, "win%",
                                        (int)wsp, "spread", (int)wt, "time");
  }
  const int first = only_last_row ? n - 1 : 0;
  for (int i = first; i < n; i++) {
    string_builder_add_formatted_string(
        sb, "%-*s  %*s  %-*s  %*s  %*s  %*s\n", (int)wd, depth_label, (int)wr,
        rankc[i], (int)wm, movec[i], (int)wp, winc[i], (int)wsp, spreadc[i],
        (int)wt, timec[i]);
  }

  free(depth_label);
  for (int i = 0; i < n; i++) {
    free(rankc[i]);
    free(movec[i]);
    free(winc[i]);
    free(spreadc[i]);
    free(timec[i]);
  }
  free(rankc);
  free(movec);
  free(winc);
  free(spreadc);
  free(timec);
  char *out = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  return out;
}

char *peg_result_get_string(const PegResult *result, const Game *game,
                            bool show_outcomes) {
  StringBuilder *sb = string_builder_create();

  if (result->last_completed_stage < 0 && result->n_stage_history == 0) {
    string_builder_add_string(sb, "no PEG results.\n");
    char *out = string_builder_dump(sb, NULL);
    string_builder_destroy(sb);
    return out;
  }

  if (result->last_completed_stage < 0) {
    const int64_t now_ns = ctimer_monotonic_ns();
    const double total_secs =
        (double)(now_ns - result->stage_history[0].start_ns) / 1e9;
    string_builder_add_formatted_string(sb, "PEG (running): %.1fs\n",
                                        total_secs);
  } else if (result->last_stage_partial) {
    string_builder_add_formatted_string(
        sb, "PEG (stage %d partial): %d candidates, %.2fs\n",
        result->last_completed_stage, result->n_top_cands,
        ctimer_elapsed_seconds(&result->timer));
  } else {
    string_builder_add_formatted_string(
        sb, "PEG (last completed stage %d): %d candidates, %.2fs\n",
        result->last_completed_stage, result->n_top_cands,
        ctimer_elapsed_seconds(&result->timer));
  }

  if (result->n_stage_history > 0) {
    peg_append_stage_table(sb, result->stage_history, result->n_stage_history);
    string_builder_add_string(sb, "\n");
  }

  if (result->n_top_cands == 0) {
    char *out = string_builder_dump(sb, NULL);
    string_builder_destroy(sb);
    return out;
  }

  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);

  // Final (graded) view: group the plays that reached each stage, deepest
  // first. The flat table below is used for the live/in-progress view and for a
  // result that never finished a halving stage (e.g. the time limit hit during
  // greedy); in that case there are no tiers, so just show the post-greedy
  // ranking.
  if (result->n_graded > 0) {
    peg_append_graded_table(sb, result, board, ld);
    if (show_outcomes && result->n_per_scenario > 0) {
      char *outcomes = peg_build_outcomes_string(result);
      string_builder_add_formatted_string(sb, "\noutcomes (best): %s\n",
                                          outcomes);
      free(outcomes);
    }
    char *out = string_builder_dump(sb, NULL);
    string_builder_destroy(sb);
    return out;
  }

  peg_append_flat_ranking(sb, result, board, ld, /*show_wins=*/true,
                          /*show_time=*/false, /*show_stats=*/true,
                          /*min_move_width=*/0);
  if (show_outcomes && result->n_per_scenario > 0) {
    char *outcomes = peg_build_outcomes_string(result);
    string_builder_add_formatted_string(sb, "\noutcomes (best): %s\n",
                                        outcomes);
    free(outcomes);
  }

  char *out = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  return out;
}
