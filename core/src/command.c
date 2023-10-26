#include <stdlib.h>

#include "autoplay.h"
#include "command.h"
#include "config.h"
#include "error_status.h"
#include "game.h"
#include "infer.h"
#include "log.h"
#include "sim.h"
#include "thread_control.h"
#include "ucgi_print.h"
#include "util.h"

CommandVars *create_command_vars(FILE *outfile) {
  CommandVars *command_vars = malloc_or_die(sizeof(CommandVars));
  command_vars->command = NULL;
  command_vars->config = NULL;
  command_vars->game = NULL;
  command_vars->simmer = NULL;
  command_vars->inference = NULL;
  command_vars->autoplay_results = NULL;
  command_vars->error_status = create_error_status(ERROR_STATUS_TYPE_NONE, 0);
  command_vars->thread_control = create_thread_control(outfile);
  command_vars->outfile = outfile;
  return command_vars;
}

void destroy_command_vars(CommandVars *command_vars) {
  // Caller needs to handle the outfile
  if (command_vars->command) {
    free(command_vars->command);
  }
  if (command_vars->config) {
    destroy_config(command_vars->config);
  }
  if (command_vars->game) {
    destroy_game(command_vars->game);
  }
  if (command_vars->simmer) {
    destroy_simmer(command_vars->simmer);
  }
  if (command_vars->inference) {
    destroy_inference(command_vars->inference);
  }
  if (command_vars->autoplay_results) {
    destroy_autoplay_results(command_vars->autoplay_results);
  }
  destroy_error_status(command_vars->error_status);
  destroy_thread_control(command_vars->thread_control);
  free(command_vars);
}

char *command_search_status(CommandVars *command_vars, bool should_halt) {
  if (!command_vars) {
    log_warn("The command variables struct has not been initialized.");
    return NULL;
  }
  if (!command_vars->thread_control) {
    // log fatally since there should always be
    // a thread control
    log_fatal("Thread controller has not been initialized.");
  }

  int mode = get_mode(command_vars->thread_control);
  if (mode != MODE_SEARCHING) {
    log_warn("Not currently searching.");
    return NULL;
  }

  if (should_halt) {
    if (!halt(command_vars->thread_control, HALT_STATUS_USER_INTERRUPT)) {
      log_warn("Command already halted.");
    }
    wait_for_mode_stopped(command_vars->thread_control);
  }

  char *status_string = NULL;
  switch (command_vars->config->command_type) {
  case COMMAND_TYPE_UNKNOWN:
    log_warn("Unknown command type");
    break;
  case COMMAND_TYPE_SIM:
    if (!command_vars->simmer) {
      log_warn("Simmer has not been initialized.");
      return NULL;
    }
    // FIXME: need an option for ucgi vs. human readable
    // since the command module is an abstraction layer
    // above UCGI.
    status_string = ucgi_sim_stats(command_vars->simmer, command_vars->game, 1);
    break;
  case COMMAND_TYPE_AUTOPLAY:
    log_warn("autoplay status unimplemented");
    break;
  case COMMAND_TYPE_LOAD_CGP:
    log_warn("no status available for load cgp");
    break;
  case COMMAND_TYPE_SET_OPTIONS:
    log_warn("no status available for set options");
    break;
  case COMMAND_TYPE_INFER:
    log_warn("infer status unimplemented");
    break;
  }
  return status_string;
}

void update_or_create_game(const Config *config, CommandVars *command_vars) {
  if (!command_vars->game) {
    command_vars->game = create_game(config);
  } else {
    update_game(config, command_vars->game);
  }
}

void execute_sim(CommandVars *command_vars, const Config *config) {
  if (!command_vars->simmer) {
    command_vars->simmer = create_simmer(config);
  }
  sim_status_t status = simulate(config, command_vars->thread_control,
                                 command_vars->simmer, command_vars->game);
  if (status != SIM_STATUS_SUCCESS) {
    set_error_status(command_vars->error_status, ERROR_STATUS_TYPE_SIM,
                     (int)status);
  }
}

