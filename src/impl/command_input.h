#ifndef COMMAND_INPUT_H
#define COMMAND_INPUT_H

#include <stdbool.h>
#include <stdio.h>

// Lines of input for the command loop, read by one thread for the life of the
// process. The command loop asks for the next line between commands, and asks
// again while an asynchronous command runs (for "stop" and "status"); both go
// through the same reader.
//
// Once begun, a read can only end when a line or the end of input arrives --
// no platform can wake a thread blocked reading -- so a read still under way
// when an asynchronous command finishes is not abandoned: its line goes to
// the next caller. At a POSIX terminal the reader instead waits for a whole
// line to be typed before reading it, so a read not yet begun becomes the
// command loop's edited, prompted one when that is what it next asks for. (A
// Windows console edits lines itself.)
//
// A read takes the input stream as it is when the read is asked for, and its
// line goes only to a caller reading that stream, so tests may swap streams
// between runs, releasing each before closing it.

// The next line of input without its newline (the caller frees it), or NULL at
// the end of input. While it waits, shows `prompt` if input is a terminal, and
// edits the line there with history.
char *command_input_read_line(const char *prompt);

// The next line of input without its newline (the caller frees it), unless
// `done(done_arg)` becomes true first, in which case it returns NULL and the
// read carries on for the next caller. Also returns NULL, with *at_end set, at
// the end of input. `done` is called with the input lock held, so it must not
// call back into this module; whatever makes it true must then call
// command_input_wake.
char *command_input_read_line_until(bool (*done)(void *), void *done_arg,
                                    bool *at_end);

// Has command_input_read_line_until check its `done` again.
void command_input_wake(void);

// Lets `stream` be closed: waits for a read of it under way to end, then
// drops its line and withdraws any read of it not yet begun. A read under
// way ends only when a line or the end of input arrives, so close the
// writing end of a pipe first.
void command_input_release_stream(const FILE *stream);

#endif
