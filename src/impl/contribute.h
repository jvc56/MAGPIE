#ifndef CONTRIBUTE_H
#define CONTRIBUTE_H

// Runs the birdtest contribution loop: claim a task, execute it with MAGPIE's
// own machinery, submit the result, repeat.

#include "../ent/autoplay_results.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"
#include "../ent/thread_control.h"
#include "../util/io_util.h"
#include "move_gen.h"

// Task execution needs to re-enter MAGPIE's own command interpreter -- a task
// becomes a command string like "autoplay games ... -lex ...", built from
// whatever the server sent, and the result is read back out of Config's
// result structs. But Config is owned by config.c, and config.c's command
// table is what dispatches to `contribute` in the first place, so this header
// cannot #include config.h without the two modules including each other.
// config.c fills in this vtable with thin wrappers around the real
// config_* functions instead, so contribute.c never needs to know Config's
// type at all.
typedef struct ContributeConfigApi {
  void *config;
  void (*load_command)(void *config, const char *command,
                       ErrorStack *error_stack);
  void (*execute_command)(void *config, ErrorStack *error_stack);
  const char *(*get_data_paths)(void *config);
  const char *(*get_magpie_version)(void);
  int (*get_num_threads)(void *config);
  ThreadControl *(*get_thread_control)(void *config);
  const AutoplayResults *(*get_autoplay_results)(void *config);
  const MoveList *(*get_move_list)(void *config);
  const Game *(*get_game)(void *config);
  const LetterDistribution *(*get_ld)(void *config);
} ContributeConfigApi;

// `settings_path` may be NULL, in which case contribute.txt in the working
// directory is used. Settings come from that file and never from the command
// line: an API key on a command line ends up in shell history and ps output.
void contribute_run(const ContributeConfigApi *config_api,
                    const char *settings_path, ErrorStack *error_stack);

// Compares dotted numeric versions, returning <0, 0 or >0. Missing components
// count as zero, so "1.4" and "1.4.0" are equal. Exposed for testing: naive
// string comparison gets this wrong ("1.10" sorts below "1.9").
int contribute_compare_versions(const char *left, const char *right);

#endif
