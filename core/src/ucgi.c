#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "log.h"
#include "sim.h"
#include "ucgi.h"

#define CMD_MAX 256

#define MODE_SEARCHING 1
#define MODE_STOPPED 0

typedef struct GoParams {
  int infinite;
  int depth;
  int stop_condition;
  int threads;
} GoParams;

static Game *loaded_game = NULL;
static Config *config = NULL;
static Simmer *simmer = NULL;
static int current_mode = MODE_STOPPED;

int prefix(const char *pre, const char *str) {
  return strncmp(pre, str, strlen(pre)) == 0;
}

void set_mode_stopped_callback() {
  current_mode = MODE_STOPPED;
  log_debug("setting current mode to stopped");
}

void load_position(const char *cgp, char *lexicon_name, char *ldname) {
  char dist[50];
  sprintf(dist, "data/letterdistributions/%s.csv", ldname);
  char leaves[50] = "data/lexica/english.klv2";
  char winpct[50] = "data/strategy/default_english/winpct.csv";
  char lexicon_file[50];
  sprintf(lexicon_file, "data/lexica/%s.kwg", lexicon_name);
  if (strcmp(lexicon_name, "CSW21") == 0) {
    strcpy(leaves, "data/lexica/CSW21.klv2");
  }
  if (config != NULL) {
    destroy_config(config);
  }
  config =
      create_config(lexicon_file, dist, cgp, leaves, SORT_BY_EQUITY,
                    PLAY_RECORDER_TYPE_ALL, "", SORT_BY_EQUITY,
                    PLAY_RECORDER_TYPE_ALL, 0, 0, "", 0, 0, 0, 0, 1, winpct);
  if (loaded_game != NULL) {
    destroy_game(loaded_game);
  }
  loaded_game = create_game(config);
  log_debug("created game. loading cgp: %s", config->cgp);
  load_cgp(loaded_game, config->cgp);
  log_debug("loaded game");
}

void start_search(GoParams params) {
  // simming for now, do more later.
  Game *game = loaded_game;
  if (game == NULL) {
    log_warn("No game loaded.");
    return;
  }
  if (params.depth == 0) {
    log_warn("Need a depth.");
    return;
  }
  if (params.threads <= 0) {
    log_warn("Cannot search with 0 threads.");
    return;
  }
  int unseen_tiles = tiles_unseen(game);
  generate_moves(game->gen, game->players[game->player_on_turn_index],
                 game->players[1 - game->player_on_turn_index]->rack,
                 unseen_tiles >= RACK_SIZE);
  MoveList *ml = game->gen->move_list;
  int nmoves = ml->count;
  sort_moves(ml);
  if (simmer != NULL) {
    destroy_simmer(simmer);
  }
  simmer = create_simmer(config, game);
  simmer->ucgi_mode = UCGI_MODE_ON;
  set_endsim_callback(simmer, set_mode_stopped_callback);
  log_debug("nmoves: %d, unseen_tiles: %d", nmoves, unseen_tiles);
  int limit = 0;
  if (unseen_tiles >= RACK_SIZE) {
    limit = 40;
  } else {
    limit = 80;
  }
  if (nmoves < limit) {
    limit = nmoves;
  }
  prepare_simmer(simmer, params.depth, params.threads, ml->moves, limit, NULL);
  if (params.stop_condition != SIM_STOPPING_CONDITION_NONE) {
    set_stopping_condition(simmer, params.stop_condition);
  }
  simulate(simmer);
  current_mode = MODE_SEARCHING;
}

void stop_search() { // stop simming for now, more later.
  stop_simming(simmer);
}

GoParams parse_go_cmd(char *params) {
  // parse the go cmd.
  char *token;
  GoParams gparams = {0};
  token = strtok(params, " ");
  int reading_depth = 0;
  int reading_stop_condition = 0;
  int reading_threads = 0;
  while (token != NULL) {
    if (reading_depth) {
      gparams.depth = atoi(token);
    } else if (reading_threads) {
      gparams.threads = atoi(token);
    } else if (reading_stop_condition) {
      if (strcmp(token, "95") == 0) {
        gparams.stop_condition = SIM_STOPPING_CONDITION_95PCT;
      } else if (strcmp(token, "98") == 0) {
        gparams.stop_condition = SIM_STOPPING_CONDITION_98PCT;
      } else if (strcmp(token, "99") == 0) {
        gparams.stop_condition = SIM_STOPPING_CONDITION_99PCT;
      } else {
        log_warn("Did not understand stopping condition %s", token);
      }
    }
    if (strcmp(token, "infinite") == 0) {
      gparams.infinite = 1;
    }
    reading_depth = strcmp(token, "depth") == 0;
    reading_stop_condition = strcmp(token, "stopcondition") == 0;
    reading_threads = strcmp(token, "threads") == 0;
    token = strtok(NULL, " ");
  }
  log_debug("Returning gparams; inf %d stop %d depth %d threads %d",
            gparams.infinite, gparams.stop_condition, gparams.depth,
            gparams.threads);
  if (gparams.stop_condition != SIM_STOPPING_CONDITION_NONE &&
      gparams.infinite) {
    log_warn("Cannot have a stopping condition and also search infinitely.");
    gparams.infinite = 0;
  }
  if (gparams.threads <= 0) {
    gparams.threads = 1;
  }
  return gparams;
}

void ucgi_scan_loop() {
  while (1) {
    char cmd[CMD_MAX];
    if (fgets(cmd, CMD_MAX, stdin) == NULL) {
      return;
    }
    // replace newline with 0 for ease in comparison
    cmd[strcspn(cmd, "\n")] = 0;

    int should_end = process_ucgi_command(cmd);
    if (should_end) {
      return;
    }
  }
}

int process_ucgi_command(char *cmd) {
  // basic commands
  if (strcmp(cmd, "ucgi") == 0) {
    fprintf(stdout, "id name MAGPIE 0.1\n");
    fprintf(stdout, "ucgiok\n");
  } else if (strcmp(cmd, "quit") == 0) {
    return 1;
  }

  // other commands
  if (prefix("position cgp ", cmd)) {
    char *cgpstr = cmd + strlen("position cgp ");
    char lexicon[20] = "";
    char ldname[20] = "";
    lexicon_ld_from_cgp(cgpstr, lexicon, ldname);
    if (strcmp(lexicon, "") == 0) {
      return 0;
    }
    load_position(cgpstr, lexicon, ldname);
  } else if (prefix("go", cmd)) {
    if (current_mode == MODE_STOPPED) {
      GoParams params = parse_go_cmd(cmd + strlen("go"));
      start_search(params);
    } else {
      log_info("There is already a search ongoing.");
    }
  } else if (strcmp(cmd, "stop") == 0) {
    if (current_mode == MODE_SEARCHING) {
      stop_search();
    } else {
      log_info("There is no search to stop.");
    }
  }

  return 0;
}