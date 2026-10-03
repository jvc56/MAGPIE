#include "gcg_export.h"

#include "../src/util/string_util.h"
#include "game_state.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { GCG_NICKNAME_MAX = 32, ENGINE_MOVE_MAX = 64 };

// A GCG nickname can't hold spaces: `name` with them turned into
// underscores, or "player<n>" when it's empty.
static void gcg_nickname(const char *name, int player_idx, char *out,
                         size_t out_size) {
  if (name == NULL || name[0] == '\0') {
    (void)snprintf(out, out_size, "player%d", player_idx + 1);
    return;
  }
  (void)snprintf(out, out_size, "%s", name);
  for (char *ch = out; *ch != '\0'; ch++) {
    if (*ch == ' ' || *ch == '\t') {
      *ch = '_';
    }
  }
}

// The turn's GCG move ("8H V.X", "-ABC", or "-" for a pass) and, into
// `played`, the tiles it put down or exchanged (a stand-in rack when the
// turn has none recorded). Returns false when the entry has no move.
static bool gcg_move(const TuiHistoryEntry *entry, char *move, size_t move_size,
                     char *played, size_t played_size) {
  char engine[ENGINE_MOVE_MAX];
  tui_history_move_to_engine(entry->move_str, engine, sizeof(engine));
  played[0] = '\0';
  if (engine[0] == '\0') {
    return false;
  }
  if (strcmp(engine, "pass") == 0) {
    (void)snprintf(move, move_size, "-");
    return true;
  }
  if (strncmp(engine, "ex ", 3) == 0) {
    (void)snprintf(move, move_size, "-%s", engine + 3);
    (void)snprintf(played, played_size, "%s", engine + 3);
    return true;
  }
  (void)snprintf(move, move_size, "%s", engine);
  const char *word = strchr(engine, ' ');
  size_t used = 0;
  for (const char *ch = word != NULL ? word + 1 : ""; *ch != '\0'; ch++) {
    if (*ch == '.' || used + 1 >= played_size) {
      continue;
    }
    // A played blank is a lowercase letter; on the rack it's "?".
    if (*ch >= 'a' && *ch <= 'z') {
      played[used++] = '?';
    } else {
      played[used++] = *ch;
    }
  }
  played[used] = '\0';
  return true;
}

bool tui_gcg_save(const TuiGameState *state, const char *path) {
  char *text = tui_gcg_export(state);
  FILE *file = fopen(path, "we");
  if (file == NULL) {
    free(text);
    return false;
  }
  const size_t len = strlen(text);
  const bool wrote = fwrite(text, 1, len, file) == len;
  free(text);
  return fclose(file) == 0 && wrote;
}

void tui_gcg_default_path(char *out, size_t out_size) {
  const time_t now = time(NULL);
  struct tm local;
  localtime_r(&now, &local);
  char stamp[32];
  (void)strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
  (void)snprintf(out, out_size, "magpie-%s.gcg", stamp);
}

char *tui_gcg_export(const TuiGameState *state) {
  StringBuilder *sb = string_builder_create();
  char nicknames[2][GCG_NICKNAME_MAX];
  string_builder_add_string(sb, "#character-encoding UTF-8\n");
  string_builder_add_string(sb, "#description Created with MAGPIE TUI\n");
  if (state->active_lexicon[0] != '\0') {
    string_builder_add_formatted_string(sb, "#lexicon %s\n",
                                        state->active_lexicon);
  }
  for (int player_idx = 0; player_idx < 2; player_idx++) {
    const char *name = state->player_names[player_idx];
    gcg_nickname(name, player_idx, nicknames[player_idx],
                 sizeof(nicknames[player_idx]));
    string_builder_add_formatted_string(
        sb, "#player%d %s %s\n", player_idx + 1, nicknames[player_idx],
        name[0] != '\0' ? name : nicknames[player_idx]);
  }
  for (int idx = 0; idx < state->history_count; idx++) {
    const TuiHistoryEntry *entry = &state->history[idx];
    if (entry->pending || entry->kind == TUI_HISTORY_ENTRY_TIME_FORFEIT) {
      continue;
    }
    const char *nick = nicknames[entry->player_idx & 1];
    if (entry->kind == TUI_HISTORY_ENTRY_TIME_PENALTY) {
      string_builder_add_formatted_string(sb, ">%s: %s (time) %d %d\n", nick,
                                          entry->rack_str, entry->score,
                                          entry->total_after);
      continue;
    }
    char move[ENGINE_MOVE_MAX];
    char played[ENGINE_MOVE_MAX];
    if (!gcg_move(entry, move, sizeof(move), played, sizeof(played))) {
      continue;
    }
    // Mid-game against the computer, its racks stay hidden, as on screen.
    const bool conceal = state->app_mode == TUI_APP_MODE_PLAY_VS_COMPUTER &&
                         !tui_game_state_play_over(state) &&
                         entry->player_idx != state->human_player_idx;
    const char *rack =
        entry->rack_str[0] != '\0' && !conceal ? entry->rack_str : played;
    // total_after includes a challenge bonus, which GCG lists as its own
    // line after the play.
    const int after_play = entry->total_after - entry->challenge_bonus;
    string_builder_add_formatted_string(sb, ">%s: %s %s +%d %d\n", nick, rack,
                                        move, entry->score, after_play);
    if (entry->challenged_off) {
      string_builder_add_formatted_string(sb, ">%s: %s -- -%d %d\n", nick, rack,
                                          entry->score,
                                          after_play - entry->score);
    }
    if (entry->challenge_bonus != 0) {
      string_builder_add_formatted_string(sb, ">%s: %s (challenge) +%d %d\n",
                                          nick, rack, entry->challenge_bonus,
                                          entry->total_after);
    }
    // Going out earns the opponent's leftover tiles ("(EE) +4"); a
    // game ended by passes costs each player their own ("EE (EE) -4").
    if (entry->end_bonus > 0) {
      string_builder_add_formatted_string(
          sb, ">%s: (%s) +%d %d\n", nick, entry->end_rack_str, entry->end_bonus,
          entry->total_after + entry->end_bonus);
    } else if (entry->end_bonus < 0) {
      string_builder_add_formatted_string(
          sb, ">%s: %s (%s) %d %d\n", nick, entry->end_rack_str,
          entry->end_rack_str, entry->end_bonus,
          entry->total_after + entry->end_bonus);
    }
  }
  char *text = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  return text;
}
