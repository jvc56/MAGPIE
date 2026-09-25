#include "input_cell_editor.h"

#include "../src/impl/gameplay.h"
#include "bot_worker.h"
#include "game_state.h"
#include "move_entry.h"
#include "render_common.h"
#include "tui_history_edit.h"
#include <notcurses/notcurses.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// A key while board move entry is active: drives the on-board move
// builder (Space toggles direction, arrows move the anchor, Backspace
// retracts, Enter submits, Esc cancels) and swallows everything else.
static bool cell_editor_board_entry_key(TuiGameState *state, uint32_t key,
                                        const ncinput *input) {
  if (key == NCKEY_ESC) {
    pthread_mutex_lock(&state->mutex);
    tui_board_builder_cancel(state);
    pthread_mutex_unlock(&state->mutex);
    return true;
  }
  if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
    pthread_mutex_lock(&state->mutex);
    tui_board_entry_submit(state);
    pthread_mutex_unlock(&state->mutex);
    return true;
  }
  if (key == ' ' || key == NCKEY_TAB || key == '\t') {
    // Space or Tab toggles direction (keyboard parallel to
    // clicking the origin cell again).
    pthread_mutex_lock(&state->mutex);
    tui_board_builder_toggle_dir(state);
    pthread_mutex_unlock(&state->mutex);
    return true;
  }
  if (key == NCKEY_LEFT || key == NCKEY_RIGHT || key == NCKEY_UP ||
      key == NCKEY_DOWN) {
    // Arrows move the ORIGIN (the user's typing start cell);
    // the walked-back anchor re-derives from it. Moving the
    // anchor directly got stuck against leading playthrough:
    // set_anchor immediately walked it back again.
    pthread_mutex_lock(&state->mutex);
    int origin_r = state->board_origin_row;
    int origin_c = state->board_origin_col;
    if (key == NCKEY_LEFT && origin_c > 0) {
      origin_c--;
    } else if (key == NCKEY_RIGHT && origin_c < BOARD_DIM - 1) {
      origin_c++;
    } else if (key == NCKEY_UP && origin_r > 0) {
      origin_r--;
    } else if (key == NCKEY_DOWN && origin_r < BOARD_DIM - 1) {
      origin_r++;
    }
    // Re-anchoring drops any in-progress tiles back to the rack
    // and keeps the current direction.
    tui_board_builder_set_anchor(state, origin_r, origin_c, state->board_dir);
    pthread_mutex_unlock(&state->mutex);
    return true;
  }
  if (key == NCKEY_BACKSPACE || key == 0x7f || key == 0x08) {
    pthread_mutex_lock(&state->mutex);
    tui_board_entry_backspace(state);
    pthread_mutex_unlock(&state->mutex);
    return true;
  }
  if (key >= 0x20 && key < 0x7f) {
    const char ch = (char)key;
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) {
      pthread_mutex_lock(&state->mutex);
      tui_move_entry_append_letter(state, ch, ncinput_shift_p(input));
      pthread_mutex_unlock(&state->mutex);
    }
    // Swallow other printables (digits, punctuation) in board
    // mode so they don't leak into the coord token.
    return true;
  }
  // Swallow anything else while board entry is active.
  return true;
}

// Shift+arrow: commits the turn being edited and opens the previous or
// next turn's MOVE field with its stored text.
static bool cell_editor_step_turn(TuiGameState *state, uint32_t key) {
  const int dir = (key == NCKEY_LEFT || key == NCKEY_UP) ? -1 : 1;
  pthread_mutex_lock(&state->mutex);
  const int target = state->edit_history_idx + dir;
  if (target >= 0 && target < state->history_count) {
    // Blur-commit the turn we're leaving before loading the
    // target's stored text.
    tui_commit_edit_and_revalidate(state);
    TuiHistoryEntry *e = &state->history[target];
    snprintf(state->edit_move_buf, sizeof(state->edit_move_buf), "%s",
             e->move_str);
    state->edit_move_len = (int)strlen(state->edit_move_buf);
    state->edit_move_cursor = state->edit_move_len;
    snprintf(state->edit_rack_buf, sizeof(state->edit_rack_buf), "%s",
             e->rack_str);
    state->edit_rack_len = (int)strlen(state->edit_rack_buf);
    state->edit_rack_cursor = state->edit_rack_len;
    // Treat a non-empty stored rack as user-authored so the
    // editor doesn't re-derive it from the move's inferred
    // letters the moment focus lands.
    state->edit_rack_user_modified = e->rack_str[0] != '\0';
    // Switching turns — drop any stale carryover seed from a
    // previously-edited turn so it doesn't merge into this
    // entry's rack.
    state->edit_rack_carryover[0] = '\0';
    state->edit_history_idx = target;
    state->history_cursor = target;
    state->edit_field = TUI_EDIT_FIELD_MOVE;
    tui_game_state_parse_edit_buf(state);
  }
  pthread_mutex_unlock(&state->mutex);
  return true;
}

