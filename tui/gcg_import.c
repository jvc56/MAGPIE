#include "gcg_import.h"

#include "../src/def/game_history_defs.h"
#include "../src/ent/board.h"
#include "../src/ent/game.h"
#include "../src/ent/game_history.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/impl/gameplay.h"
#include "../src/str/move_string.h"
#include "../src/str/rack_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Walk a GCG tile-placement word starting at (row, col) in `dir`
// (0 = across, 1 = down) and stamp every newly-placed tile's
// square with `player_idx` so the board renderer can color it
// correctly. Played-through tiles (marked '.' in the word) are
// skipped — they retain whichever player_idx the earlier event
// already wrote. Out-of-bounds coordinates terminate the walk.
static void tui_apply_gcg_move_owner(Board *board, int row, int col, int dir,
                                     const char *word, int player_idx) {
  int r = row;
  int c = col;
  for (const char *p = word; *p != '\0'; p++) {
    if (r < 0 || r >= BOARD_DIM || c < 0 || c >= BOARD_DIM) {
      break;
    }
    if (*p != '.') {
      board_set_square_owner(board, r, c, player_idx);
    }
    if (dir == 0) {
      c++;
    } else {
      r++;
    }
  }
}

// Walk a parsed GCG's events and append one TuiHistoryEntry per move so
// the History panel reflects the whole game, with per-entry pre-move
// board / rack snapshots and tile owners, then leave the live game at its
// final state with the cursor on turn 1. Caller holds state->mutex and has
// already replayed the events onto state->game.
void tui_gcg_import_history(TuiGameState *state, GameHistory *history) {
  // Walk the parsed events and append a TuiHistoryEntry
  // per move so the History panel reflects the whole
  // game. The walker runs in three passes:
  //   1. Text-field pass — build each entry's move/score/
  //      rack strings using the game at its final post-
  //      replay state (so '.' playthrough markers can be
  //      resolved against the final board). Parsed
  //      position / direction / word are stashed into
  //      `entry_meta` for the later passes.
  //   2. Snapshot pass — for each entry, replay events
  //      [0..k) to recover the state immediately before
  //      that move, duplicate the board, apply owner
  //      stamps for the prior events' tiles, and snapshot
  //      racks from event metadata. Quadratic in number
  //      of moves but n is tiny so it doesn't matter.
  //   3. Restore-final pass — replay all events to put
  //      the live game back at its final state, then
  //      stamp owners on the live board so the cursor-
  //      off-history view still colors correctly.
  typedef struct {
    int engine_idx;
    int player_idx;
    int row;
    int col;
    int dir; // 0 = across, 1 = down
    char word[80];
    bool has_position;
  } GcgEntryMeta;
  static GcgEntryMeta entry_meta[TUI_HISTORY_MAX];
  int meta_count = 0;

  const int num_events = game_history_get_num_events(history);
  // ── Pass 1: text fields ─────────────────────────────────────
  for (int evi = 0; evi < num_events && state->history_count < TUI_HISTORY_MAX;
       evi++) {
    const GameEvent *event = game_history_get_event(history, evi);
    const game_event_t etype = game_event_get_type(event);
    if (etype == GAME_EVENT_PHONY_TILES_RETURNED && state->history_count > 0) {
      // A withdrawn phony ("--" in GCG) folds into the play's own entry,
      // the way play-vs-computer records an auto-challenge, so the
      // history shows it challenged off and a replay treats the turn
      // as lost rather than leaving the play on the board.
      TuiHistoryEntry *withdrawn = &state->history[state->history_count - 1];
      if (withdrawn->player_idx == game_event_get_player_index(event)) {
        withdrawn->challenged_off = true;
      }
      continue;
    }
    if (etype == GAME_EVENT_CHALLENGE_BONUS && state->history_count > 0) {
      // A valid play that was challenged earns its player a bonus; keep
      // it on that play's entry so a replay's totals match the record.
      TuiHistoryEntry *challenged = &state->history[state->history_count - 1];
      const Equity bonus_eq = game_event_get_score_adjustment(event);
      if (challenged->player_idx == game_event_get_player_index(event) &&
          bonus_eq != EQUITY_UNDEFINED_VALUE) {
        challenged->challenge_bonus += equity_to_int(bonus_eq);
        const Equity cume_eq = game_event_get_cumulative_score(event);
        if (cume_eq != EQUITY_UNDEFINED_VALUE) {
          challenged->total_after = equity_to_int(cume_eq);
        }
      }
      continue;
    }
    if (etype == GAME_EVENT_END_RACK_POINTS && state->history_count > 0) {
      // Going out earns the opponent's leftover rack; show it on the
      // play that went out, as a live game's history does.
      TuiHistoryEntry *went_out = &state->history[state->history_count - 1];
      const Equity bonus_eq = game_event_get_score_adjustment(event);
      const Rack *end_rack = game_event_get_const_rack(event);
      if (went_out->player_idx == game_event_get_player_index(event) &&
          bonus_eq != EQUITY_UNDEFINED_VALUE && end_rack != NULL) {
        went_out->end_bonus = equity_to_int(bonus_eq);
        StringBuilder *rack_sb = string_builder_create();
        string_builder_add_rack(rack_sb, end_rack, game_get_ld(state->game),
                                false);
        snprintf(went_out->end_rack_str, sizeof(went_out->end_rack_str), "%s",
                 string_builder_peek(rack_sb));
        string_builder_destroy(rack_sb);
      }
      continue;
    }
    if (etype != GAME_EVENT_TILE_PLACEMENT_MOVE && etype != GAME_EVENT_PASS &&
        etype != GAME_EVENT_EXCHANGE) {
      continue;
    }
    const int player_idx = game_event_get_player_index(event);
    const Equity move_eq = game_event_get_move_score(event);
    const Equity cume_eq = game_event_get_cumulative_score(event);
    const char *cgp_move = game_event_get_cgp_move_string(event);
    const Rack *rack = game_event_get_const_rack(event);
    TuiHistoryEntry *entry = &state->history[state->history_count++];
    memset(entry, 0, sizeof(*entry));
    entry->player_idx = player_idx;
    entry->pending = false;
    entry->score =
        (move_eq != EQUITY_UNDEFINED_VALUE && move_eq != EQUITY_INITIAL_VALUE &&
         move_eq != EQUITY_PASS_VALUE)
            ? equity_to_int(move_eq)
            : 0;
    entry->total_after =
        (cume_eq != EQUITY_UNDEFINED_VALUE && cume_eq != EQUITY_INITIAL_VALUE &&
         cume_eq != EQUITY_PASS_VALUE)
            ? equity_to_int(cume_eq)
            : 0;
    entry->clock_at_start = state->time_per_side_seconds;
    entry->opp_clock_at_start = state->time_per_side_seconds;

    GcgEntryMeta *meta = &entry_meta[meta_count++];
    meta->engine_idx = evi;
    meta->player_idx = player_idx;
    meta->has_position = false;
    meta->word[0] = '\0';

    // Snapshot the engine's validated move into the entry so
    // the board renderer can ghost the played tiles when
    // the cursor lands on the "Plays" row in the Analysis
    // panel. parse_gcg_events ran with validate=true so
    // every tile-placement event has its vms populated;
    // we deep-copy out of it since the GameHistory will be
    // destroyed before the TuiHistoryEntry is.
    const ValidatedMoves *vms = game_event_get_vms(event);
    if (vms != NULL && validated_moves_get_number_of_moves(vms) > 0) {
      const Move *src = validated_moves_get_move(vms, 0);
      if (src != NULL) {
        entry->loaded_move = move_create();
        move_copy(entry->loaded_move, src);
      }
    }

    if (cgp_move != NULL) {
      // Reformat GCG moves into the compact form the rest
      // of the TUI expects: exchanges → "-AAU", and tile
      // placements get each '.' resolved to "(L)" using
      // the final board. Newly-played blanks stay
      // lowercase — render_move_styled handles the
      // bold/dim distinction.
      if (strncmp(cgp_move, "ex ", 3) == 0) {
        snprintf(entry->move_str, sizeof(entry->move_str), "-%s", cgp_move + 3);
      } else if (strncmp(cgp_move, "(exch ", 6) == 0) {
        const char *close_paren = strchr(cgp_move, ')');
        if (close_paren != NULL) {
          const int letters_len = (int)(close_paren - (cgp_move + 6));
          char tmp[sizeof(entry->move_str)];
          tmp[0] = '-';
          const int cap = (int)sizeof(tmp) - 2;
          const int copy = letters_len < cap ? letters_len : cap;
          memcpy(tmp + 1, cgp_move + 6, (size_t)copy);
          tmp[1 + copy] = '\0';
          snprintf(entry->move_str, sizeof(entry->move_str), "%s", tmp);
        } else {
          snprintf(entry->move_str, sizeof(entry->move_str), "%s", cgp_move);
        }
      } else {
        const char *m = cgp_move;
        int row = -1;
        int col = -1;
        int dir = 0;
        if (*m >= '0' && *m <= '9') {
          int r = 0;
          while (*m >= '0' && *m <= '9') {
            r = r * 10 + (*m - '0');
            m++;
          }
          if (*m >= 'A' && *m <= 'A' + BOARD_DIM - 1) {
            col = *m - 'A';
            m++;
          }
          row = r - 1;
          dir = 0;
        } else if (*m >= 'A' && *m <= 'A' + BOARD_DIM - 1) {
          col = *m - 'A';
          m++;
          int r = 0;
          while (*m >= '0' && *m <= '9') {
            r = r * 10 + (*m - '0');
            m++;
          }
          row = r - 1;
          dir = 1;
        }
        if (row >= 0 && row < BOARD_DIM && col >= 0 && col < BOARD_DIM &&
            *m == ' ') {
          meta->row = row;
          meta->col = col;
          meta->dir = dir;
          meta->has_position = true;
          snprintf(meta->word, sizeof(meta->word), "%s", m + 1);

          char buf[80];
          const size_t prefix_len = (size_t)(m - cgp_move) + 1;
          if (prefix_len < sizeof(buf)) {
            memcpy(buf, cgp_move, prefix_len);
            size_t out = prefix_len;
            m++;
            int r = row;
            int c = col;
            const Board *board = game_get_board(state->game);
            while (*m != '\0' && out + 5 < sizeof(buf) && r < BOARD_DIM &&
                   c < BOARD_DIM) {
              if (*m == '.') {
                const MachineLetter ml = board_get_letter(board, r, c);
                const char *hl = ml != ALPHABET_EMPTY_SQUARE_MARKER
                                     ? state->ld->ld_ml_to_hl[ml]
                                     : ".";
                out +=
                    (size_t)snprintf(buf + out, sizeof(buf) - out, "(%s)", hl);
              } else {
                buf[out++] = *m;
              }
              m++;
              if (dir == 0) {
                c++;
              } else {
                r++;
              }
            }
            buf[out] = '\0';
            snprintf(entry->move_str, sizeof(entry->move_str), "%s", buf);
          } else {
            snprintf(entry->move_str, sizeof(entry->move_str), "%s", cgp_move);
          }
        } else {
          snprintf(entry->move_str, sizeof(entry->move_str), "%s", cgp_move);
        }
      }
    }
    if (rack != NULL && !rack_is_empty(rack)) {
      StringBuilder *rsb = string_builder_create();
      string_builder_add_rack(rsb, rack, state->ld, false);
      char *rack_dump = string_builder_dump(rsb, NULL);
      if (rack_dump != NULL) {
        snprintf(entry->rack_str, sizeof(entry->rack_str), "%s", rack_dump);
        free(rack_dump);
      }
      string_builder_destroy(rsb);

      // Leave = rack − tiles played by this event. Mirrors
      // what the bot-worker stashes for live turns and lets
      // the Sim/Analysis panel show "move · leave · score"
      // when the user is reviewing a loaded GCG and no sim
      // ran for the turn.
      Rack *leave = rack_duplicate(rack);
      if (etype == GAME_EVENT_TILE_PLACEMENT_MOVE && meta->has_position) {
        for (const char *p = meta->word; *p != '\0'; p++) {
          if (*p == '.') {
            continue;
          }
          MachineLetter ml = 0;
          if (*p >= 'A' && *p <= 'Z') {
            ml = (MachineLetter)(*p - 'A' + 1);
          } else if (*p >= 'a' && *p <= 'z') {
            ml = 0; // newly played blank
          } else {
            continue;
          }
          if (rack_get_letter(leave, ml) > 0) {
            rack_take_letter(leave, ml);
          }
        }
      } else if (etype == GAME_EVENT_EXCHANGE && cgp_move != NULL) {
        const char *t = NULL;
        if (strncmp(cgp_move, "ex ", 3) == 0) {
          t = cgp_move + 3;
        } else if (strncmp(cgp_move, "(exch ", 6) == 0) {
          t = cgp_move + 6;
        } else if (cgp_move[0] == '-') {
          t = cgp_move + 1;
        }
        if (t != NULL) {
          for (; *t != '\0' && *t != ')'; t++) {
            MachineLetter ml = 0;
            if (*t >= 'A' && *t <= 'Z') {
              ml = (MachineLetter)(*t - 'A' + 1);
            } else if (*t == '?' || (*t >= 'a' && *t <= 'z')) {
              ml = 0;
            } else {
              continue;
            }
            if (rack_get_letter(leave, ml) > 0) {
              rack_take_letter(leave, ml);
            }
          }
        }
      }
      // GAME_EVENT_PASS: leave is unchanged from rack.
      if (!rack_is_empty(leave)) {
        StringBuilder *lsb = string_builder_create();
        string_builder_add_rack(lsb, leave, state->ld, false);
        char *leave_dump = string_builder_dump(lsb, NULL);
        if (leave_dump != NULL) {
          snprintf(entry->leave_str, sizeof(entry->leave_str), "%s",
                   leave_dump);
          free(leave_dump);
        }
        string_builder_destroy(lsb);
      }
      rack_destroy(leave);
    }
  }

  // ── Pass 2: per-turn snapshots via incremental replay ────────
  ErrorStack *replay_err = error_stack_create();
  for (int k = 0; k < meta_count; k++) {
    const GcgEntryMeta *meta = &entry_meta[k];
    game_play_n_events(history, state->game, meta->engine_idx, false,
                       replay_err);
    if (!error_stack_is_empty(replay_err)) {
      error_stack_reset(replay_err);
      break;
    }
    TuiHistoryEntry *entry = &state->history[k];
    Board *board_snap = board_duplicate(game_get_board(state->game));
    // Stamp owners onto the snapshot for every move that
    // played before this one.
    for (int prior = 0; prior < k; prior++) {
      const GcgEntryMeta *pm = &entry_meta[prior];
      if (pm->has_position) {
        tui_apply_gcg_move_owner(board_snap, pm->row, pm->col, pm->dir,
                                 pm->word, pm->player_idx);
      }
    }
    entry->board_before = board_snap;

    // Pre-move rack snapshot from the event's own rack
    // field (more reliable than the engine's mid-replay
    // game state, which can have stale or empty racks
    // depending on which sub-step we stopped at).
    const GameEvent *event = game_history_get_event(history, meta->engine_idx);
    const Rack *evt_rack = game_event_get_const_rack(event);
    if (evt_rack != NULL && !rack_is_empty(evt_rack)) {
      entry->rack_before = rack_duplicate(evt_rack);
    }
    // Opponent's rack at this moment is approximated by
    // the rack the opponent had at THEIR next move (they
    // hadn't drawn anything between the two turns).
    for (int next = meta->engine_idx + 1; next < num_events; next++) {
      const GameEvent *nev = game_history_get_event(history, next);
      if (game_event_get_player_index(nev) == (1 - meta->player_idx)) {
        const Rack *nrack = game_event_get_const_rack(nev);
        if (nrack != NULL && !rack_is_empty(nrack)) {
          entry->opp_rack_before = rack_duplicate(nrack);
        }
        break;
      }
    }
  }

  // ── Pass 3: restore final state, stamp live-board owners ─────
  game_play_n_events(history, state->game, num_events, false, replay_err);
  error_stack_reset(replay_err);
  error_stack_destroy(replay_err);
  {
    Board *live_board = game_get_board(state->game);
    for (int k = 0; k < meta_count; k++) {
      const GcgEntryMeta *pm = &entry_meta[k];
      if (pm->has_position) {
        tui_apply_gcg_move_owner(live_board, pm->row, pm->col, pm->dir,
                                 pm->word, pm->player_idx);
      }
    }
  }

  // Land the user on turn 1 so the loaded game opens with
  // the first move highlighted and that turn's pre-move
  // board / rack already visible.
  if (state->history_count > 0) {
    state->history_cursor = 0;
  }
}
