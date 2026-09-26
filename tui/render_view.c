#include "render_view.h"

#include "../src/def/game_history_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_results.h"
#include "config.h"
#include "game_state.h"
#include "tui_ui_types.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Pick which Board the board renderer should display: the entry's
// pre-move snapshot when the History panel is focused and the
// cursor is on a committed entry, otherwise the live game board.
// Pending entries fall back to live since their snapshot IS the
// current board and the spinner UX is already conveying that the
// move is in flight.
const Board *pick_render_board(const TuiGameState *state) {
  if (state == NULL) {
    return NULL;
  }
  if (state->history_cursor >= 0 &&
      state->history_cursor < state->history_count) {
    const TuiHistoryEntry *entry = &state->history[state->history_cursor];
    if (!entry->pending && entry->board_before != NULL) {
      return entry->board_before;
    }
  }
  return state->game != NULL ? game_get_board(state->game) : NULL;
}
// Returns the committed history entry the user has cursored to —
// the "rewind target" for the rack panel + player pills. NULL when
// the cursor is on the label or on a pending entry, meaning the
// live game state should be shown. Cursor persists across panel
// focus changes by design, so we deliberately don't require
// History to be focused — wherever the cursor sits is what every
// rewindable view shows.
const TuiHistoryEntry *pick_history_view(const TuiGameState *state) {
  if (state == NULL) {
    return NULL;
  }
  if (state->history_cursor < 0 ||
      state->history_cursor >= state->history_count) {
    return NULL;
  }
  const TuiHistoryEntry *entry = &state->history[state->history_cursor];
  if (entry->pending) {
    return NULL;
  }
  return entry;
}
// Rack to display for `player_idx`. When the cursor is on a
// committed turn the entry stashes both the on-turn (rack_before)
// and the off-turn (opp_rack_before) snapshots; we match by
// player_idx so the pill for the player who PLAYED that turn
// shows the rack they faced, and the opposite pill shows what
// their opponent was holding at the same moment.
const Rack *pick_render_rack(const TuiGameState *state, int player_idx) {
  // In play-vs-computer, conceal the computer's tiles from the human
  // until the game is over (when the final position is fully revealed).
  // Gate this before the history-snapshot block so cursoring back
  // through committed turns doesn't leak the computer's past racks
  // either.
  if (state->app_mode == TUI_APP_MODE_PLAY_VS_COMPUTER &&
      player_idx != state->human_player_idx && state->game != NULL &&
      !tui_game_state_play_over(state)) {
    return NULL;
  }
  const TuiHistoryEntry *entry = pick_history_view(state);
  if (entry != NULL) {
    if (entry->player_idx == player_idx && entry->rack_before != NULL) {
      return entry->rack_before;
    }
    if (entry->player_idx != player_idx && entry->opp_rack_before != NULL) {
      return entry->opp_rack_before;
    }
  }
  return state->game != NULL
             ? player_get_rack(game_get_player(state->game, player_idx))
             : NULL;
}
// Which player_idx is "on turn" for rendering purposes. Cursored
// to a committed entry: that turn's player_idx (they were about
// to play). Else: the live on-turn index.
int pick_render_on_turn(const TuiGameState *state) {
  const TuiHistoryEntry *entry = pick_history_view(state);
  if (entry != NULL) {
    return entry->player_idx;
  }
  // Once the game is over, the engine's on_turn_index has
  // advanced to whoever would have played next — but visually
  // we want the pill to stay with the player who *ended* the
  // game (the one who went out, or whose last play closed it).
  // That player is the author of the last history entry.
  if (state->game != NULL && game_over(state->game) &&
      state->history_count > 0) {
    return state->history[state->history_count - 1].player_idx;
  }
  return state->game != NULL ? game_get_player_on_turn_index(state->game) : 0;
}
// When the Analysis cursor is in MOVE column, find the current
// row index of the anchored move by scanning the prepared
// rows-for-this-frame. Returns -1 if the anchored move is not
// present in the current rows. Caller falls back to RANK-mode
// behavior in that case.
static int find_anchored_move_row(const TuiGameState *state) {
  if (state == NULL || state->analysis_anchored_move[0] == '\0') {
    return -1;
  }
  for (int i = 0; i < state->last_rendered_analysis_row_count; i++) {
    if (strcmp(state->last_rendered_analysis_rows[i].move,
               state->analysis_anchored_move) == 0) {
      return i;
    }
  }
  return -1;
}
// Effective cursor row for the current frame. In RANK column the
// cursor is just state->analysis_cursor (a fixed row index). In
// MOVE column the cursor follows the anchored move's current
// position — find_anchored_move_row resolves it against the
// rows already prepared for this frame.
int effective_analysis_cursor(const TuiGameState *state) {
  if (state == NULL) {
    return -1;
  }
  if (state->analysis_cursor < 0) {
    // Cursor on the [5] label rather than a specific candidate:
    // implicitly highlight the top rank since that's the move
    // the panel previews on the board. Falls back to -1 when
    // there's nothing to preview.
    return state->last_rendered_analysis_row_count > 0 ? 0 : -1;
  }
  if (state->analysis_cursor_column == TUI_ANALYSIS_COLUMN_MOVE) {
    const int idx = find_anchored_move_row(state);
    if (idx >= 0) {
      return idx;
    }
  }
  return state->analysis_cursor;
}
// Resolve the Analysis-cursor row to the underlying Move for
// preview rendering on the board. Returns NULL when no preview
// applies — no row cursored, no live results, or out-of-range.
// Caller must NOT call sim_results_lock_and_sort_*; we don't
// hold the display lock across the return, so the returned
// Move pointer is borrowed and only valid while state's
// SimResults / endgame_snapshot are stable (which they are
// inside a render frame holding state->mutex).
const Move *pick_analysis_preview_move(const TuiGameState *state,
                                       int *out_player_idx) {
  if (state == NULL) {
    return NULL;
  }
  // While the annotation cell editor is open and the engine has
  // validated the typed move, ghost THAT play on the board — it's
  // what the user is shaping right now, takes precedence over any
  // sim/endgame candidate or saved snapshot preview.
  if (state->edit_history_idx >= 0 && state->edit_preview_move_valid &&
      state->edit_preview_move != NULL &&
      state->edit_history_idx < state->history_count) {
    if (out_player_idx != NULL) {
      *out_player_idx = state->history[state->edit_history_idx].player_idx;
    }
    return state->edit_preview_move;
  }
  // Play-vs-computer: during the live game the only board/rack preview
  // is the human's own in-progress move (handled by the edit-preview
  // branch above). Suppress every other analysis candidate — both the
  // computer's live sim (which would reveal its move) and any stale sim
  // results left over from the computer's previous turn (which were
  // ghosting tiles on the human's rack). Analysis previews return at
  // game over for post-game review.
  if (state->app_mode == TUI_APP_MODE_PLAY_VS_COMPUTER && state->game != NULL &&
      !tui_game_state_play_over(state)) {
    return NULL;
  }
  const int idx = effective_analysis_cursor(state);
  if (idx < 0) {
    return NULL;
  }
  // History cursor on a committed entry → preview must come from
  // the SAVED analysis for THAT turn, not the live sim. Otherwise
  // a user navigating to turn 3 would see the Analysis panel
  // show turn 3's saved leaderboard but the board would draw a
  // candidate from the bot's current-turn sim. Use the entry's
  // sim_results_saved (a deep clone of the SimResults captured at
  // finalize time), whose display_simmed_plays order matches the
  // analysis_snapshot.rows order the user is reading.
  const TuiHistoryEntry *hist_entry = pick_history_view(state);
  if (hist_entry != NULL && hist_entry->analysis_snapshot.valid &&
      hist_entry->analysis_snapshot.is_static) {
    // A "/kibitz" ranking is on screen; preview its rows.
    if (idx >= hist_entry->static_moves_saved_count) {
      return NULL;
    }
    if (out_player_idx != NULL) {
      *out_player_idx = hist_entry->player_idx;
    }
    return &hist_entry->static_moves_saved[idx];
  }
  if (hist_entry != NULL) {
    if (hist_entry->sim_results_saved != NULL) {
      SimResults *saved = hist_entry->sim_results_saved;
      if (!sim_results_lock_and_sort_display_simmed_plays(saved)) {
        return NULL;
      }
      const Move *out_move = NULL;
      if (idx < sim_results_get_number_of_plays(saved)) {
        const SimmedPlay *play =
            sim_results_get_display_simmed_play(saved, idx);
        if (play != NULL) {
          out_move = simmed_play_get_move(play);
        }
      }
      sim_results_unlock_display_infos(saved);
      if (out_player_idx != NULL) {
        *out_player_idx = hist_entry->player_idx;
      }
      return out_move;
    }
    if (hist_entry->endgame_moves_saved != NULL &&
        idx < hist_entry->endgame_moves_saved_count) {
      // Endgame turn — replay the saved leaderboard moves the
      // same way sim turns do, so the on-board preview works
      // uniformly across both modes.
      if (out_player_idx != NULL) {
        *out_player_idx = hist_entry->player_idx;
      }
      return &hist_entry->endgame_moves_saved[idx];
    }
    // Loaded GCG entry — no sim/endgame leaderboard, just the
    // single move that was played. Surface it as the preview so
    // the board ghosts the played tiles when the cursor lands on
    // the "Plays" row.
    if (hist_entry->loaded_move != NULL && idx == 0 &&
        tui_history_spoiler(state, state->history_cursor) == TUI_SPOILER_NONE) {
      if (out_player_idx != NULL) {
        *out_player_idx = hist_entry->player_idx;
      }
      return hist_entry->loaded_move;
    }
    return NULL;
  }
  // No history cursor — preview from live state. Endgame snapshot
  // takes precedence when the bag is empty and a solve has landed.
  const bool bag_empty =
      state->game != NULL && bag_get_letters(game_get_bag(state->game)) == 0;
  if (bag_empty && state->endgame_snapshot.valid &&
      idx < state->endgame_snapshot.num_entries &&
      state->endgame_snapshot.moves != NULL) {
    const Move *m = state->endgame_snapshot.moves[idx];
    if (out_player_idx != NULL) {
      *out_player_idx = state->endgame_snapshot.solving_player;
    }
    return m;
  }
  if (state->sim_results == NULL) {
    return NULL;
  }
  // Same gate the analysis row populator uses: sim_results
  // contents persist across game resets, so without this check
  // we'd ghost a candidate from the prior game (e.g., "GUV"
  // crosses H8 after starting annotation). sim_results_turn_idx
  // is flipped to -1 by the reset paths.
  if (atomic_load(&((TuiGameState *)state)->sim_results_turn_idx) < 0) {
    return NULL;
  }
  SimResults *results = state->sim_results;
  if (!sim_results_lock_and_sort_display_simmed_plays(results)) {
    return NULL;
  }
  const Move *out_move = NULL;
  if (idx < sim_results_get_number_of_plays(results)) {
    const SimmedPlay *play = sim_results_get_display_simmed_play(results, idx);
    if (play != NULL) {
      out_move = simmed_play_get_move(play);
    }
  }
  sim_results_unlock_display_infos(results);
  if (out_player_idx != NULL) {
    *out_player_idx = game_get_player_on_turn_index(state->game);
  }
  return out_move;
}
// Fill `out_slots` with the visible rack tiles for `rack` in the
// display order chosen by `sort`. Returns the slot count. The
// engine's internal rack ordering isn't touched — this is purely
// presentation. Vowel grouping uses the letter-distribution's
// is_vowel flag so the same setting works for any language.
int sort_rack_for_display(const Rack *rack, const LetterDistribution *ld,
                          TuiRackSort sort, MachineLetter *out_slots,
                          int max_slots) {
  if (rack == NULL || ld == NULL || out_slots == NULL || max_slots <= 0) {
    return 0;
  }
  int n = 0;
  const int ld_size = ld_get_size(ld);
#define EMIT_ML(_ml)                                                           \
  do {                                                                         \
    const int _cnt = rack_get_letter(rack, (MachineLetter)(_ml));              \
    for (int _c = 0; _c < _cnt && n < max_slots; _c++) {                       \
      out_slots[n++] = (MachineLetter)(_ml);                                   \
    }                                                                          \
  } while (0)
  switch (sort) {
  case TUI_RACK_SORT_BLANKS_ALPHA:
    // ? first, then A..Z. Machine-letter order already encodes
    // this since BLANK_MACHINE_LETTER == 0.
    for (int ml = 0; ml < ld_size && n < max_slots; ml++) {
      EMIT_ML(ml);
    }
    break;
  case TUI_RACK_SORT_BLANKS_VOWELS:
    EMIT_ML(BLANK_MACHINE_LETTER);
    for (int ml = 1; ml < ld_size && n < max_slots; ml++) {
      if (ld_get_is_vowel(ld, (MachineLetter)ml)) {
        EMIT_ML(ml);
      }
    }
    for (int ml = 1; ml < ld_size && n < max_slots; ml++) {
      if (!ld_get_is_vowel(ld, (MachineLetter)ml)) {
        EMIT_ML(ml);
      }
    }
    break;
  case TUI_RACK_SORT_VOWELS:
    for (int ml = 1; ml < ld_size && n < max_slots; ml++) {
      if (ld_get_is_vowel(ld, (MachineLetter)ml)) {
        EMIT_ML(ml);
      }
    }
    for (int ml = 1; ml < ld_size && n < max_slots; ml++) {
      if (!ld_get_is_vowel(ld, (MachineLetter)ml)) {
        EMIT_ML(ml);
      }
    }
    EMIT_ML(BLANK_MACHINE_LETTER);
    break;
  case TUI_RACK_SORT_ALPHA:
  case TUI_RACK_SORT_COUNT:
  default:
    // Default: A..Z first, blank last.
    for (int ml = 1; ml < ld_size && n < max_slots; ml++) {
      EMIT_ML(ml);
    }
    EMIT_ML(BLANK_MACHINE_LETTER);
    break;
  }
#undef EMIT_ML
  return n;
}
// Mark which rack slots correspond to tiles consumed by the
// currently-previewed move. Visited in slot order so the first
// occurrence of each used letter gets ghosted (the rack panel
// renders tiles in alphabetical order, so this stays stable).
// out_ghost[i] is set to true when slot_letters[i] is a rack
// tile the preview move would spend.
void compute_rack_ghost_mask(const TuiGameState *state,
                             const MachineLetter *slot_letters, int slot_count,
                             bool *out_ghost) {
  for (int i = 0; i < slot_count; i++) {
    out_ghost[i] = false;
  }
  const Move *m = pick_analysis_preview_move(state, NULL);
  if (m == NULL) {
    return;
  }
  const game_event_t mtype = move_get_type(m);
  if (mtype != GAME_EVENT_TILE_PLACEMENT_MOVE && mtype != GAME_EVENT_EXCHANGE) {
    return;
  }
  // Count rack-tiles needed by the move. Blanks consume a blank
  // rack tile (ml = 0) regardless of which letter they stand in
  // for, so strip the blank bit before bucketing. Exchanges have
  // no PLAYED_THROUGH_MARKER positions; placements may.
  int needed[256] = {0};
  const int n = mtype == GAME_EVENT_EXCHANGE ? move_get_tiles_played(m)
                                             : move_get_tiles_length(m);
  for (int t = 0; t < n; t++) {
    const MachineLetter tile = move_get_tile(m, t);
    if (tile == PLAYED_THROUGH_MARKER) {
      continue;
    }
    const MachineLetter rack_ml =
        get_is_blanked(tile) ? BLANK_MACHINE_LETTER : tile;
    if ((int)rack_ml < 256) {
      needed[(int)rack_ml]++;
    }
  }
  for (int i = 0; i < slot_count; i++) {
    const MachineLetter ml = slot_letters[i];
    if ((int)ml < 256 && needed[(int)ml] > 0) {
      out_ghost[i] = true;
      needed[(int)ml]--;
    }
  }
}
// Clock seconds to display for `player_idx`. When cursor is on a
// committed entry, returns the player's time-remaining as
// snapshotted at the start of that turn (the same moment the
// rack snapshot was taken). Else falls through to the live
// monotonic-clock derivation.
double pick_render_clock_seconds(const TuiGameState *state, int player_idx) {
  const TuiHistoryEntry *entry = pick_history_view(state);
  if (entry != NULL) {
    const int snap = entry->player_idx == player_idx
                         ? entry->clock_at_start
                         : entry->opp_clock_at_start;
    // Negative snapshots are real under the overtime rules (the turn
    // began past 0:00) — pass them through so the pill shows the
    // overtime clock. Outside overtime the snapshots are never
    // negative, so no defensive floor is needed here.
    return (double)snap;
  }
  return seconds_remaining(state, player_idx);
}
// Score to display for `player_idx`. When cursor is on entry K
// we return the score they had going INTO turn K (i.e. before
// turn K's play was applied). For the player who played K that
// is total_after - score; for any earlier player we walk back to
// their most-recent prior total_after.
int pick_render_score(const TuiGameState *state, int player_idx) {
  const TuiHistoryEntry *entry = pick_history_view(state);
  if (entry == NULL) {
    return state->game != NULL ? equity_to_int(player_get_score(
                                     game_get_player(state->game, player_idx)))
                               : 0;
  }
  for (int idx = state->history_cursor - 1; idx >= 0; idx--) {
    const TuiHistoryEntry *prior = &state->history[idx];
    if (prior->pending) {
      continue;
    }
    if (prior->player_idx == player_idx) {
      // A challenged-off play netted zero — its as-if total minus the
      // cancelled score is the player's real running total.
      if (prior->challenged_off) {
        return prior->total_after - prior->score;
      }
      return prior->total_after + prior->end_bonus;
    }
  }
  return 0;
}
// Live clock: how many seconds remain for player_idx, accounting for the
// time elapsed in the current on-turn player's turn so the display ticks
// in real time. Caller must hold state->mutex.
//
// Defensive clamping: a stray bad turn_started or seconds_used should
// produce a flat 0:00 / time_per_side display, never multi-million-minute
// nonsense. Negative `used` becomes 0; remaining is clamped to
// [0, time_per_side] so the visible clock always lives in the player's
// budget.
double seconds_remaining(const TuiGameState *state, int player_idx) {
  double used = state->seconds_used[player_idx];
  if (used < 0.0) {
    used = 0.0;
  }
  // Only tick the live elapsed-time portion when a game is actually
  // in progress (the bot worker is running). At app launch the bot
  // is idle behind the startup menu — without this gate the on-turn
  // clock would count down even though no game has started.
  if (state->bot_started &&
      game_get_player_on_turn_index(state->game) == player_idx &&
      !tui_game_state_play_over(state)) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = (double)(now.tv_sec - state->turn_started.tv_sec) +
                     (double)(now.tv_nsec - state->turn_started.tv_nsec) / 1e9;
    if (elapsed < 0.0) {
      elapsed = 0.0;
    }
    used += elapsed;
  }
  const double total = (double)state->time_per_side_seconds;
  double remaining = total - used;
  // In play-vs-computer overtime (MAX / UNLIMITED rules) the clock
  // legitimately runs negative and the display shows it ("-1:23").
  // Everywhere else, keep the defensive floor at 0.
  const bool overtime_allowed =
      state->app_mode == TUI_APP_MODE_PLAY_VS_COMPUTER &&
      state->overtime_rule != UI_OVERTIME_FLAG;
  if (remaining < 0.0 && !overtime_allowed) {
    remaining = 0.0;
  }
  if (remaining > total) {
    remaining = total;
  }
  return remaining;
}
