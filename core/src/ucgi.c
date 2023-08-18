#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "log.h"
#include "sim.h"
#include "ucgi.h"
#include "ucgi_formats.h"
#include "util.h"

#define CMD_MAX 256

#define MODE_SEARCHING 1
#define MODE_STOPPED 0

#define SEARCH_TYPE_MONTECARLO 1
#define SEARCH_TYPE_INFERENCE 2
#define SEARCH_TYPE_ENDGAME 3
#define SEARCH_TYPE_PREENDGAME 4
#define SEARCH_TYPE_STATICONLY 5

typedef struct GoParams {
  int infinite;
  int depth;
  int stop_condition;
  int threads;
  int static_search_only;
} GoParams;

struct ucgithreadcontrol {
  int search_type;
};

static Game *loaded_game = NULL;
static Config *config = NULL;
static Simmer *simmer = NULL;
static int current_mode = MODE_STOPPED;
static pthread_t manager_thread;
static struct ucgithreadcontrol manager_thread_controller;
static char last_lexicon_name[16] = "";
static char last_ld_name[16] = "";

void load_position(const char *cgp, char *lexicon_name, char *ldname,
                   int move_list_capacity) {

  load_config_from_lexargs(&config, cgp, lexicon_name, ldname);
  config->move_list_capacity = move_list_capacity;

  if (loaded_game == NULL || strcmp(last_lexicon_name, lexicon_name) ||
      strcmp(last_ld_name, ldname)) {
    log_debug("creating game");
    if (loaded_game != NULL) {
      destroy_game(loaded_game);
    }
    loaded_game = create_game(config);
  } else {
    // assume config is the same so just reset the game.
    log_debug("resetting game");
    reset_game(loaded_game);
  }

  log_debug("loading cgp: %s", config->cgp);
  load_cgp(loaded_game, config->cgp);
  log_debug("loaded game");

  strcpy(last_lexicon_name, lexicon_name);
  strcpy(last_ld_name, ldname);
}

void ucgi_print_moves(MoveList *ml, Game *game, int nmoves) {
  for (int i = 0; i < nmoves; i++) {
    char move[30];
    store_move_ucgi(ml->moves[i], game->gen->board, move,
                    game->gen->letter_distribution);
    fprintf(stdout, "info currmove %s sc %d eq %.3f it 0\n", move,
            ml->moves[i]->score, ml->moves[i]->equity);
  }
  char move[30];
  store_move_ucgi(ml->moves[0], game->gen->board, move,
                  game->gen->letter_distribution);

  fprintf(stdout, "bestmove %s\n", move);
}

void *ucgi_manager_thread(void *ptr) {
  struct ucgithreadcontrol *tc = ptr;
  switch (tc->search_type) {
  case SEARCH_TYPE_MONTECARLO:
    simulate(simmer);
    // Join when we are done. This will block, but in its own separate thread.
    join_threads(simmer);
    break;
  default:
    log_warn("Search type not set; exiting immediately.");
  }
  current_mode = MODE_STOPPED;
  log_debug("setting current mode to stopped");
  return NULL;
}

void start_search(GoParams params) {
  // simming for now, do more later.
  Game *game = loaded_game;
  if (game == NULL) {
    log_warn("No game loaded.");
    return;
  }

  // unseen_tiles are the number of tiles not on the board and not
  // on the player-on-turn's rack. They're split up between the bag
  // and the opponent's rack. Only when that number is RACK_SIZE*2 or
  // bigger should we allow an exchange.
  int unseen_tiles = tiles_unseen(game);
  reset_move_list(game->gen->move_list);
  generate_moves(game->gen, game->players[game->player_on_turn_index],
                 game->players[1 - game->player_on_turn_index]->rack,
                 unseen_tiles >= RACK_SIZE * 2);
  MoveList *ml = game->gen->move_list;
  int nmoves = ml->count;
  sort_moves(ml);

  // No sim needed.
  if (params.static_search_only) {
    ucgi_print_moves(ml, game, 15);
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

  if (simmer != NULL) {
    log_debug("Destroying old simmer");
    destroy_simmer(simmer);
  }
  simmer = create_simmer(config, game);
  simmer->ucgi_mode = UCGI_MODE_ON;
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

  manager_thread_controller.search_type = SEARCH_TYPE_MONTECARLO;

  current_mode = MODE_SEARCHING;

  pthread_create(&manager_thread, NULL, ucgi_manager_thread,
                 &manager_thread_controller);
  pthread_detach(manager_thread);
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
    if (strcmp(token, "static") == 0) {
      gparams.static_search_only = 1;
    }
    reading_depth = strcmp(token, "depth") == 0;
    reading_stop_condition = strcmp(token, "stopcondition") == 0;
    reading_threads = strcmp(token, "threads") == 0;
    token = strtok(NULL, " ");
  }
  log_debug("Returning gparams; inf %d stop %d depth %d threads %d ss %d",
            gparams.infinite, gparams.stop_condition, gparams.depth,
            gparams.threads, gparams.static_search_only);
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
      break;
    }
    // replace newline with 0 for ease in comparison
    cmd[strcspn(cmd, "\n")] = 0;

    int should_end = process_ucgi_command(cmd);
    if (should_end) {
      break;
    }
  }

  // clean up
  if (loaded_game != NULL) {
    destroy_game(loaded_game);
  }
  if (config != NULL) {
    destroy_config(config);
  }
  if (simmer != NULL) {
    destroy_simmer(simmer);
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
    char lexicon[16] = "";
    char ldname[16] = "";
    lexicon_ld_from_cgp(cgpstr, lexicon, ldname);
    if (strcmp(lexicon, "") == 0) {
      return 0;
    }
    load_position(cgpstr, lexicon, ldname, 100);
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