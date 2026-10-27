#include "exchange_dialog.h"

#include "../src/def/letter_distribution_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "game_state.h"
#include "list_nav.h"
#include "move_entry.h"
#include "render_hit_test.h"
#include "render_view.h"
#include "tile_input.h"
#include "tui_ui_state.h"
#include "tui_ui_types.h"
#include <notcurses/notcurses.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void tui_exchange_dialog_open(TuiGameState *state, TuiUiState *ui) {
  pthread_mutex_lock(&state->mutex);
  const char *reason = tui_pvc_move_unavailable_reason(state, true);
  if (reason != NULL) {
    tui_game_state_notice(state, reason);
    pthread_mutex_unlock(&state->mutex);
    return;
  }
  const Rack *rack =
      player_get_rack(game_get_player(state->game, state->human_player_idx));
  MachineLetter slots[RACK_SIZE];
  const int count = sort_rack_for_display(rack, state->ld, state->rack_sort,
                                          slots, RACK_SIZE);
  for (int tile_idx = 0; tile_idx < count; tile_idx++) {
    char *notation = ld_ml_to_hl(state->ld, slots[tile_idx]);
    (void)snprintf(ui->exchange_tiles[tile_idx],
                   sizeof(ui->exchange_tiles[tile_idx]), "%s",
                   notation != NULL ? notation : "?");
    free(notation);
    ui->exchange_picked[tile_idx] = false;
  }
  ui->exchange_tile_count = count;
  ui->exchange_focus = 0;
  ui->modal = TUI_MODAL_EXCHANGE;
  pthread_mutex_unlock(&state->mutex);
}

void tui_pass_confirm_open(TuiGameState *state, TuiUiState *ui) {
  pthread_mutex_lock(&state->mutex);
  const char *reason = tui_pvc_move_unavailable_reason(state, false);
  if (reason != NULL) {
    tui_game_state_notice(state, reason);
  } else {
    ui->pass_confirm_focus = 0;
    ui->modal = TUI_MODAL_PASS_CONFIRM;
  }
  pthread_mutex_unlock(&state->mutex);
}

static int picked_count(const TuiUiState *ui) {
  int picked = 0;
  for (int tile_idx = 0; tile_idx < ui->exchange_tile_count; tile_idx++) {
    picked += ui->exchange_picked[tile_idx] ? 1 : 0;
  }
  return picked;
}

// Plays the picked tiles as "-ABC" and closes the picker.
static void exchange_picked_tiles(TuiGameState *state, TuiUiState *ui) {
  char move_text[2 + RACK_SIZE * TUI_TILE_TEXT_MAX] = "-";
  for (int tile_idx = 0; tile_idx < ui->exchange_tile_count; tile_idx++) {
    if (ui->exchange_picked[tile_idx]) {
      (void)strncat(move_text, ui->exchange_tiles[tile_idx],
                    sizeof(move_text) - strlen(move_text) - 1);
    }
  }
  if (move_text[1] == '\0') {
    return;
  }
  ui->modal = TUI_MODAL_NONE;
  pthread_mutex_lock(&state->mutex);
  (void)tui_pvc_play_typed(state, move_text);
  pthread_mutex_unlock(&state->mutex);
}

// Picks the first unpicked tile whose face starts with `key` (a letter in
// either case, or ?), else unpicks the last picked one. Returns whether a
// tile matched.
static bool toggle_tile_for_key(TuiUiState *ui, uint32_t key) {
  const char wanted =
      (char)(key >= 'a' && key <= 'z' ? key - 'a' + 'A' : (int)key);
  int unpicked = -1;
  int picked = -1;
  for (int tile_idx = 0; tile_idx < ui->exchange_tile_count; tile_idx++) {
    const char *face = ui->exchange_tiles[tile_idx];
    if (face[0] == '[') {
      face++;
    }
    if (face[0] != wanted) {
      continue;
    }
    if (!ui->exchange_picked[tile_idx] && unpicked < 0) {
      unpicked = tile_idx;
    } else if (ui->exchange_picked[tile_idx]) {
      picked = tile_idx;
    }
  }
  const int target = unpicked >= 0 ? unpicked : picked;
  if (target < 0) {
    return false;
  }
  ui->exchange_picked[target] = !ui->exchange_picked[target];
  ui->exchange_focus = target;
  return true;
}

