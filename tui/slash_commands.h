#ifndef TUI_SLASH_COMMANDS_H
#define TUI_SLASH_COMMANDS_H

#include <stdbool.h>

// The command bar's slash commands: one table that dispatch, Tab
// completion, and the "/" popup all read, so a command can't exist in
// one without the others.
typedef enum {
  TUI_SLASH_COPY,
  TUI_SLASH_EXIT,
  TUI_SLASH_NEW,
  TUI_SLASH_QUIT,
  TUI_SLASH_RESUME,
  TUI_SLASH_SETTINGS,
  TUI_SLASH_STOP,
  TUI_SLASH_COUNT,
} TuiSlashCommandId;

typedef struct {
  TuiSlashCommandId id;
  const char *name;
  const char *desc;
} TuiSlashCommand;

// Every command, alphabetically by name; `*count` receives the length.
const TuiSlashCommand *tui_slash_commands(int *count);

// Whether `name` starts with the first `len` characters of `typed`.
bool tui_slash_command_matches(const TuiSlashCommand *cmd, const char *typed,
                               int len);

// The command `typed` (its first `len` characters) names: an exact name,
// else the only command it is a prefix of. NULL when none or ambiguous.
const TuiSlashCommand *tui_slash_command_resolve(const char *typed, int len);

#endif
