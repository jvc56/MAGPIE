#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "go_params.h"
#include "infer.h"
#include "log.h"
#include "sim.h"
#include "thread_control.h"
#include "ucgi.h"
#include "ucgi_command.h"
#include "ucgi_formats.h"
#include "ucgi_print.h"
#include "util.h"

#define CMD_MAX 256

void ucgi_scan_loop() {
  UCGICommandVars *ucgi_command_vars = create_ucgi_command_vars(stdout);

  while (1) {
    char cmd[CMD_MAX];
    if (!fgets(cmd, CMD_MAX, stdin)) {
      break;
    }
    // replace newline with 0 for ease in comparison
    // FIXME, this should probably just be a more generic
    // trim whitespace function.
    remove_first_newline(cmd);

    int should_end = process_ucgi_command_async(cmd, ucgi_command_vars);
    if (should_end) {
      break;
    }
  }
  if (ucgi_command_vars) {
    destroy_ucgi_command_vars(ucgi_command_vars);
  }
}
