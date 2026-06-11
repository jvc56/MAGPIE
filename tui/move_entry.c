#include "move_entry.h"

#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/impl/gameplay.h"
#include "../src/str/rack_string.h"
#include "../src/util/string_util.h"
#include "bot_worker.h"
#include "game_state.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Machine letter for a single uppercase A-Z letter, or 0 if not found.
static MachineLetter tui_ml_for_upper(const TuiGameState *gs, char up) {
  if (gs->ld == NULL) {
    return 0;
  }
  for (MachineLetter ml = 1; ml < MACHINE_LETTER_MAX_VALUE; ml++) {
    const char *hl = gs->ld->ld_ml_to_hl[ml];
    if (hl != NULL && hl[0] == up && hl[1] == '\0') {
      return ml;
    }
  }
  return 0;
}

// Resolve a typed letter against the human's live rack in
// play-vs-computer: real tile, blank, or rejected. The rack is finite
// and known — only allow a tile the rack still has AFTER accounting for
// tiles the in-progress move already placed (so you can't type a second
// E when you hold one E; the played-through E on the board is not yours
// to retype). Unshifted letters fall back to a blank when the real tile
// is exhausted but a blank remains; Shift+letter explicitly requests a
// blank. Returns false to reject the keystroke (neither available).
static bool tui_pvc_resolve_typed_letter(const TuiGameState *gs, char up,
                                         bool shift, bool *out_blank) {
  const MachineLetter ml = tui_ml_for_upper(gs, up);
  const Rack *rack =
      gs->game != NULL
          ? player_get_rack(game_get_player(gs->game, gs->human_player_idx))
          : NULL;
  int placed_real = 0;
  int placed_blank = 0;
  for (const char *p = gs->edit_move_inferred_rack; *p != '\0'; p++) {
    if (*p == up) {
      placed_real++;
    } else if (*p == '?') {
      placed_blank++;
    }
  }
  const int avail_real = (rack != NULL && ml != 0)
                             ? (int)rack_get_letter(rack, ml) - placed_real
                             : 0;
  const int avail_blank =
      rack != NULL
          ? (int)rack_get_letter(rack, BLANK_MACHINE_LETTER) - placed_blank
          : 0;
  if (shift) {
    if (avail_blank <= 0) {
      return false; // explicit blank requested but none left
    }
    *out_blank = true;
  } else if (avail_real > 0) {
    *out_blank = false;
  } else if (avail_blank > 0) {
    *out_blank = true; // out of the real tile — use a blank
  } else {
    return false; // no real tile and no blank — reject the keystroke
  }
  return true;
}

TuiTypedLetterAction tui_move_entry_resolve_letter(const TuiGameState *gs,
                                                   char typed, bool shift,
                                                   int land_row, int land_col,
                                                   char *out_glyph) {
  if (!((typed >= 'a' && typed <= 'z') || (typed >= 'A' && typed <= 'Z'))) {
    return TUI_TYPED_LETTER_REJECT;
  }
  const char up =
      (typed >= 'a' && typed <= 'z') ? (char)(typed - 'a' + 'A') : typed;
  const bool pvc = gs->app_mode == TUI_APP_MODE_PLAY_VS_COMPUTER;
  // Occupied landing square: a matching letter is playthrough spelling
  // (take the BOARD tile's notation case — lowercase when it's a
  // designated blank — so the buffer stays canonical); a mismatched
  // letter is a collision, rejected where racks are real. Annotation
  // stays free-text on a mismatch: mid-edit states collide legitimately
  // while the user is still reshaping the move.
  if (gs->game != NULL && land_row >= 0 && land_row < BOARD_DIM &&
      land_col >= 0 && land_col < BOARD_DIM) {
    const Board *brd = game_get_board(gs->game);
    const MachineLetter board_ml =
        brd != NULL ? board_get_letter(brd, land_row, land_col)
                    : ALPHABET_EMPTY_SQUARE_MARKER;
    if (board_ml != ALPHABET_EMPTY_SQUARE_MARKER) {
      if (get_unblanked_machine_letter(board_ml) == tui_ml_for_upper(gs, up)) {
        const char *hl = gs->ld != NULL ? gs->ld->ld_ml_to_hl[board_ml] : NULL;
        *out_glyph = (hl != NULL && hl[0] != '\0') ? hl[0] : up;
        return TUI_TYPED_LETTER_PLAYTHROUGH;
      }
      if (pvc) {
        return TUI_TYPED_LETTER_REJECT;
      }
    }
  }
  if (pvc) {
    bool blank = false;
    if (!tui_pvc_resolve_typed_letter(gs, up, shift, &blank)) {
      return TUI_TYPED_LETTER_REJECT;
    }
    *out_glyph = blank ? (char)(up - 'A' + 'a') : up;
    return TUI_TYPED_LETTER_PLACE;
  }
  *out_glyph = shift ? (char)(up - 'A' + 'a') : up;
  return TUI_TYPED_LETTER_PLACE;
}

