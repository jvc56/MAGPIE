#include "move_entry.h"

#include "../src/def/board_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/words.h"
#include "../src/impl/gameplay.h"
#include "../src/str/letter_distribution_string.h"
#include "../src/str/move_string.h"
#include "../src/str/rack_string.h"
#include "../src/util/string_util.h"
#include "bot_worker.h"
#include "config.h"
#include "game_state.h"
#include "tile_input.h"
#include <dirent.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Resolve a typed letter against the human's live rack in
// play-vs-computer: real tile, blank, or rejected. The rack is finite
// and known — only allow a tile the rack still has AFTER accounting for
// tiles the in-progress move already placed (so you can't type a second
// E when you hold one E; the played-through E on the board is not yours
// to retype). Unshifted letters fall back to a blank when the real tile
// is exhausted but a blank remains; Shift+letter explicitly requests a
// blank. Returns false to reject the keystroke (neither available).
static bool tui_pvc_resolve_typed_letter(const TuiGameState *gs, int ml,
                                         bool shift, bool *out_blank) {
  const Rack *rack =
      gs->game != NULL
          ? player_get_rack(game_get_player(gs->game, gs->human_player_idx))
          : NULL;
  // Tiles the move already uses, from its inferred rack ("A[QU]?E").
  int placed_real = 0;
  int placed_blank = 0;
  for (const char *pos = gs->edit_move_inferred_rack; *pos != '\0';) {
    const int len = tui_tile_token_len(pos);
    char face[TUI_TILE_TEXT_MAX];
    tui_tile_token_face(pos, len, face, sizeof(face));
    if (strcmp(face, "?") == 0) {
      placed_blank++;
    } else if (tui_tile_for_text(gs->ld, face) == ml) {
      placed_real++;
    }
    pos += len;
  }
  const int avail_real =
      (rack != NULL && ml > 0)
          ? (int)rack_get_letter(rack, (MachineLetter)ml) - placed_real
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

TuiTypedLetterAction tui_move_entry_resolve_tile(const TuiGameState *gs, int ml,
                                                 bool shift, int land_row,
                                                 int land_col, char *out_token,
                                                 size_t token_size) {
  if (gs->ld == NULL || ml <= 0) {
    return TUI_TYPED_LETTER_REJECT;
  }
  const bool pvc = gs->app_mode == TUI_APP_MODE_PLAY_VS_COMPUTER;
  // Occupied landing square: a matching tile is playthrough spelling
  // (take the BOARD tile's notation case — lowercase when it's a
  // designated blank — so the buffer stays canonical); a mismatched
  // tile is a collision, rejected where racks are real. Annotation
  // stays free-text on a mismatch: mid-edit states collide legitimately
  // while the user is still reshaping the move.
  if (gs->game != NULL && land_row >= 0 && land_row < BOARD_DIM &&
      land_col >= 0 && land_col < BOARD_DIM) {
    const Board *brd = game_get_board(gs->game);
    const MachineLetter board_ml =
        brd != NULL ? board_get_letter(brd, land_row, land_col)
                    : ALPHABET_EMPTY_SQUARE_MARKER;
    if (board_ml != ALPHABET_EMPTY_SQUARE_MARKER) {
      if (get_unblanked_machine_letter(board_ml) == (MachineLetter)ml) {
        char *token = ld_ml_to_hl(gs->ld, board_ml);
        (void)snprintf(out_token, token_size, "%s", token);
        free(token);
        return TUI_TYPED_LETTER_PLAYTHROUGH;
      }
      if (pvc) {
        return TUI_TYPED_LETTER_REJECT;
      }
    }
  }
  bool blank = shift;
  if (pvc && !tui_pvc_resolve_typed_letter(gs, ml, shift, &blank)) {
    return TUI_TYPED_LETTER_REJECT;
  }
  tui_tile_token(gs->ld, ml, blank, out_token, token_size);
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
  // Count word tiles before the cursor ("[QU]" is one).
  const int word_start = (int)(space - buf) + 1;
  const int word_letters =
      gs->edit_move_cursor > word_start
          ? tui_tile_count(buf + word_start, gs->edit_move_cursor - word_start)
          : 0;
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
    // ld_ml_to_hl gives the move notation: lowercase for a blanked
    // tile, a multi-letter tile in brackets.
    char *hl = ld_ml_to_hl(gs->ld, ml);
    if (hl == NULL || hl[0] == '\0') {
      free(hl);
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
      free(hl);
      break;
    }
    if (need_space) {
      gs->edit_move_buf[gs->edit_move_len++] = ' ';
    }
    memcpy(gs->edit_move_buf + gs->edit_move_len, hl, (size_t)hlen);
    free(hl);
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

// The byte offset of the last tile token in the MOVE buffer's word, or
// -1 when the word is empty.
static int last_word_token(const TuiGameState *gs) {
  const char *space = strchr(gs->edit_move_buf, ' ');
  if (space == NULL) {
    return -1;
  }
  int last = -1;
  for (int pos = (int)(space - gs->edit_move_buf) + 1;
       pos < gs->edit_move_len;) {
    last = pos;
    pos += tui_tile_token_len(gs->edit_move_buf + pos);
  }
  return last;
}

// Adds a resolved tile for machine letter `ml` at the end of the MOVE
// buffer (with the coord/word space when needed). Returns false when it
// can't go there.
static bool append_tile(TuiGameState *gs, int ml, bool shift) {
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
  char token[TUI_TILE_TEXT_MAX + 2];
  if (tui_move_entry_resolve_tile(gs, ml, shift, land_row, land_col, token,
                                  sizeof(token)) == TUI_TYPED_LETTER_REJECT) {
    return false;
  }
  const bool need_space = strchr(gs->edit_move_buf, ' ') == NULL;
  const int token_len = (int)strlen(token);
  if (gs->edit_move_len + token_len + (need_space ? 1 : 0) >=
      (int)sizeof(gs->edit_move_buf)) {
    return false;
  }
  if (need_space) {
    gs->edit_move_buf[gs->edit_move_len++] = ' ';
  }
  memcpy(gs->edit_move_buf + gs->edit_move_len, token, (size_t)token_len);
  gs->edit_move_len += token_len;
  gs->edit_move_buf[gs->edit_move_len] = '\0';
  gs->edit_move_cursor = gs->edit_move_len;
  return true;
}

bool tui_move_entry_append_key(TuiGameState *gs, const char *key, bool shift) {
  if (gs->ld == NULL) {
    return false;
  }
  // The previous tile can still become a multi-letter tile when the
  // typist placed it (not a played-through board tile).
  const int last = last_word_token(gs);
  char last_face[TUI_TILE_TEXT_MAX] = "";
  if (last >= 0) {
    const int len = tui_tile_token_len(gs->edit_move_buf + last);
    int land_row = -1;
    int land_col = -1;
    const int saved_cursor = gs->edit_move_cursor;
    gs->edit_move_cursor = last;
    const bool landed = tui_move_entry_landing_square(gs, &land_row, &land_col);
    gs->edit_move_cursor = saved_cursor;
    const Board *brd = gs->game != NULL ? game_get_board(gs->game) : NULL;
    const bool played_through = landed && brd != NULL &&
                                board_get_letter(brd, land_row, land_col) !=
                                    ALPHABET_EMPTY_SQUARE_MARKER;
    if (!played_through) {
      tui_tile_token_face(gs->edit_move_buf + last, len, last_face,
                          sizeof(last_face));
    }
  }
  int ml = -1;
  const TuiTileKeyAction action =
      tui_tile_key(gs->ld, &gs->tile_keys, key,
                   last_face[0] != '\0' ? last_face : NULL, &ml);
  switch (action) {
  case TUI_TILE_KEY_WAIT:
  case TUI_TILE_KEY_ABSORBED:
    return true;
  case TUI_TILE_KEY_REJECT:
    return false;
  case TUI_TILE_KEY_REPLACE_LAST: {
    // The last tile joins into a multi-letter one: take it back, keeping
    // its blank-ness, then place the joined tile where it was.
    const int last_len = tui_tile_token_len(gs->edit_move_buf + last);
    char blank_token[TUI_TILE_TEXT_MAX + 2];
    tui_tile_token(gs->ld, tui_tile_for_text(gs->ld, last_face), true,
                   blank_token, sizeof(blank_token));
    const bool was_blank =
        (int)strlen(blank_token) == last_len &&
        strncmp(gs->edit_move_buf + last, blank_token, (size_t)last_len) == 0;
    char saved[sizeof(gs->edit_move_buf)];
    (void)snprintf(saved, sizeof(saved), "%s", gs->edit_move_buf);
    const int saved_len = gs->edit_move_len;
    gs->edit_move_buf[last] = '\0';
    gs->edit_move_len = last;
    gs->edit_move_cursor = last;
    tui_game_state_parse_edit_buf(gs);
    if (!append_tile(gs, ml, shift || was_blank)) {
      (void)snprintf(gs->edit_move_buf, sizeof(gs->edit_move_buf), "%s", saved);
      gs->edit_move_len = saved_len;
      gs->edit_move_cursor = saved_len;
      tui_game_state_parse_edit_buf(gs);
      tui_tile_key_reset(&gs->tile_keys);
      return false;
    }
    break;
  }
  case TUI_TILE_KEY_PLACE:
  default:
    if (!append_tile(gs, ml, shift)) {
      tui_tile_key_reset(&gs->tile_keys);
      return false;
    }
    break;
  }
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
    (void)snprintf(out, out_cap, "%c%d", col_letter, row1);
  } else {
    (void)snprintf(out, out_cap, "%d%c", row1, col_letter);
  }
}

// Copy the word token (everything after the first space) of edit_move_buf
// into `out`. Empty string when no word has been typed yet.
static void tui_board_builder_extract_word(const TuiGameState *gs, char *out,
                                           size_t out_cap) {
  out[0] = '\0';
  const char *space = strchr(gs->edit_move_buf, ' ');
  if (space != NULL && space[1] != '\0') {
    (void)snprintf(out, out_cap, "%s", space + 1);
  }
}

// True when the tiles from (row, col) onward in `dir` run to the board
// edge, leaving no empty square to start typing on.
static bool tiles_run_to_edge(const Board *brd, int row, int col,
                              bool vertical) {
  while (row < BOARD_DIM && col < BOARD_DIM) {
    if (board_get_letter(brd, row, col) == ALPHABET_EMPTY_SQUARE_MARKER) {
      return false;
    }
    if (vertical) {
      row++;
    } else {
      col++;
    }
  }
  return true;
}

// A fresh anchor starts ACROSS — matching Woogles and every mainstream
// Scrabble UI, and predictability beats cleverness here: an earlier
// "whichever direction has the longer empty run" heuristic meant the
// same click could anchor differently depending on nearby tiles, which
// read as random. Down is one toggle away (click the cell again, or
// Space/Tab). The one exception is a tile whose across run reaches the
// right edge, where across has nowhere to type.
int tui_board_builder_default_dir(const TuiGameState *gs, int row, int col) {
  const Board *brd = gs->game != NULL ? game_get_board(gs->game) : NULL;
  if (brd != NULL && tiles_run_to_edge(brd, row, col, false) &&
      !tiles_run_to_edge(brd, row, col, true)) {
    return BOARD_VERTICAL_DIRECTION;
  }
  return BOARD_HORIZONTAL_DIRECTION;
}

void tui_board_builder_set_anchor(TuiGameState *gs, int row, int col, int dir) {
  tui_tile_key_reset(&gs->tile_keys);
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
  // Typing starts at the first empty square at or after the origin: a
  // click on a tile (or an arrow onto one) continues the word it belongs
  // to rather than typing onto the tile.
  int start_row = row;
  int start_col = col;
  while (brd != NULL && start_row < BOARD_DIM && start_col < BOARD_DIM &&
         board_get_letter(brd, start_row, start_col) !=
             ALPHABET_EMPTY_SQUARE_MARKER) {
    if (vertical) {
      start_row++;
    } else {
      start_col++;
    }
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
  // Seed the word with the playthrough letters from the true anchor up to
  // (but excluding) the start square, so the typing cursor starts there.
  char coord[8];
  tui_board_builder_coord_token(gs, coord, sizeof(coord));
  char leading[48];
  int li = 0;
  {
    int r = anchor_row;
    int c = anchor_col;
    while (!(r == start_row && c == start_col) && brd != NULL &&
           r < BOARD_DIM && c < BOARD_DIM && li < (int)sizeof(leading) - 4) {
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
    (void)snprintf(gs->edit_move_buf, sizeof(gs->edit_move_buf), "%s %s", coord,
                   leading);
  } else {
    (void)snprintf(gs->edit_move_buf, sizeof(gs->edit_move_buf), "%s", coord);
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
  // Extract the user's placed tiles: skip word tiles sitting on
  // occupied squares along the OLD direction (those are absorbed
  // playthrough, meaningless in the new direction — carrying them over
  // was the "direction is stuck horizontal after playthrough" bug).
  enum { MAX_USER_TILES = 32 };
  char user_tiles[MAX_USER_TILES][TUI_TILE_TEXT_MAX];
  int n_user = 0;
  const bool old_vertical = board_is_dir_vertical(gs->board_dir);
  int r = gs->board_anchor_row;
  int c = gs->board_anchor_col;
  for (int i = 0; word[i] != '\0' && n_user < MAX_USER_TILES;) {
    const int len = tui_tile_token_len(word + i);
    const bool on_board =
        brd != NULL && r >= 0 && r < BOARD_DIM && c >= 0 && c < BOARD_DIM &&
        board_get_letter(brd, r, c) != ALPHABET_EMPTY_SQUARE_MARKER;
    if (!on_board) {
      (void)snprintf(user_tiles[n_user++], TUI_TILE_TEXT_MAX, "%.*s", len,
                     word + i);
    }
    i += len;
    if (old_vertical) {
      r++;
    } else {
      c++;
    }
  }
  const int new_dir =
      old_vertical ? BOARD_HORIZONTAL_DIRECTION : BOARD_VERTICAL_DIRECTION;
  tui_board_builder_set_anchor(gs, gs->board_origin_row, gs->board_origin_col,
                               new_dir);
  // Re-place the user's tiles along the new direction, blanks as blanks.
  for (int tile_idx = 0; tile_idx < n_user && gs->ld != NULL; tile_idx++) {
    const char *token = user_tiles[tile_idx];
    char face[TUI_TILE_TEXT_MAX];
    tui_tile_token_face(token, (int)strlen(token), face, sizeof(face));
    const int ml = tui_tile_for_text(gs->ld, face);
    char blank_token[TUI_TILE_TEXT_MAX + 2];
    tui_tile_token(gs->ld, ml, true, blank_token, sizeof(blank_token));
    if (ml > 0 && append_tile(gs, ml, strcmp(token, blank_token) == 0)) {
      tui_game_state_parse_edit_buf(gs);
      tui_autofill_playthrough(gs);
    }
  }
}

void tui_board_builder_cancel(TuiGameState *gs) {
  tui_tile_key_reset(&gs->tile_keys);
  gs->board_entry_active = false;
  gs->edit_move_buf[0] = '\0';
  gs->edit_move_len = 0;
  gs->edit_move_cursor = 0;
  gs->edit_history_idx = -1;
  tui_game_state_parse_edit_buf(gs);
}

void tui_board_entry_begin_keyboard(TuiGameState *gs) {
  if (gs->app_mode == TUI_APP_MODE_WATCH || gs->game == NULL ||
      tui_game_state_play_over(gs)) {
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
  tui_tile_key_reset(&gs->tile_keys);
  const char *space = strchr(gs->edit_move_buf, ' ');
  const int word_off =
      space != NULL ? (int)(space - gs->edit_move_buf) + 1 : -1;
  int word_len = (word_off >= 0) ? gs->edit_move_len - word_off : 0;
  if (word_len > 0) {
    const Board *brd = game_get_board(gs->game);
    const bool vertical = board_is_dir_vertical(gs->board_dir);
    // Tile tokens of the word, in order ("[QU]" is one).
    int token_start[sizeof(gs->edit_move_buf)];
    int tokens = 0;
    for (int pos = word_off; pos < gs->edit_move_len;) {
      token_start[tokens++] = pos;
      pos += tui_tile_token_len(gs->edit_move_buf + pos);
    }
    // Each token maps to cell anchor + index along the direction. Pop
    // trailing played-through cells (occupied on the board), then pop
    // one placed tile.
    while (tokens > 0) {
      const int r = gs->board_anchor_row + (vertical ? (tokens - 1) : 0);
      const int c = gs->board_anchor_col + (vertical ? 0 : (tokens - 1));
      const bool occupied =
          brd != NULL && r >= 0 && r < BOARD_DIM && c >= 0 && c < BOARD_DIM &&
          board_get_letter(brd, r, c) != ALPHABET_EMPTY_SQUARE_MARKER;
      tokens--; // drop this tile
      if (!occupied) {
        break; // it was a placed tile — stop here
      }
    }
    word_len = tokens > 0 ? token_start[tokens] - word_off : 0;
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

// Format a validated placement move for the History panel the same
// way the bot's entries are formatted: string_builder_add_move wraps
// played-through letters in parens so the renderer can dim them
// (copying the typed text verbatim rendered playthrough bold).
//
// Single-tile plays whose typed direction forms no word there (a
// one-letter "word", e.g. "10H K" typed with a horizontal cursor)
// are re-expressed along the perpendicular word they complete —
// "H8 (OI)K" — matching the GCG convention that a lone tile belongs
// to the word it makes. Must run BEFORE play_move: the walk and the
// playthrough-letter lookup read the pre-play board.
static void format_history_move(const TuiGameState *gs, const Move *move,
                                char *out, size_t out_size) {
  const Board *brd = game_get_board(gs->game);
  Move display_move;
  move_copy(&display_move, move);
  if (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE &&
      move_get_tiles_length(move) == 1) {
    const int row = move_get_row_start(move);
    const int col = move_get_col_start(move);
    const bool typed_vertical = board_is_dir_vertical(move_get_dir(move));
    const int typed_dr = typed_vertical ? 1 : 0;
    const int typed_dc = typed_vertical ? 0 : 1;
    const int perp_dr = typed_vertical ? 0 : 1;
    const int perp_dc = typed_vertical ? 1 : 0;
    // Any neighbor along the typed axis means the typed direction
    // already forms a real word — leave the move as entered.
    const bool word_in_typed_dir =
        (row - typed_dr >= 0 && col - typed_dc >= 0 &&
         board_get_letter(brd, row - typed_dr, col - typed_dc) !=
             ALPHABET_EMPTY_SQUARE_MARKER) ||
        (row + typed_dr < BOARD_DIM && col + typed_dc < BOARD_DIM &&
         board_get_letter(brd, row + typed_dr, col + typed_dc) !=
             ALPHABET_EMPTY_SQUARE_MARKER);
    if (!word_in_typed_dir) {
      int start_row = row;
      int start_col = col;
      while (start_row - perp_dr >= 0 && start_col - perp_dc >= 0 &&
             board_get_letter(brd, start_row - perp_dr, start_col - perp_dc) !=
                 ALPHABET_EMPTY_SQUARE_MARKER) {
        start_row -= perp_dr;
        start_col -= perp_dc;
      }
      int end_row = row;
      int end_col = col;
      while (end_row + perp_dr < BOARD_DIM && end_col + perp_dc < BOARD_DIM &&
             board_get_letter(brd, end_row + perp_dr, end_col + perp_dc) !=
                 ALPHABET_EMPTY_SQUARE_MARKER) {
        end_row += perp_dr;
        end_col += perp_dc;
      }
      const int word_len = (end_row - start_row) + (end_col - start_col) + 1;
      if (word_len > 1) {
        move_set_dir(&display_move, typed_vertical ? BOARD_HORIZONTAL_DIRECTION
                                                   : BOARD_VERTICAL_DIRECTION);
        move_set_row_start(&display_move, start_row);
        move_set_col_start(&display_move, start_col);
        move_set_tiles_length(&display_move, word_len);
        int walk_row = start_row;
        int walk_col = start_col;
        for (int tile_idx = 0; tile_idx < word_len; tile_idx++) {
          const bool is_placed_square = walk_row == row && walk_col == col;
          move_set_tile(&display_move,
                        is_placed_square ? move_get_tile(move, 0)
                                         : PLAYED_THROUGH_MARKER,
                        tile_idx);
          walk_row += perp_dr;
          walk_col += perp_dc;
        }
      }
    }
  }
  StringBuilder *sb = string_builder_create();
  string_builder_add_move(sb, brd, &display_move, gs->ld, false);
  char *dump = string_builder_dump(sb, NULL);
  (void)snprintf(out, out_size, "%s", dump);
  free(dump);
  string_builder_destroy(sb);
}

bool tui_pvc_commit_preview_move(TuiGameState *gs) {
  if (gs->app_mode != TUI_APP_MODE_PLAY_VS_COMPUTER || gs->game == NULL ||
      tui_game_state_play_over(gs)) {
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

  // Word validity per the session's challenge rule. The engine's
  // FormedWords lists every word the play makes; any invalid one
  // makes the whole play a phony.
  bool phony = false;
  char invalid_words[48];
  invalid_words[0] = '\0';
  if (gs->app_mode == TUI_APP_MODE_PLAY_VS_COMPUTER) {
    const Player *player = game_get_player(gs->game, player_idx);
    FormedWords *fw =
        formed_words_create(game_get_board(gs->game), gs->edit_preview_move);
    formed_words_populate_validities(player_get_kwg(player), fw,
                                     game_get_variant(gs->game) ==
                                         GAME_VARIANT_WORDSMOG);
    StringBuilder *wsb = string_builder_create();
    const int num_words = formed_words_get_num_words(fw);
    int num_invalid = 0;
    for (int word_idx = 0; word_idx < num_words; word_idx++) {
      if (formed_words_get_word_valid(fw, word_idx)) {
        continue;
      }
      if (num_invalid > 0) {
        string_builder_add_string(wsb, ", ");
      }
      const int word_len = formed_words_get_word_length(fw, word_idx);
      for (int letter_idx = 0; letter_idx < word_len; letter_idx++) {
        string_builder_add_user_visible_letter(
            wsb, gs->ld,
            (MachineLetter)formed_words_get_word_letter(fw, word_idx,
                                                        letter_idx));
      }
      string_builder_add_string(wsb, "*");
      num_invalid++;
    }
    if (num_invalid > 0) {
      phony = true;
      char *dump = string_builder_dump(wsb, NULL);
      (void)snprintf(invalid_words, sizeof(invalid_words), "%s", dump);
      free(dump);
    }
    string_builder_destroy(wsb);
    formed_words_destroy(fw);
  }
  if (phony && gs->challenge_rule == UI_CHALLENGE_VOID) {
    // VOID: invalid plays never reach the board. Reject the commit
    // and leave the editor open for a do-over.
    (void)snprintf(gs->notice_buf, sizeof(gs->notice_buf), "not valid: %s",
                   invalid_words);
    clock_gettime(CLOCK_MONOTONIC, &gs->notice_expires_at);
    gs->notice_expires_at.tv_sec += 3;
    atomic_fetch_add(&gs->render_version, 1);
    return false;
  }

  Rack leave;
  rack_set_dist_size(&leave, ld_get_size(gs->ld));
  // Format the display notation before play_move — the playthrough
  // walk needs the pre-play board.
  char display_move[sizeof(e->move_str)];
  format_history_move(gs, gs->edit_preview_move, display_move,
                      sizeof(display_move));
  if (phony) {
    // SINGLE / DOUBLE / PENALTY: the play is recorded, then
    // auto-challenged off for loss of turn (the computer never misses
    // a phony). The board, rack, and score never change — the engine
    // sees a pass, which advances the turn and counts the zero-point
    // turn toward the consecutive-scoreless end condition, exactly
    // like a challenged-off play does under live rules.
    Move pass_move;
    move_set_as_pass(&pass_move);
    play_move(&pass_move, gs->game, &leave);
  } else {
    play_move(gs->edit_preview_move, gs->game, &leave);
    tui_tag_move_owners(game_get_board(gs->game), gs->edit_preview_move,
                        player_idx);
  }
  (void)snprintf(e->move_str, sizeof(e->move_str), "%s", display_move);
  e->score = gs->edit_move_score;
  if (!phony) {
    StringBuilder *sb = string_builder_create();
    string_builder_add_rack(sb, &leave, gs->ld, false);
    char *dump = string_builder_dump(sb, NULL);
    (void)snprintf(e->leave_str, sizeof(e->leave_str), "%s", dump);
    free(dump);
    string_builder_destroy(sb);
  }
  const int post =
      equity_to_int(player_get_score(game_get_player(gs->game, player_idx)));
  int bonus = 0;
  if (!phony && game_over(gs->game)) {
    const Rack *opp =
        player_get_rack(game_get_player(gs->game, 1 - player_idx));
    if (opp != NULL && !rack_is_empty(opp)) {
      bonus = equity_to_int(calculate_end_rack_points(opp, gs->ld));
      e->end_bonus = bonus;
      StringBuilder *sb = string_builder_create();
      string_builder_add_rack(sb, opp, gs->ld, false);
      char *dump = string_builder_dump(sb, NULL);
      (void)snprintf(e->end_rack_str, sizeof(e->end_rack_str), "%s", dump);
      free(dump);
      string_builder_destroy(sb);
    }
  }
  // For a challenged-off play the engine score is untouched; the
  // entry keeps the play's as-if score / total and its extra
  // "challenged off −N" row pair cancels them (mirroring
  // GAME_EVENT_PHONY_TILES_RETURNED's adjustment + cumulative-score
  // pair). Kept inside the play's own entry — not appended as a
  // separate one — so the two-column history's index-parity column
  // assignment stays in sync.
  e->challenged_off = phony;
  e->total_after = phony ? post + e->score : post - bonus;
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
  if (phony) {
    (void)snprintf(gs->notice_buf, sizeof(gs->notice_buf),
                   "challenged off (%s) - turn lost", invalid_words);
    clock_gettime(CLOCK_MONOTONIC, &gs->notice_expires_at);
    gs->notice_expires_at.tv_sec += 4;
  }
  gs->board_entry_active = false;
  gs->edit_history_idx = -1;
  gs->edit_move_buf[0] = '\0';
  gs->edit_move_len = 0;
  gs->edit_move_cursor = 0;
  if (game_over(gs->game) && gs->history_count > 0) {
    gs->history_cursor = gs->history_count - 1;
    gs->analysis_cursor = 0;
    // Settle overtime penalties now that the game is over. Appended
    // after the cursor parking so the cursor stays on the going-out
    // move (penalty entries have no analysis to browse).
    tui_bot_worker_apply_time_penalties(gs);
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
  tui_game_state_edit_move_display(gs, e->move_str, sizeof(e->move_str));
  e->score = gs->edit_move_score;
  // Seed the rack from the move's played tiles when the annotator
  // hasn't typed a fuller rack — matches the cell editor's behavior.
  if (e->rack_str[0] == '\0' && gs->edit_move_inferred_rack[0] != '\0') {
    (void)snprintf(e->rack_str, sizeof(e->rack_str), "%s",
                   gs->edit_move_inferred_rack);
  }
  if (gs->edit_move_leave[0] != '\0') {
    (void)snprintf(e->leave_str, sizeof(e->leave_str), "%s",
                   gs->edit_move_leave);
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
