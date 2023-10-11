#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "command.h"
#include "thread_control.h"
#include "ucgi_command.h"

void *execute_command_async(CommandVars *command_vars) {
  unhalt(command_vars->thread_control);
  pthread_t cmd_execution_thread;
  pthread_create(&cmd_execution_thread, NULL, execute_command, command_vars);
  pthread_detach(cmd_execution_thread);
  return NULL;
}

int ucgi_go_async(CommandVars *command_vars) {
  int status = UCGI_COMMAND_STATUS_SUCCESS;
  return status;
}

int process_ucgi_command_async(CommandVars *command_vars) {
  // basic commands
  if (strings_equal(cmd, "ucgi")) {
    fprintf(command_vars->outfile, "id name MAGPIE 0.1\n");
    fprintf(command_vars->outfile, "ucgiok\n");
    fflush(command_vars->outfile);
  } else if (strings_equal(cmd, "quit")) {
    return UCGI_COMMAND_STATUS_QUIT;
  } else if (strings_equal(cmd, "stop")) {
    if (get_mode(command_vars->thread_control)) {
      if (!halt(command_vars->thread_control, HALT_STATUS_USER_INTERRUPT)) {
        log_warn("Search already received stop signal but has not stopped.");
      }
    } else {
      log_info("There is no search to stop.");
    }
  } else {
    execute_ucgi_command_async(command_vars);
  }

  // other commands
  if (has_prefix("position cgp ", cmd)) {
    const char *cgpstr = cmd + string_length("position cgp ");
    CGPOperations *cgp_operations = get_default_cgp_operations();
    ucgi_command_status_t command_status =
        ucgi_load_position(command_vars, cgp_operations, cgpstr);
    destroy_cgp_operations(cgp_operations);
    if (command_status != UCGI_COMMAND_STATUS_SUCCESS) {
      return command_status;
    }
  } else if (has_prefix("go", cmd)) {
    ucgi_command_status_t command_status =
        ucgi_go_async(cmd + string_length("go"), command_vars);
    if (command_status == UCGI_COMMAND_STATUS_COMMAND_PARSE_FAILED) {
      log_warn("Failed to parse go command.");
    } else if (command_status == UCGI_COMMAND_STATUS_NOT_STOPPED) {
      log_info("There is already a search ongoing.");
    }
    return command_status;
  }

  return UCGI_COMMAND_STATUS_SUCCESS;
}

// ucgi_search_status returns the current status of the ongoing search. The
// returned string must be freed by the caller.
// Note: this function does not currently work with the `sim static` search.
// It will deadlock.
char *ucgi_search_status(CommandVars *command_vars) {
  if (!command_vars) {
    log_warn("The UCGI Command variables struct has not been initialized.");
    return NULL;
  }
  if (!command_vars->thread_control) {
    log_warn("Thread controller has not been initialized.");
    return NULL;
  }
  if (!command_vars->go_params) {
    log_warn("Search params have not been initialized.");
    return NULL;
  }
  int mode = get_mode(command_vars->thread_control);
  // Maybe the search is done already; in that case, we want to see the last
  // results.
  switch (command_vars->go_params->search_type) {
  case SEARCH_TYPE_SIM_MONTECARLO:
    if (!command_vars->simmer) {
      log_warn("Simmer has not been initialized.");
      return NULL;
    }
    return ucgi_sim_stats(command_vars->simmer, command_vars->loaded_game,
                          mode == MODE_STOPPED);
    break;
  }
}

char *ucgi_stop_search(CommandVars *command_vars) {
  if (!command_vars) {
    log_warn("The UCGI Command variables struct has not been initialized.");
    return NULL;
  }
  if (!command_vars->thread_control) {
    log_warn("Thread controller has not been initialized.");
    return NULL;
  }
  if (!command_vars->go_params) {
    log_warn("Search params have not been initialized.");
    return NULL;
  }

  int mode = get_mode(command_vars->thread_control);
  if (mode != MODE_SEARCHING) {
    log_warn("Not currently searching.");
    return NULL;
  }
  switch (command_vars->go_params->search_type) {
  case SEARCH_TYPE_SIM_MONTECARLO:
    if (!command_vars->simmer) {
      log_warn("Simmer has not been initialized.");
      return NULL;
    }
    halt(command_vars->thread_control, HALT_STATUS_USER_INTERRUPT);
    wait_for_mode_stopped(command_vars->thread_control);
    return ucgi_sim_stats(command_vars->simmer, command_vars->loaded_game, 1);
    break;
  }
}