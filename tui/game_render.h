#ifndef TUI_GAME_RENDER_H
#define TUI_GAME_RENDER_H

#include "game_state.h"
#include "theme.h"
#include <notcurses/notcurses.h>
#include <stdbool.h>

struct Game;
struct LetterDistribution;

// Which modal (if any) is currently open. Drives both rendering of the
// modal itself and what control hints the status bar advertises.
typedef enum {
  TUI_MODAL_NONE = 0,
  TUI_MODAL_MAIN_MENU = 1,
  TUI_MODAL_SETTINGS = 2,
  TUI_MODAL_TIME_PICKER = 3,
  TUI_MODAL_LEXICON_PICKER = 4,
  TUI_MODAL_QUIT_CONFIRM = 5,
  TUI_MODAL_STARTUP_MENU = 6,
  TUI_MODAL_WATCH_SETUP = 7,
  TUI_MODAL_LOAD_POSITION = 8,
  TUI_MODAL_LOAD_GAME = 9,
  TUI_MODAL_ANNOTATE_SETUP = 10,
  TUI_MODAL_PLAY_SETUP = 11,
} TuiModalState;

// Play-vs-computer setup modal. A single modal with everything: editable
// human + computer names, who moves first, time control, language /
// lexicon, and the simmer (computer-strength) params, then Start. The
// human's seat is derived from the first-move choice.
typedef enum {
  TUI_PLAY_SETUP_HUMAN_NAME = 0,
  TUI_PLAY_SETUP_COMPUTER_NAME = 1,
  TUI_PLAY_SETUP_FIRST_MOVE = 2,
  TUI_PLAY_SETUP_TIME = 3,
  TUI_PLAY_SETUP_OVERTIME = 4,
  TUI_PLAY_SETUP_OVERTIME_CAP = 5,
  TUI_PLAY_SETUP_TIME_PENALTY = 6,
  TUI_PLAY_SETUP_CHALLENGE = 7,
  TUI_PLAY_SETUP_CHALLENGE_PENALTY = 8,
  TUI_PLAY_SETUP_LANGUAGE = 9,
  TUI_PLAY_SETUP_LEXICON = 10,
  TUI_PLAY_SETUP_SIM_PLIES = 11,
  TUI_PLAY_SETUP_SIM_CANDIDATES = 12,
  TUI_PLAY_SETUP_START = 13,
  TUI_PLAY_SETUP_ITEM_COUNT = 14,
} TuiPlaySetupItem;

// First-move choice on the play-setup modal.
typedef enum {
  TUI_PLAY_FIRST_RANDOM = 0,
  TUI_PLAY_FIRST_HUMAN = 1,
  TUI_PLAY_FIRST_COMPUTER = 2,
  TUI_PLAY_FIRST_COUNT = 3,
} TuiPlayFirstMove;

void tui_game_render_play_setup(
    struct ncplane *plane, const Theme *theme, int focus,
    const char *human_name, const char *computer_name, int first_move,
    int name_edit_pos, int time_seconds, UiOvertimeRule overtime_rule,
    int overtime_cap_minutes, UiTimePenaltyRate time_penalty_rate,
    UiChallengeRule challenge_rule, UiChallengePenalty challenge_penalty,
    const char *language, const char *lexicon, int sim_plies,
    int sim_candidates);

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

// Annotate-game setup modal. Lets the user pick the lexicon and
// both player names before entering annotation mode (where the
// game starts with an empty board, full bag, and no racks drawn
// — the annotator fills in racks + moves manually as the live
// game plays out).
typedef enum {
  TUI_ANNOTATE_SETUP_LEXICON = 0,
  TUI_ANNOTATE_SETUP_P1_NAME = 1,
  TUI_ANNOTATE_SETUP_P2_NAME = 2,
  TUI_ANNOTATE_SETUP_START = 3,
  TUI_ANNOTATE_SETUP_ITEM_COUNT = 4,
} TuiAnnotateSetupItem;

void tui_game_render_annotate_setup(struct ncplane *plane, const Theme *theme,
                                    int focus, const char *lexicon,
                                    const char *p1_name, const char *p2_name,
                                    int name_edit_pos);

// Which panel currently has keyboard focus. NONE means no panel is
// focused — pressing 1..5 selects the matching panel; 0 unfocuses.
// Panel-specific behaviors (history scrollback, analysis candidate
// preview, etc.) consult this enum to decide whether to intercept
// keys; render code uses it to bold/highlight the focused panel's
// [N] indicator.
typedef enum {
  TUI_FOCUS_NONE = 0,
  TUI_FOCUS_BOARD = 1,
  TUI_FOCUS_RACK = 2,
  TUI_FOCUS_BAG = 3,
  TUI_FOCUS_HISTORY = 4,
  TUI_FOCUS_ANALYSIS = 5,
} TuiPanelFocus;

void tui_game_render(struct ncplane *plane, const Theme *theme,
                     const TuiGameState *state, int time_per_side_seconds,
                     TuiModalState modal);