bool tui_move_entry_landing_square(const TuiGameState *gs, int *out_row,
                                   int *out_col) {
  const char *buf = gs->edit_move_buf;
  const char *space = strchr(buf, ' ');
  if (space == NULL) {
    return false;
  }
  // Coord token: digits-then-letter = horizontal; letter-then-digits =
  // vertical. Same convention as parse_coord_token / the CGP notation.
  int row = -1;
  int col = -1;
  bool vertical = false;
  const char c0 = buf[0];
  if (c0 >= '0' && c0 <= '9') {
    int digits = 0;
    const char *p = buf;
    while (*p >= '0' && *p <= '9') {
      digits = digits * 10 + (*p - '0');
      p++;
    }
    if (!((*p >= 'A' && *p <= 'O') || (*p >= 'a' && *p <= 'o'))) {
      return false;
    }
    row = digits - 1;
    col = (*p >= 'a') ? *p - 'a' : *p - 'A';
    vertical = false;
  } else if ((c0 >= 'A' && c0 <= 'O') || (c0 >= 'a' && c0 <= 'o')) {
    col = (c0 >= 'a') ? c0 - 'a' : c0 - 'A';
    int digits = 0;
    const char *p = buf + 1;
    while (*p >= '0' && *p <= '9') {
      digits = digits * 10 + (*p - '0');
      p++;
    }
    if (digits == 0) {
      return false;
    }
    row = digits - 1;
    vertical = true;
  } else {
    return false;
  }
  if (row < 0 || row >= BOARD_DIM || col < 0 || col >= BOARD_DIM) {
    return false;
  }
  // Count word letters before the cursor.
  int word_letters = 0;
  const int word_start = (int)(space - buf) + 1;
  for (int i = word_start; i < gs->edit_move_cursor && buf[i] != '\0'; i++) {
    const char wc = buf[i];
    if ((wc >= 'a' && wc <= 'z') || (wc >= 'A' && wc <= 'Z')) {
      word_letters++;
    }
  }
  *out_row = vertical ? row + word_letters : row;
  *out_col = vertical ? col : col + word_letters;
  return *out_row < BOARD_DIM && *out_col < BOARD_DIM;
}

void tui_autofill_playthrough(TuiGameState *gs) {
  if (!gs->edit_preview_move_valid || gs->edit_preview_move == NULL ||
      gs->game == NULL || gs->ld == NULL) {
    return;
  }
  const Move *pm = gs->edit_preview_move;
  if (move_get_type(pm) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return;
  }
  const Board *brd = game_get_board(gs->game);
  if (brd == NULL) {
    return;
  }
  const bool vertical = board_is_dir_vertical(move_get_dir(pm));
  const int span = move_get_tiles_length(pm);
  int next_r =
      vertical ? move_get_row_start(pm) + span : move_get_row_start(pm);
  int next_c =
      vertical ? move_get_col_start(pm) : move_get_col_start(pm) + span;
  bool changed = false;
  while (next_r >= 0 && next_r < BOARD_DIM && next_c >= 0 &&
         next_c < BOARD_DIM) {
    const MachineLetter ml = board_get_letter(brd, next_r, next_c);
    if (ml == ALPHABET_EMPTY_SQUARE_MARKER) {
      break;
    }
    // ld_ml_to_hl is already lowercase for blanked tiles, uppercase
    // otherwise — exactly the move-notation convention.
    const char *hl = gs->ld->ld_ml_to_hl[ml];
    if (hl == NULL || hl[0] == '\0') {
      break;
    }
    // The very first absorbed letter may arrive while the buffer is
    // still just the coord token (typing "13A" with a tile sitting ON
    // A13). Without the coord/word space the letter glues onto the
    // coord — "13A" became "13AV", which then poisoned every
    // subsequent parse of the move.
    const bool need_space = strchr(gs->edit_move_buf, ' ') == NULL;
    const int hlen = (int)strlen(hl);
    if (gs->edit_move_len + hlen + (need_space ? 1 : 0) >=
        (int)sizeof(gs->edit_move_buf)) {
      break;
    }
    if (need_space) {
      gs->edit_move_buf[gs->edit_move_len++] = ' ';
    }
    memcpy(gs->edit_move_buf + gs->edit_move_len, hl, (size_t)hlen);
    gs->edit_move_len += hlen;
    gs->edit_move_buf[gs->edit_move_len] = '\0';
    gs->edit_move_cursor = gs->edit_move_len;
    changed = true;
    if (vertical) {
      next_r++;
    } else {
      next_c++;
    }
  }
  if (changed) {
    tui_game_state_parse_edit_buf(gs);
  }
}