// Tab or Up from RACK or LEAVE: commits the rack (and derived leave) to
// the entry, alphagrammed, and returns focus to MOVE.
static bool cell_editor_back_to_move(TuiGameState *state, int idx) {
  pthread_mutex_lock(&state->mutex);
  tui_game_state_parse_edit_buf(state);
  // Alphagram the rack buffer on focus-leave so the sorted
  // form sticks across re-edits. Once the user has tabbed
  // out, the typed order is no longer interesting — what
  // they see in the cell is the alphagram, and re-opening
  // the field should land them on that same alphagram.
  // Only the sort order (state->rack_sort) can change it
  // from here on.
  if (state->edit_rack_valid && state->edit_rack_len > 0) {
    char sorted_rack[24];
    char raw[24];
    const int n_raw = state->edit_rack_len < (int)sizeof(raw) - 1
                          ? state->edit_rack_len
                          : (int)sizeof(raw) - 1;
    memcpy(raw, state->edit_rack_buf, (size_t)n_raw);
    raw[n_raw] = '\0';
    tui_format_alphagram_for_sort(raw, state->ld, state->rack_sort, sorted_rack,
                                  sizeof(sorted_rack));
    snprintf(state->edit_rack_buf, sizeof(state->edit_rack_buf), "%s",
             sorted_rack);
    state->edit_rack_len = (int)strlen(state->edit_rack_buf);
    state->edit_rack_cursor = state->edit_rack_len;
  }
  if (idx < state->history_count && state->edit_rack_valid) {
    TuiHistoryEntry *e = &state->history[idx];
    int n = state->edit_rack_len;
    if (n >= (int)sizeof(e->rack_str)) {
      n = (int)sizeof(e->rack_str) - 1;
    }
    memcpy(e->rack_str, state->edit_rack_buf, (size_t)n);
    e->rack_str[n] = '\0';
    // User-typed leave (via click into LEAVE) wins over the
    // auto-derived edit_move_leave. Otherwise fall back to
    // the auto-derived value.
    if (state->edit_leave_len > 0) {
      snprintf(e->leave_str, sizeof(e->leave_str), "%s", state->edit_leave_buf);
    } else if (state->edit_move_leave[0] != '\0') {
      snprintf(e->leave_str, sizeof(e->leave_str), "%s",
               state->edit_move_leave);
    }
  }
  state->edit_field = TUI_EDIT_FIELD_MOVE;
  pthread_mutex_unlock(&state->mutex);
  return true;
}