// Main menu modal — rendered on top of the game frame when the user
// presses Esc.
typedef enum {
  TUI_MENU_NEW_GAME = 0,
  TUI_MENU_SETTINGS = 1,
  TUI_MENU_BACK = 2,
  TUI_MENU_QUIT = 3,
  TUI_MENU_ITEM_COUNT = 4,
} TuiMenuItem;

void tui_game_render_menu(struct ncplane *plane, const Theme *theme, int focus);

// Startup-menu modal. Shown at app launch (instead of jumping
// directly into a bot-vs-bot game) and reachable from Esc → "New
// game". Lets the user choose how they'd like to use MAGPIE:
// watch the bots play, load a position / game, annotate a live
// OTB game, or play against the computer. Unbuilt modes render
// dimmed with a "(coming soon)" trailer and can't be cursored.
typedef enum {
  TUI_STARTUP_WATCH = 0,
  TUI_STARTUP_LOAD_POSITION = 1,
  TUI_STARTUP_LOAD_GAME = 2,
  TUI_STARTUP_ANNOTATE = 3,
  TUI_STARTUP_PLAY_VS_COMPUTER = 4,
  TUI_STARTUP_ITEM_COUNT = 5,
} TuiStartupItem;

void tui_game_render_startup_menu(struct ncplane *plane, const Theme *theme,
                                  int focus);

// Watch-game setup modal. Lets the user pick time control, lexicon,
// and sim parameters before starting a bot-vs-bot game. Pre-focused
// on the "Start game" row so Enter immediately starts with the
// currently-displayed values.
typedef enum {
  TUI_WATCH_SETUP_TIME = 0,
  TUI_WATCH_SETUP_LANGUAGE = 1,
  TUI_WATCH_SETUP_LEXICON = 2,
  TUI_WATCH_SETUP_SIM_PLIES = 3,
  TUI_WATCH_SETUP_SIM_CANDIDATES = 4,
  TUI_WATCH_SETUP_START = 5,
  TUI_WATCH_SETUP_ITEM_COUNT = 6,
} TuiWatchSetupItem;

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

// Settings modal — opened from the main menu. Left/Right arrows on the
// focused row adjust that setting's value.
typedef enum {
  TUI_SETTINGS_SCALE = 0,
  TUI_SETTINGS_AA = 1,
  TUI_SETTINGS_SUBSCRIPTS = 2,
  TUI_SETTINGS_BORDER = 3,
  TUI_SETTINGS_PREMIUM = 4,
  TUI_SETTINGS_BLANKS = 5,
  TUI_SETTINGS_RACK_SORT = 6,
  TUI_SETTINGS_RIT = 7,
  TUI_SETTINGS_BACK = 8,
  TUI_SETTINGS_ITEM_COUNT = 9,
} TuiSettingsItem;

// `board_scale` is 1 or 2; the scale row is grayed out when 2x is
// unavailable (no pixel support or font load failed). `antialias`
// applies to the 2x render only and is grayed at 1x. `score_subscripts`
// is also 2x-only. `border_thickness` is the current pixel-grid
// thickness (0..6). `pixel_supported` is true when the host terminal
// can render pixel graphics. `font_available` is true when the bundled
// TTF loaded. `premium_labels` selects the TW/tw/none labeling style
// for premium squares. `blank_uppercase` controls whether played blanks
// render uppercase (with blank_tile_fg) or lowercase (with tile_fg).
// Shared modal-plane accessor (used by render_modal and the lexicon
// picker). Creates the plane on first use; resizes/repositions on
// subsequent calls. The plane is destroyed when tui_game_render sees
// modal == TUI_MODAL_NONE, so callers don't manage its lifetime.
struct ncplane *
tui_game_render_get_or_create_modal_plane(struct ncplane *parent, int top,
                                          int left, int rows, int cols);

// Hit-test a (y, x) cell coordinate against the panel layout that
// tui_game_render would draw for the given state on the given plane.
// Returns the TuiPanelFocus index of the panel containing the click,
// or -1 if the click misses every panel.
int tui_game_panel_at(struct ncplane *plane, const TuiGameState *state, int y,
                      int x);

// Map a screen (y, x) cell coordinate to a board cell. Returns true and
// fills *out_row / *out_col (both 0-based, 0..BOARD_DIM-1) when the point
// lands inside the 15x15 grid; false otherwise. Uses the same layout /
// scale selection as tui_game_panel_at, so it handles both 1x text and
// 2x pixel board rendering. Only meaningful right after tui_game_render
// has run on the same plane geometry.
bool tui_board_cell_at(struct ncplane *plane, const TuiGameState *state, int y,
                       int x, int *out_row, int *out_col);

// Hit-test (y, x) against the most-recently-rendered modal (one
// of the modals routed through render_modal[_ex]: main menu,
// startup menu, settings, time picker, quit confirm, watch
// setup). Returns:
//   -2 — point is outside the modal entirely
//   -1 — point is inside the modal but on chrome (title, border,
//        or a disabled item). Caller should absorb the click
//        without activating anything.
//   0..N-1 — the item index that was clicked, ready for the
//        caller to treat as if the user pressed Enter on that
//        focused item.
int tui_modal_item_at(int y, int x);