bool tui_move_entry_append_letter(TuiGameState *gs, char ch, bool shift) {
  int land_row = -1;
  int land_col = -1;
  // Pre-space (coord-only) buffers have no landing square yet; the
  // resolver then applies pure rack/mode policy, exactly the old
  // board-entry behavior (the anchor cell is empty by construction,
  // and leading playthrough was absorbed at anchor time).
  if (!tui_move_entry_landing_square(gs, &land_row, &land_col)) {
    land_row = -1;
    land_col = -1;
  }
  char glyph = '\0';
  if (tui_move_entry_resolve_letter(gs, ch, shift, land_row, land_col,
                                    &glyph) == TUI_TYPED_LETTER_REJECT) {
    return false;
  }
  // Insert a space before the first word character (buffer is just the
  // coord token after anchoring).
  const bool need_space = strchr(gs->edit_move_buf, ' ') == NULL;
  const int extra = need_space ? 2 : 1;
  if (gs->edit_move_len + extra >= (int)sizeof(gs->edit_move_buf)) {
    return false;
  }
  if (need_space) {
    gs->edit_move_buf[gs->edit_move_len++] = ' ';
  }
  gs->edit_move_buf[gs->edit_move_len++] = glyph;
  gs->edit_move_buf[gs->edit_move_len] = '\0';
  gs->edit_move_cursor = gs->edit_move_len;
  tui_game_state_parse_edit_buf(gs);
  tui_autofill_playthrough(gs);
  return true;
}

// ── Board-entry builder state ────────────────────────────────────────

// Format the coordinate token for the current anchor + direction. Across
// is "<row+1><col-letter>" (e.g. "8H"); down is "<col-letter><row+1>"
// (e.g. "H8") — the engine infers direction from the token order.
static void tui_board_builder_coord_token(const TuiGameState *gs, char *out,
                                          size_t out_cap) {
  const int row1 = gs->board_anchor_row + 1;
  const char col_letter = (char)('A' + gs->board_anchor_col);
  if (board_is_dir_vertical(gs->board_dir)) {
    snprintf(out, out_cap, "%c%d", col_letter, row1);
  } else {
    snprintf(out, out_cap, "%d%c", row1, col_letter);
  }
}

// Copy the word token (everything after the first space) of edit_move_buf
// into `out`. Empty string when no word has been typed yet.
static void tui_board_builder_extract_word(const TuiGameState *gs, char *out,
                                           size_t out_cap) {
  out[0] = '\0';
  const char *space = strchr(gs->edit_move_buf, ' ');
  if (space != NULL && space[1] != '\0') {
    snprintf(out, out_cap, "%s", space + 1);
  }
}

// A fresh anchor always starts ACROSS — matching Woogles and every
// mainstream Scrabble UI, and predictability beats cleverness here: an
// earlier "whichever direction has the longer empty run" heuristic
// meant the same click could anchor differently depending on nearby
// tiles, which read as random. Down is one toggle away (click the
// cell again, or Space/Tab).
int tui_board_builder_default_dir(const TuiGameState *gs, int row, int col) {
  (void)gs;
  (void)row;
  (void)col;
  return BOARD_HORIZONTAL_DIRECTION;
}

