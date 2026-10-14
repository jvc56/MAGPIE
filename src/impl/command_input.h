#ifndef COMMAND_INPUT_H
#define COMMAND_INPUT_H

#include <stdbool.h>

// Lines of input for the command loop, read by one thread for the life of the
// process. The command loop asks for the next line between commands, and asks
// again while an asynchronous command runs (for "stop" and "status"); both go
// through the same reader. A read can only end when a line or the end of input
// arrives -- no platform can wake a thread blocked reading the console -- so a
// read still pending when an asynchronous command finishes is not abandoned:
// its line goes to the next caller. Each read takes its stream as it is when
// the read starts, so tests may swap streams between runs.

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

#endif
