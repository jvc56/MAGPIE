#include "../src/compat/cpthread.h"
#include "../src/ent/thread_control.h"
#include "../src/impl/cmd_api.h"
#include "../src/impl/config.h"
#include "../src/impl/exec.h"
#include "../src/util/fileproxy.h"
#include "../src/util/io_util.h"
#include <stdlib.h>
#include <string.h>

static Magpie *wasm_magpie = NULL;
static pthread_t command_handler_thread;
static bool command_handler_running = false;

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

  // Now that we've bypassed mutexes for WASM, this should work
  int result = magpie_run_sync(args->mp, args->command);

  if (args->done_flag) {
    *args->done_flag = 1;
  }
  free(args->command);
  free(args);
  return NULL;
}

// Synchronous version - now works since we bypass mutexes for WASM
int wasm_run_command(const char *command) {
  if (!wasm_magpie) {
    return 2; // MAGPIE_DID_NOT_RUN
  }
  return magpie_run_sync(wasm_magpie, command);
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
  cpthread_detach(thread); // Don't need to join - let it run independently
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
// Uses lock-free read to avoid deadlock when called from main thread
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
  return (int)thread_control_get_status(tc);
}

void wasm_stop_command(void) {
  if (wasm_magpie) {
    magpie_stop_current_command(wasm_magpie);
  }
}

int main(void) {
  log_set_level(LOG_INFO);
  return 0;
}