void tui_board_builder_set_anchor(TuiGameState *gs, int row, int col, int dir) {
  // The clicked cell is the ORIGIN — remembered so direction toggles
  // and arrow moves can re-derive everything from the user's cell. The
  // walked-back anchor below is a derived value.
  gs->board_origin_row = row;
  gs->board_origin_col = col;
  // Absorb any contiguous on-board tiles immediately BEFORE the clicked
  // cell (in the play direction) so a word that plays through them is
  // anchored at the true word start. e.g. clicking the empty cell right
  // after an existing "O" and typing "WNER" yields "OWNER" anchored at
  // the O's square, not the invalid "WNER" anchored after it.
  const Board *brd = gs->game != NULL ? game_get_board(gs->game) : NULL;
  const bool vertical = board_is_dir_vertical(dir);
  int anchor_row = row;
  int anchor_col = col;
  while (brd != NULL) {
    const int pr = vertical ? anchor_row - 1 : anchor_row;
    const int pc = vertical ? anchor_col : anchor_col - 1;
    if (pr < 0 || pc < 0 ||
        board_get_letter(brd, pr, pc) == ALPHABET_EMPTY_SQUARE_MARKER) {
      break;
    }
    anchor_row = pr;
    anchor_col = pc;
  }
  gs->board_anchor_row = anchor_row;
  gs->board_anchor_col = anchor_col;
  gs->board_dir = dir;
  gs->board_entry_active = true;
  // The pending entry (last in history) is the turn being entered.
  if (gs->history_count > 0) {
    gs->edit_history_idx = gs->history_count - 1;
  }
  gs->edit_field = TUI_EDIT_FIELD_MOVE;
  gs->edit_rack_user_modified = false;
  gs->edit_leave_buf[0] = '\0';
  gs->edit_leave_len = 0;
  gs->edit_leave_cursor = 0;
  // Seed the word with the leading playthrough letters (from the true
  // anchor up to — but excluding — the clicked empty cell) so the typing
  // cursor starts on the clicked cell.
  char coord[8];
  tui_board_builder_coord_token(gs, coord, sizeof(coord));
  char leading[48];
  int li = 0;
  {
    int r = anchor_row;
    int c = anchor_col;
    while (!(r == row && c == col) && brd != NULL &&
           li < (int)sizeof(leading) - 4) {
      const MachineLetter ml = board_get_letter(brd, r, c);
      const char *hl = gs->ld != NULL ? gs->ld->ld_ml_to_hl[ml] : NULL;
      for (int k = 0;
           hl != NULL && hl[k] != '\0' && li < (int)sizeof(leading) - 1; k++) {
        leading[li++] = hl[k];
      }
      if (vertical) {
        r++;
      } else {
        c++;
      }
    }
  }
  leading[li] = '\0';
  if (li > 0) {
    snprintf(gs->edit_move_buf, sizeof(gs->edit_move_buf), "%s %s", coord,
             leading);
  } else {
    snprintf(gs->edit_move_buf, sizeof(gs->edit_move_buf), "%s", coord);
  }
  gs->edit_move_len = (int)strlen(gs->edit_move_buf);
  gs->edit_move_cursor = gs->edit_move_len;
  tui_game_state_parse_edit_buf(gs);
  tui_autofill_playthrough(gs);
}

