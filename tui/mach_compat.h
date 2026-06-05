#ifndef TUI_MACH_COMPAT_H
#define TUI_MACH_COMPAT_H

// Thin wrapper so the conditional <mach/mach.h> include doesn't sit
// inside game_render.c's main include block — format.py rejects
// preprocessor conditionals between the first and last include. On
// non-Apple platforms this header is empty and the status-bar code
// that uses task_info(...) is itself #ifdef'd out at the call site.
#ifdef __APPLE__
#include <mach/mach.h>
#endif

#endif // TUI_MACH_COMPAT_H
