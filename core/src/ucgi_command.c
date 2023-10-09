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
#include "ucgi_print.h"
#include "util.h"

typedef enum {
  GO_PARAMS_PARSE_SUCCESS,
  GO_PARAMS_PARSE_FAILURE,
} go_params_parse_status_t;

UCGICommandVars *create_ucgi_command_vars(FILE *outfile) {
  UCGICommandVars *ucgi_command_vars = malloc_or_die(sizeof(UCGICommandVars));
  ucgi_command_vars->loaded_game = NULL;
  ucgi_command_vars->config = NULL;
  ucgi_command_vars->simmer = NULL;
  ucgi_command_vars->inference = NULL;
  ucgi_command_vars->outfile = outfile;
  ucgi_command_vars->go_params = create_go_params();
  ucgi_command_vars->thread_control = create_thread_control(outfile);
  return ucgi_command_vars;
}

void destroy_ucgi_command_vars(UCGICommandVars *ucgi_command_vars) {
  // Caller needs to handle the outfile
  if (ucgi_command_vars->loaded_game) {
    destroy_game(ucgi_command_vars->loaded_game);
  }
  if (ucgi_command_vars->config) {
    destroy_config(ucgi_command_vars->config);
  }
  if (ucgi_command_vars->simmer) {
    destroy_simmer(ucgi_command_vars->simmer);
  }
  if (ucgi_command_vars->inference) {
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
  int reading_depth = 0;
  int reading_stop_condition = 0;
  int reading_threads = 0;
  int reading_num_plays = 0;
  int reading_max_iterations = 0;
  int reading_tiles = 0;
  int reading_player_index = 0;
  int reading_score = 0;
  int reading_number_of_tiles_exchanged = 0;
  int reading_equity_margin = 0;
  int reading_print_info_interval = 0;
  int reading_check_stopping_condition_interval = 0;
  while (token) {
    if (reading_num_plays) {
      go_params->num_plays = atoi(token);
    } else if (reading_max_iterations) {
      go_params->max_iterations = atoi(token);
    } else if (reading_depth) {
      go_params->depth = atoi(token);
    } else if (reading_print_info_interval) {
      go_params->print_info_interval = atoi(token);
    } else if (reading_tiles) {
      size_t tiles_size = string_length(token);
      if (tiles_size > (RACK_SIZE)) {
        log_warn("Too many played tiles for inference.");
        return GO_PARAMS_PARSE_FAILURE;
      }
      for (size_t i = 0; i < tiles_size; i++) {
        go_params->tiles[i] = token[i];
      }
      go_params->tiles[tiles_size] = '\0';
    } else if (reading_player_index) {
      go_params->player_index = atoi(token);
      if (go_params->player_index < 0 || go_params->player_index > 1) {
        log_warn("Player index not 0 or 1.");
        return GO_PARAMS_PARSE_FAILURE;
      }
    } else if (reading_score) {
      go_params->score = atoi(token);
    } else if (reading_number_of_tiles_exchanged) {
      go_params->number_of_tiles_exchanged = atoi(token);
    } else if (reading_equity_margin) {
      go_params->equity_margin = strtod(token, NULL);
    } else if (reading_check_stopping_condition_interval) {
      go_params->check_stopping_condition_interval = atoi(token);
    } else if (reading_threads) {
      go_params->threads = atoi(token);
    } else if (reading_stop_condition) {
      if (strings_equal(token, "95")) {
        go_params->stop_condition = SIM_STOPPING_CONDITION_95PCT;
      } else if (strings_equal(token, "98")) {
        go_params->stop_condition = SIM_STOPPING_CONDITION_98PCT;
      } else if (strings_equal(token, "99")) {
        go_params->stop_condition = SIM_STOPPING_CONDITION_99PCT;
      } else {
        log_warn("Did not understand stopping condition %s", token);
        return GO_PARAMS_PARSE_FAILURE;
      }
    }
    if (strings_equal(token, "static")) {
      go_params->static_search_only = 1;
    }

    if (strings_equal(token, "sim")) {
      if (go_params->search_type != SEARCH_TYPE_NONE) {
        log_warn("Too many search types specified.");
        return GO_PARAMS_PARSE_FAILURE;
      }
      go_params->search_type = SEARCH_TYPE_SIM_MONTECARLO;
    } else if (strings_equal(token, "infer")) {
      if (go_params->search_type != SEARCH_TYPE_NONE) {
        log_warn("Too many search types specified.");
        return GO_PARAMS_PARSE_FAILURE;
      }
      go_params->search_type = SEARCH_TYPE_INFERENCE_SOLVE;
    }

    reading_num_plays = strings_equal(token, "plays");
    reading_print_info_interval = strings_equal(token, "info");
    reading_check_stopping_condition_interval =
        strings_equal(token, "checkstop");
    reading_max_iterations = strings_equal(token, "i");
    reading_depth = strings_equal(token, "depth");
    reading_stop_condition = strings_equal(token, "stopcondition");
    reading_threads = strings_equal(token, "threads");
    reading_tiles = strings_equal(token, "tiles");
    reading_player_index = strings_equal(token, "pidx");
    reading_score = strings_equal(token, "score");
    reading_number_of_tiles_exchanged = strings_equal(token, "exch");
    reading_equity_margin = strings_equal(token, "eqmargin");
    token = strtok(NULL, " ");
  }
  log_debug("Returning go_params; i %d stop %d depth %d threads %d ss %d",
            go_params->max_iterations, go_params->stop_condition,
            go_params->depth, go_params->threads,
            go_params->static_search_only);
  if (go_params->stop_condition != SIM_STOPPING_CONDITION_NONE &&
      go_params->max_iterations <= 0) {
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
  if (!ucgi_command_vars->simmer) {
    ucgi_command_vars->simmer = create_simmer(ucgi_command_vars->config);
  }
  simulate(ucgi_command_vars->thread_control, ucgi_command_vars->simmer,
           ucgi_command_vars->loaded_game, NULL,
           ucgi_command_vars->go_params->depth,
           ucgi_command_vars->go_params->threads,
           ucgi_command_vars->go_params->num_plays,
           ucgi_command_vars->go_params->max_iterations,
           ucgi_command_vars->go_params->stop_condition,
           ucgi_command_vars->go_params->static_search_only);
}

void ucgi_infer(UCGICommandVars *ucgi_command_vars) {
  if (!ucgi_command_vars->inference) {
    ucgi_command_vars->inference = create_inference(
        ucgi_command_vars->go_params->num_plays,
        ucgi_command_vars->loaded_game->gen->letter_distribution->size);
  }
  Rack *actual_tiles_played = create_rack(
      ucgi_command_vars->loaded_game->gen->letter_distribution->size);
  set_rack_to_string(actual_tiles_played, ucgi_command_vars->go_params->tiles,
                     ucgi_command_vars->loaded_game->gen->letter_distribution);
  infer(ucgi_command_vars->thread_control, ucgi_command_vars->inference,
        ucgi_command_vars->loaded_game, actual_tiles_played,
        ucgi_command_vars->go_params->player_index,
        ucgi_command_vars->go_params->score,
        ucgi_command_vars->go_params->number_of_tiles_exchanged,
        ucgi_command_vars->go_params->equity_margin,
        ucgi_command_vars->go_params->threads);
  destroy_rack(actual_tiles_played);
  if (ucgi_command_vars->inference->status != INFERENCE_STATUS_SUCCESS) {
    char *failure_message = get_formatted_string(
        "inferfail %d\n", ucgi_command_vars->inference->status);
    print_to_file(ucgi_command_vars->thread_control, failure_message);
    free(failure_message);
  }
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
  }
  log_debug("setting current mode to stopped");
  set_mode_stopped(ucgi_command_vars->thread_control);
  return NULL;
}

void *execute_ucgi_command_async(UCGICommandVars *ucgi_command_vars) {
  set_print_info_interval(ucgi_command_vars->thread_control,
                          ucgi_command_vars->go_params->print_info_interval);
  set_check_stopping_condition_interval(
      ucgi_command_vars->thread_control,
      ucgi_command_vars->go_params->check_stopping_condition_interval);
  unhalt(ucgi_command_vars->thread_control);
  pthread_t cmd_execution_thread;
  pthread_create(&cmd_execution_thread, NULL, execute_ucgi_command,
                 ucgi_command_vars);
  pthread_detach(cmd_execution_thread);
  return NULL;
}

int ucgi_go_async(const char *go_cmd, UCGICommandVars *ucgi_command_vars) {
  int status = UCGI_COMMAND_STATUS_SUCCESS;
  if (set_mode_searching(ucgi_command_vars->thread_control)) {
    // FIXME: should use string copy, or better yet
    // refactor all of this stuff argparse stuff.
    char *mutable_go_cmd = get_formatted_string(go_cmd);
    int parse_status =
        parse_go_cmd(mutable_go_cmd, ucgi_command_vars->go_params);
    free(mutable_go_cmd);
    if (parse_status == GO_PARAMS_PARSE_SUCCESS) {
      execute_ucgi_command_async(ucgi_command_vars);
    } else {
      // No async command was started, so set the search
      // status to stopped.
      set_mode_stopped(ucgi_command_vars->thread_control);
      status = UCGI_COMMAND_STATUS_COMMAND_PARSE_FAILED;
    }
  } else {
    status = UCGI_COMMAND_STATUS_NOT_STOPPED;
  }
  return status;
}

cgp_parse_status_t load_position(UCGICommandVars *ucgi_command_vars,
                                 const char *cgp, char *lexicon_name,
                                 char *ldname, int move_list_capacity) {

  load_config_from_lexargs(&ucgi_command_vars->config, cgp, lexicon_name,
                           ldname);
  ucgi_command_vars->config->move_list_capacity = move_list_capacity;

  if (!ucgi_command_vars->loaded_game ||
      !strings_equal(ucgi_command_vars->last_lexicon_name, lexicon_name) ||
      !strings_equal(ucgi_command_vars->last_ld_name, ldname)) {
    log_debug("creating game");
    if (ucgi_command_vars->loaded_game) {
      destroy_game(ucgi_command_vars->loaded_game);
    }
    ucgi_command_vars->loaded_game = create_game(ucgi_command_vars->config);
  } else {
    // assume config is the same so just reset the game.
    log_debug("resetting game");
    reset_game(ucgi_command_vars->loaded_game);
  }

  log_debug("loading cgp: %s", ucgi_command_vars->config->cgp);
  cgp_parse_status_t cgp_parse_status =
      load_cgp(ucgi_command_vars->loaded_game, ucgi_command_vars->config->cgp);
  log_debug("loaded game");

  string_copy(ucgi_command_vars->last_lexicon_name, lexicon_name);
  string_copy(ucgi_command_vars->last_ld_name, ldname);
  return cgp_parse_status;
}

cgp_parse_status_t ucgi_load_position(UCGICommandVars *ucgi_command_vars,
                                      CGPOperations *cgp_operations,
                                      const char *cgpstr) {
  cgp_parse_status_t cgp_parse_status =
      load_cgp_operations(cgp_operations, cgpstr);
  if (cgp_parse_status != CGP_PARSE_STATUS_SUCCESS) {
    return UCGI_COMMAND_STATUS_CGP_PARSE_FAILED;
  }
  if (!cgp_operations->lexicon_name) {
    return UCGI_COMMAND_STATUS_LEXICON_LD_FAILURE;
  }
  if (!cgp_operations->letter_distribution_name) {
    cgp_operations->letter_distribution_name =
        get_letter_distribution_name_from_lexicon_name(
            cgp_operations->lexicon_name);
  }
  cgp_parse_status =
      load_position(ucgi_command_vars, cgpstr, cgp_operations->lexicon_name,
                    cgp_operations->letter_distribution_name, 100);
  if (cgp_parse_status != CGP_PARSE_STATUS_SUCCESS) {
    return UCGI_COMMAND_STATUS_CGP_PARSE_FAILED;
  }
  return UCGI_COMMAND_STATUS_SUCCESS;
}

int process_ucgi_command_async(const char *cmd,
                               UCGICommandVars *ucgi_command_vars) {
  // basic commands
  if (strings_equal(cmd, "ucgi")) {
    fprintf(ucgi_command_vars->outfile, "id name MAGPIE 0.1\n");
    fprintf(ucgi_command_vars->outfile, "ucgiok\n");
    fflush(ucgi_command_vars->outfile);
  } else if (strings_equal(cmd, "quit")) {
    return UCGI_COMMAND_STATUS_QUIT;
  }

  // other commands
  if (has_prefix("position cgp ", cmd)) {
    const char *cgpstr = cmd + string_length("position cgp ");
    CGPOperations *cgp_operations = get_default_cgp_operations();
    ucgi_command_status_t command_status =
        ucgi_load_position(ucgi_command_vars, cgp_operations, cgpstr);
    destroy_cgp_operations(cgp_operations);
    if (command_status != UCGI_COMMAND_STATUS_SUCCESS) {
      return command_status;
    }
  } else if (has_prefix("go", cmd)) {
    ucgi_command_status_t command_status =
        ucgi_go_async(cmd + string_length("go"), ucgi_command_vars);
    if (command_status == UCGI_COMMAND_STATUS_COMMAND_PARSE_FAILED) {
      log_warn("Failed to parse go command.");
    } else if (command_status == UCGI_COMMAND_STATUS_NOT_STOPPED) {
      log_info("There is already a search ongoing.");
    }
    return command_status;
  } else if (strings_equal(cmd, "stop")) {
    if (get_mode(ucgi_command_vars->thread_control)) {
      if (!halt(ucgi_command_vars->thread_control,
                HALT_STATUS_USER_INTERRUPT)) {
        log_warn("Search already received stop signal but has not stopped.");
      }
    } else {
      log_info("There is no search to stop.");
    }
  }

  return UCGI_COMMAND_STATUS_SUCCESS;
}

// ucgi_search_status returns the current status of the ongoing search. The
// returned string must be freed by the caller.
// Note: this function does not currently work with the `sim static` search.
// It will deadlock.
char *ucgi_search_status(UCGICommandVars *ucgi_command_vars) {
  if (!ucgi_command_vars) {
    log_warn("The UCGI Command variables struct has not been initialized.");
    return NULL;
  }
  if (!ucgi_command_vars->thread_control) {
    log_warn("Thread controller has not been initialized.");
    return NULL;
  }
  if (!ucgi_command_vars->go_params) {
    log_warn("Search params have not been initialized.");
    return NULL;
  }
  int mode = get_mode(ucgi_command_vars->thread_control);
  // Maybe the search is done already; in that case, we want to see the last
  // results.
  switch (ucgi_command_vars->go_params->search_type) {
  case SEARCH_TYPE_SIM_MONTECARLO:
    if (!ucgi_command_vars->simmer) {
      log_warn("Simmer has not been initialized.");
      return NULL;
    }
    return ucgi_sim_stats(ucgi_command_vars->simmer,
                          ucgi_command_vars->loaded_game, mode == MODE_STOPPED);
    break;
  }
}

char *ucgi_stop_search(UCGICommandVars *ucgi_command_vars) {
  if (!ucgi_command_vars) {
    log_warn("The UCGI Command variables struct has not been initialized.");
    return NULL;
  }
  if (!ucgi_command_vars->thread_control) {
    log_warn("Thread controller has not been initialized.");
    return NULL;
  }
  if (!ucgi_command_vars->go_params) {
    log_warn("Search params have not been initialized.");
    return NULL;
  }

  int mode = get_mode(ucgi_command_vars->thread_control);
  if (mode != MODE_SEARCHING) {
    log_warn("Not currently searching.");
    return NULL;
  }
  switch (ucgi_command_vars->go_params->search_type) {
  case SEARCH_TYPE_SIM_MONTECARLO:
    if (!ucgi_command_vars->simmer) {
      log_warn("Simmer has not been initialized.");
      return NULL;
    }
    halt(ucgi_command_vars->thread_control, HALT_STATUS_USER_INTERRUPT);
    wait_for_mode_stopped(ucgi_command_vars->thread_control);
    return ucgi_sim_stats(ucgi_command_vars->simmer,
                          ucgi_command_vars->loaded_game, 1);
    break;
  }
}