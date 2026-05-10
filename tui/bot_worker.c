#include "bot_worker.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../src/def/move_defs.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/str/move_string.h"
#include "../src/str/rack_string.h"
#include "../src/util/string_util.h"
#include "game_state.h"

enum {
  TURN_DELAY_NS = 3 * 1000 * 1000 * 1000L,  // 3 s
  POLL_NS = 100 * 1000 * 1000L,             // 100 ms
};

static void copy_str(char *dst, size_t dst_size, const char *src) {
  if (dst_size == 0) {
    return;
  }
  if (src == NULL) {
    dst[0] = '\0';
    return;
  }
  size_t len = strlen(src);
  if (len >= dst_size) {
    len = dst_size - 1;
  }
  memcpy(dst, src, len);
  dst[len] = '\0';
}

// Caller must hold state->mutex.
static void append_history(TuiGameState *state, int player_idx,
                           const Move *move, int score, int total_after) {
  if (state->history_count >= TUI_HISTORY_MAX) {
    // Drop oldest.
    memmove(&state->history[0], &state->history[1],
            sizeof(state->history[0]) * (TUI_HISTORY_MAX - 1));
    state->history_count = TUI_HISTORY_MAX - 1;
  }

  TuiHistoryEntry *entry = &state->history[state->history_count++];
  entry->player_idx = player_idx;
  entry->score = score;
  entry->total_after = total_after;

  StringBuilder *sb = string_builder_create();
  string_builder_add_move(sb, game_get_board(state->game), move, state->ld,
                          true);
  size_t move_len = 0;
  char *move_dump = string_builder_dump(sb, &move_len);
  copy_str(entry->move_str, sizeof(entry->move_str), move_dump);
  free(move_dump);
  string_builder_destroy(sb);

  // Leave: rack contents AFTER playing (the player's current rack now that
  // play_move has drawn replacements? No — we want the leave from the move,
  // i.e., the tiles that were not played). Compute via get_leave_for_move
  // before the play, OR just dump the player's rack after — but the player
  // already drew replacements. So caller passes a pre-computed leave string.
  entry->leave_str[0] = '\0';
}

// Caller must hold state->mutex. Sets entry's leave_str.
static void set_history_leave(TuiGameState *state, const Rack *leave_rack) {
  if (state->history_count == 0) {
    return;
  }
  TuiHistoryEntry *entry = &state->history[state->history_count - 1];
  if (leave_rack == NULL) {
    entry->leave_str[0] = '\0';
    return;
  }
  StringBuilder *sb = string_builder_create();
  string_builder_add_rack(sb, leave_rack, state->ld, false);
  size_t leave_len = 0;
  char *leave_dump = string_builder_dump(sb, &leave_len);
  copy_str(entry->leave_str, sizeof(entry->leave_str), leave_dump);
  free(leave_dump);
  string_builder_destroy(sb);
}

// Sleep up to TURN_DELAY_NS, polling stop every POLL_NS so we exit
// promptly when the main thread asks us to.
static void interruptible_sleep(_Atomic bool *stop) {
  long remaining = TURN_DELAY_NS;
  while (remaining > 0 && !atomic_load(stop)) {
    long step = remaining < POLL_NS ? remaining : POLL_NS;
    struct timespec ts = {.tv_sec = step / 1000000000L,
                          .tv_nsec = step % 1000000000L};
    nanosleep(&ts, NULL);
    remaining -= step;
  }
}

static void *bot_thread_main(void *arg) {
  TuiGameState *state = (TuiGameState *)arg;
  // Persistent move list: capacity 1 since we use MOVE_RECORD_BEST.
  MoveList *move_list = move_list_create(1);

  while (!atomic_load(&state->bot_stop)) {
    bool finished = false;
    pthread_mutex_lock(&state->mutex);
    if (game_over(state->game)) {
      finished = true;
    } else {
      const int player_idx = game_get_player_on_turn_index(state->game);
      const MoveGenArgs args = {
          .game = state->game,
          .move_record_type = MOVE_RECORD_BEST,
          .move_sort_type = MOVE_SORT_EQUITY,
          .override_kwg = NULL,
          .eq_margin_movegen = 0,
          .target_equity = EQUITY_MAX_VALUE,
          .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
          .thread_index = 0,
          .move_list = move_list,
          .tiles_played_bv = NULL,
          .initial_tiles_bv = 0,
      };
      generate_moves_for_game(&args);

      if (move_list_get_count(move_list) > 0) {
        const Move *best = move_list_get_move(move_list, 0);
        const int score = equity_to_int(move_get_score(best));
        Rack leave;
        rack_set_dist_size(&leave, ld_get_size(state->ld));
        play_move(best, state->game, &leave);
        const int total_after = equity_to_int(player_get_score(
            game_get_player(state->game, player_idx)));
        append_history(state, player_idx, best, score, total_after);
        set_history_leave(state, &leave);
        if (game_over(state->game)) {
          finished = true;
        }
      } else {
        // No legal moves — shouldn't happen, but bail to avoid spinning.
        finished = true;
      }
    }
    pthread_mutex_unlock(&state->mutex);

    if (finished) {
      break;
    }
    interruptible_sleep(&state->bot_stop);
  }

  moves_for_move_list_destroy(move_list);
  free(move_list);
  return NULL;
}

void tui_bot_worker_start(TuiGameState *state) {
  if (state == NULL || state->bot_started) {
    return;
  }
  if (pthread_create(&state->bot_thread, NULL, bot_thread_main, state) == 0) {
    state->bot_started = true;
  }
}
