#include <stdio.h>
#include <stdlib.h>

#include "command.h"
#include "string_util.h"
#include "ucgi_command.h"

#define CMD_MAX 256

void ucgi_scan_loop() {
  CommandVars *command_vars = create_command_vars(stdout);

  while (1) {
    char cmd[CMD_MAX];
    if (!fgets(cmd, CMD_MAX, stdin)) {
      break;
    }

    trim_whitespace(cmd);

    int should_end = process_ucgi_command_async(command_vars, cmd);
    if (should_end) {
      break;
    }
  }
  if (command_vars) {
    destroy_command_vars(command_vars);
  }
}
