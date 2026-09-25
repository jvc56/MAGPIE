#ifndef TUI_UI_STATE_H
#define TUI_UI_STATE_H

#include "config.h"
#include "lexicon_picker.h"
#include "tui_ui_types.h"
#include <stdbool.h>

// UI state for the main loop: which modal is open, each modal's focus /
// return target, the setup dialogs' in-progress values, and the Load
// position / Load game text buffers. One instance lives in main().
typedef struct {
  bool mouse_enabled;
  bool running;
  int focus_state;
  bool focus_pending_esc;
  TuiModalState modal;
  int startup_menu_focus;
  int main_menu_focus;
  int settings_focus;
  TuiModalState settings_return;
  int time_focus;
  TuiModalState time_picker_return;
  TuiModalState startup_menu_return;
  int watch_setup_focus;
  char watch_setup_lexicon[TUI_LEXICON_NAME_MAX];
  int watch_setup_time;
  int play_setup_focus;
  char play_setup_human_name[32];
  char play_setup_computer_name[32];
  int play_setup_first_move;
  int play_setup_name_cursor;
  UiOvertimeRule play_setup_overtime_rule;
  int play_setup_overtime_cap;
  UiTimePenaltyRate play_setup_penalty_rate;
  UiChallengeRule play_setup_challenge_rule;
  UiChallengePenalty play_setup_challenge_penalty;
  char annotate_setup_lexicon[TUI_LEXICON_NAME_MAX];
  char annotate_setup_p1_name[32];
  char annotate_setup_p2_name[32];
  int annotate_setup_focus;
  int annotate_setup_name_cursor;
  char load_position_buf[2048];
  int load_position_len;
  int load_position_cursor;
  bool load_position_dirty;
  bool load_position_parse_ok;
  char load_position_error[160];
  char load_game_buf[16384];
  int load_game_len;
  int load_game_cursor;
  bool load_game_dirty;
  bool load_game_parse_ok;
  char load_game_error[160];
  int quit_confirm_focus;
  TuiModalState quit_confirm_return;
  LexiconList *lexicon_list;
  int lexicon_focus;
  bool frame_dirty;
} TuiUiState;

#endif
