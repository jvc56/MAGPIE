#ifndef TUI_RENDER_MODALS_H
#define TUI_RENDER_MODALS_H

#include "config.h"
#include "game_state.h"
#include "theme.h"
#include "tui_ui_types.h"
#include <notcurses/notcurses.h>
#include <stdbool.h>

// Play-vs-computer setup modal; rows are TuiPlaySetupItem.
void tui_game_render_play_setup(
    struct ncplane *plane, const Theme *theme, int focus,
    const char *human_name, const char *computer_name, int first_move,
    int name_edit_pos, int time_seconds, UiOvertimeRule overtime_rule,
    int overtime_cap_minutes, UiTimePenaltyRate time_penalty_rate,
    UiChallengeRule challenge_rule, UiChallengePenalty challenge_penalty,
    const char *language, const char *lexicon, int sim_plies,
    int sim_candidates);

// The Play-setup Overtime row's one scale: step 0 is no overtime (lose at
// 0:00), 1..TUI_OVERTIME_MAX_CAP_MINUTES a cap in minutes, the last step
// unlimited. Maps the overtime rule and cap to a step and back (a step
// keeps `*cap_minutes` unless it sets one).
enum {
  TUI_OVERTIME_MAX_CAP_MINUTES = 10,
  TUI_OVERTIME_STEP_COUNT = TUI_OVERTIME_MAX_CAP_MINUTES + 2,
};
int tui_overtime_step(UiOvertimeRule overtime_rule, int cap_minutes);
void tui_overtime_from_step(int step, UiOvertimeRule *overtime_rule,
                            int *cap_minutes);

// Which Play-setup rows are adjustable given the current rule
// settings: the overtime-cap row only applies under UI_OVERTIME_MAX,
// the time-penalty row never applies under UI_OVERTIME_FLAG (the game
// simply ends at 0:00), every clock-dependent row is off when the
// game is untimed (time_seconds <= 0), and the challenge-penalty row
// only applies under UI_CHALLENGE_PENALTY. Disabled rows render
// dimmed and are skipped by cursor navigation / clicks.
void tui_play_setup_enabled_rows(UiOvertimeRule overtime_rule, int time_seconds,
                                 UiChallengeRule challenge_rule,
                                 bool out_enabled[TUI_PLAY_SETUP_ITEM_COUNT]);

// Annotate-game setup modal; rows are TuiAnnotateSetupItem.
void tui_game_render_annotate_setup(struct ncplane *plane, const Theme *theme,
                                    int focus, const char *language,
                                    const char *lexicon, const char *p1_name,
                                    const char *p2_name, int name_edit_pos);

// Main menu modal (Esc); items are TuiMenuItem.
void tui_game_render_menu(struct ncplane *plane, const Theme *theme, int focus);

// Annotation's phony dialog, titled with the words `lexicon` lacks.
void tui_game_render_phony_confirm(struct ncplane *plane, const Theme *theme,
                                   int focus, const char *lexicon,
                                   const char *words);

// Startup menu modal; items are TuiStartupItem.
void tui_game_render_startup_menu(struct ncplane *plane, const Theme *theme,
                                  int focus);

// Watch-game setup modal; rows are TuiWatchSetupItem.
void tui_game_render_watch_setup(struct ncplane *plane, const Theme *theme,
                                 int focus, int time_seconds,
                                 const char *language, const char *lexicon,
                                 int sim_plies, int sim_candidates);

// Load-position modal. A multi-line text input where the user
// can paste a raw CGP string or drag a .cgp file in (which most
// terminals translate to a paste of the file's path). The
// position is parsed live and previewed behind the modal;
// Enter loads (if the buffer parses cleanly), Esc cancels.
// `buf` is the editable text; `cursor` is the byte offset of
// the insertion point. `error` is an optional message to
// display below the input area (NULL or empty = none).
void tui_game_render_load_position(struct ncplane *plane, const Theme *theme,
                                   const char *buf, int cursor,
                                   const char *error);

// Load-game modal — same shape as load-position but for GCG
// (game record) input. Buffer holds a raw GCG or a dragged file
// path; the position is parsed live, with the final-state board
// previewed behind the modal. Enter commits, Esc cancels.
void tui_game_render_load_game(struct ncplane *plane, const Theme *theme,
                               const char *buf, int cursor, const char *error);

// Time-picker modal — opened from "New game" in the main menu.
// Items come from time_picker.h's preset accessors.
void tui_game_render_time_picker(struct ncplane *plane, const Theme *theme,
                                 int focus);

// Quit-confirmation modal. Two items: No (focus 0) / Yes (focus 1).
// Default focus is 0 (No) so the safer option is Enter-confirmable;
// Y / N shortcuts trigger their action regardless of current focus.
void tui_game_render_quit_confirm(struct ncplane *plane, const Theme *theme,
                                  int focus);

// Settings dialog, or a panel menu's settings section: `row_count` rows
// of `labels` with their `values` (NULL for a row without one, like
// Back). `heading` rows are section titles, never focused; `unavailable`
// rows are dimmed but still focusable, so the help line can say why.
// Scrolls when the rows don't fit, keeping `focus` in view; `*scroll` is
// the first row shown, updated here and read by click handling.
void tui_game_render_settings(struct ncplane *plane, const Theme *theme,
                              const char *title, const char *const *labels,
                              const char *const *values, const bool *heading,
                              const bool *unavailable, int row_count, int focus,
                              int *scroll);

// One-line description of row `focus` in `modal`, shown on the
// command-bar row while the dialog is open; NULL when there is none.
const char *tui_modal_help(TuiModalState modal, int focus);

#endif
