#ifndef TUI_TUI_CRASH_H
#define TUI_TUI_CRASH_H

#include <notcurses/notcurses.h>

void install_crash_handlers(void);

// Record the live notcurses context so a crash handler can restore the
// terminal before writing the backtrace. NULL when notcurses is down.
void tui_crash_set_nc(struct notcurses *nc);

#endif
