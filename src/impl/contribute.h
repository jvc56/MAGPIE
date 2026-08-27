#ifndef CONTRIBUTE_H
#define CONTRIBUTE_H

#include "config.h"

// Runs the birdtest contribution loop: claim a task, execute it with MAGPIE's
// own machinery, submit the result, repeat.
//
// `settings_path` may be NULL, in which case contribute.txt in the working
// directory is used. Settings come from that file and never from the command
// line: an API key on a command line ends up in shell history and ps output.
void impl_contribute(Config *config, const char *settings_path,
                     ErrorStack *error_stack);

// Compares dotted numeric versions, returning <0, 0 or >0. Missing components
// count as zero, so "1.4" and "1.4.0" are equal. Exposed for testing: naive
// string comparison gets this wrong ("1.10" sorts below "1.9").
int contribute_compare_versions(const char *left, const char *right);

#endif
