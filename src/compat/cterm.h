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
// sequences (the board's colors), which cterm_restore undoes, at exit or
// before an abort: the console outlives the process. Elsewhere terminals do
// both already and this does nothing.
void cterm_init(void);

// Puts the console back as cterm_init found it. Runs at exit; an abort skips
// that, so log_fatal calls it first. Does nothing if there is nothing to undo.
void cterm_restore(void);

// For unattended runs (the test binary in CI): an abort, a crash or a missing
// drive ends the process with no dialog waiting for someone to click it --
// Windows Error Reporting's would hang the job. Does nothing elsewhere.
void cterm_disable_error_dialogs(void);

// Whether this process has its console window to itself, and someone at it:
// on Windows, true when it was started by double-clicking it in Explorer
// (Windows made a console for it, which closes when it exits) and reads and
// writes that console rather than pipes. A program a GUI starts (a front end,
// Task Scheduler) gets a console of its own too, but talks to it through
// pipes, so that is not enough. False everywhere else.
bool cterm_owns_console(void);

// A wait for a terminal's next line that another thread can end. A thread
// blocked reading cannot be woken on any platform, but a POSIX terminal reads
// a line at a time (canonical mode), so it can be waited on until a whole
// line is there, and the wait ended so that the line is read some other way
// (line editing) instead.
typedef struct CTermLineWait CTermLineWait;

// NULL where such a wait is not possible: on Windows, where waiting on a
// console without reading it would not echo what is typed.
CTermLineWait *cterm_line_wait_create(void);

// Waits until `stream`, a terminal, has a line to read (or has ended) and
// returns true, or until cterm_line_wait_end is called and returns false.
bool cterm_line_wait(CTermLineWait *line_wait, FILE *stream);

// Ends a cterm_line_wait under way, or the next one to start. Safe to call
// from any thread.
void cterm_line_wait_end(CTermLineWait *line_wait);

#endif
