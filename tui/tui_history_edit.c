#include "tui_history_edit.h"

#include "game_state.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// Persist the current edit buffers (move / rack / leave) into the
// entry the editor is focused on. ALWAYS saves — valid or not —
// so navigating away never loses typed input; revalidation then
// surfaces any error inline. Caller holds game_state.mutex.
static void tui_commit_edit_to_entry(TuiGameState *gs) {
  if (gs->edit_history_idx < 0 || gs->edit_history_idx >= gs->history_count) {
    return;
  }
  TuiHistoryEntry *e = &gs->history[gs->edit_history_idx];
  tui_game_state_parse_edit_buf(gs);
  switch (gs->edit_move_kind) {
  case TUI_EDIT_MOVE_KIND_PLACEMENT:
  case TUI_EDIT_MOVE_KIND_EXCHANGE:
  case TUI_EDIT_MOVE_KIND_PASS:
    // Engine-recognized move: store the canonical form ("-ABC"
    // display normalization for exchanges) and its score.
    tui_game_state_edit_move_display(gs, e->move_str, sizeof(e->move_str));
    e->score = gs->edit_move_score >= 0 ? gs->edit_move_score : 0;
    e->pending = false;
    break;
  default:
    // Partial / invalid / bare-word / empty. Persist the raw typed
    // text (so it survives the blur and revalidation can flag it),
    // or clear + revert to pending when the move field is empty.
    if (gs->edit_move_len > 0) {
      snprintf(e->move_str, sizeof(e->move_str), "%.*s", gs->edit_move_len,
               gs->edit_move_buf);
      e->pending = false;
    } else {
      e->move_str[0] = '\0';
      e->pending = true;
    }
    e->score = 0;
    break;
  }
  if (gs->edit_rack_len > 0) {
    snprintf(e->rack_str, sizeof(e->rack_str), "%.*s", gs->edit_rack_len,
             gs->edit_rack_buf);
  } else {
    e->rack_str[0] = '\0';
  }
  if (gs->edit_leave_len > 0) {
    snprintf(e->leave_str, sizeof(e->leave_str), "%.*s", gs->edit_leave_len,
             gs->edit_leave_buf);
  } else if (gs->edit_move_leave[0] != '\0') {
    snprintf(e->leave_str, sizeof(e->leave_str), "%s", gs->edit_move_leave);
  } else {
    e->leave_str[0] = '\0';
  }
}
// Commit-on-blur + full revalidation. Call before any navigation
// that moves focus off the currently-edited turn.
void tui_commit_edit_and_revalidate(TuiGameState *gs) {
  if (gs->edit_history_idx < 0) {
    return;
  }
  // Play-vs-computer: blur (Esc / turn-nav / click-away) ABANDONS the
  // typed text instead of committing it — moves only land through the
  // explicit PvC commit (Enter), which plays them for real. The
  // annotation commit would stamp unplayed text onto the live pending
  // entry, and revalidate_history's full no-draw replay rebuilds racks
  // from entry text — wiping the real bag-drawn racks (the "(no rack)"
  // pill + empty-racks CGP bug).
  if (gs->app_mode == TUI_APP_MODE_PLAY_VS_COMPUTER) {
    return;
  }
  tui_commit_edit_to_entry(gs);
  tui_game_state_revalidate_history(gs);
}