void tui_board_builder_toggle_dir(TuiGameState *gs) {
  const Board *brd = gs->game != NULL ? game_get_board(gs->game) : NULL;
  char word[64];
  tui_board_builder_extract_word(gs, word, sizeof(word));
  // Extract the user's placed tiles: skip word letters sitting on
  // occupied squares along the OLD direction (those are absorbed
  // playthrough, meaningless in the new direction — carrying them over
  // was the "direction is stuck horizontal after playthrough" bug).
  char user_tiles[64];
  int n_user = 0;
  const bool old_vertical = board_is_dir_vertical(gs->board_dir);
  int r = gs->board_anchor_row;
  int c = gs->board_anchor_col;
  for (int i = 0; word[i] != '\0' && n_user < (int)sizeof(user_tiles) - 1;) {
    const bool on_board =
        brd != NULL && r >= 0 && r < BOARD_DIM && c >= 0 && c < BOARD_DIM &&
        board_get_letter(brd, r, c) != ALPHABET_EMPTY_SQUARE_MARKER;
    if (on_board) {
      // Played-through square: its human-readable letter may span
      // multiple word chars — skip them all.
      const MachineLetter ml = board_get_letter(brd, r, c);
      const char *hl = gs->ld != NULL ? gs->ld->ld_ml_to_hl[ml] : NULL;
      int skip = hl != NULL ? (int)strlen(hl) : 1;
      while (skip-- > 0 && word[i] != '\0') {
        i++;
      }
    } else {
      user_tiles[n_user++] = word[i++];
    }
    if (old_vertical) {
      r++;
    } else {
      c++;
    }
  }
  user_tiles[n_user] = '\0';
  const int new_dir =
      old_vertical ? BOARD_HORIZONTAL_DIRECTION : BOARD_VERTICAL_DIRECTION;
  tui_board_builder_set_anchor(gs, gs->board_origin_row, gs->board_origin_col,
                               new_dir);
  // Retype the user's tiles so they re-place along the new direction
  // (lowercase in the buffer = blank, retyped as Shift+letter).
  for (int i = 0; i < n_user; i++) {
    const char ch = user_tiles[i];
    const bool was_blank = ch >= 'a' && ch <= 'z';
    const char up = was_blank ? (char)(ch - 'a' + 'A') : ch;
    tui_move_entry_append_letter(gs, up, was_blank);
  }
}

void tui_board_builder_cancel(TuiGameState *gs) {
  gs->board_entry_active = false;
  gs->edit_move_buf[0] = '\0';
  gs->edit_move_len = 0;
  gs->edit_move_cursor = 0;
  gs->edit_history_idx = -1;
  tui_game_state_parse_edit_buf(gs);
}

void tui_board_entry_begin_keyboard(TuiGameState *gs) {
  if (gs->app_mode == TUI_APP_MODE_WATCH || gs->game == NULL ||
      game_over(gs->game)) {
    return;
  }
  const Board *brd = game_get_board(gs->game);
  if (brd == NULL) {
    return;
  }
  // Anchor preference: the board center when it's open (the opening
  // play), else the last origin used this game when still empty, else
  // the first empty cell scanning row-major. Center-first means a
  // fresh game starts at H8 instead of wherever origin's zero-init
  // points (A1).
  int row = BOARD_DIM / 2;
  int col = BOARD_DIM / 2;
  if (board_get_letter(brd, row, col) != ALPHABET_EMPTY_SQUARE_MARKER) {
    row = gs->board_origin_row;
    col = gs->board_origin_col;
    const bool origin_ok =
        row >= 0 && row < BOARD_DIM && col >= 0 && col < BOARD_DIM &&
        board_get_letter(brd, row, col) == ALPHABET_EMPTY_SQUARE_MARKER;
    if (!origin_ok) {
      row = -1;
      for (int r = 0; r < BOARD_DIM && row < 0; r++) {
        for (int c = 0; c < BOARD_DIM; c++) {
          if (board_get_letter(brd, r, c) == ALPHABET_EMPTY_SQUARE_MARKER) {
            row = r;
            col = c;
            break;
          }
        }
      }
      if (row < 0) {
        return; // full board — nothing to anchor on
      }
    }
  }
  tui_board_builder_set_anchor(gs, row, col,
                               tui_board_builder_default_dir(gs, row, col));
}

