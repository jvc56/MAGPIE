#include "../src/impl/cmd_api.h"
#include "../src/impl/exec.h"
#include "../src/impl/config.h"
#include "../src/impl/wmp_maker.h"
#include "../src/ent/game.h"
#include "../src/ent/player.h"
#include "../src/ent/players_data.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/wmp.h"
#include "../src/util/io_util.h"
#include "../src/util/fileproxy.h"
#include "../src/compat/cpthread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Magpie *wasm_magpie = NULL;

// Track user's WMP enabled setting to restore after commands
static bool wmp_enabled = true;

// Thread argument for async command execution
typedef struct {
  Magpie *mp;
  char *command;
  volatile int *done_flag;
} AsyncCommandArgs;

int wasm_magpie_init(const char *data_paths) {
  if (wasm_magpie) {
    magpie_destroy(wasm_magpie);
  }
  wasm_magpie = magpie_create(data_paths);
  return wasm_magpie ? 0 : 1;
}

void wasm_magpie_destroy(void) {
  if (wasm_magpie) {
    magpie_destroy(wasm_magpie);
    wasm_magpie = NULL;
  }
  wmp_enabled = true;
  caches_destroy();
}

// Worker function that runs on a pthread
void *wasm_command_thread_worker(void *arg) {
  if (!arg) {
    return NULL;
  }

  AsyncCommandArgs *args = (AsyncCommandArgs *)arg;

  if (!args->mp) {
    free(args->command);
    free(args);
    return NULL;
  }

  if (!args->command) {
    free(args);
    return NULL;
  }

  magpie_run_sync(args->mp, args->command);

  if (args->done_flag) {
    *args->done_flag = 1;
  }
  free(args->command);
  free(args);
  return NULL;
}

// Helper to restore WMP enabled state after commands run
static void restore_wmp_enabled_state(void) {
  if (!wasm_magpie) {
    return;
  }

  Config *config = magpie_get_config(wasm_magpie);
  if (!config) {
    return;
  }

  PlayersData *players_data = config_get_players_data(config);
  if (!players_data) {
    return;
  }

  // Only restore if we have an external WMP
  if (players_data_wmp_is_external(players_data)) {
    players_data_set_use_when_available(players_data, PLAYERS_DATA_TYPE_WMP, 0, wmp_enabled);
    players_data_set_use_when_available(players_data, PLAYERS_DATA_TYPE_WMP, 1, wmp_enabled);
  }
}

// Synchronous version
int wasm_run_command(const char *command) {
  if (!wasm_magpie) {
    return 2; // MAGPIE_DID_NOT_RUN
  }
  int result = magpie_run_sync(wasm_magpie, command);

  // Restore WMP enabled state in case command changed it
  restore_wmp_enabled_state();

  return result;
}

// Async version - spawns a pthread for better responsiveness
void wasm_run_command_async(const char *command) {
  if (!wasm_magpie) {
    return;
  }

  // Allocate args on heap - thread will free them
  AsyncCommandArgs *args = malloc(sizeof(AsyncCommandArgs));
  args->mp = wasm_magpie;
  args->command = strdup(command);
  args->done_flag = NULL;

  cpthread_t thread;
  cpthread_create(&thread, wasm_command_thread_worker, args);
  cpthread_detach(thread);
}

char *wasm_get_output(void) {
  if (!wasm_magpie) {
    return NULL;
  }
  return magpie_get_last_command_output(wasm_magpie);
}

char *wasm_get_error(void) {
  if (!wasm_magpie) {
    return NULL;
  }
  return magpie_get_and_clear_error(wasm_magpie);
}

char *wasm_get_status(void) {
  if (!wasm_magpie) {
    return NULL;
  }
  return magpie_get_last_command_status_message(wasm_magpie);
}

// Returns 0=uninitialized, 1=started, 2=user_interrupt, 3=finished
int wasm_get_thread_status(void) {
  if (!wasm_magpie) {
    return 0;
  }
  Config *config = magpie_get_config(wasm_magpie);
  if (!config) {
    return 0;
  }
  ThreadControl *tc = config_get_thread_control(config);
  if (!tc) {
    return 0;
  }
  return (int)thread_control_get_status_unsafe(tc);
}

void wasm_stop_command(void) {
  if (wasm_magpie) {
    magpie_stop_current_command(wasm_magpie);
  }
}

// Build WMP from the loaded KWG lexicon
// Returns 0 on success, 1 if no magpie instance, 2 if no KWG loaded
int wasm_build_wmp(int num_threads) {
  if (!wasm_magpie) {
    return 1;
  }

  Config *config = magpie_get_config(wasm_magpie);
  if (!config) {
    return 1;
  }

  PlayersData *players_data = config_get_players_data(config);
  if (!players_data) {
    return 1;
  }

  KWG *kwg = players_data_get_kwg(players_data, 0);
  if (!kwg) {
    return 2;
  }

  LetterDistribution *ld = config_get_ld(config);
  if (!ld) {
    return 1;
  }

  // Force single-threaded WMP building in WASM
  int actual_threads = 1;
  (void)num_threads;

  WMP *wmp = make_wmp_from_kwg(kwg, ld, actual_threads);
  if (!wmp) {
    return 1;
  }

  players_data_set_wmp_direct(players_data, wmp);

  // Update existing Players so they pick up the new WMP
  // (Players cache WMP at creation time, so we need to refresh them)
  Game *game = config_get_game(config);
  if (game) {
    for (int i = 0; i < 2; i++) {
      Player *player = game_get_player(game, i);
      if (player) {
        player_update(players_data, player);
      }
    }
  }

  return 0;
}

// Enable or disable WMP usage (even if WMP is loaded)
int wasm_set_wmp_enabled(int enabled) {
  if (!wasm_magpie) {
    return 1;
  }

  Config *config = magpie_get_config(wasm_magpie);
  if (!config) {
    return 1;
  }

  PlayersData *players_data = config_get_players_data(config);
  if (!players_data) {
    return 1;
  }

  bool use_wmp = enabled != 0;
  wmp_enabled = use_wmp;

  players_data_set_use_when_available(players_data, PLAYERS_DATA_TYPE_WMP, 0, use_wmp);
  players_data_set_use_when_available(players_data, PLAYERS_DATA_TYPE_WMP, 1, use_wmp);

  // Update Players so they pick up the changed WMP availability
  Game *game = config_get_game(config);
  if (game) {
    for (int i = 0; i < 2; i++) {
      Player *player = game_get_player(game, i);
      if (player) {
        player_update(players_data, player);
      }
    }
  }

  return 0;
}

// Returns: 0 = no WMP, 1 = WMP loaded but disabled, 2 = WMP loaded and enabled
int wasm_get_wmp_status(void) {
  if (!wasm_magpie) {
    return 0;
  }

  Config *config = magpie_get_config(wasm_magpie);
  if (!config) {
    return 0;
  }

  PlayersData *players_data = config_get_players_data(config);
  if (!players_data) {
    return 0;
  }

  WMP *wmp = players_data_get_wmp(players_data, 0);
  if (!wmp) {
    return 0;
  }

  bool enabled = players_data_get_use_when_available(players_data, PLAYERS_DATA_TYPE_WMP, 0);
  return enabled ? 2 : 1;
}

int main(void) {
  log_set_level(LOG_INFO);
  return 0;
}
