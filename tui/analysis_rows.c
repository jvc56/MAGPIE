#include "analysis_rows.h"

#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/stats.h"
#include "../src/impl/endgame.h"
#include "../src/impl/peg.h"
#include "../src/str/move_string.h"
#include "../src/util/string_util.h"
#include "game_state.h"
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Populate the row cache from the sim leaderboard. Returns the
// number of rows filled (≤ max_rows).
// True when the analysis-resume worker is actively recomputing the
// turn the History cursor is parked on — the panel should show the
// live (ticking) results instead of the entry's frozen snapshot.
bool resume_active_for_cursor(const TuiGameState *state) {
  if (state->history_cursor < 0) {
    return false;
  }
  TuiGameState *mut = (TuiGameState *)state;
  if (atomic_load(&mut->sim_results_active) &&
      atomic_load(&mut->sim_results_turn_idx) == state->history_cursor) {
    return true;
  }
  return atomic_load(&mut->endgame_results_active) &&
         atomic_load(&mut->endgame_results_turn_idx) == state->history_cursor;
}
// The game the analysis panel should read positions from: the
// resume worker's reconstructed past position while a "/resume" is
// running, the live game otherwise.
const Game *analysis_source_game(const TuiGameState *state) {
  return state->analysis_game != NULL ? state->analysis_game : state->game;
}
static int fill_analysis_rows_from_sim(const TuiGameState *state,
                                       AnalysisRow *rows, int max_rows) {
  SimResults *results = state->sim_results;
  if (results == NULL ||
      !sim_results_lock_and_sort_display_simmed_plays(results)) {
    return 0;
  }
  const int num_plays = sim_results_get_number_of_plays(results);
  int n = num_plays < max_rows ? num_plays : max_rows;
  // Format against the position the sim is actually running on — the
  // reconstructed past position during a "/resume", the live game
  // otherwise.
  const Game *fmt_game = analysis_source_game(state);
  const Board *board = game_get_board(fmt_game);
  // Read the rack from the game (the on-turn player's rack at
  // the moment we're rendering, which during a sim is still the
  // pre-play rack since play_move runs only after the sim returns).
  // The sim_results internal rack is also valid in principle, but
  // sourcing it from the game directly removes a memory-ordering
  // dependency on the simmer's writes from another thread.
  const int on_turn = game_get_player_on_turn_index(fmt_game);
  const Rack *sim_rack = player_get_rack(game_get_player(fmt_game, on_turn));
  for (int i = 0; i < n; i++) {
    rows[i].valid = false;
    rows[i].move[0] = '\0';
    rows[i].leave[0] = '\0';
    rows[i].score[0] = '\0';
    rows[i].primary[0] = '\0';
    rows[i].secondary[0] = '\0';
    rows[i].ply_count = 0;
    const SimmedPlay *play = sim_results_get_display_simmed_play(results, i);
    if (play == NULL) {
      continue;
    }
    const Move *move = simmed_play_get_move(play);
    const double win_pct =
        stat_get_mean(simmed_play_get_win_pct_stat(play)) * 100.0;
    const double eq_pts = stat_get_mean(simmed_play_get_equity_stat(play));
    // When the value rounds to 100.0%, drop the decimal so the cell
    // shows "  100%" instead of "100.0%". Same 6-col width, but
    // preserves the leading-space pad that the leave column relies
    // on for its 1-col visual gap from the win%.
    if (win_pct >= 99.95) {
      snprintf(rows[i].primary, sizeof(rows[i].primary), "  100%%");
    } else {
      snprintf(rows[i].primary, sizeof(rows[i].primary), "%5.1f%%", win_pct);
    }
    snprintf(rows[i].secondary, sizeof(rows[i].secondary), "%+6.1f", eq_pts);
    const int play_score = equity_to_int(move_get_score(move));
    snprintf(rows[i].score, sizeof(rows[i].score), "%d", play_score);
    rows[i].score_value = play_score;
    rows[i].primary_value = win_pct;
    rows[i].secondary_value = eq_pts;

    // Per-ply average move-score (mean of every iteration's move-
    // score at that ply, including 0 for pass / exchange). Capped at
    // MAX_ANALYSIS_PLIES so the row stays small.
    const int sim_plies = sim_results_get_num_plies(results);
    const int plies_to_store =
        sim_plies < MAX_ANALYSIS_PLIES ? sim_plies : MAX_ANALYSIS_PLIES;
    rows[i].ply_count = plies_to_store;
    rows[i].candidate_player_idx = on_turn;
    for (int ply = 0; ply < plies_to_store; ply++) {
      const Stat *score_stat = simmed_play_get_score_stat(play, ply);
      rows[i].ply_avg[ply] = score_stat ? stat_get_mean(score_stat) : 0.0;
    }

    StringBuilder *sb = string_builder_create();
    string_builder_add_move(sb, board, move, state->ld, false);
    size_t mlen = 0;
    char *mdump = string_builder_dump(sb, &mlen);
    if (mdump != NULL) {
      const size_t copy =
          mlen < sizeof(rows[i].move) ? mlen : sizeof(rows[i].move) - 1;
      memcpy(rows[i].move, mdump, copy);
      rows[i].move[copy] = '\0';
      free(mdump);
    }
    string_builder_destroy(sb);

    if (sim_rack != NULL) {
      StringBuilder *lsb = string_builder_create();
      string_builder_add_move_leave(lsb, sim_rack, move, state->ld);
      size_t llen = 0;
      char *ldump = string_builder_dump(lsb, &llen);
      if (ldump != NULL) {
        const size_t copy =
            llen < sizeof(rows[i].leave) ? llen : sizeof(rows[i].leave) - 1;
        memcpy(rows[i].leave, ldump, copy);
        rows[i].leave[copy] = '\0';
        free(ldump);
      }
      string_builder_destroy(lsb);
    }
    rows[i].valid = true;
  }
  sim_results_unlock_display_infos(results);
  return n;
}
// Populate the row cache from the most recent endgame snapshot. The
// snapshot owns its own Board, Rack, and Move array (captured when
// the solve finished, before play_move shifted the live state), so
// this is safe to call any time the snapshot is marked valid.
static int fill_analysis_rows_from_endgame(const TuiGameState *state,
                                           AnalysisRow *rows, int max_rows) {
  const TuiEndgameSnapshot *snap = &state->endgame_snapshot;
  if (!snap->valid || snap->num_entries <= 0 || snap->board == NULL) {
    return 0;
  }
  const int n = snap->num_entries < max_rows ? snap->num_entries : max_rows;
  for (int i = 0; i < n; i++) {
    rows[i].valid = false;
    rows[i].move[0] = '\0';
    rows[i].leave[0] = '\0';
    rows[i].score[0] = '\0';
    rows[i].primary[0] = '\0';
    rows[i].secondary[0] = '\0';
    rows[i].ply_count = 0;
    const Move *move = snap->moves[i];
    if (move == NULL) {
      continue;
    }
    const int value = snap->values[i];
    // No W/L/T column for endgame anymore — the signed spread
    // already conveys win/loss, and the W/L being a single char
    // got eaten by the leave column visually.
    rows[i].primary[0] = '\0';
    snprintf(rows[i].secondary, sizeof(rows[i].secondary), "%+d", value);

    StringBuilder *sb = string_builder_create();
    string_builder_add_move(sb, snap->board, move, state->ld, false);
    size_t mlen = 0;
    char *mdump = string_builder_dump(sb, &mlen);
    if (mdump != NULL) {
      const size_t copy =
          mlen < sizeof(rows[i].move) ? mlen : sizeof(rows[i].move) - 1;
      memcpy(rows[i].move, mdump, copy);
      rows[i].move[copy] = '\0';
      free(mdump);
    }
    string_builder_destroy(sb);

    if (snap->solve_rack != NULL) {
      StringBuilder *lsb = string_builder_create();
      string_builder_add_move_leave(lsb, snap->solve_rack, move, state->ld);
      size_t llen = 0;
      char *ldump = string_builder_dump(lsb, &llen);
      if (ldump != NULL) {
        const size_t copy =
            llen < sizeof(rows[i].leave) ? llen : sizeof(rows[i].leave) - 1;
        memcpy(rows[i].leave, ldump, copy);
        rows[i].leave[copy] = '\0';
        free(ldump);
      }
      string_builder_destroy(lsb);
    }
    rows[i].valid = true;
  }
  return n;
}
// Populate the row cache from the live PEG poll. peg_poll_read is
// thread-safe by contract (the solver updates the poll under its
// mutex as candidates and stages complete), so this can run at
// render cadence while peg_solve is in flight. Rows render against
// the analyzed position the same way the sim rows do. When the
// current stage hasn't finished any candidate yet, the poll's
// baseline (previous completed stage's ranking) is shown instead —
// the same fallback the CLI's `status peg` uses — so the panel
// never blanks between stages. `out_meta`, when non-NULL, receives
// the title metadata matching the rows written.
static int fill_analysis_rows_from_peg(const TuiGameState *state,
                                       AnalysisRow *rows, int max_rows,
                                       TuiPegLiveMeta *out_meta) {
  if (state->peg_poll == NULL) {
    return 0;
  }
  PegPollSnapshot poll_snap;
  peg_poll_read(state->peg_poll, &poll_snap);

  // Prefer the current stage's leaderboard; fall back to the previous
  // completed stage's ranking while the new stage warms up.
  const PegRankedCand *cands = poll_snap.entries;
  int n_cands = poll_snap.n_entries;
  int shown_fidelity = poll_snap.fidelity_plies;
  if (n_cands <= 0 && poll_snap.n_baseline_entries > 0) {
    cands = poll_snap.baseline_entries;
    n_cands = poll_snap.n_baseline_entries;
    shown_fidelity = poll_snap.baseline_fidelity;
  }

  if (out_meta != NULL) {
    out_meta->stage = poll_snap.stage;
    out_meta->fidelity = shown_fidelity;
    out_meta->cands_done = 0;
    out_meta->field_size = poll_snap.field_size;
    if (poll_snap.n_stage_history > 0) {
      out_meta->cands_done =
          poll_snap.stage_history[poll_snap.n_stage_history - 1].cands_done;
    }
    out_meta->solve_done = poll_snap.done;
    out_meta->valid = n_cands > 0;
  }
  if (n_cands <= 0) {
    return 0;
  }

  const int n = n_cands < max_rows ? n_cands : max_rows;
  // Format against the position the solve is running on — the
  // reconstructed past position during a "/resume", the live game
  // otherwise (sim pattern; the solve holds the pre-play position
  // until the bot finalizes the turn).
  const Game *fmt_game = analysis_source_game(state);
  const Board *board = game_get_board(fmt_game);
  const int on_turn = game_get_player_on_turn_index(fmt_game);
  const Rack *peg_rack = player_get_rack(game_get_player(fmt_game, on_turn));
  for (int row_idx = 0; row_idx < n; row_idx++) {
    rows[row_idx].valid = false;
    rows[row_idx].move[0] = '\0';
    rows[row_idx].leave[0] = '\0';
    rows[row_idx].score[0] = '\0';
    rows[row_idx].primary[0] = '\0';
    rows[row_idx].secondary[0] = '\0';
    rows[row_idx].ply_count = 0;
    const PegRankedCand *cand = &cands[row_idx];
    const Move *move = &cand->move;
    const double win_pct = cand->win_pct * 100.0;
    if (win_pct >= 99.95) {
      snprintf(rows[row_idx].primary, sizeof(rows[row_idx].primary), "  100%%");
    } else {
      snprintf(rows[row_idx].primary, sizeof(rows[row_idx].primary), "%5.1f%%",
               win_pct);
    }
    snprintf(rows[row_idx].secondary, sizeof(rows[row_idx].secondary), "%+6.1f",
             cand->mean_spread);
    const int play_score = equity_to_int(move_get_score(move));
    snprintf(rows[row_idx].score, sizeof(rows[row_idx].score), "%d",
             play_score);
    rows[row_idx].score_value = play_score;
    rows[row_idx].primary_value = win_pct;
    rows[row_idx].secondary_value = cand->mean_spread;
    rows[row_idx].candidate_player_idx = on_turn;

    StringBuilder *sb = string_builder_create();
    string_builder_add_move(sb, board, move, state->ld, false);
    size_t mlen = 0;
    char *mdump = string_builder_dump(sb, &mlen);
    if (mdump != NULL) {
      const size_t copy = mlen < sizeof(rows[row_idx].move)
                              ? mlen
                              : sizeof(rows[row_idx].move) - 1;
      memcpy(rows[row_idx].move, mdump, copy);
      rows[row_idx].move[copy] = '\0';
      free(mdump);
    }
    string_builder_destroy(sb);

    if (peg_rack != NULL) {
      StringBuilder *lsb = string_builder_create();
      string_builder_add_move_leave(lsb, peg_rack, move, state->ld);
      size_t llen = 0;
      char *ldump = string_builder_dump(lsb, &llen);
      if (ldump != NULL) {
        const size_t copy = llen < sizeof(rows[row_idx].leave)
                                ? llen
                                : sizeof(rows[row_idx].leave) - 1;
        memcpy(rows[row_idx].leave, ldump, copy);
        rows[row_idx].leave[copy] = '\0';
        free(ldump);
      }
      string_builder_destroy(lsb);
    }
    rows[row_idx].valid = true;
  }
  return n;
}
// Populate state->last_rendered_analysis_rows for the current
// frame. Called once at the top of tui_game_render (before
// render_board, so the on-board candidate preview can resolve
// the cursor against this frame's row order even after a sim
// re-sort). Picks source the same way render_analysis_panel
// does: saved snapshot when the History cursor sits on a
// committed entry, otherwise live sim or endgame results.
void populate_frame_analysis_rows(TuiGameState *state) {
  if (state == NULL) {
    return;
  }
  state->last_rendered_analysis_row_count = 0;
  // Play-vs-computer hides the Analysis panel for the whole live game
  // and the on-board candidate preview is disabled too — there is no
  // consumer for these rows, so don't build them. This was the 4-5fps /
  // 200-300ms input-lag stall: the sim row builder's per-letter string
  // churn fights a heavily contended allocator while the bot's search
  // threads hammer malloc, measured at ~280ms per frame in `sample`.
  if (state->app_mode == TUI_APP_MODE_PLAY_VS_COMPUTER && state->game != NULL &&
      !tui_game_state_play_over(state)) {
    return;
  }
  // While the "/resume" worker is recomputing the cursored turn, the
  // live (ticking) results take precedence over the entry's frozen
  // snapshot.
  const bool resuming_cursor = resume_active_for_cursor(state);
  const TuiAnalysisSnapshot *snap = NULL;
  if (!resuming_cursor && state->history_cursor >= 0 &&
      state->history_cursor < state->history_count) {
    const TuiHistoryEntry *e = &state->history[state->history_cursor];
    if (!e->pending && e->analysis_snapshot.valid) {
      snap = &e->analysis_snapshot;
    }
  }
  if (snap != NULL) {
    const int n =
        snap->num_rows < ANALYSIS_ROW_CAP ? snap->num_rows : ANALYSIS_ROW_CAP;
    memcpy(state->last_rendered_analysis_rows, snap->rows,
           sizeof(AnalysisRow) * (size_t)n);
    state->last_rendered_analysis_row_count = n;
    return;
  }
  // Bag emptiness follows the position being analyzed — the resumed
  // turn's reconstruction during a "/resume", the live game otherwise
  // — so a resumed mid-game sim doesn't get misread as endgame just
  // because the finished game's bag is empty.
  const Game *src_game = analysis_source_game(state);
  const bool bag_empty =
      src_game != NULL && bag_get_letters(game_get_bag(src_game)) == 0;
  const bool use_endgame = bag_empty && state->endgame_snapshot.valid &&
                           state->endgame_snapshot.num_entries > 0;
  // PEG when the analyzed position is in pre-endgame range (1-4
  // effective bag tiles) and the poll's contents belong to this game
  // — same turn-idx gating as the sim path below.
  const bool live_peg = !use_endgame && tui_position_in_peg_range(src_game) &&
                        state->peg_poll != NULL &&
                        atomic_load(&state->peg_results_turn_idx) >= 0;
  // Gate sim row population on sim_results_turn_idx — the
  // reset paths flip it to -1 to signal "the sim_results
  // object's contents aren't valid for the current game".
  // The engine doesn't zero sim_results itself, so without
  // this check the prior game's leaderboard keeps rendering
  // (and burning per-frame CPU) after annotation start.
  const bool live_sim = !use_endgame && !live_peg &&
                        state->sim_results != NULL &&
                        atomic_load(&state->sim_results_turn_idx) >= 0;
  if (use_endgame || live_peg || live_sim) {
    // Throttle live-leaderboard rebuilds to ~10Hz. Rebuilding the row
    // strings at 60fps is pure waste — the leaderboard doesn't change
    // meaningfully frame-to-frame — and it's expensive far beyond its
    // size: each row's move/leave strings do per-letter strdup/free,
    // and while a search is running those allocations contend with
    // the engine threads' own malloc traffic (~280ms/frame measured).
    // The previous rows persist in last_rendered_analysis_rows, so
    // between rebuilds the panel simply keeps showing them.
    static struct timespec last_build;
    static int last_build_count;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const long since_ms = (long)(now.tv_sec - last_build.tv_sec) * 1000L +
                          (long)(now.tv_nsec - last_build.tv_nsec) / 1000000L;
    if (last_build_count > 0 && since_ms >= 0 && since_ms < 100) {
      state->last_rendered_analysis_row_count = last_build_count;
      return;
    }
    if (use_endgame) {
      state->last_rendered_analysis_row_count = fill_analysis_rows_from_endgame(
          state, state->last_rendered_analysis_rows, ANALYSIS_ROW_CAP);
    } else if (live_peg) {
      state->last_rendered_analysis_row_count =
          fill_analysis_rows_from_peg(state, state->last_rendered_analysis_rows,
                                      ANALYSIS_ROW_CAP, &state->peg_live_meta);
    } else {
      state->last_rendered_analysis_row_count = fill_analysis_rows_from_sim(
          state, state->last_rendered_analysis_rows, ANALYSIS_ROW_CAP);
    }
    last_build = now;
    last_build_count = state->last_rendered_analysis_row_count;
  }

  // Fallback: nothing computed (loaded GCG turn that wasn't simmed
  // by this TUI), but the cursor is parked on a committed entry —
  // surface the played move + leave + score as a single
  // "Analysis" row so the panel isn't blank while reviewing.
  if (state->last_rendered_analysis_row_count == 0 &&
      state->history_cursor >= 0 &&
      state->history_cursor < state->history_count) {
    const TuiHistoryEntry *e = &state->history[state->history_cursor];
    if (!e->pending && e->move_str[0] != '\0') {
      AnalysisRow *row = &state->last_rendered_analysis_rows[0];
      memset(row, 0, sizeof(*row));
      snprintf(row->move, sizeof(row->move), "%s", e->move_str);
      snprintf(row->leave, sizeof(row->leave), "%s", e->leave_str);
      snprintf(row->score, sizeof(row->score), "%d", e->score);
      row->score_value = e->score;
      row->candidate_player_idx = e->player_idx;
      row->valid = true;
      state->last_rendered_analysis_row_count = 1;
    }
  }
}
void tui_capture_analysis_snapshot(const TuiGameState *state,
                                   TuiAnalysisSnapshot *out) {
  if (state == NULL || out == NULL) {
    return;
  }
  memset(out, 0, sizeof(*out));
  // Bag emptiness follows the analyzed position (the resume worker's
  // reconstruction during a "/resume", the live game otherwise) so a
  // resumed mid-game sim captures as a sim snapshot even though the
  // finished game's bag is empty.
  const Game *src_game = analysis_source_game(state);
  const bool bag_empty =
      src_game != NULL && bag_get_letters(game_get_bag(src_game)) == 0;
  const bool use_endgame = bag_empty && state->endgame_snapshot.valid &&
                           state->endgame_snapshot.num_entries > 0;
  const bool use_peg = !use_endgame && tui_position_in_peg_range(src_game) &&
                       state->peg_poll != NULL &&
                       atomic_load(&state->peg_results_turn_idx) >= 0;
  if (use_endgame) {
    out->is_sim = false;
    out->num_rows =
        fill_analysis_rows_from_endgame(state, out->rows, ANALYSIS_ROW_CAP);
    out->endgame_depth = state->endgame_snapshot.depth;
    out->endgame_exhaustive = state->endgame_snapshot.exhaustive;
    if (state->endgame_ctx != NULL) {
      out->endgame_nodes = endgame_ctx_get_nodes_searched(state->endgame_ctx);
    }
    out->valid = out->num_rows > 0;
  } else if (use_peg) {
    out->is_peg = true;
    TuiPegLiveMeta meta = {0};
    out->num_rows =
        fill_analysis_rows_from_peg(state, out->rows, ANALYSIS_ROW_CAP, &meta);
    out->peg_fidelity = meta.fidelity;
    out->peg_done = meta.solve_done;
    out->valid = out->num_rows > 0;
  } else if (state->sim_results != NULL) {
    out->is_sim = true;
    out->num_rows =
        fill_analysis_rows_from_sim(state, out->rows, ANALYSIS_ROW_CAP);
    out->sim_plies = sim_results_get_num_plies(state->sim_results);
    out->sim_iterations = sim_results_get_iteration_count(state->sim_results);
    out->sim_nodes = out->sim_iterations * (uint64_t)(out->sim_plies + 1);
    out->valid = out->num_rows > 0;
  }
}
