#ifndef TUI_SLASH_COMMANDS_H
#define TUI_SLASH_COMMANDS_H

#include <stdbool.h>

// The command bar's slash commands: one table that dispatch, Tab
// completion, and the "/" popup all read, so a command can't exist in
// one without the others.
typedef enum {
  TUI_SLASH_COPY,
  TUI_SLASH_COPY_GCG,
  TUI_SLASH_EXIT,
  TUI_SLASH_KIBITZ,
  TUI_SLASH_NEW,
  TUI_SLASH_QUIT,
  TUI_SLASH_RESUME,
  TUI_SLASH_SAVE,
  TUI_SLASH_SET,
  TUI_SLASH_SETTINGS,
  TUI_SLASH_SIM,
  TUI_SLASH_STOP,
  TUI_SLASH_COUNT,
} TuiSlashCommandId;

// `menu_label` names a command in the panel menus (panel_menu.c lists
// which panel shows it); NULL for commands no menu shows, and aliases.
typedef struct {
  TuiSlashCommandId id;
  const char *name;
  const char *desc;
  const char *menu_label;
} TuiSlashCommand;

// Every command, alphabetically by name (aliases are separate entries
// sharing an id); `*count` receives the length.
const TuiSlashCommand *tui_slash_commands(int *count);

// The command `id` names: its menu entry when it has one, else its
// first entry.
const TuiSlashCommand *tui_slash_command(TuiSlashCommandId id);

// Whether `name` starts with the first `len` characters of `typed`.
bool tui_slash_command_matches(const TuiSlashCommand *cmd, const char *typed,
                               int len);

enum { TUI_SLASH_MAX_WORDS = 4 };

// The words of a typed command line: `start[i]` / `len[i]` locate word i
// in the buffer. When the buffer ends in a space, a final empty word
// starts there, so the word being typed is always the last one.
typedef struct {
  int count;
  int start[TUI_SLASH_MAX_WORDS];
  int len[TUI_SLASH_MAX_WORDS];
} TuiSlashWords;

// Splits the first `len` characters of `buf` on spaces. Words past
// TUI_SLASH_MAX_WORDS are dropped.
void tui_slash_split(const char *buf, int len, TuiSlashWords *words);

// The command `typed` (its first `len` characters) names: an exact name,
// else the only command it is a prefix of. NULL when none or ambiguous.
const TuiSlashCommand *tui_slash_command_resolve(const char *typed, int len);

#endif
