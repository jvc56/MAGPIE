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

// Forward declarations for helpers defined later in this file.
static char *peg_fidelity_label(int fidelity);
static bool peg_moves_match(const Move *m1, const Move *m2);
static double peg_graded_history_time(const PegPollSnapshot *snap, int slot,
                                      const Move *move);

// Render the final graded ranking: every play that entered a halving stage,
// grouped by the deepest endgame fidelity it reached. Deepest tier first, a
// dashed separator between tiers, and the rank continuing across them. Shows
// the whole graded list (its size is the post-greedy cutoff, e.g. 32 by default
// or the entire field under -pegtopk all), so the row buffers are sized to it.
// When snap is non-NULL, shows "total" + per-fidelity time columns (one per
// non-greedy completed stage). When snap is NULL, shows a single "time" column.
static void peg_append_graded_table(StringBuilder *sb, const PegResult *result,
                                    const Board *board, const LetterDistribution *ld,
                                    const PegPollSnapshot *snap) {
  // Collect distinct non-greedy fidelities from the graded result, in ascending
  // order (graded_cands is stored shallowest-first so iteration is ordered).
  int unique_fids[PEG_POLL_MAX_STAGES];
  int n_unique_fids = 0;
  if (snap != NULL) {
    for (int graded_idx = 0; graded_idx < result->n_graded; graded_idx++) {
      const int fid = result->graded_fidelity[graded_idx];
      if (fid == 0) {
        continue;
      }
      bool already = false;
      for (int uf_idx = 0; uf_idx < n_unique_fids; uf_idx++) {
        if (unique_fids[uf_idx] == fid) {
          already = true;
          break;
        }
      }
      if (!already && n_unique_fids < PEG_POLL_MAX_STAGES) {
        unique_fids[n_unique_fids++] = fid;
      }
    }
  }
  // Show time columns only when there are non-greedy fidelities.
  const bool show_time = n_unique_fids > 0;
  // History slot index for each unique fidelity (-1 = not in history, use
  // eval_seconds instead, which happens for the deepest completed stage).
  int fid_to_hist[PEG_POLL_MAX_STAGES];
  for (int uf_idx = 0; uf_idx < n_unique_fids; uf_idx++) {
    fid_to_hist[uf_idx] = -1;
    if (snap != NULL) {
      for (int hist_slot = 0; hist_slot < snap->n_history_stages; hist_slot++) {
        if (snap->history_fidelities[hist_slot] == unique_fids[uf_idx]) {
          fid_to_hist[uf_idx] = hist_slot;
          break;
        }
      }
    }
  }

  const int total = result->n_graded;
  char **depth = malloc_or_die((size_t)total * sizeof(char *));
  char **rankc = malloc_or_die((size_t)total * sizeof(char *));
  char **movec = malloc_or_die((size_t)total * sizeof(char *));
  char **winsc = malloc_or_die((size_t)total * sizeof(char *));
  char **tiesc = malloc_or_die((size_t)total * sizeof(char *));
  char **winc = malloc_or_die((size_t)total * sizeof(char *));
  char **spreadc = malloc_or_die((size_t)total * sizeof(char *));
  char **totalc = malloc_or_die((size_t)total * sizeof(char *));
  // timecols[fid_col][row]
  char ***timecols =
      show_time ? malloc_or_die((size_t)n_unique_fids * sizeof(char **)) : NULL;
  for (int uf_idx = 0; uf_idx < n_unique_fids; uf_idx++) {
    timecols[uf_idx] = malloc_or_die((size_t)total * sizeof(char *));
  }
  int *rowfid = malloc_or_die((size_t)total * sizeof(int));
  int n_rows = 0;

  int64_t weighted_orderings = 0;
  int unique_orderings = 0;

  // Walk blocks deepest-to-shallowest for display order (deepest tier first).
  int block_end = total;
  while (block_end > 0) {
    const int fid = result->graded_fidelity[block_end - 1];
    int block_start = block_end - 1;
    while (block_start > 0 &&
           result->graded_fidelity[block_start - 1] == fid) {
      block_start--;
    }
    for (int graded_idx = block_start; graded_idx < block_end; graded_idx++) {
      const PegRankedCand *cand = &result->graded_cands[graded_idx];
      if (n_rows == 0) {
        weighted_orderings = cand->weight_sum;
        unique_orderings = cand->n_scenarios;
      }
      depth[n_rows] = peg_fidelity_label(fid);
      rankc[n_rows] = get_formatted_string("%d", n_rows + 1);
      StringBuilder *move_sb = string_builder_create();
      string_builder_add_move(move_sb, board, &cand->move, ld, false);
      movec[n_rows] = string_builder_dump(move_sb, NULL);
      string_builder_destroy(move_sb);
      winsc[n_rows] = get_formatted_string("%" PRId64, cand->win_count);
      tiesc[n_rows] = get_formatted_string("%" PRId64, cand->tie_count);
      winc[n_rows] = get_formatted_string("%.1f", 100.0 * cand->win_pct);
      spreadc[n_rows] = get_formatted_string("%+.1f", cand->mean_spread);
      rowfid[n_rows] = fid;

      if (show_time) {
        double total_secs = 0.0;
        for (int uf_idx = 0; uf_idx < n_unique_fids; uf_idx++) {
          const int col_fid = unique_fids[uf_idx];
          double t = -1.0;
          if (col_fid > fid) {
            // This candidate was not evaluated at this depth.
            t = -1.0;
          } else {
            const int hist_slot = fid_to_hist[uf_idx];
            if (hist_slot >= 0) {
              t = peg_graded_history_time(snap, hist_slot, &cand->move);
            }
            // Not in history → this is the deepest completed stage for this
            // tier; use eval_seconds (which equals the time at col_fid).
            if (t < 0.0 && col_fid == fid) {
              t = cand->eval_seconds;
            }
          }
          timecols[uf_idx][n_rows] =
              t >= 0.0 ? get_formatted_string("%.1fs", t)
                       : string_duplicate("-");
          if (t >= 0.0) {
            total_secs += t;
          }
        }
        totalc[n_rows] = total_secs > 0.0
                             ? get_formatted_string("%.1fs", total_secs)
                             : string_duplicate("-");
      } else {
        totalc[n_rows] = get_formatted_string("%.1fs", cand->eval_seconds);
      }
      n_rows++;
    }
    block_end = block_start;
  }

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
  const char *total_hdr = show_time ? "total" : "time";
  size_t wtotal = string_length(total_hdr);
  // Per-fidelity column widths, labeled by their fidelity.
  char **fid_hdrs =
      show_time ? malloc_or_die((size_t)n_unique_fids * sizeof(char *)) : NULL;
  size_t *wfid =
      show_time ? malloc_or_die((size_t)n_unique_fids * sizeof(size_t)) : NULL;
  for (int uf_idx = 0; uf_idx < n_unique_fids; uf_idx++) {
    fid_hdrs[uf_idx] = peg_fidelity_label(unique_fids[uf_idx]);
    wfid[uf_idx] = string_length(fid_hdrs[uf_idx]);
  }
  for (int row_idx = 0; row_idx < n_rows; row_idx++) {
    wd = string_length(depth[row_idx]) > wd ? string_length(depth[row_idx]) : wd;
    wr = string_length(rankc[row_idx]) > wr ? string_length(rankc[row_idx]) : wr;
    wm = string_length(movec[row_idx]) > wm ? string_length(movec[row_idx]) : wm;
    ww = string_length(winsc[row_idx]) > ww ? string_length(winsc[row_idx]) : ww;
    wti = string_length(tiesc[row_idx]) > wti ? string_length(tiesc[row_idx]) : wti;
    wp = string_length(winc[row_idx]) > wp ? string_length(winc[row_idx]) : wp;
    wsp = string_length(spreadc[row_idx]) > wsp ? string_length(spreadc[row_idx]) : wsp;
    wtotal = string_length(totalc[row_idx]) > wtotal
                 ? string_length(totalc[row_idx])
                 : wtotal;
    for (int uf_idx = 0; uf_idx < n_unique_fids; uf_idx++) {
      wfid[uf_idx] = string_length(timecols[uf_idx][row_idx]) > wfid[uf_idx]
                         ? string_length(timecols[uf_idx][row_idx])
                         : wfid[uf_idx];
    }
  }

  // Total row width for the tier-separator dashes.
  // 7 fixed gaps (between 8 fixed columns) + 1 gap before total + n_unique_fids gaps.
  size_t total_w = wd + wr + wm + ww + wti + wp + wsp + wtotal +
                   (size_t)(14 + 2 + 2 * n_unique_fids);
  for (int uf_idx = 0; uf_idx < n_unique_fids; uf_idx++) {
    total_w += wfid[uf_idx];
  }

  // Header.
  string_builder_add_formatted_string(
      sb, "%-*s  %*s  %-*s  %*s  %*s  %*s  %*s  %*s", (int)wd, "depth",
      (int)wr, "rank", (int)wm, "move", (int)ww, "wins", (int)wti, "ties",
      (int)wp, "win%", (int)wsp, "spread", (int)wtotal, total_hdr);
  for (int uf_idx = 0; uf_idx < n_unique_fids; uf_idx++) {
    string_builder_add_formatted_string(sb, "  %*s", (int)wfid[uf_idx],
                                        fid_hdrs[uf_idx]);
  }
  string_builder_add_string(sb, "\n");

  // Data rows with tier-separator lines between fidelity groups.
  for (int row_idx = 0; row_idx < n_rows; row_idx++) {
    if (row_idx > 0 && rowfid[row_idx] != rowfid[row_idx - 1]) {
      for (size_t k = 0; k < total_w; k++) {
        string_builder_add_char(sb, '-');
      }
      string_builder_add_char(sb, '\n');
    }
    string_builder_add_formatted_string(
        sb, "%-*s  %*s  %-*s  %*s  %*s  %*s  %*s  %*s", (int)wd, depth[row_idx],
        (int)wr, rankc[row_idx], (int)wm, movec[row_idx], (int)ww,
        winsc[row_idx], (int)wti, tiesc[row_idx], (int)wp, winc[row_idx],
        (int)wsp, spreadc[row_idx], (int)wtotal, totalc[row_idx]);
    for (int uf_idx = 0; uf_idx < n_unique_fids; uf_idx++) {
      string_builder_add_formatted_string(sb, "  %*s", (int)wfid[uf_idx],
                                          timecols[uf_idx][row_idx]);
    }
    string_builder_add_string(sb, "\n");
    free(depth[row_idx]);
    free(rankc[row_idx]);
    free(movec[row_idx]);
    free(winsc[row_idx]);
    free(tiesc[row_idx]);
    free(winc[row_idx]);
    free(spreadc[row_idx]);
    free(totalc[row_idx]);
    for (int uf_idx = 0; uf_idx < n_unique_fids; uf_idx++) {
      free(timecols[uf_idx][row_idx]);
    }
  }

  free(depth);
  free(rankc);
  free(movec);
  free(winsc);
  free(tiesc);
  free(winc);
  free(spreadc);
  free(totalc);
  if (show_time) {
    for (int uf_idx = 0; uf_idx < n_unique_fids; uf_idx++) {
      free(timecols[uf_idx]);
      free(fid_hdrs[uf_idx]);
    }
  }
  free(timecols);
  free(fid_hdrs);
  free(wfid);
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

// True when two moves are the same play (type, position, tiles).
static bool peg_moves_match(const Move *m1, const Move *m2) {
  if (m1->move_type != m2->move_type || m1->dir != m2->dir ||
      m1->row_start != m2->row_start || m1->col_start != m2->col_start ||
      m1->tiles_length != m2->tiles_length ||
      m1->tiles_played != m2->tiles_played) {
    return false;
  }
  for (int tile_idx = 0; tile_idx < m1->tiles_length; tile_idx++) {
    if (m1->tiles[tile_idx] != m2->tiles[tile_idx]) {
      return false;
    }
  }
  return true;
}

static bool peg_cands_same_move(const PegRankedCand *a,
                                const PegRankedCand *b) {
  return peg_moves_match(&a->move, &b->move);
}

// Per-candidate merged record for the cross-depth live status display.
typedef struct {
  PegRankedCand cand;  // stats at `fidelity` (last fully-completed depth)
  int fidelity;        // depth at which cand was LAST FULLY evaluated
  // Per-history-slot eval times, indexed 1:1 with snap->history_fidelities[].
  // -1.0 means this candidate was not found in that history slot.
  double depth_times[PEG_POLL_MAX_HISTORY_STAGES];
  bool is_evaluating;  // currently being evaluated at the next depth
  double live_secs;    // live elapsed seconds when is_evaluating (else 0)
} PegMergedEntry;

static int peg_merged_entry_cmp(const void *va, const void *vb) {
  const PegMergedEntry *a = (const PegMergedEntry *)va;
  const PegMergedEntry *b = (const PegMergedEntry *)vb;
  const double ka = a->cand.win_pct + 1e-4 * a->cand.mean_spread;
  const double kb = b->cand.win_pct + 1e-4 * b->cand.mean_spread;
  if (ka > kb) {
    return -1;
  }
  if (ka < kb) {
    return 1;
  }
  return 0;
}

// Return a heap-allocated depth label string. Caller frees.
static char *peg_fidelity_label(int fidelity) {
  return fidelity == 0 ? string_duplicate("greedy")
                       : get_formatted_string("%d-ply", fidelity);
}

// Look up a candidate's eval_seconds in a specific history slot by move
// identity. Returns -1.0 if not found.
static double peg_graded_history_time(const PegPollSnapshot *snap, int slot,
                                      const Move *move) {
  for (int hcand_idx = 0; hcand_idx < snap->history_n_cands[slot];
       hcand_idx++) {
    if (peg_moves_match(move, &snap->history_cands[slot][hcand_idx].move)) {
      return snap->history_cands[slot][hcand_idx].eval_seconds;
    }
  }
  return -1.0;
}

// Render a cross-depth merged ranking table with one time column per completed
// non-greedy stage plus the currently active stage (if non-greedy). History
// columns are labeled by fidelity (e.g. "2-ply"); greedy is never shown since
// it is always instant. The in-flight candidate's depth label gets a trailing '*'.
static void peg_append_cross_depth_ranking(StringBuilder *sb,
                                           const PegMergedEntry *merged,
                                           int n,
                                           const int *history_fidelities,
                                           int n_history,
                                           int current_fidelity,
                                           const Board *board,
                                           const LetterDistribution *ld) {
  // Time columns: one per history stage + one for the current stage (if non-greedy).
  const bool show_current_col = current_fidelity > 0;
  const int n_time_cols = n_history + (show_current_col ? 1 : 0);

  const bool show_time = n_time_cols > 0;

  // Per-depth column headers (not including "total").
  char **time_hdrs =
      show_time ? malloc_or_die((size_t)n_time_cols * sizeof(char *)) : NULL;
  for (int hist_idx = 0; hist_idx < n_history; hist_idx++) {
    time_hdrs[hist_idx] = peg_fidelity_label(history_fidelities[hist_idx]);
  }
  if (show_current_col) {
    time_hdrs[n_history] = peg_fidelity_label(current_fidelity);
  }

  char **depthc = malloc_or_die((size_t)n * sizeof(char *));
  char **rankc = malloc_or_die((size_t)n * sizeof(char *));
  char **movec = malloc_or_die((size_t)n * sizeof(char *));
  char **winsc = malloc_or_die((size_t)n * sizeof(char *));
  char **tiesc = malloc_or_die((size_t)n * sizeof(char *));
  char **winc = malloc_or_die((size_t)n * sizeof(char *));
  char **spreadc = malloc_or_die((size_t)n * sizeof(char *));
  char **totalc = show_time ? malloc_or_die((size_t)n * sizeof(char *)) : NULL;
  // timecols[col_idx][cand_idx]
  char ***timecols =
      show_time ? malloc_or_die((size_t)n_time_cols * sizeof(char **)) : NULL;
  for (int col_idx = 0; col_idx < n_time_cols; col_idx++) {
    timecols[col_idx] = malloc_or_die((size_t)n * sizeof(char *));
  }

  for (int cand_idx = 0; cand_idx < n; cand_idx++) {
    const PegMergedEntry *entry = &merged[cand_idx];
    char *dlabel = peg_fidelity_label(entry->fidelity);
    if (entry->is_evaluating) {
      char *starred = get_formatted_string("%s*", dlabel);
      free(dlabel);
      dlabel = starred;
    }
    depthc[cand_idx] = dlabel;
    rankc[cand_idx] = get_formatted_string("%d", cand_idx + 1);
    StringBuilder *move_sb = string_builder_create();
    string_builder_add_move(move_sb, board, &entry->cand.move, ld, false);
    movec[cand_idx] = string_builder_dump(move_sb, NULL);
    string_builder_destroy(move_sb);
    winsc[cand_idx] = get_formatted_string("%" PRId64, entry->cand.win_count);
    tiesc[cand_idx] = get_formatted_string("%" PRId64, entry->cand.tie_count);
    winc[cand_idx] = get_formatted_string("%.1f", 100.0 * entry->cand.win_pct);
    spreadc[cand_idx] = get_formatted_string("%+.1f", entry->cand.mean_spread);

    if (show_time) {
      double total_secs = 0.0;
      for (int hist_idx = 0; hist_idx < n_history; hist_idx++) {
        const double t = entry->depth_times[hist_idx];
        timecols[hist_idx][cand_idx] =
            t >= 0.0 ? get_formatted_string("%.1fs", t)
                     : string_duplicate("-");
        if (t >= 0.0) {
          total_secs += t;
        }
      }
      if (show_current_col) {
        if (entry->is_evaluating) {
          timecols[n_history][cand_idx] =
              get_formatted_string("%.1fs", entry->live_secs);
          total_secs += entry->live_secs;
        } else if (entry->fidelity == current_fidelity &&
                   entry->cand.eval_seconds > 0.0) {
          timecols[n_history][cand_idx] =
              get_formatted_string("%.1fs", entry->cand.eval_seconds);
          total_secs += entry->cand.eval_seconds;
        } else {
          timecols[n_history][cand_idx] = string_duplicate("-");
        }
      }
      totalc[cand_idx] = total_secs > 0.0
                             ? get_formatted_string("%.1fs", total_secs)
                             : string_duplicate("-");
    }
  }

  size_t wd = string_length("depth");
  size_t wr = string_length("rank");
  size_t wm = string_length("move");
  size_t ww = string_length("wins");
  size_t wti = string_length("ties");
  size_t wp = string_length("win%");
  size_t wsp = string_length("spread");
  size_t wtotal = show_time ? string_length("total") : 0;
  size_t *wt =
      show_time ? malloc_or_die((size_t)n_time_cols * sizeof(size_t)) : NULL;
  for (int col_idx = 0; col_idx < n_time_cols; col_idx++) {
    wt[col_idx] = string_length(time_hdrs[col_idx]);
  }
  for (int cand_idx = 0; cand_idx < n; cand_idx++) {
    wd = string_length(depthc[cand_idx]) > wd
             ? string_length(depthc[cand_idx])
             : wd;
    wr = string_length(rankc[cand_idx]) > wr ? string_length(rankc[cand_idx])
                                             : wr;
    wm = string_length(movec[cand_idx]) > wm ? string_length(movec[cand_idx])
                                             : wm;
    ww = string_length(winsc[cand_idx]) > ww ? string_length(winsc[cand_idx])
                                             : ww;
    wti = string_length(tiesc[cand_idx]) > wti
              ? string_length(tiesc[cand_idx])
              : wti;
    wp = string_length(winc[cand_idx]) > wp ? string_length(winc[cand_idx])
                                            : wp;
    wsp = string_length(spreadc[cand_idx]) > wsp
              ? string_length(spreadc[cand_idx])
              : wsp;
    if (show_time) {
      wtotal = string_length(totalc[cand_idx]) > wtotal
                   ? string_length(totalc[cand_idx])
                   : wtotal;
      for (int col_idx = 0; col_idx < n_time_cols; col_idx++) {
        wt[col_idx] =
            string_length(timecols[col_idx][cand_idx]) > wt[col_idx]
                ? string_length(timecols[col_idx][cand_idx])
                : wt[col_idx];
      }
    }
  }

  // Header: fixed columns, then "total", then per-depth columns.
  string_builder_add_formatted_string(
      sb, "%-*s  %*s  %-*s  %*s  %*s  %*s  %*s", (int)wd, "depth", (int)wr,
      "rank", (int)wm, "move", (int)ww, "wins", (int)wti, "ties", (int)wp,
      "win%", (int)wsp, "spread");
  if (show_time) {
    string_builder_add_formatted_string(sb, "  %*s", (int)wtotal, "total");
    for (int col_idx = 0; col_idx < n_time_cols; col_idx++) {
      string_builder_add_formatted_string(sb, "  %*s", (int)wt[col_idx],
                                          time_hdrs[col_idx]);
    }
  }
  string_builder_add_string(sb, "\n");

  for (int cand_idx = 0; cand_idx < n; cand_idx++) {
    string_builder_add_formatted_string(
        sb, "%-*s  %*s  %-*s  %*s  %*s  %*s  %*s", (int)wd, depthc[cand_idx],
        (int)wr, rankc[cand_idx], (int)wm, movec[cand_idx], (int)ww,
        winsc[cand_idx], (int)wti, tiesc[cand_idx], (int)wp, winc[cand_idx],
        (int)wsp, spreadc[cand_idx]);
    if (show_time) {
      string_builder_add_formatted_string(sb, "  %*s", (int)wtotal,
                                          totalc[cand_idx]);
      for (int col_idx = 0; col_idx < n_time_cols; col_idx++) {
        string_builder_add_formatted_string(sb, "  %*s", (int)wt[col_idx],
                                            timecols[col_idx][cand_idx]);
      }
    }
    string_builder_add_string(sb, "\n");
    free(depthc[cand_idx]);
    free(rankc[cand_idx]);
    free(movec[cand_idx]);
    free(winsc[cand_idx]);
    free(tiesc[cand_idx]);
    free(winc[cand_idx]);
    free(spreadc[cand_idx]);
    if (show_time) {
      free(totalc[cand_idx]);
      for (int col_idx = 0; col_idx < n_time_cols; col_idx++) {
        free(timecols[col_idx][cand_idx]);
      }
    }
  }

  free(depthc);
  free(rankc);
  free(movec);
  free(winsc);
  free(tiesc);
  free(winc);
  free(spreadc);
  free(totalc);
  if (show_time) {
    for (int col_idx = 0; col_idx < n_time_cols; col_idx++) {
      free(timecols[col_idx]);
      free(time_hdrs[col_idx]);
    }
  }
  free(timecols);
  free(time_hdrs);
  free(wt);
}

// Build and render the cross-depth merged view from a live poll snapshot.
// Baseline entries (previous completed stage) seed the display; current-stage
// candidates replace their counterparts as they finish. History slots supply
// per-candidate times for each completed non-greedy stage's time column.
// The in-flight candidate gets a '*' depth label and a live current-column time.
static void peg_append_live_ranking(StringBuilder *sb,
                                    const PegPollSnapshot *snap,
                                    const Board *board,
                                    const LetterDistribution *ld) {
  const int max_merged = snap->n_baseline_entries + snap->n_entries;
  if (max_merged == 0) {
    return;
  }
  PegMergedEntry *merged =
      malloc_or_die((size_t)max_merged * sizeof(PegMergedEntry));
  int n_merged = 0;

  // Start with baseline entries (previous completed stage's full ranking).
  for (int baseline_idx = 0; baseline_idx < snap->n_baseline_entries;
       baseline_idx++) {
    merged[n_merged].cand = snap->baseline_entries[baseline_idx];
    merged[n_merged].fidelity = snap->baseline_fidelity;
    for (int hist_idx = 0; hist_idx < PEG_POLL_MAX_HISTORY_STAGES; hist_idx++) {
      merged[n_merged].depth_times[hist_idx] = -1.0;
    }
    merged[n_merged].is_evaluating = false;
    merged[n_merged].live_secs = 0.0;
    n_merged++;
  }

  // Overlay current-stage candidates that have finished.
  for (int entry_idx = 0; entry_idx < snap->n_entries; entry_idx++) {
    bool found = false;
    for (int merged_idx = 0; merged_idx < n_merged; merged_idx++) {
      if (peg_cands_same_move(&snap->entries[entry_idx],
                              &merged[merged_idx].cand)) {
        merged[merged_idx].cand = snap->entries[entry_idx];
        merged[merged_idx].fidelity = snap->fidelity_plies;
        found = true;
        break;
      }
    }
    if (!found && n_merged < max_merged) {
      merged[n_merged].cand = snap->entries[entry_idx];
      merged[n_merged].fidelity = snap->fidelity_plies;
      for (int hist_idx = 0; hist_idx < PEG_POLL_MAX_HISTORY_STAGES;
           hist_idx++) {
        merged[n_merged].depth_times[hist_idx] = -1.0;
      }
      merged[n_merged].is_evaluating = false;
      merged[n_merged].live_secs = 0.0;
      n_merged++;
    }
  }

  // Populate per-history-slot times by move-matching through history.
  for (int merged_idx = 0; merged_idx < n_merged; merged_idx++) {
    for (int hist_slot = 0; hist_slot < snap->n_history_stages; hist_slot++) {
      for (int hcand_idx = 0; hcand_idx < snap->history_n_cands[hist_slot];
           hcand_idx++) {
        if (peg_moves_match(&merged[merged_idx].cand.move,
                            &snap->history_cands[hist_slot][hcand_idx].move)) {
          merged[merged_idx].depth_times[hist_slot] =
              snap->history_cands[hist_slot][hcand_idx].eval_seconds;
          break;
        }
      }
    }
  }

  // Mark the in-flight candidate and compute its live elapsed time.
  const int eval_idx = snap->currently_evaluating_move_idx;
  if (eval_idx >= 0 && eval_idx < snap->n_stage_moves &&
      snap->eval_start_ns > 0) {
    const int64_t now_ns = ctimer_monotonic_ns();
    const double live_secs = (double)(now_ns - snap->eval_start_ns) / 1e9;
    const Move *eval_move = &snap->stage_moves[eval_idx];
    for (int merged_idx = 0; merged_idx < n_merged; merged_idx++) {
      if (peg_moves_match(eval_move, &merged[merged_idx].cand.move)) {
        merged[merged_idx].is_evaluating = true;
        merged[merged_idx].live_secs = live_secs;
        break;
      }
    }
  }

  if (n_merged > 1) {
    qsort(merged, (size_t)n_merged, sizeof(PegMergedEntry),
          peg_merged_entry_cmp);
  }

  peg_append_cross_depth_ranking(sb, merged, n_merged,
                                 snap->history_fidelities,
                                 snap->n_history_stages, snap->fidelity_plies,
                                 board, ld);
  free(merged);
}

char *peg_result_get_string(const PegResult *result, const Game *game,
                            bool show_outcomes, PegPoll *poll) {
  StringBuilder *sb = string_builder_create();

  // Read poll snapshot once. Used for both the live path and (when done) to
  // supply per-stage timing history to the completed-result display.
  PegPollSnapshot snap;
  bool have_snap = false;
  if (poll != NULL) {
    peg_poll_read(poll, &snap);
    have_snap = true;
  }

  // Live path: poll is set and the solve is still running.
  if (have_snap && !snap.done) {
    const int64_t now_ns = ctimer_monotonic_ns();
    const int64_t start_ns = snap.n_stage_history > 0
                                 ? snap.stage_history[0].start_ns
                                 : now_ns;
    const double total_secs = (double)(now_ns - start_ns) / 1e9;
    string_builder_add_formatted_string(sb, "PEG (running): %.1fs\n",
                                        total_secs);
    if (snap.n_stage_history > 0) {
      peg_append_stage_table(sb, snap.stage_history, snap.n_stage_history);
      string_builder_add_string(sb, "\n");
    }
    const Board *board = game_get_board(game);
    const LetterDistribution *ld = game_get_ld(game);
    peg_append_live_ranking(sb, &snap, board, ld);
    char *out = string_builder_dump(sb, NULL);
    string_builder_destroy(sb);
    return out;
  }

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
    peg_append_graded_table(sb, result, board, ld,
                            have_snap ? &snap : NULL);
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
