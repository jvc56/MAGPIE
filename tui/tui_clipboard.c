#include "tui_clipboard.h"

#include "../src/ent/game.h"
#include "../src/impl/cgp.h"
#include "game_state.h"
#include "gcg_export.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// Copy `text` to the system clipboard. Primary mechanism is OSC 52,
// written straight to the tty the same way the focus-reporting
// toggles are: supported by Ghostty / Kitty / WezTerm / iTerm2 (with
// the pref enabled) and works over SSH. On macOS, also pipe through
// pbcopy as a fallback for terminals without OSC 52 (Terminal.app).
static void tui_copy_to_clipboard(const char *text) {
  if (text == NULL) {
    return;
  }
  const size_t text_len = strlen(text);
  // Base64-encode for OSC 52: ESC ] 52 ; c ; <base64> BEL.
  static const char b64_alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  static const char osc_prefix[] = "\x1b]52;c;";
  const size_t prefix_len = sizeof(osc_prefix) - 1;
  const size_t b64_len = 4 * ((text_len + 2) / 3);
  char *seq = malloc(prefix_len + b64_len + 2);
  if (seq == NULL) {
    return;
  }
  memcpy(seq, osc_prefix, prefix_len);
  char *out = seq + prefix_len;
  size_t in_idx = 0;
  while (in_idx < text_len) {
    const uint32_t byte0 = (uint8_t)text[in_idx];
    const uint32_t byte1 =
        in_idx + 1 < text_len ? (uint8_t)text[in_idx + 1] : 0;
    const uint32_t byte2 =
        in_idx + 2 < text_len ? (uint8_t)text[in_idx + 2] : 0;
    const uint32_t chunk = (byte0 << 16) | (byte1 << 8) | byte2;
    *out++ = b64_alphabet[(chunk >> 18) & 0x3f];
    *out++ = b64_alphabet[(chunk >> 12) & 0x3f];
    *out++ =
        (char)(in_idx + 1 < text_len ? b64_alphabet[(chunk >> 6) & 0x3f] : '=');
    *out++ = (char)(in_idx + 2 < text_len ? b64_alphabet[chunk & 0x3f] : '=');
    in_idx += 3;
  }
  *out++ = '\x07';
  (void)!write(STDOUT_FILENO, seq, (size_t)(out - seq));
  free(seq);
#ifdef __APPLE__
  FILE *clip_pipe = popen("pbcopy", "w");
  if (clip_pipe != NULL) {
    fwrite(text, 1, text_len, clip_pipe);
    pclose(clip_pipe);
  }
#endif
}
// Copy the current live position to the clipboard as a CGP string
// and flash a status-bar notice. In a live play-vs-computer game the
// computer's rack is blanked out of the CGP — consistent with the
// rack-panel / history concealment, the clipboard must not leak the
// computer's tiles; once the game is over the full position is
// copied. Takes gs->mutex itself, and releases it before writing the
// clipboard so a slow pbcopy can't stall the bot or the renderer.
void tui_copy_game_gcg(TuiGameState *gs) {
  pthread_mutex_lock(&gs->mutex);
  char *gcg = tui_gcg_export(gs);
  tui_game_state_notice(gs, "Copied GCG");
  pthread_mutex_unlock(&gs->mutex);
  tui_copy_to_clipboard(gcg);
  free(gcg);
}

void tui_copy_position_cgp(TuiGameState *gs) {
  pthread_mutex_lock(&gs->mutex);
  char *cgp = gs->game != NULL ? game_get_cgp(gs->game, true) : NULL;
  if (cgp == NULL) {
    pthread_mutex_unlock(&gs->mutex);
    return;
  }
  const bool conceal = gs->app_mode == TUI_APP_MODE_PLAY_VS_COMPUTER &&
                       !tui_game_state_play_over(gs);
  if (conceal) {
    // CGP is "<board> <rack_on_turn>/<rack_other> <scores> <zeros>".
    // Blank the computer's side of the racks token by splicing the
    // string — the real Game is left untouched.
    const int on_turn_idx = game_get_player_on_turn_index(gs->game);
    const bool computer_on_turn = on_turn_idx != gs->human_player_idx;
    char *racks = strchr(cgp, ' ');
    if (racks != NULL) {
      racks++;
      char *rack_slash = strchr(racks, '/');
      const char *racks_end = strchr(racks, ' ');
      if (rack_slash != NULL && racks_end != NULL && rack_slash < racks_end) {
        if (computer_on_turn) {
          memmove(racks, rack_slash, strlen(rack_slash) + 1);
        } else {
          memmove(rack_slash + 1, racks_end, strlen(racks_end) + 1);
        }
      }
    }
  }
  (void)snprintf(gs->notice_buf, sizeof(gs->notice_buf), "Copied CGP%s",
                 conceal ? " (computer rack hidden)" : "");
  clock_gettime(CLOCK_MONOTONIC, &gs->notice_expires_at);
  gs->notice_expires_at.tv_sec += 2;
  pthread_mutex_unlock(&gs->mutex);
  tui_copy_to_clipboard(cgp);
  free(cgp);
}
