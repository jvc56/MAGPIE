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

// Caller must hold state->mutex. Snapshots the player's full rack BEFORE
// play_move (i.e., the tiles they had at the start of their turn) plus
// the move and metadata.
static void append_history(TuiGameState *state, int player_idx,
                           const Move *move, const Rack *rack_at_start,
                           int score, int total_after, int clock_at_start) {
  if (state->history_count >= TUI_HISTORY_MAX) {
    memmove(&state->history[0], &state->history[1],
            sizeof(state->history[0]) * (TUI_HISTORY_MAX - 1));
    state->history_count = TUI_HISTORY_MAX - 1;
  }

  TuiHistoryEntry *entry = &state->history[state->history_count++];
  entry->player_idx = player_idx;
  entry->score = score;
  entry->total_after = total_after;
  entry->clock_at_start = clock_at_start;
  entry->end_bonus = 0;
  entry->end_rack_str[0] = '\0';

  StringBuilder *sb = string_builder_create();
  // add_score=false: we render the score in our own column.
  string_builder_add_move(sb, game_get_board(state->game), move, state->ld,
                          false);
  size_t move_len = 0;
  char *move_dump = string_builder_dump(sb, &move_len);
  copy_str(entry->move_str, sizeof(entry->move_str), move_dump);
  free(move_dump);
  string_builder_destroy(sb);

  if (rack_at_start != NULL) {
    StringBuilder *rsb = string_builder_create();
    string_builder_add_rack(rsb, rack_at_start, state->ld, false);
    size_t rack_len = 0;
    char *rack_dump = string_builder_dump(rsb, &rack_len);
    copy_str(entry->rack_str, sizeof(entry->rack_str), rack_dump);
    free(rack_dump);
    string_builder_destroy(rsb);
  } else {
    entry->rack_str[0] = '\0';
  }
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
    // Sleep up front so the very first move also gets a 3-second pause
    // (matching the gap between subsequent moves). The corresponding
    // wall time is charged to the on-turn player by the elapsed-time
    // accounting after their move plays.
    interruptible_sleep(&state->bot_stop);
    if (atomic_load(&state->bot_stop)) {
      break;
    }

    bool finished = false;
    pthread_mutex_lock(&state->mutex);
    if (game_over(state->game)) {
      finished = true;
    } else {
      const int player_idx = game_get_player_on_turn_index(state->game);
      // Snapshot the clock at the moment this turn started — that is, the
      // value at the end of the previous play, before any of this turn's
      // think time has been counted against the player.
      const int clock_at_start = state->time_per_side_seconds -
                                 (int)state->seconds_used[player_idx];

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

        // Snapshot the full rack BEFORE play_move modifies the player's
        // rack — this is what they had at the start of their turn.
        Rack rack_at_start;
        rack_set_dist_size(&rack_at_start, ld_get_size(state->ld));
        rack_copy(&rack_at_start, player_get_rack(game_get_player(
                                       state->game, player_idx)));

        Rack leave;
        rack_set_dist_size(&leave, ld_get_size(state->ld));
        play_move(best, state->game, &leave);
        const int post_play_score = equity_to_int(player_get_score(
            game_get_player(state->game, player_idx)));

        // If play_move just applied an end-of-game bonus to this player
        // (they went out and the opponent has tiles left), pull that out
        // so the move's own entry shows the score from this play alone.
        int bonus = 0;
        const Rack *opp_rack = NULL;
        if (game_over(state->game)) {
          opp_rack = player_get_rack(
              game_get_player(state->game, 1 - player_idx));
          if (!rack_is_empty(opp_rack)) {
            bonus = equity_to_int(
                calculate_end_rack_points(opp_rack, state->ld));
          }
        }
        const int total_after_move = post_play_score - bonus;
        append_history(state, player_idx, best, &rack_at_start, score,
                       total_after_move, clock_at_start);

        // Charge this turn's elapsed time to the player who just moved,
        // and reset turn_started so the next player's clock begins now.
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        const double elapsed =
            (double)(now.tv_sec - state->turn_started.tv_sec) +
            (double)(now.tv_nsec - state->turn_started.tv_nsec) / 1e9;
        state->seconds_used[player_idx] += elapsed;
        state->turn_started = now;

        // Surface the going-out bonus as a third line on this entry —
        // not as a separate entry — by stashing the bonus and the
        // opponent's leftover into the just-appended history row.
        if (game_over(state->game)) {
          if (bonus != 0 && opp_rack != NULL && state->history_count > 0) {
            TuiHistoryEntry *entry =
                &state->history[state->history_count - 1];
            entry->end_bonus = bonus;
            StringBuilder *rsb = string_builder_create();
            string_builder_add_rack(rsb, opp_rack, state->ld, false);
            size_t rlen = 0;
            char *rdump = string_builder_dump(rsb, &rlen);
            copy_str(entry->end_rack_str, sizeof(entry->end_rack_str), rdump);
            free(rdump);
            string_builder_destroy(rsb);
          }
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
