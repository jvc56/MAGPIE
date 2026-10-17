#ifndef TUI_SETTINGS_TABLE_H
#define TUI_SETTINGS_TABLE_H

#include "game_state.h"
#include "tui_ui_state.h"
#include <stdbool.h>
#include <stddef.h>

// Every user setting, in one table that the Settings dialog, the panel
// menus, and "/set <key> <value>" (with its autocomplete) all read, so a
// setting can't exist in one without the others. Each belongs to the
// panel whose display or behavior it changes, or to General.
typedef enum {
  TUI_SETTING_SCALE,
  TUI_SETTING_ANTIALIAS,
  TUI_SETTING_SUBSCRIPTS,
  TUI_SETTING_BORDER,
  TUI_SETTING_PREMIUM_LABELS,
  TUI_SETTING_BLANKS,
  TUI_SETTING_RACK_SORT,
  TUI_SETTING_SIM_PLIES,
  TUI_SETTING_SIM_CANDIDATES,
  TUI_SETTING_THEME,
  TUI_SETTING_RIT,
  TUI_SETTING_COUNT,
} TuiSettingId;

// The panel a setting belongs to: its panel number (1 Board ... 5
// Analysis), or General for app-wide settings.
enum { TUI_SETTING_PANEL_GENERAL = 0 };

typedef enum {
  TUI_SETTING_KIND_BOOL,   // off / on
  TUI_SETTING_KIND_CHOICE, // one of `choices`, stored as its index
  TUI_SETTING_KIND_INT,    // min..max
} TuiSettingKind;

typedef struct {
  TuiSettingId id;
  int panel;
  const char *key;   // "/set" name
  const char *label; // dialog label
  const char *help;  // one line, for the help line and autocomplete
  TuiSettingKind kind;
  const char *const *choices; // CHOICE: value names, also the "/set" values
  int choice_count;
  int min; // INT range; BOOL and CHOICE use 0..count-1
  int max;
  int step;               // INT: Left/Right step in the dialogs
  const char *unit;       // INT: shown after the number ("px", "plies")
  const char *zero_label; // INT: shown instead of 0 ("off"), or NULL
} TuiSettingDef;

// Every setting in display order: grouped by panel, Board first and
// General last. `*count` receives the length.
const TuiSettingDef *tui_setting_defs(int *count);

const TuiSettingDef *tui_setting_def(TuiSettingId id);

// "Board", "Rack", ..., "General".
const char *tui_setting_panel_title(int panel);

// The setting `typed` (its first `len` characters) names: an exact key,
// else the only key it is a prefix of. NULL when none or ambiguous.
const TuiSettingDef *tui_setting_resolve(const char *typed, int len);

// Current value (a CHOICE's index, a BOOL's 0/1). Caller holds
// state->mutex.
int tui_setting_get(const TuiGameState *state, TuiSettingId id);

// Why the setting can't be changed right now, or NULL when it can.
// Caller holds state->mutex.
const char *tui_setting_unavailable_reason(const TuiGameState *state,
                                           const TuiSession *session,
                                           TuiSettingId id);

// `value` as the dialogs show it: "on", "2x", "3px", "off", "4 plies".
void tui_setting_format(const TuiSettingDef *def, int value, char *out,
                        size_t out_size);

// The values the setting accepts, for autocomplete: "on | off",
// "1x | 2x", "0-6 px (0 = off)".
void tui_setting_describe_values(const TuiSettingDef *def, char *out,
                                 size_t out_size);

// Parses a "/set" value: a choice name or unique prefix of one (any
// case), on/off/true/false/yes/no/1/0 for BOOL, or a number in range
// for INT ("off" for 0 when the setting has a zero_label). Returns
// false, with a message in `err`, when it doesn't parse or is out of
// range.
bool tui_setting_parse(const TuiSettingDef *def, const char *text,
                       int *out_value, char *err, size_t err_size);

// The value name (a choice, or on/off) the first `len` characters of
// `typed` are the unique prefix of, for Tab completion; NULL when none,
// several, or the setting takes a number.
const char *tui_setting_complete_value(const TuiSettingDef *def,
                                       const char *typed, int len);

// Sets the value, applies it to the running game, and saves it to the
// config (unless the session runs without one). Clamps INT values and
// ignores out-of-range BOOL / CHOICE values. Takes state->mutex.
void tui_setting_set(TuiGameState *state, TuiSession *session, TuiSettingId id,
                     int value);

// Left/Right in a dialog: cycles a BOOL or CHOICE by `dir`, steps an
// INT by its step (clamped). Takes state->mutex.
void tui_setting_step(TuiGameState *state, TuiSession *session, TuiSettingId id,
                      int dir);

// Every setting's value, for carrying them across a game-state re-create
// (which starts from defaults): snapshot before, restore after. Restore
// doesn't save the config, which already holds them. Caller holds
// state->mutex, or no other thread touches the state yet.
void tui_settings_snapshot(const TuiGameState *state,
                           int values[TUI_SETTING_COUNT]);
void tui_settings_restore(TuiGameState *state, TuiSession *session,
                          const int values[TUI_SETTING_COUNT]);

// One row of the Settings dialog: a panel's heading, a setting, or Back.
typedef enum {
  TUI_SETTINGS_ROW_HEADING,
  TUI_SETTINGS_ROW_SETTING,
  TUI_SETTINGS_ROW_BACK,
} TuiSettingsRowKind;

typedef struct {
  TuiSettingsRowKind kind;
  int panel;       // HEADING
  TuiSettingId id; // SETTING
} TuiSettingsRow;

enum { TUI_SETTINGS_MAX_ROWS = TUI_SETTING_COUNT + 8 };

// The Settings dialog's rows: each panel's heading followed by its
// settings, then Back. Returns the row count.
int tui_settings_rows(TuiSettingsRow rows[TUI_SETTINGS_MAX_ROWS]);

#endif
