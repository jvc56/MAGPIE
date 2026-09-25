#include "tui_session.h"

#include "config.h"
#include "game_state.h"
#include "tui_ui_state.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

bool tui_reinit_game_state_if_needed(TuiGameState *state, TuiSession *session,
                                     bool *reinitialized) {
  *reinitialized = false;
  const bool needs_reinit =
      strcmp(state->pending_lexicon, state->active_lexicon) != 0 ||
      state->pending_load_rit != state->active_load_rit;
  if (!needs_reinit) {
    return true;
  }
  char new_lexicon[TUI_LEXICON_NAME_MAX];
  snprintf(new_lexicon, sizeof(new_lexicon), "%s", state->pending_lexicon);
  const bool new_load_rit = state->pending_load_rit;
  const int saved_sim_plies = state->sim_plies;
  const int saved_sim_candidates = state->sim_candidates;
  tui_game_state_destroy(state);
  char reinit_error[256] = {0};
  if (!tui_game_state_init(new_lexicon, (uint64_t)time(NULL), new_load_rit,
                           state, reinit_error, sizeof(reinit_error))) {
    if (!tui_game_state_init(session->chosen_lexicon, (uint64_t)time(NULL),
                             session->initial_load_rit, state, reinit_error,
                             sizeof(reinit_error))) {
      return false;
    }
  } else {
    snprintf(session->chosen_lexicon, sizeof(session->chosen_lexicon), "%s",
             new_lexicon);
  }
  state->sim_plies = saved_sim_plies;
  state->sim_candidates = saved_sim_candidates;
  *reinitialized = true;
  return true;
}
