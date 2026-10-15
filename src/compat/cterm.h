#ifndef CTERM_H
#define CTERM_H

#include <stdbool.h>
#include <stdio.h>

// Whether `stream` is an interactive terminal (a console on Windows) rather
// than a file, a pipe or the null device.
bool cterm_is_terminal(FILE *stream);

// Whether `stream` can show the board's wide glyphs: fullwidth column letters
// and squares, and the on-turn arrowhead. False only for a Windows console
// window (conhost) outside Windows Terminal and VS Code, since its default
// fonts lack them and it has no font fallback. True for files, pipes and every
// other terminal.
bool cterm_shows_wide_glyphs(FILE *stream);

// Prepares the console for MAGPIE's output, once, at startup. On Windows that
// means UTF-8 in and out (tiles and board glyphs are UTF-8) and VT escape
// sequences (the board's colors), restoring the console's code pages when
// the process exits, since the console outlives it. Elsewhere terminals do
// both already and this does nothing.
void cterm_init(void);

// For unattended runs (the test binary in CI): an abort, a crash or a missing
// drive ends the process with no dialog waiting for someone to click it --
// Windows Error Reporting's would hang the job. Does nothing elsewhere.
void cterm_disable_error_dialogs(void);

// Whether this process has its console to itself: on Windows, true when it
// was started by double-clicking it in Explorer (Windows made a console for
// it, which closes when it exits) rather than from a terminal. False
// everywhere else.
bool cterm_owns_console(void);

// If this process owns its console, asks the user to press Enter and waits,
// so that the last thing printed (an error, say) stays on screen rather than
// vanishing with the window. Does nothing otherwise. log_fatal calls it
// before aborting, since abort() skips atexit handlers.
void cterm_wait_before_close(void);

#endif