void execute_autoplay(CommandVars *command_vars, const Config *config) {
  if (!command_vars->autoplay_results) {
    command_vars->autoplay_results = create_autoplay_results();
  }
  autoplay_status_t status = autoplay(config, command_vars->thread_control,
                                      command_vars->autoplay_results);
  if (status != AUTOPLAY_STATUS_SUCCESS) {
    set_error_status(command_vars->error_status, ERROR_STATUS_TYPE_AUTOPLAY,
                     (int)status);
  }
}

void execute_infer(CommandVars *command_vars, const Config *config) {
  if (!command_vars->inference) {
    command_vars->inference = create_inference();
  }
  inference_status_t status =
      infer(config, command_vars->thread_control, command_vars->game,
            command_vars->inference);
  if (status != INFERENCE_STATUS_SUCCESS) {
    set_error_status(command_vars->error_status, ERROR_STATUS_TYPE_INFER,
                     (int)status);
  }
}

void load_thread_control(CommandVars *command_vars, const Config *config) {
  command_vars->thread_control->number_of_threads = config->number_of_threads;
  command_vars->thread_control->print_info_interval =
      config->print_info_interval;
  command_vars->thread_control->check_stopping_condition_interval =
      config->check_stopping_condition_interval;
}

void execute_command(CommandVars *command_vars) {
  if (!command_vars->config) {
    command_vars->config = create_default_config();
  }

  set_error_status(command_vars->error_status, ERROR_STATUS_TYPE_NONE, 0);
  config_load_status_t config_load_status =
      load_config(command_vars->config, command_vars->command);

  if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
    set_error_status(command_vars->error_status, ERROR_STATUS_TYPE_CONFIG_LOAD,
                     (int)config_load_status);
    return;
  }

  // Once the config is loaded, we should regard it as
  // read-only. We create a new const pointer to enforce this.
  const Config *config = command_vars->config;

  if (config->command_set_cgp) {
    update_or_create_game(config, command_vars);
    cgp_parse_status_t cgp_parse_status =
        load_cgp(command_vars->game, config->cgp);
    if (cgp_parse_status != CGP_PARSE_STATUS_SUCCESS) {
      set_error_status(command_vars->error_status, ERROR_STATUS_TYPE_CGP_LOAD,
                       (int)cgp_parse_status);
      return;
    }
  }

  load_thread_control(command_vars, config);
  switch (config->command_type) {
  case COMMAND_TYPE_UNKNOWN:
    set_error_status(command_vars->error_status, ERROR_STATUS_TYPE_CONFIG_LOAD,
                     (int)CONFIG_LOAD_STATUS_UNRECOGNIZED_COMMAND);
    break;
  case COMMAND_TYPE_SET_OPTIONS:
    // this operation is just for loading the config
    // so the execution is a no-op
    break;
  case COMMAND_TYPE_LOAD_CGP:
    // Any command can potentially load
    // a CGP, so it is handled generically
    // above. No further processing is necessary.
    break;
  case COMMAND_TYPE_SIM:
    execute_sim(command_vars, config);
    break;
  case COMMAND_TYPE_AUTOPLAY:
    execute_autoplay(command_vars, config);
    break;
  case COMMAND_TYPE_INFER:
    execute_infer(command_vars, config);
    break;
  }
}

void execute_command_and_set_mode_stopped(CommandVars *command_vars) {
  execute_command(command_vars);
  set_mode_stopped(command_vars->thread_control);
}

void *execute_command_thread_worker(void *uncasted_command_vars) {
  CommandVars *command_vars = (CommandVars *)uncasted_command_vars;
  execute_command_and_set_mode_stopped(command_vars);
  return NULL;
}

void execute_command_sync_or_async(CommandVars *command_vars, bool sync) {
  if (!set_mode_searching(command_vars->thread_control)) {
    log_warn("still searching");
    return;
  }
  unhalt(command_vars->thread_control);
  if (sync) {
    execute_command_and_set_mode_stopped(command_vars);
  } else {
    pthread_t cmd_execution_thread;
    pthread_create(&cmd_execution_thread, NULL, execute_command_thread_worker,
                   command_vars);
    pthread_detach(cmd_execution_thread);
  }
}

void execute_command_sync(CommandVars *command_vars) {
  execute_command_sync_or_async(command_vars, true);
}

void execute_command_async(CommandVars *command_vars) {
  execute_command_sync_or_async(command_vars, false);
}