#include "../src/impl/cmd_api.h"
#include "../src/impl/exec.h"
#include "../src/util/io_util.h"

static Magpie *wasm_magpie = NULL;

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

int wasm_run_command(const char *command) {
  if (!wasm_magpie) {
    return MAGPIE_DID_NOT_RUN;
  }
  return magpie_run_sync(wasm_magpie, command);
}

// Async version - runs the command on a worker thread for responsiveness.
// Poll wasm_get_thread_status for completion, then read the output.
void wasm_run_command_async(const char *command) {
  if (!wasm_magpie) {
    return;
  }
  magpie_run_async(wasm_magpie, command);
}

char *wasm_get_output(void) {
  if (!wasm_magpie) {
    return NULL;
  }
  // Reap the worker thread if the async command has finished; if one is
  // still running this returns the output of the previous command.
  if (magpie_get_thread_status(wasm_magpie) != MAGPIE_THREAD_STATUS_STARTED) {
    magpie_await(wasm_magpie);
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
    return MAGPIE_THREAD_STATUS_UNINITIALIZED;
  }
  return magpie_get_thread_status(wasm_magpie);
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
