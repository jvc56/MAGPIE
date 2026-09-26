#ifndef TUI_UI_TYPES_H
#define TUI_UI_TYPES_H

// UI enums shared by the renderer and input handling. Kept in a leaf
// header (no other tui/ includes) so render and input modules can use
// them without depending on game_render.h.

// Which modal (if any) is currently open. Drives both rendering of the
// modal itself and what control hints the status bar advertises.
typedef enum {
  TUI_MODAL_NONE = 0,
  TUI_MODAL_MAIN_MENU = 1,
  TUI_MODAL_SETTINGS = 2,
  TUI_MODAL_TIME_PICKER = 3,
  TUI_MODAL_QUIT_CONFIRM = 4,
  TUI_MODAL_STARTUP_MENU = 5,
  TUI_MODAL_WATCH_SETUP = 6,
  TUI_MODAL_LOAD_POSITION = 7,
  TUI_MODAL_LOAD_GAME = 8,
  TUI_MODAL_ANNOTATE_SETUP = 9,
  TUI_MODAL_PLAY_SETUP = 10,
  TUI_MODAL_ANALYSIS_MENU = 11,
  TUI_MODAL_PHONY_CONFIRM = 12,
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

// Annotate-game setup modal. Lets the user pick the lexicon and
// both player names before entering annotation mode (where the
// game starts with an empty board, full bag, and no racks drawn
// — the annotator fills in racks + moves manually as the live
// game plays out).
typedef enum {
  TUI_ANNOTATE_SETUP_LANGUAGE = 0,
  TUI_ANNOTATE_SETUP_LEXICON = 1,
  TUI_ANNOTATE_SETUP_P1_NAME = 2,
  TUI_ANNOTATE_SETUP_P2_NAME = 3,
  TUI_ANNOTATE_SETUP_START = 4,
  TUI_ANNOTATE_SETUP_ITEM_COUNT = 5,
} TuiAnnotateSetupItem;

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

// Main menu modal — rendered on top of the game frame when the user
// presses Esc.
typedef enum {
  TUI_MENU_NEW_GAME = 0,
  TUI_MENU_SETTINGS = 1,
  TUI_MENU_BACK = 2,
  TUI_MENU_QUIT = 3,
  TUI_MENU_ITEM_COUNT = 4,
} TuiMenuItem;

// The analysis panel's menu (Space/Enter on its [5] badge): what can be
// done with the turn selected in History.
typedef enum {
  TUI_ANALYSIS_MENU_SIM = 0,
  TUI_ANALYSIS_MENU_KIBITZ = 1,
  TUI_ANALYSIS_MENU_RESUME = 2,
  TUI_ANALYSIS_MENU_STOP = 3,
  TUI_ANALYSIS_MENU_BACK = 4,
  TUI_ANALYSIS_MENU_ITEM_COUNT = 5,
} TuiAnalysisMenuItem;

// Annotation's phony dialog: a play forms words not in the lexicon.
typedef enum {
  TUI_PHONY_CONFIRM_UNDO = 0,
  TUI_PHONY_CONFIRM_KEEP = 1,
  TUI_PHONY_CONFIRM_ITEM_COUNT = 2,
} TuiPhonyConfirmItem;

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

// Watch-game setup modal. Lets the user pick time control, lexicon,
// and sim parameters before starting a bot-vs-bot game. Pre-focused
// on the "Start" row so Enter immediately starts with the
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

// Analysis cursor column. The Analysis cursor remembers not just
// which row it's on but which "column" — rank-anchored (cursor
// pins to the row index, ignoring sim reorderings) or move-
// anchored (cursor pins to a specific move and follows it as the
// leaderboard reorders). Left/Right arrows toggle the column.
typedef enum {
  TUI_ANALYSIS_COLUMN_RANK = 0,
  TUI_ANALYSIS_COLUMN_MOVE = 1,
} TuiAnalysisColumn;

#endif