void tui_board_entry_backspace(TuiGameState *gs) {
  const char *space = strchr(gs->edit_move_buf, ' ');
  const int word_off =
      space != NULL ? (int)(space - gs->edit_move_buf) + 1 : -1;
  int word_len = (word_off >= 0) ? gs->edit_move_len - word_off : 0;
  if (word_len > 0) {
    const Board *brd = game_get_board(gs->game);
    const bool vertical = board_is_dir_vertical(gs->board_dir);
    // Each word char maps to cell anchor + index along the direction.
    // Pop trailing played-through cells (occupied on the board), then
    // pop one placed tile.
    while (word_len > 0) {
      const int r = gs->board_anchor_row + (vertical ? (word_len - 1) : 0);
      const int c = gs->board_anchor_col + (vertical ? 0 : (word_len - 1));
      const bool occupied =
          brd != NULL && r >= 0 && r < BOARD_DIM && c >= 0 && c < BOARD_DIM &&
          board_get_letter(brd, r, c) != ALPHABET_EMPTY_SQUARE_MARKER;
      word_len--; // drop this char
      if (!occupied) {
        break; // it was a placed tile — stop here
      }
    }
    if (word_len <= 0) {
      gs->edit_move_buf[word_off - 1] = '\0'; // drop the space too
      gs->edit_move_len = word_off - 1;
    } else {
      gs->edit_move_buf[word_off + word_len] = '\0';
      gs->edit_move_len = word_off + word_len;
    }
    gs->edit_move_cursor = gs->edit_move_len;
    tui_game_state_parse_edit_buf(gs);
    tui_autofill_playthrough(gs);
  } else {
    // Nothing placed: step the ORIGIN back one cell (the walked-back
    // anchor re-derives; stepping the anchor itself got pinned against
    // leading playthrough).
    int nr = gs->board_origin_row;
    int nc = gs->board_origin_col;
    if (board_is_dir_vertical(gs->board_dir)) {
      if (nr > 0) {
        nr--;
      }
    } else if (nc > 0) {
      nc--;
    }
    tui_board_builder_set_anchor(gs, nr, nc, gs->board_dir);
  }
}

// ── Commits ──────────────────────────────────────────────────────────

void tui_tag_move_owners(Board *board, const Move *move, int player_idx) {
  if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return;
  }
  const int dir = move_get_dir(move);
  int r = move_get_row_start(move);
  int c = move_get_col_start(move);
  const int n = move_get_tiles_length(move);
  for (int t = 0; t < n; t++) {
    if (move_get_tile(move, t) != PLAYED_THROUGH_MARKER) {
      board_set_square_owner(board, r, c, player_idx);
    }
    if (board_is_dir_vertical(dir)) {
      r++;
    } else {
      c++;
    }
  }
}

bool tui_pvc_commit_preview_move(TuiGameState *gs) {
  if (gs->app_mode != TUI_APP_MODE_PLAY_VS_COMPUTER || gs->game == NULL ||
      game_over(gs->game)) {
    return false;
  }
  tui_game_state_parse_edit_buf(gs);
  if (!gs->edit_preview_move_valid || gs->edit_preview_move == NULL ||
      gs->edit_move_score < 0 ||
      gs->edit_move_kind != TUI_EDIT_MOVE_KIND_PLACEMENT) {
    return false; // not a legal placement yet — keep editing
  }
  const int idx = gs->edit_history_idx;
  if (idx < 0 || idx >= gs->history_count) {
    return false;
  }
  TuiHistoryEntry *e = &gs->history[idx];
  const int player_idx = e->player_idx;
  // Only the human's own live pending turn is committable. Without the
  // player/on-turn guards, opening the COMPUTER's pending entry (it exists
  // while the bot is thinking) and pressing Enter would play a move as the
  // computer and race the bot thread's own commit.
  if (!e->pending || player_idx != gs->human_player_idx ||
      game_get_player_on_turn_index(gs->game) != gs->human_player_idx) {
    return false;
  }

  Rack leave;
  rack_set_dist_size(&leave, ld_get_size(gs->ld));
  play_move(gs->edit_preview_move, gs->game, &leave);
  tui_tag_move_owners(game_get_board(gs->game), gs->edit_preview_move,
                      player_idx);
  snprintf(e->move_str, sizeof(e->move_str), "%s", gs->edit_move_canonical);
  e->score = gs->edit_move_score;
  {
    StringBuilder *sb = string_builder_create();
    string_builder_add_rack(sb, &leave, gs->ld, false);
    char *dump = string_builder_dump(sb, NULL);
    snprintf(e->leave_str, sizeof(e->leave_str), "%s", dump);
    free(dump);
    string_builder_destroy(sb);
  }
  const int post =
      equity_to_int(player_get_score(game_get_player(gs->game, player_idx)));
  int bonus = 0;
  if (game_over(gs->game)) {
    const Rack *opp =
        player_get_rack(game_get_player(gs->game, 1 - player_idx));
    if (opp != NULL && !rack_is_empty(opp)) {
      bonus = equity_to_int(calculate_end_rack_points(opp, gs->ld));
      e->end_bonus = bonus;
      StringBuilder *sb = string_builder_create();
      string_builder_add_rack(sb, opp, gs->ld, false);
      char *dump = string_builder_dump(sb, NULL);
      snprintf(e->end_rack_str, sizeof(e->end_rack_str), "%s", dump);
      free(dump);
      string_builder_destroy(sb);
    }
  }
  e->total_after = post - bonus;
  e->pending = false;
  // Charge wall time to the human, clamping clock skew to 0.
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  double elapsed = (double)(now.tv_sec - gs->turn_started.tv_sec) +
                   (double)(now.tv_nsec - gs->turn_started.tv_nsec) / 1e9;
  if (elapsed < 0.0) {
    elapsed = 0.0;
  }
  gs->seconds_used[player_idx] += elapsed;
  gs->turn_started = now;
  e->clock_at_end =
      gs->time_per_side_seconds - (int)gs->seconds_used[player_idx];
  gs->board_entry_active = false;
  gs->edit_history_idx = -1;
  gs->edit_move_buf[0] = '\0';
  gs->edit_move_len = 0;
  gs->edit_move_cursor = 0;
  if (game_over(gs->game) && gs->history_count > 0) {
    gs->history_cursor = gs->history_count - 1;
    gs->analysis_cursor = 0;
  } else {
    // Keep following the live game so the next (computer) turn shows.
    gs->history_cursor = -1;
  }
  tui_game_state_parse_edit_buf(gs);
  atomic_fetch_add(&gs->render_version, 1);
  return true;
}

