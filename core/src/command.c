
#include "command.h"
#include "autoplay.h"
#include "cgp.h"
#include "config.h"
#include "error_status.h"
#include "game.h"
#include "infer.h"
#include "sim.h"

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

void execute_command_internal(CommandVars *command_vars) {
  if (!command_vars->config) {
    command_vars->config = create_config();
  }
  if (!command_vars->error_status) {
    command_vars->error_status = create_error_status(ERROR_STATUS_TYPE_NONE, 0);
  }
  config_load_status_t config_load_status =
      load_config(command_vars->config, command_vars->command);
  if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
    set_error_status(command_vars->error_status, ERROR_STATUS_TYPE_CONFIG_LOAD,
                     (int)config_load_status);
  } else {

    switch (command_vars->config->command_type) {
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
      if (!command_vars->game) {
        command_vars->game = create_game(command_vars->config);
      }
      cgp_parse_status_t cgp_parse_status =
          load_cgp(command_vars->game, command_vars->config->cgp);
      if (cgp_parse_status != CGP_PARSE_STATUS_SUCCESS) {
        set_error_status(command_vars->error_status, ERROR_STATUS_TYPE_CGP_LOAD,
                         (int)cgp_parse_status);
      }
      break;
    case COMMAND_TYPE_SIM:
      if (!command_vars->game) {
        command_vars->game = create_game(command_vars->config);
      }
      if (!command_vars->simmer) {
        command_vars->simmer = create_simmer(command_vars->config);
      }
      simulate(command_vars->config, command_vars->simmer, command_vars->game);
      break;
    case COMMAND_TYPE_AUTOPLAY:
      if (!command_vars->game) {
        command_vars->game = create_game(command_vars->config);
      }
      if (!command_vars->autoplay_results) {
        command_vars->autoplay_results = create_autoplay_results();
      }
      autoplay(command_vars->config, command_vars->game,
               command_vars->autoplay_results);
      break;
    case COMMAND_TYPE_INFER:
      if (!command_vars->game) {
        command_vars->game = create_game(command_vars->config);
      }
      if (!command_vars->inference) {
        command_vars->inference = create_inference();
      }
      infer(command_vars->config, command_vars->game, command_vars->inference);
      break;
    }
  }
}

// FIXME: think about how to make thread safe
void execute_command(CommandVars *command_vars) {
  if (set_mode_searching(command_vars->thread_control)) {
    execute_command_internal(command_vars);
  } else {
    status = UCGI_COMMAND_STATUS_NOT_STOPPED;
  }
}