// Adjuster-row chevron click detection. For settings / watch-
// setup rows that render with "◀ value ▶" decorations, this
// reports which chevron (if any) the click landed on. Caller
// should treat LEFT/RIGHT hits as if the user pressed the
// corresponding arrow key on the focused row.
typedef enum {
  TUI_MODAL_CHEVRON_NONE = 0,
  TUI_MODAL_CHEVRON_LEFT,  // ◀ — synthesize NCKEY_LEFT
  TUI_MODAL_CHEVRON_RIGHT, // ▶ — synthesize NCKEY_RIGHT
} TuiModalChevron;
TuiModalChevron tui_modal_chevron_at(int y, int x);

// Hit-test (y, x) against the History panel's last-rendered entry
// rectangles. Returns:
//   -2 — the point is outside the History panel entirely
//   -1 — inside the panel but not on a specific entry row (title,
//        chrome, empty space below the entries) — caller should
//        snap history_cursor to -1 (the [4>] label)
//   0..N-1 — the history entry index that was clicked
//
// Reads a per-frame cache the renderer populates each tick, so this
// is only meaningful right after tui_game_render has run on the
// same plane geometry.
int tui_history_cursor_at(int y, int x);

// Like tui_history_cursor_at, but also reports which row of the
// entry was clicked: 0 = row 1 (the move text); 1 = row 2 (the
// rack); >= 2 = end-bonus rows (treated as MOVE for now). Used
// by the annotation cell editor to route clicks to the right
// edit field. *out_field is set only when the return is >= 0.
int tui_history_cursor_field_at(int y, int x, int *out_field);

// Analysis cursor column. The Analysis cursor remembers not just
// which row it's on but which "column" — rank-anchored (cursor
// pins to the row index, ignoring sim reorderings) or move-
// anchored (cursor pins to a specific move and follows it as the
// leaderboard reorders). Left/Right arrows toggle the column.
typedef enum {
  TUI_ANALYSIS_COLUMN_RANK = 0,
  TUI_ANALYSIS_COLUMN_MOVE = 1,
} TuiAnalysisColumn;

// Hit-test (y, x) against the Analysis panel's last-rendered row
// rectangles. Returns:
//   -2 — point is outside the Analysis panel
//   -1 — inside the panel but not on a specific row (title /
//        column-header strip / blank space below the list); caller
//        should snap analysis_cursor back to -1 (the [5>] label)
//   0..N-1 — the row index that was clicked
int tui_analysis_cursor_at(int y, int x);

// Like tui_analysis_cursor_at, but also reports which column of the
// row was clicked via *out_column. Returns the same row-index code
// as tui_analysis_cursor_at. *out_column is set to the appropriate
// TuiAnalysisColumn value when the click hits a row, and is left
// unchanged otherwise.
int tui_analysis_cursor_column_at(int y, int x, TuiAnalysisColumn *out_column);

// Populate a snapshot of the Analysis-panel contents for the
// currently-active sim or endgame solve. Called by the bot worker
// at finalize time (just after a move is chosen, just before
// play_move advances the board) so each history entry can
// preserve the analysis the user was looking at when the bot
// committed. Picks sim vs endgame the same way the live render
// does: endgame when the bag is empty and the endgame snapshot
// is valid, sim otherwise. Safe to call from any thread that
// already holds state->mutex.
void tui_capture_analysis_snapshot(const TuiGameState *state,
                                   TuiAnalysisSnapshot *out);

void tui_game_render_settings(struct ncplane *plane, const Theme *theme,
                              int focus, int board_scale, bool antialias,
                              TuiScoreSubscripts score_subscripts,
                              int border_thickness, bool pixel_supported,
                              bool font_available,
                              TuiPremiumLabels premium_labels,
                              bool blank_uppercase, TuiRackSort rack_sort,
                              const char *lexicon, bool load_rit);

// Render just the board cells (no row/col labels) at (top, left). Each
// cell is 2 columns wide; the rendered region is BOARD_DIM rows tall and
// BOARD_DIM*2 columns wide. When the host terminal supports pixel
// graphics and `border_thickness` is positive, a grid overlay is drawn
// on top using a private cached child plane.
void tui_render_board_at(struct ncplane *plane, int top, int left,
                         const Theme *theme, const struct Game *game,
                         const struct LetterDistribution *ld,
                         bool blank_uppercase, TuiPremiumLabels premium_labels,
                         int border_thickness);

// Destroy any cached pixel-grid child planes (board, rack, both pills,
// preview). Call after the theme picker exits so the preview's grid
// doesn't linger under the in-game UI, and on resize so a font-size
// change rebuilds them at the new cell-to-pixel ratio.
void tui_game_render_reset_grids(void);

// Alphagram-sort a rack-like input string according to the user's
// rack_sort preference (vowels-first, blanks-first, etc.). Output is
// the sorted form in a caller-owned buffer with a NUL terminator.
// Public form of the in-renderer helper so the annotation editor can
// canonicalize the rack buffer on focus-leave.
void tui_format_alphagram_for_sort(const char *in,
                                   const struct LetterDistribution *ld,
                                   TuiRackSort sort, char *out,
                                   size_t out_size);

#endif
