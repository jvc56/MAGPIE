#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "go_params.h"
#include "infer.h"
#include "log.h"
#include "sim.h"
#include "thread_control.h"
#include "ucgi_command.h"
#include "util.h"

#define GO_PARAMS_PARSE_SUCCESS 0
#define GO_PARAMS_PARSE_FAILURE 1

UCGICommandVars *create_ucgi_command_vars() {
  UCGICommandVars *ucgi_command_vars = malloc(sizeof(UCGICommandVars));
  ucgi_command_vars->loaded_game = NULL;
  ucgi_command_vars->config = NULL;
  ucgi_command_vars->simmer = NULL;
  ucgi_command_vars->inference = NULL;
  ucgi_command_vars->outfile = NULL;
  ucgi_command_vars->go_params = create_go_params();
  ucgi_command_vars->thread_control = create_thread_control();
  return ucgi_command_vars;
}

void destroy_ucgi_command_vars(UCGICommandVars *ucgi_command_vars) {
  // Caller needs to handle the outfile
  if (ucgi_command_vars->loaded_game != NULL) {
    destroy_game(ucgi_command_vars->loaded_game);
  }
  if (ucgi_command_vars->config != NULL) {
    destroy_config(ucgi_command_vars->config);
  }
  if (ucgi_command_vars->simmer != NULL) {
    destroy_simmer(ucgi_command_vars->simmer);
  }
  if (ucgi_command_vars->inference != NULL) {
    destroy_inference(ucgi_command_vars->inference);
  }
  destroy_go_params(ucgi_command_vars->go_params);
  destroy_thread_control(ucgi_command_vars->thread_control);
  free(ucgi_command_vars);
}

void set_outfile(UCGICommandVars *ucgi_command_vars, FILE *outfile) {
  ucgi_command_vars->outfile = outfile;
}

int parse_go_cmd(char *params, GoParams *go_params) {
  // Reset params to erase previous settings
  reset_go_params(go_params);
  // parse the go cmd.
  char *token;
  token = strtok(params, " ");
  int reading_search_type = 0;
  int reading_depth = 0;
  int reading_stop_condition = 0;
  int reading_threads = 0;
  int reading_num_plays = 0;
  int reading_max_iterations = 0;
  while (token != NULL) {
    if (reading_num_plays) {
      go_params->num_plays = atoi(token);
    } else if (reading_max_iterations) {
      go_params->max_iterations = atoi(token);
    } else if (reading_depth) {
      go_params->depth = atoi(token);
    } else if (reading_threads) {
      go_params->threads = atoi(token);
    } else if (reading_stop_condition) {
      if (strcmp(token, "95") == 0) {
        go_params->stop_condition = SIM_STOPPING_CONDITION_95PCT;
      } else if (strcmp(token, "98") == 0) {
        go_params->stop_condition = SIM_STOPPING_CONDITION_98PCT;
      } else if (strcmp(token, "99") == 0) {
        go_params->stop_condition = SIM_STOPPING_CONDITION_99PCT;
      } else {
        log_warn("Did not understand stopping condition %s", token);
        return GO_PARAMS_PARSE_FAILURE;
      }
    }
    if (strcmp(token, "infinite") == 0) {
      go_params->infinite = 1;
    }
    if (strcmp(token, "static") == 0) {
      go_params->static_search_only = 1;
    }

    if (strcmp(token, "sim") == 0) {
      if (go_params->search_type != SEARCH_TYPE_NONE) {
        log_warn("Too many search types specified.");
        return GO_PARAMS_PARSE_FAILURE;
      }
      go_params->search_type = SEARCH_TYPE_SIM_MONTECARLO;
    } else if (strcmp(token, "infer") == 0) {
      if (go_params->search_type != SEARCH_TYPE_NONE) {
        log_warn("Too many search types specified.");
        return GO_PARAMS_PARSE_FAILURE;
      }
      go_params->search_type = SEARCH_TYPE_INFERENCE_SOLVE;
    }

    reading_num_plays = strcmp(token, "plays") == 0;
    reading_max_iterations = strcmp(token, "i") == 0;
    reading_depth = strcmp(token, "depth") == 0;
    reading_stop_condition = strcmp(token, "stopcondition") == 0;
    reading_threads = strcmp(token, "threads") == 0;
    token = strtok(NULL, " ");
  }
  log_debug("Returning go_params; inf %d stop %d depth %d threads %d ss %d",
            go_params->infinite, go_params->stop_condition, go_params->depth,
            go_params->threads, go_params->static_search_only);
  if (go_params->stop_condition != SIM_STOPPING_CONDITION_NONE &&
      go_params->infinite) {
    log_warn("Cannot have a stopping condition and also search infinitely.");
    return GO_PARAMS_PARSE_FAILURE;
  }
  if (go_params->search_type == SEARCH_TYPE_SIM_MONTECARLO &&
      go_params->depth <= 0) {
    log_warn("Need a positive depth for sim.");
    return GO_PARAMS_PARSE_FAILURE;
  }
  if (go_params->threads <= 0) {
    log_warn("Need a positive number of threads.");
    return GO_PARAMS_PARSE_FAILURE;
  }
  return GO_PARAMS_PARSE_SUCCESS;
}

