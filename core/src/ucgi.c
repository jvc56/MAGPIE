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

static UCGICommandVars *ucgi_command_vars = NULL;

void ucgi_scan_loop() {
  if (ucgi_command_vars == NULL) {
    ucgi_command_vars = create_ucgi_command_vars(stdout);
  }
  while (1) {
    char cmd[CMD_MAX];
    if (fgets(cmd, CMD_MAX, stdin) == NULL) {
      break;
    }
    // replace newline with 0 for ease in comparison
    cmd[strcspn(cmd, "\n")] = 0;

    int should_end = process_ucgi_command_async(cmd, ucgi_command_vars);
    if (should_end) {
      break;
    }
  }
  if (ucgi_command_vars != NULL) {
    destroy_ucgi_command_vars(ucgi_command_vars);
  }
}
