#ifndef TUI_ONBOARDING_H
#define TUI_ONBOARDING_H

#include <notcurses/notcurses.h>
#include "theme.h"

// Runs the interactive theme picker. Up/Down (or j/k, or 1-4) to navigate,
// Enter to confirm, Esc to cancel. The whole picker re-renders in the
// focused theme so the user sees a live preview as they move. On cancel
// returns `initial`.
ThemeName tui_onboarding_run(struct notcurses *nc, ThemeName initial);

#endif