void ucgi_simulate(UCGICommandVars *ucgi_command_vars) {
  simulate(ucgi_command_vars->thread_control, ucgi_command_vars->simmer,
           ucgi_command_vars->loaded_game, NULL,
           ucgi_command_vars->go_params->depth,
           ucgi_command_vars->go_params->threads,
           ucgi_command_vars->go_params->num_plays,
           ucgi_command_vars->go_params->max_iterations,
           ucgi_command_vars->go_params->stop_condition);
  // Print out the stats
  print_ucgi_stats(ucgi_command_vars->simmer, ucgi_command_vars->loaded_game,
                   1);
}

void ucgi_infer(UCGICommandVars *ucgi_command_vars) {
  abort();
  printf("%p\n", ucgi_command_vars);
}

void *execute_ucgi_command(void *uncasted_ucgi_command_vars) {
  UCGICommandVars *ucgi_command_vars =
      (UCGICommandVars *)uncasted_ucgi_command_vars;
  switch (ucgi_command_vars->go_params->search_type) {
  case SEARCH_TYPE_SIM_MONTECARLO:
    ucgi_simulate(ucgi_command_vars);
    break;
  case SEARCH_TYPE_INFERENCE_SOLVE:
    ucgi_infer(ucgi_command_vars);
    break;
  default:
    log_warn("Search type not set; exiting immediately.");
  }
  log_debug("setting current mode to stopped");
  set_mode(ucgi_command_vars->thread_control, MODE_STOPPED);
  return NULL;
}

void *execute_ucgi_command_async(UCGICommandVars *ucgi_command_vars) {
  set_mode(ucgi_command_vars->thread_control, MODE_SEARCHING);
  pthread_t cmd_execution_thread;
  pthread_create(&cmd_execution_thread, NULL, execute_ucgi_command,
                 ucgi_command_vars);
  pthread_detach(cmd_execution_thread);
  return NULL;
}

int ucgi_go_async(char *go_cmd, UCGICommandVars *ucgi_command_vars) {
  int status = UCGI_COMMAND_STATUS_SUCCESS;
  if (get_mode(ucgi_command_vars->thread_control) == MODE_STOPPED) {
    int parse_status = parse_go_cmd(go_cmd, ucgi_command_vars->go_params);
    if (parse_status == GO_PARAMS_PARSE_SUCCESS) {
      execute_ucgi_command_async(ucgi_command_vars);
    } else {
      status = UCGI_COMMAND_STATUS_PARSE_FAILED;
    }
  } else {
    status = UCGI_COMMAND_STATUS_NOT_STOPPED;
  }
  return status;
}

void load_position(UCGICommandVars *ucgi_command_vars, const char *cgp,
                   char *lexicon_name, char *ldname, int move_list_capacity) {

  load_config_from_lexargs(&ucgi_command_vars->config, cgp, lexicon_name,
                           ldname);
  ucgi_command_vars->config->move_list_capacity = move_list_capacity;

  if (ucgi_command_vars->loaded_game == NULL ||
      strcmp(ucgi_command_vars->last_lexicon_name, lexicon_name) ||
      strcmp(ucgi_command_vars->last_ld_name, ldname)) {
    log_debug("creating game");
    if (ucgi_command_vars->loaded_game != NULL) {
      destroy_game(ucgi_command_vars->loaded_game);
    }
    ucgi_command_vars->loaded_game = create_game(ucgi_command_vars->config);
  } else {
    // assume config is the same so just reset the game.
    log_debug("resetting game");
    reset_game(ucgi_command_vars->loaded_game);
  }

  log_debug("loading cgp: %s", ucgi_command_vars->config->cgp);
  load_cgp(ucgi_command_vars->loaded_game, ucgi_command_vars->config->cgp);
  log_debug("loaded game");

  strcpy(ucgi_command_vars->last_lexicon_name, lexicon_name);
  strcpy(ucgi_command_vars->last_ld_name, ldname);
}

int process_ucgi_command_async(char *cmd, UCGICommandVars *ucgi_command_vars) {
  // basic commands
  if (strcmp(cmd, "ucgi") == 0) {
    fprintf(ucgi_command_vars->outfile, "id name MAGPIE 0.1\n");
    fprintf(ucgi_command_vars->outfile, "ucgiok\n");
    fflush(ucgi_command_vars->outfile);
  } else if (strcmp(cmd, "quit") == 0) {
    return UCGI_COMMAND_STATUS_QUIT;
  }

  // other commands
  if (prefix("position cgp ", cmd)) {
    char *cgpstr = cmd + strlen("position cgp ");
    char lexicon[16] = "";
    char ldname[16] = "";
    lexicon_ld_from_cgp(cgpstr, lexicon, ldname);
    if (strcmp(lexicon, "") == 0) {
      return UCGI_COMMAND_STATUS_LEXICON_LD_FAILURE;
    }
    load_position(ucgi_command_vars, cgpstr, lexicon, ldname, 100);
  } else if (prefix("go", cmd)) {
    int command_status = ucgi_go_async(cmd + strlen("go"), ucgi_command_vars);
    if (command_status == UCGI_COMMAND_STATUS_PARSE_FAILED) {
      log_warn("Failed to parse go command.");
    } else if (command_status == UCGI_COMMAND_STATUS_NOT_STOPPED) {
      log_info("There is already a search ongoing.");
    }
    return command_status;
  } else if (strcmp(cmd, "stop") == 0) {
    if (get_mode(ucgi_command_vars->thread_control) == MODE_SEARCHING) {
      halt(ucgi_command_vars->thread_control);
    } else {
      log_info("There is no search to stop.");
    }
  }

  return UCGI_COMMAND_STATUS_SUCCESS;
}