// Enter, Tab, or Down on MOVE: commits the move to the entry and moves
// focus to RACK, seeding an empty rack from the move's inferred letters.
static bool cell_editor_advance_to_rack(TuiGameState *state, int idx) {
  pthread_mutex_lock(&state->mutex);
  tui_game_state_parse_edit_buf(state);
  if (idx < state->history_count) {
    TuiHistoryEntry *e = &state->history[idx];
    switch (state->edit_move_kind) {
    case TUI_EDIT_MOVE_KIND_PLACEMENT:
    case TUI_EDIT_MOVE_KIND_EXCHANGE:
    case TUI_EDIT_MOVE_KIND_PASS: {
      // Exchanges go onto the entry in the TUI's compact
      // "-ABC" form (matching how finalize_history shows
      // bot-played exchanges). The engine validator still
      // sees "ex ABC" via edit_move_canonical above; this
      // is purely a display normalization at the commit
      // boundary.
      if (strncmp(state->edit_move_canonical, "ex ", 3) == 0) {
        snprintf(e->move_str, sizeof(e->move_str), "-%s",
                 state->edit_move_canonical + 3);
      } else {
        snprintf(e->move_str, sizeof(e->move_str), "%s",
                 state->edit_move_canonical);
      }
      // Always re-sync the score on every commit, even
      // when the engine rejected the new move (score < 0).
      // Otherwise editing XIPHOID → XYSTI leaves the old
      // XIPHOID score stale on the entry. Same for the
      // leave: clear it when the new state doesn't infer
      // one.
      e->score = state->edit_move_score >= 0 ? state->edit_move_score : 0;
      // While the user hasn't authored the rack, the
      // entry's rack_str needs to track the move's
      // inferred letters. Editing XiPHOID → XIPHOID would
      // otherwise leave the stale "IODHPX?" baked onto the
      // entry, even though the rack panel updates live.
      // Same idea for the rack buffer (so re-opening the
      // editor lands on the fresh inferred seed). When the
      // rack tracks inferred, leave is exactly empty —
      // skip the parser's edit_move_leave value (which was
      // computed against the old buffer and is stale by now).
      if (!state->edit_rack_user_modified) {
        if (state->edit_move_inferred_rack[0] != '\0') {
          char sorted[24];
          tui_format_alphagram_for_sort(state->edit_move_inferred_rack,
                                        state->ld, state->rack_sort, sorted,
                                        sizeof(sorted));
          snprintf(e->rack_str, sizeof(e->rack_str), "%s", sorted);
          snprintf(state->edit_rack_buf, sizeof(state->edit_rack_buf), "%s",
                   sorted);
          state->edit_rack_len = (int)strlen(state->edit_rack_buf);
          state->edit_rack_cursor = state->edit_rack_len;
        } else {
          e->rack_str[0] = '\0';
          state->edit_rack_buf[0] = '\0';
          state->edit_rack_len = 0;
          state->edit_rack_cursor = 0;
        }
        e->leave_str[0] = '\0';
        state->edit_move_leave[0] = '\0';
      } else if (state->edit_leave_len > 0) {
        snprintf(e->leave_str, sizeof(e->leave_str), "%s",
                 state->edit_leave_buf);
      } else if (state->edit_move_leave[0] != '\0') {
        snprintf(e->leave_str, sizeof(e->leave_str), "%s",
                 state->edit_move_leave);
      } else {
        e->leave_str[0] = '\0';
      }
      break;
    }
    default:
      break;
    }
  }
  // If the rack buffer is still empty, seed it from the
  // move's inferred letters — alphagrammed per the user's
  // rack_sort so the seed matches what the rack panel
  // shows for the same tiles. Subsequent typing into RACK
  // appends raw to the seeded form (and a focus-leave will
  // re-sort if needed).
  if (state->edit_rack_len == 0 && state->edit_move_inferred_rack[0] != '\0') {
    char sorted_seed[24];
    tui_format_alphagram_for_sort(state->edit_move_inferred_rack, state->ld,
                                  state->rack_sort, sorted_seed,
                                  sizeof(sorted_seed));
    snprintf(state->edit_rack_buf, sizeof(state->edit_rack_buf), "%s",
             sorted_seed);
    state->edit_rack_len = (int)strlen(state->edit_rack_buf);
    state->edit_rack_cursor = state->edit_rack_len;
    // Auto-seed from inferred — the user hasn't taken
    // authorship of the rack content yet, so the rack
    // should continue tracking move edits.
    state->edit_rack_user_modified = false;
    tui_game_state_parse_edit_buf(state);
  }
  state->edit_field = TUI_EDIT_FIELD_RACK;
  pthread_mutex_unlock(&state->mutex);
  return true;
}

