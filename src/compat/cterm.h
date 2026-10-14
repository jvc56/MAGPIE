#ifndef CTERM_H
#define CTERM_H

#include <stdbool.h>
#include <stdio.h>

// Whether `stream` is an interactive terminal (a console on Windows) rather
// than a file, a pipe or the null device.
bool cterm_is_terminal(FILE *stream);

// Prepares the console for MAGPIE's output, once, at startup. On Windows that
// means UTF-8 in and out (tiles and board glyphs are UTF-8) and VT escape
// sequences (the board's colors), restoring the console's code pages when
// the process exits, since the console outlives it. Elsewhere terminals do
// both already and this does nothing.
void cterm_init(void);

#endif
