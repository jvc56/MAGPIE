#include "command.h"
#include "autoplay.h"
#include "cgp.h"
#include "config.h"
#include "error_status.h"
#include "game.h"
#include "infer.h"
#include "log.h"
#include "sim.h"
#include "thread_control.h"
#include "ucgi_print.h"

CommandVars *create_command_vars(FILE *outfile) {
  CommandVars *command_vars = malloc_or_die(sizeof(CommandVars));
  command_vars->config = NULL;
  command_vars->game = NULL;
  command_vars->simmer = NULL;
  command_vars->inference = NULL;
  command_vars->autoplay_results = NULL;
  command_vars->thread_control = NULL;
  command_vars->outfile = outfile;
  command_vars->thread_control = create_thread_control(outfile);
  return command_vars;
}

void destroy_command_vars(CommandVars *command_vars) {
  // Caller needs to handle the outfile
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
  if (command_vars->thread_control) {
    destroy_thread_control(command_vars->thread_control);
  }
  if (command_vars->error_status) {
    destroy_error_status(command_vars->error_status);
  }
  free(command_vars);
}

char *command_search_status(CommandVars *command_vars, bool should_halt) {
  if (!command_vars) {
    log_warn("The Command variables struct has not been initialized.");
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
    status_string =
        ucgi_sim_stats(command_vars->simmer, command_vars->loaded_game, 1);
    break;
  case COMMAND_TYPE_AUTOPLAY:
    log_warn("autoplay status unimplemented");
    break;
  case COMMAND_TYPE_LOAD_CGP:
    log_warn("no status available for load cgp");
    break;
  case COMMAND_TYPE_SIM:
    log_warn("sim status unimplemented");
    break;
  case COMMAND_TYPE_INFER:
    log_warn("infer status unimplemented");
    break;
  }
  return status_string;
}

void execute_load_cgp(CommandVars *command_vars, const Config *config) {
  if (!command_vars->game) {
    command_vars->game = create_game(config);
  }
  cgp_parse_status_t cgp_parse_status =
      load_cgp(command_vars->game, config->cgp);
  if (cgp_parse_status != CGP_PARSE_STATUS_SUCCESS) {
    set_error_status(command_vars->error_status, ERROR_STATUS_TYPE_CGP_LOAD,
                     (int)cgp_parse_status);
  }
}

void execute_sim(CommandVars *command_vars, const Config *config) {
  if (!command_vars->game) {
    command_vars->game = create_game(config);
  }
  if (!command_vars->simmer) {
    command_vars->simmer = create_simmer(config);
  }
  simulate(config, command_vars->thread_control, command_vars->simmer,
           command_vars->game);
}

void execute_autoplay(CommandVars *command_vars, const Config *config) {
  if (!command_vars->game) {
    command_vars->game = create_game(config);
  }
  if (!command_vars->autoplay_results) {
    command_vars->autoplay_results = create_autoplay_results();
  }
  autoplay(config, command_vars->thread_control, command_vars->game,
           command_vars->autoplay_results);
}

void execute_infer(CommandVars *command_vars, const Config *config) {
  if (!command_vars->game) {
    command_vars->game = create_game(config);
  }
  if (!command_vars->inference) {
    command_vars->inference = create_inference();
  }
  infer(config, command_vars->thread_control, command_vars->game,
        command_vars->inference);
}

void load_thread_control(CommandVars *command_vars, const Config *config) {
  command_vars->thread_control->number_of_threads = config->number_of_threads;
  command_vars->thread_control->print_info_interval =
      config->print_info_interval;
  command_vars->thread_control->check_stopping_condition_interval =
      config->check_stopping_condition_interval;
}

void execute_command(CommandVars *command_vars) {
  // Do not modify any variables here, this could
  // be multithreaded if commands are executed
  // quickly enough.
  if (!command_vars->thread_control) {
    log_fatal("missing thread control for command execution\n");
  }
  // Up until this point, there could be multiple
  // threads running 'execute_command'. The set_mode_searching
  // function will lock on the current mode mutex
  // and set the search value to searching
  // to ensure that at most 1 thread is running concurrently
  // after this point.
  if (!set_mode_searching(command_vars->thread_control)) {
    log_warn("still searching");
    return;
  }
  if (!command_vars->config) {
    command_vars->config = create_default_config();
  }
  if (!command_vars->error_status) {
    command_vars->error_status = create_error_status(ERROR_STATUS_TYPE_NONE, 0);
  }
  config_load_status_t config_load_status =
      load_config(command_vars->config, command_vars->command);

  // Once the config is loaded, we should regard it as
  // read-only. We create a new const pointer to enforce this.
  const Config *config = command_vars->config;

  load_thread_control(command_vars, config);
  if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
    set_error_status(command_vars->error_status, ERROR_STATUS_TYPE_CONFIG_LOAD,
                     (int)config_load_status);
  } else {
    unhalt(command_vars->thread_control);
    switch (config->command_type) {
    case COMMAND_TYPE_UNKNOWN:
      set_error_status(command_vars->error_status,
                       ERROR_STATUS_TYPE_CONFIG_LOAD,
                       (int)CONFIG_LOAD_STATUS_UNRECOGNIZED_COMMAND);
      break;
    case COMMAND_TYPE_SET_OPTIONS:
      // this operation is just for loading the config
      // so the execution is a no-op
      break;
    case COMMAND_TYPE_LOAD_CGP:
      execute_load_cgp(command_vars, config);
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
  set_mode_stopped(command_vars->thread_control);
}