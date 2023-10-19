#include "command.h"
#include "log.h"
#include "thread_control.h"

#define UCGI_COMMAND_STRING "ucgi"
#define QUIT_COMMAND_STRING "quit"
#define STOP_COMMAND_STRING "stop"

char *ucgi_search_status(CommandVars *command_vars) {
  return command_search_status(command_vars, false);
}

char *ucgi_stop_search(CommandVars *command_vars) {
  return command_search_status(command_vars, true);
}

void *execute_command_worker(void *uncasted_command_vars) {
  CommandVars *command_vars = (CommandVars *)uncasted_command_vars;
  execute_command(command_vars);
  return NULL;
}

void *execute_command_async(CommandVars *command_vars) {
  pthread_t cmd_execution_thread;
  pthread_create(&cmd_execution_thread, NULL, execute_command_worker,
                 command_vars);
  pthread_detach(cmd_execution_thread);
  return NULL;
}

bool process_ucgi_command_async(CommandVars *command_vars) {
  // Assume cmd is already trimmed of whitespace
  bool should_end = false;
  if (strings_equal(command_vars->command, UCGI_COMMAND_STRING)) {
    // More of a formality to align with UCI
    fprintf(command_vars->outfile, "id name MAGPIE 0.1\n");
    fprintf(command_vars->outfile, "ucgiok\n");
    fflush(command_vars->outfile);
  } else if (strings_equal(command_vars->command, QUIT_COMMAND_STRING)) {
    should_end = true;
  } else if (strings_equal(command_vars->command, STOP_COMMAND_STRING)) {
    if (get_mode(command_vars->thread_control) == MODE_SEARCHING) {
      if (!halt(command_vars->thread_control, HALT_STATUS_USER_INTERRUPT)) {
        log_warn("Search already received stop signal but has not stopped.");
      }
    } else {
      log_info("There is no search to stop.");
    }
  } else {
    execute_command_async(command_vars);
  }
  return should_end;
}