// Enter on RACK or LEAVE (Enter on MOVE goes to
// cell_editor_advance_to_rack): plays the turn into the game, marks the
// entry committed, and moves the editor onto the next turn, appending
// a pending entry when this was the last one.
static bool cell_editor_commit(TuiGameState *state, int idx, bool field_move) {
  pthread_mutex_lock(&state->mutex);
  tui_game_state_parse_edit_buf(state);
  if (idx < state->history_count) {
    TuiHistoryEntry *e = &state->history[idx];
    if (field_move) {
      // MOVE commit branches by the parser's kind. The
      // canonical string (already normalized to engine
      // form: "8H WORD" / "ex AB" / "pass") goes into
      // move_str, and the parser's inferred rack fills
      // rack_str when the user hasn't typed one. The
      // score (when the engine returned one) is stamped
      // on the entry so the cell keeps showing it after
      // edit mode exits.
      switch (state->edit_move_kind) {
      case TUI_EDIT_MOVE_KIND_PLACEMENT:
      case TUI_EDIT_MOVE_KIND_EXCHANGE:
      case TUI_EDIT_MOVE_KIND_PASS: {
        // Same "-ABC" display normalization the Tab/Enter-
        // on-MOVE commit path uses. Belt-and-braces — this
        // branch should be unreachable since Enter-on-MOVE
        // is caught above, but if anything ever routes here
        // we don't want stale "ex ABC" leaking onto entries.
        if (strncmp(state->edit_move_canonical, "ex ", 3) == 0) {
          snprintf(e->move_str, sizeof(e->move_str), "-%s",
                   state->edit_move_canonical + 3);
        } else {
          snprintf(e->move_str, sizeof(e->move_str), "%s",
                   state->edit_move_canonical);
        }
        if (e->rack_str[0] == '\0' &&
            state->edit_move_inferred_rack[0] != '\0') {
          snprintf(e->rack_str, sizeof(e->rack_str), "%s",
                   state->edit_move_inferred_rack);
        }
        // Always re-sync the score on commit (see the
        // earlier commit branch's note).
        e->score = state->edit_move_score >= 0 ? state->edit_move_score : 0;
        if (state->edit_leave_len > 0) {
          snprintf(e->leave_str, sizeof(e->leave_str), "%s",
                   state->edit_leave_buf);
        } else if (state->edit_move_leave[0] != '\0') {
          snprintf(e->leave_str, sizeof(e->leave_str), "%s",
                   state->edit_move_leave);
        } else {
          e->leave_str[0] = '\0';
        }
        break;
      }
      case TUI_EDIT_MOVE_KIND_WORD_ONLY:
      case TUI_EDIT_MOVE_KIND_PARTIAL:
      case TUI_EDIT_MOVE_KIND_INVALID:
      case TUI_EDIT_MOVE_KIND_EMPTY:
      default:
        // Nothing to commit yet — bare word triggers the
        // (not-yet-implemented) placement enumerator on
        // focus-away, and partial / invalid buffers wait
        // for the user to keep typing.
        break;
      }
    } else {
      // RACK field commit. Alphagram the buffer in place so
      // re-opening this entry lands on the sorted form
      // rather than the raw typed order. Matches the Tab/Up
      // path's behavior.
      if (state->edit_rack_valid && state->edit_rack_len > 0) {
        char sorted_rack[24];
        char raw[24];
        const int n_raw = state->edit_rack_len < (int)sizeof(raw) - 1
                              ? state->edit_rack_len
                              : (int)sizeof(raw) - 1;
        memcpy(raw, state->edit_rack_buf, (size_t)n_raw);
        raw[n_raw] = '\0';
        tui_format_alphagram_for_sort(raw, state->ld, state->rack_sort,
                                      sorted_rack, sizeof(sorted_rack));
        snprintf(state->edit_rack_buf, sizeof(state->edit_rack_buf), "%s",
                 sorted_rack);
        state->edit_rack_len = (int)strlen(state->edit_rack_buf);
        state->edit_rack_cursor = state->edit_rack_len;
      }
      if (state->edit_rack_valid) {
        int n = state->edit_rack_len;
        if (n >= (int)sizeof(e->rack_str)) {
          n = (int)sizeof(e->rack_str) - 1;
        }
        memcpy(e->rack_str, state->edit_rack_buf, (size_t)n);
        e->rack_str[n] = '\0';
      }
      // Stamp the leave. User-typed buffer (via click+type
      // in LEAVE) overrides the parser's auto-derived value.
      if (state->edit_leave_len > 0) {
        snprintf(e->leave_str, sizeof(e->leave_str), "%s",
                 state->edit_leave_buf);
      } else if (state->edit_move_leave[0] != '\0') {
        snprintf(e->leave_str, sizeof(e->leave_str), "%s",
                 state->edit_move_leave);
      }
    }
  }
  // If the rack commit followed a *legal* play (engine-
  // accepted placement / exchange / pass), advance the
  // annotation to the next turn: play the move on the
  // engine, tag tile owners so placed tiles render in their
  // player's permanent color, append a fresh pending entry
  // for the OTHER player, and re-open the editor on it. If
  // the play wasn't legal (score < 0, partial, bare word,
  // invalid), fall through to the existing "just exit edit
  // mode" behavior — the user keeps editing the same turn.
  if (field_move == false && state->edit_preview_move_valid &&
      state->edit_preview_move != NULL && state->edit_move_score >= 0 &&
      (state->edit_move_kind == TUI_EDIT_MOVE_KIND_PLACEMENT ||
       state->edit_move_kind == TUI_EDIT_MOVE_KIND_EXCHANGE ||
       state->edit_move_kind == TUI_EDIT_MOVE_KIND_PASS)) {
    const int playing_idx =
        (idx < state->history_count) ? state->history[idx].player_idx : 0;
    // play_move_without_drawing_tiles: place the tiles, debit
    // them from the player's rack, update score, advance the
    // turn — but DON'T draw replacements from the bag. The
    // annotator owns the rack content. After this call the
    // player's rack holds only their leave; the next turn's
    // editor sees that leave when sync_player_rack_to_editor
    // checks the live rack, and shows it in the pill.
    play_move_without_drawing_tiles(state->edit_preview_move, state->game);
    // Tag the placed-tile squares with the player_idx so the
    // per-cell pixel plane picks the correct tile_bg color.
    // Mirrors the bot-worker path at bot_worker.c:918-933.
    if (move_get_type(state->edit_preview_move) ==
        GAME_EVENT_TILE_PLACEMENT_MOVE) {
      const int dir = move_get_dir(state->edit_preview_move);
      int r = move_get_row_start(state->edit_preview_move);
      int c = move_get_col_start(state->edit_preview_move);
      const int n_tiles = move_get_tiles_length(state->edit_preview_move);
      for (int t = 0; t < n_tiles; t++) {
        if (move_get_tile(state->edit_preview_move, t) !=
            PLAYED_THROUGH_MARKER) {
          board_set_square_owner(game_get_board(state->game), r, c,
                                 playing_idx);
        }
        if (board_is_dir_vertical(dir)) {
          r++;
        } else {
          c++;
        }
      }
    }
    // Mark the current entry as no longer pending and stamp
    // its cumulative score. play_move_without_drawing_tiles
    // already added the move's points to the engine player's
    // running total, so reading it now gives the correct
    // total_after for this turn (e.g., turn 3's DONKEYS adds
    // 87 to P1's 24 from turn 1 → total_after = 111). The
    // history renderer uses this for the "+87/111" column.
    if (idx < state->history_count) {
      state->history[idx].pending = false;
      state->history[idx].total_after = equity_to_int(
          player_get_score(game_get_player(state->game, playing_idx)));
    }
    // Advance to the next turn. If the committed turn was the
    // LAST entry, append a fresh pending entry for the next
    // player. If it was a MIDDLE turn being re-committed (a
    // later turn already exists), DON'T append — just slide
    // the editor onto the already-existing next turn. Without
    // this guard, re-committing turn 2 would spawn a phantom
    // extra pending turn at the end.
    const int next_player = game_get_player_on_turn_index(state->game);
    if (idx + 1 >= state->history_count) {
      tui_bot_worker_append_pending_history(state, next_player, NULL,
                                            state->time_per_side_seconds);
      state->edit_history_idx = state->history_count - 1;
    } else {
      state->edit_history_idx = idx + 1;
    }
    // Move the history-cursor onto the new pending entry too.
    // pick_render_board / pick_render_rack rewind to a cursor-
    // pinned committed entry's snapshot, so leaving the cursor
    // on turn 1 would keep showing the pre-turn-1 (empty) board
    // and racks even after several commits. Pending entries
    // fall through to the live engine state, which is exactly
    // what the annotator wants to see while editing the next
    // turn.
    state->history_cursor = state->edit_history_idx;
    state->edit_field = TUI_EDIT_FIELD_MOVE;
    state->edit_leave_buf[0] = '\0';
    state->edit_leave_len = 0;
    state->edit_leave_cursor = 0;
    state->edit_rack_carryover[0] = '\0';
    state->edit_rack_user_modified = false;
    {
      const TuiHistoryEntry *dest = &state->history[state->edit_history_idx];
      if (dest->move_str[0] != '\0' || dest->rack_str[0] != '\0') {
        // Advancing onto an EXISTING (already-edited) next
        // turn: load its stored move / rack so re-committing
        // a middle turn doesn't blank the turn after it.
        snprintf(state->edit_move_buf, sizeof(state->edit_move_buf), "%s",
                 dest->move_str);
        state->edit_move_len = (int)strlen(state->edit_move_buf);
        state->edit_move_cursor = state->edit_move_len;
        snprintf(state->edit_rack_buf, sizeof(state->edit_rack_buf), "%s",
                 dest->rack_str);
        state->edit_rack_len = (int)strlen(state->edit_rack_buf);
        state->edit_rack_cursor = state->edit_rack_len;
        state->edit_rack_user_modified = dest->rack_str[0] != '\0';
      } else {
        // Fresh turn: empty buffers, then carry the player's
        // most-recent leave forward as the rack seed. Stored
        // in edit_rack_carryover so sync merges carryover +
        // the move's played tiles into the effective rack as
        // the user types (e.g. "KAM" on a turn carrying
        // "ERST" yields "AEKMRST").
        state->edit_move_buf[0] = '\0';
        state->edit_move_len = 0;
        state->edit_move_cursor = 0;
        state->edit_rack_buf[0] = '\0';
        state->edit_rack_len = 0;
        state->edit_rack_cursor = 0;
        for (int prev = state->edit_history_idx - 1; prev >= 0; prev--) {
          const TuiHistoryEntry *pe = &state->history[prev];
          if (pe->pending || pe->player_idx != next_player) {
            continue;
          }
          if (pe->leave_str[0] != '\0') {
            snprintf(state->edit_rack_carryover,
                     sizeof(state->edit_rack_carryover), "%s", pe->leave_str);
            snprintf(state->edit_rack_buf, sizeof(state->edit_rack_buf), "%s",
                     pe->leave_str);
            state->edit_rack_len = (int)strlen(state->edit_rack_buf);
            state->edit_rack_cursor = state->edit_rack_len;
          }
          break;
        }
      }
    }
    tui_game_state_parse_edit_buf(state);
    // Replay the full committed history forward on a fresh
    // board, surfacing any per-entry validation errors (tile
    // collision, disconnected play, rack missing letters,
    // etc.) so the user sees them inline. Cheap-enough at
    // typical history lengths and only runs on commit, not
    // every keystroke.
    tui_game_state_revalidate_history(state);
    pthread_mutex_unlock(&state->mutex);
    return true;
  }
  // Exit edit mode but keep the buffers around so a
  // re-open lands on whatever the user was typing.
  state->edit_history_idx = -1;
  pthread_mutex_unlock(&state->mutex);
  return true;
}

