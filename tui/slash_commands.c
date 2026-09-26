#include "slash_commands.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>

static const TuiSlashCommand slash_commands[] = {
    {TUI_SLASH_COPY, "copy", "Copy current position to clipboard as CGP",
     "Copy position (CGP)"},
    {TUI_SLASH_COPY_GCG, "copygcg",
     "Copy the game so far to the clipboard as GCG", "Copy game (GCG)"},
    {TUI_SLASH_EXIT, "exit", "Quit MAGPIE TUI (alias for /quit)", NULL},
    {TUI_SLASH_KIBITZ, "gen", "Alias for /kibitz", NULL},
    {TUI_SLASH_KIBITZ, "generate", "Alias for /kibitz", NULL},
    {TUI_SLASH_KIBITZ, "kibitz",
     "Rank the selected turn's moves by static equity", "Kibitz (static eval)"},
    {TUI_SLASH_NEW, "new", "Start a new game", NULL},
    {TUI_SLASH_PLAY_FROM, "playfrom",
     "Take over the selected turn and play on against the computer",
     "Play on from here"},
    {TUI_SLASH_QUIT, "quit", "Quit MAGPIE TUI", NULL},
    {TUI_SLASH_RESUME, "resume",
     "Continue the selected turn's saved analysis (finished games)",
     "Resume saved analysis"},
    {TUI_SLASH_SAVE, "save", "Save the game as GCG: /save [file]",
     "Save game (GCG)"},
    {TUI_SLASH_SET, "set", "Change a setting: /set <name> <value>", NULL},
    {TUI_SLASH_SETTINGS, "settings", "Open settings", NULL},
    {TUI_SLASH_SIM, "sim", "Simulate the selected turn (continues a saved sim)",
     "Simulate"},
    {TUI_SLASH_STOP, "stop", "Stop the running analysis", "Stop analysis"},
    {TUI_SLASH_WATCH_FROM, "watchfrom",
     "Let the computer play on from the selected turn", "Watch from here"},
};

const TuiSlashCommand *tui_slash_command(TuiSlashCommandId id) {
  int count = 0;
  const TuiSlashCommand *cmds = tui_slash_commands(&count);
  for (int cmd_idx = 0; cmd_idx < count; cmd_idx++) {
    if (cmds[cmd_idx].id == id && cmds[cmd_idx].menu_label != NULL) {
      return &cmds[cmd_idx];
    }
  }
  for (int cmd_idx = 0; cmd_idx < count; cmd_idx++) {
    if (cmds[cmd_idx].id == id) {
      return &cmds[cmd_idx];
    }
  }
  return NULL;
}

const TuiSlashCommand *tui_slash_commands(int *count) {
  *count = (int)(sizeof(slash_commands) / sizeof(slash_commands[0]));
  return slash_commands;
}

bool tui_slash_command_matches(const TuiSlashCommand *cmd, const char *typed,
                               int len) {
  return (int)strlen(cmd->name) >= len &&
         strncasecmp(cmd->name, typed, (size_t)len) == 0;
}

void tui_slash_split(const char *buf, int len, TuiSlashWords *words) {
  words->count = 0;
  int pos = 0;
  while (words->count < TUI_SLASH_MAX_WORDS) {
    while (pos < len && buf[pos] == ' ') {
      pos++;
    }
    const int start = pos;
    while (pos < len && buf[pos] != ' ') {
      pos++;
    }
    if (pos == start) {
      // Nothing left. After a trailing space the next word has begun.
      if (len > 0 && buf[len - 1] == ' ') {
        words->start[words->count] = len;
        words->len[words->count] = 0;
        words->count++;
      }
      break;
    }
    words->start[words->count] = start;
    words->len[words->count] = pos - start;
    words->count++;
  }
}

const TuiSlashCommand *tui_slash_command_resolve(const char *typed, int len) {
  if (len <= 0) {
    return NULL;
  }
  int count = 0;
  const TuiSlashCommand *cmds = tui_slash_commands(&count);
  const TuiSlashCommand *prefix_match = NULL;
  int prefix_matches = 0;
  for (int cmd_idx = 0; cmd_idx < count; cmd_idx++) {
    if (!tui_slash_command_matches(&cmds[cmd_idx], typed, len)) {
      continue;
    }
    if ((int)strlen(cmds[cmd_idx].name) == len) {
      return &cmds[cmd_idx];
    }
    prefix_match = &cmds[cmd_idx];
    prefix_matches++;
  }
  return prefix_matches == 1 ? prefix_match : NULL;
}