void tui_board_entry_submit(TuiGameState *gs) {
  if (!gs->board_entry_active) {
    return;
  }
  if (gs->app_mode == TUI_APP_MODE_PLAY_VS_COMPUTER) {
    tui_pvc_commit_preview_move(gs);
    return;
  }
  tui_game_state_parse_edit_buf(gs);
  if (!gs->edit_preview_move_valid || gs->edit_preview_move == NULL ||
      gs->edit_move_score < 0 ||
      gs->edit_move_kind != TUI_EDIT_MOVE_KIND_PLACEMENT) {
    return; // not a legal placement yet — keep editing
  }
  const int idx = gs->edit_history_idx;
  if (idx < 0 || idx >= gs->history_count) {
    return;
  }
  const int player_idx = gs->history[idx].player_idx;
  TuiHistoryEntry *e = &gs->history[idx];

  // Annotation: place tiles without drawing; the annotator owns racks.
  // Mirrors the history-cell RACK-Enter commit (no revalidate — the
  // incremental engine state advanced by play_move_without_drawing_tiles
  // is authoritative for a forward move).
  play_move_without_drawing_tiles(gs->edit_preview_move, gs->game);
  tui_tag_move_owners(game_get_board(gs->game), gs->edit_preview_move,
                      player_idx);
  snprintf(e->move_str, sizeof(e->move_str), "%s", gs->edit_move_canonical);
  e->score = gs->edit_move_score;
  // Seed the rack from the move's played tiles when the annotator
  // hasn't typed a fuller rack — matches the cell editor's behavior.
  if (e->rack_str[0] == '\0' && gs->edit_move_inferred_rack[0] != '\0') {
    snprintf(e->rack_str, sizeof(e->rack_str), "%s",
             gs->edit_move_inferred_rack);
  }
  if (gs->edit_move_leave[0] != '\0') {
    snprintf(e->leave_str, sizeof(e->leave_str), "%s", gs->edit_move_leave);
  }
  e->pending = false;
  e->total_after =
      equity_to_int(player_get_score(game_get_player(gs->game, player_idx)));
  const int next_player = game_get_player_on_turn_index(gs->game);
  if (idx + 1 >= gs->history_count) {
    tui_bot_worker_append_pending_history(gs, next_player, NULL,
                                          gs->time_per_side_seconds);
  }
  gs->board_entry_active = false;
  gs->edit_history_idx = -1;
  gs->edit_move_buf[0] = '\0';
  gs->edit_move_len = 0;
  gs->edit_move_cursor = 0;
  gs->history_cursor = gs->history_count - 1;
  tui_game_state_parse_edit_buf(gs);
  atomic_fetch_add(&gs->render_version, 1);
}