// Inserts a printable key at the cursor of the active field (MOVE, RACK,
// or LEAVE), applying that field's case and character rules.
static bool cell_editor_type_char(TuiGameState *state, uint32_t key,
                                  const ncinput *input, bool field_move,
                                  char *buf, int *plen, int *pcur,
                                  size_t buf_cap) {
  pthread_mutex_lock(&state->mutex);
  // RACK field caps at 7 tiles regardless of buffer size —
  // a Scrabble rack never holds more than that, and the
  // visible zone in the cell is 7 + 1 (cursor) cells wide.
  // Hitting the cap leaves the cursor parked in the 8th
  // cell as a visual "no room" signal.
  const int per_field_cap = field_move ? (int)buf_cap - 1 : 7;
  if (*plen < per_field_cap && *plen + 1 < (int)buf_cap) {
    // Default: uppercase letters in both fields. Space is
    // meaningful in MOVE (between coord and word) but not
    // in RACK.
    // In the MOVE field, a Shift+letter on a real key
    // (i.e., the terminal delivers an uppercase ASCII
    // letter, not a lowercase one) means "this tile is a
    // played blank designated as that letter". Store the
    // glyph as LOWERCASE so it parses as a blank in the
    // engine's move notation, which is the same convention
    // GCG / CGP / sim outputs use. The parser turns each
    // lowercase letter in the word into a '?' for the
    // inferred rack — so the rack panel correctly shows
    // one blank tile per played blank.
    char ch = (char)key;
    if (!field_move && ch == ' ') {
      pthread_mutex_unlock(&state->mutex);
      return true;
    }
    // MOVE field: a second space is never meaningful (the one
    // space separates coord from word), and autofill may have
    // already supplied it when it absorbed a leading played-
    // through letter right after the coord — swallow dupes so
    // "8H<space>" out of habit can't split the word token.
    if (field_move && ch == ' ' && strchr(buf, ' ') != NULL) {
      pthread_mutex_unlock(&state->mutex);
      return true;
    }
    const bool is_letter = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
    // Coord-token letters (before the first space, e.g. the F
    // in "8F ...") are positions, not tiles — only letters in
    // the word part go through rack resolution.
    const bool typing_word =
        *pcur > 0 && memchr(buf, ' ', (size_t)*pcur) != NULL;
    if (field_move && is_letter && typing_word) {
      // Word letters go through the shared move-entry resolver
      // (rack gating + blank fallback in play-vs-computer,
      // playthrough pass-through with canonical casing in
      // every mode) so the cell and the board surfaces can't
      // drift — that divergence was the TUI's biggest bug
      // farm. With the cursor at the end this is the whole
      // shared keystroke path (append + re-parse + absorb);
      // mid-text edits resolve the glyph here and fall
      // through to the generic insert-at-cursor below.
      if (*pcur == *plen) {
        tui_move_entry_append_letter(state, ch, ncinput_shift_p(input));
        pthread_mutex_unlock(&state->mutex);
        return true;
      }
      int land_row = -1;
      int land_col = -1;
      if (!tui_move_entry_landing_square(state, &land_row, &land_col)) {
        land_row = -1;
        land_col = -1;
      }
      char glyph = '\0';
      if (tui_move_entry_resolve_letter(state, ch, ncinput_shift_p(input),
                                        land_row, land_col,
                                        &glyph) == TUI_TYPED_LETTER_REJECT) {
        pthread_mutex_unlock(&state->mutex);
        return true;
      }
      ch = glyph;
    } else if (field_move && is_letter && ncinput_shift_p(input)) {
      // Shift+letter on MOVE → played blank (lowercase).
      if (ch >= 'A' && ch <= 'Z') {
        ch = (char)(ch - 'A' + 'a');
      }
    } else if (ch >= 'a' && ch <= 'z') {
      // Default: case-fold lowercase up to uppercase.
      ch = (char)(ch - 'a' + 'A');
    }
    memmove(&buf[*pcur + 1], &buf[*pcur], (size_t)(*plen - *pcur + 1));
    buf[*pcur] = ch;
    (*pcur)++;
    (*plen)++;
    if (!field_move) {
      // User typed into the RACK field — the buffer is now
      // theirs; the rack should NOT snap back to whatever
      // the move infers on the next move edit. Also drop the
      // carryover seed so it stops auto-merging played tiles.
      state->edit_rack_user_modified = true;
      state->edit_rack_carryover[0] = '\0';
    }
    tui_game_state_parse_edit_buf(state);
    // MOVE field: auto-extend through any existing tiles the
    // word now runs into, so played-through letters fill in
    // without retyping.
    if (field_move && *pcur == *plen) {
      tui_autofill_playthrough(state);
    }
  }
  pthread_mutex_unlock(&state->mutex);
  return true;
}