// Acts on row `row`: toggles a tile, plays the exchange, or cancels.
static void activate_exchange_row(TuiGameState *state, TuiUiState *ui,
                                  int row) {
  const int tile_count = ui->exchange_tile_count;
  if (row < tile_count) {
    ui->exchange_picked[row] = !ui->exchange_picked[row];
  } else if (row == tile_count) {
    exchange_picked_tiles(state, ui);
  } else {
    ui->modal = TUI_MODAL_NONE;
  }
}

static void input_exchange(TuiGameState *state, TuiUiState *ui, uint32_t key,
                           ncinput input) {
  const int tile_count = ui->exchange_tile_count;
  const int row_count = tile_count + 2;
  if (key == NCKEY_BUTTON1) {
    if (input.evtype == NCTYPE_RELEASE) {
      return;
    }
    const int hit = tui_modal_item_at(input.y, input.x);
    if (hit >= 0 && hit < row_count) {
      ui->exchange_focus = hit;
      activate_exchange_row(state, ui, hit);
    }
    return;
  }
  // Exchange is skipped while no tile is picked. Letters pick tiles, so
  // j/k don't navigate here.
  bool enabled[RACK_SIZE + 2] = {false};
  for (int row = 0; row < row_count; row++) {
    enabled[row] = row != tile_count || picked_count(ui) > 0;
  }
  const int nav = tui_list_nav(key, &input, ui->exchange_focus, row_count,
                               enabled, TUI_LIST_NAV_HOME_END);
  if (nav >= 0) {
    ui->exchange_focus = nav;
  } else if (key == NCKEY_ESC) {
    ui->modal = TUI_MODAL_NONE;
  } else if (key == ' ') {
    activate_exchange_row(state, ui, ui->exchange_focus);
  } else if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
    // Enter plays the exchange from any row once a tile is picked; before
    // that it picks the focused tile.
    if (ui->exchange_focus == tile_count + 1) {
      ui->modal = TUI_MODAL_NONE;
    } else if (picked_count(ui) > 0) {
      exchange_picked_tiles(state, ui);
    } else {
      activate_exchange_row(state, ui, ui->exchange_focus);
    }
  } else if ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z') ||
             key == '?') {
    (void)toggle_tile_for_key(ui, key);
  }
}

static void input_pass_confirm(TuiGameState *state, TuiUiState *ui,
                               uint32_t key, ncinput input) {
  if (key == NCKEY_BUTTON1) {
    if (input.evtype == NCTYPE_RELEASE) {
      return;
    }
    const int hit = tui_modal_item_at(input.y, input.x);
    if (hit < 0 || hit > 1) {
      return;
    }
    ui->pass_confirm_focus = hit;
    key = NCKEY_ENTER;
  }
  const int nav = tui_list_nav(key, &input, ui->pass_confirm_focus, 2, NULL,
                               TUI_LIST_NAV_HOME_END | TUI_LIST_NAV_VI);
  bool pass = false;
  if (nav >= 0) {
    ui->pass_confirm_focus = nav;
  } else if (key == NCKEY_ESC || key == 'n' || key == 'N') {
    ui->modal = TUI_MODAL_NONE;
  } else if (key == 'y' || key == 'Y') {
    pass = true;
  } else if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
    pass = ui->pass_confirm_focus == 1;
    ui->modal = TUI_MODAL_NONE;
  }
  if (pass) {
    ui->modal = TUI_MODAL_NONE;
    pthread_mutex_lock(&state->mutex);
    (void)tui_pvc_play_typed(state, "pass");
    pthread_mutex_unlock(&state->mutex);
  }
}

bool tui_input_exchange_dialog(TuiGameState *state, TuiUiState *ui,
                               uint32_t key, ncinput input) {
  if (ui->modal == TUI_MODAL_EXCHANGE) {
    input_exchange(state, ui, key, input);
    return true;
  }
  if (ui->modal == TUI_MODAL_PASS_CONFIRM) {
    input_pass_confirm(state, ui, key, input);
    return true;
  }
  return false;
}