// Returns true when the key was consumed (the caller moves on to the next
// input event), false to let the remaining key handling see it.
bool tui_input_cell_editor(TuiGameState *state, uint32_t key, ncinput input) {
  const int idx = state->edit_history_idx;
  const bool field_move = state->edit_field == TUI_EDIT_FIELD_MOVE;
  const bool field_leave = state->edit_field == TUI_EDIT_FIELD_LEAVE;
  char *buf =
      field_move ? state->edit_move_buf
                 : (field_leave ? state->edit_leave_buf : state->edit_rack_buf);
  int *plen = field_move ? &state->edit_move_len
                         : (field_leave ? &state->edit_leave_len
                                        : &state->edit_rack_len);
  int *pcur = field_move ? &state->edit_move_cursor
                         : (field_leave ? &state->edit_leave_cursor
                                        : &state->edit_rack_cursor);
  const size_t buf_cap = field_move
                             ? sizeof(state->edit_move_buf)
                             : (field_leave ? sizeof(state->edit_leave_buf)
                                            : sizeof(state->edit_rack_buf));

  // ── Board move-entry sub-mode ───────────────────────────────
  // When a board anchor is active, keystrokes drive the on-board
  // move builder rather than the history-cell text editor. The
  // builder keeps edit_move_buf in sync so the board ghost, the
  // on-board cursor arrow, and the History cell all track what's
  // being typed. Space toggles direction (keyboard parallel to
  // clicking the anchor); arrows relocate the anchor; Backspace
  // retracts a tile; Enter submits; Esc cancels.
  if (state->board_entry_active) {
    return cell_editor_board_entry_key(state, key, &input);
  }

  if (key == NCKEY_ESC) {
    pthread_mutex_lock(&state->mutex);
    // Blur-commit: save whatever's typed before leaving.
    tui_commit_edit_and_revalidate(state);
    state->edit_history_idx = -1;
    pthread_mutex_unlock(&state->mutex);
    return true;
  }

  // Shift+arrow: navigate between turns (plain arrows cycle
  // within the current turn's editable fields below). Loads the
  // target entry's stored move_str / rack_str into the edit
  // buffers, lands on the MOVE field, and slides the history
  // cursor along so the board / pills snap to the target turn.
  if (ncinput_shift_p(&input) && (key == NCKEY_LEFT || key == NCKEY_RIGHT ||
                                  key == NCKEY_UP || key == NCKEY_DOWN)) {
    return cell_editor_step_turn(state, key);
  }

  // Tab from RACK commits the rack (and the leave derived
  // from it) onto the entry, then hops back to MOVE without
  // exiting edit mode. That way the row-2 rack display goes
  // through the same alphagram-sorted committed-entry path
  // as Enter, so tabbing between fields doesn't leave the
  // cell showing a raw typed-order rack.
  // Up-arrow shares this path — it's the "row above me is
  // the move row, take me there" gesture annotation users
  // reach for instinctively.
  if ((key == NCKEY_TAB || key == '\t' || key == NCKEY_UP) && !field_move) {
    return cell_editor_back_to_move(state, idx);
  }

  // Play-vs-computer: the history cell doubles as a keyboard
  // move-entry surface for the human's live pending turn — type
  // "8D WORD" and press Enter. Enter routes through the same
  // commit as board entry (real bag draw, clock charge, bot
  // handoff). The annotation commit paths below must stay
  // unreachable in this mode: they play WITHOUT drawing, which
  // desyncs the bag and never hands the turn to the bot. Tab /
  // Down field switches from MOVE are swallowed too — the rack
  // row is read-only in play-vs-computer (racks come from the
  // bag). All other keys (typing, Backspace, arrows, Esc) fall
  // through to the normal editor handlers.
  if (state->app_mode == TUI_APP_MODE_PLAY_VS_COMPUTER &&
      ((key == NCKEY_ENTER || key == '\r' || key == '\n') ||
       ((key == NCKEY_TAB || key == '\t' || key == NCKEY_DOWN) &&
        field_move))) {
    if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
      pthread_mutex_lock(&state->mutex);
      tui_pvc_commit_preview_move(state);
      pthread_mutex_unlock(&state->mutex);
    }
    return true;
  }

  // Enter on the MOVE field advances to the RACK field
  // (the user's "natural next step" after typing a move).
  // The move ALSO commits to the entry here — without that,
  // pressing Enter twice (move-Enter then rack-Enter) would
  // leave the move never persisted: the second Enter only
  // commits the rack, and the user would lose the move text
  // along with its score when edit mode exited.
  // Enter on the RACK field is the final commit — it stamps
  // the entry's rack and exits edit mode.
  // Tab from MOVE shares this branch so the two keys feel
  // interchangeable.
  const bool tab_from_move = (key == NCKEY_TAB || key == '\t') && field_move;
  // Down-arrow from MOVE is a Tab alias — the "row below me
  // is the rack row, take me there" gesture.
  const bool down_from_move = key == NCKEY_DOWN && field_move;
  if (((key == NCKEY_ENTER || key == '\r' || key == '\n') || tab_from_move ||
       down_from_move) &&
      field_move) {
    return cell_editor_advance_to_rack(state, idx);
  }

  if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
    return cell_editor_commit(state, idx, field_move);
  }
  // Plain Left/Right inside the editor follow the unified
  // step-through sequence:
  //   [4>] / 1> → 1.MOVE → 1.RACK → 2> → 2.MOVE → 2.RACK → 3> → ...
  // Within a field's text the arrow moves the cursor; at the
  // far edge of the field it advances/retreats one position
  // in the sequence (which may exit the editor onto the next
  // / current label). Shift+arrow stays as label-skip and was
  // handled by the earlier Shift+arrow block.
  if (key == NCKEY_LEFT) {
    pthread_mutex_lock(&state->mutex);
    if (*pcur > 0) {
      (*pcur)--;
    } else if (field_leave) {
      // LEAVE start → RACK end of same turn. (LEAVE itself
      // is not in the forward arrow sequence — it's only
      // reachable via click — but if you arrived there by
      // clicking, Left walks you back to RACK.)
      state->edit_field = TUI_EDIT_FIELD_RACK;
      state->edit_rack_cursor = state->edit_rack_len;
    } else if (!field_move) {
      // RACK start → MOVE end of same turn.
      state->edit_field = TUI_EDIT_FIELD_MOVE;
      state->edit_move_cursor = state->edit_move_len;
    } else {
      // MOVE start → exit editor onto this entry's label.
      // Blur-commit first so the typed move/rack/leave persist.
      const int leaving = state->edit_history_idx;
      tui_commit_edit_and_revalidate(state);
      state->history_cursor = leaving;
      state->edit_history_idx = -1;
    }
    pthread_mutex_unlock(&state->mutex);
    return true;
  }
  if (key == NCKEY_RIGHT) {
    pthread_mutex_lock(&state->mutex);
    if (*pcur < *plen) {
      (*pcur)++;
    } else if (field_move) {
      // MOVE end → RACK start of same turn.
      state->edit_field = TUI_EDIT_FIELD_RACK;
      state->edit_rack_cursor = 0;
    } else {
      // RACK end (or LEAVE end) → forward out of the turn.
      // Blur-commit, then advance: onto the next turn's label
      // if it exists, or — when this is the LAST turn and it
      // holds a valid move — create the next turn and land on
      // its label. Forward-nav past the final turn is how new
      // turns get created (no Enter required).
      const int cur = state->edit_history_idx;
      tui_commit_edit_and_revalidate(state);
      if (cur + 1 < state->history_count) {
        state->history_cursor = cur + 1;
        state->edit_history_idx = -1;
      } else if (cur >= 0 && cur < state->history_count &&
                 state->history[cur].move_str[0] != '\0' &&
                 state->history[cur].error_str[0] == '\0') {
        // After revalidate the engine sits at the post-game
        // position, so the on-turn index is the next player.
        const int next_player = game_get_player_on_turn_index(state->game);
        tui_bot_worker_append_pending_history(state, next_player, NULL,
                                              state->time_per_side_seconds);
        state->history_cursor = state->history_count - 1;
        state->edit_history_idx = -1;
      }
      // else: last turn with no valid move — stay put.
    }
    pthread_mutex_unlock(&state->mutex);
    return true;
  }
  if (key == NCKEY_HOME) {
    pthread_mutex_lock(&state->mutex);
    *pcur = 0;
    pthread_mutex_unlock(&state->mutex);
    return true;
  }
  if (key == NCKEY_END) {
    pthread_mutex_lock(&state->mutex);
    *pcur = *plen;
    pthread_mutex_unlock(&state->mutex);
    return true;
  }
  if (key == NCKEY_BACKSPACE || key == 0x7f || key == 0x08) {
    pthread_mutex_lock(&state->mutex);
    if (*pcur > 0) {
      memmove(&buf[*pcur - 1], &buf[*pcur], (size_t)(*plen - *pcur + 1));
      (*pcur)--;
      (*plen)--;
      if (!field_move) {
        state->edit_rack_user_modified = true;
        state->edit_rack_carryover[0] = '\0';
      }
      tui_game_state_parse_edit_buf(state);
    }
    pthread_mutex_unlock(&state->mutex);
    return true;
  }
  if (key == NCKEY_DEL) {
    pthread_mutex_lock(&state->mutex);
    if (*pcur < *plen) {
      memmove(&buf[*pcur], &buf[*pcur + 1], (size_t)(*plen - *pcur));
      (*plen)--;
      if (!field_move) {
        state->edit_rack_user_modified = true;
        state->edit_rack_carryover[0] = '\0';
      }
      tui_game_state_parse_edit_buf(state);
    }
    pthread_mutex_unlock(&state->mutex);
    return true;
  }
  if (key >= 0x20 && key < 0x7f) {
    return cell_editor_type_char(state, key, &input, field_move, buf, plen,
                                 pcur, buf_cap);
  }
  (void)idx;
  (void)buf;
  return true;
  return false;
}
