#include "../src/compat/cpthread.h"
#include "../src/def/cpthread_defs.h"
#include "../src/def/wmp_defs.h"
#include "../src/ent/wmp.h"
#include "../src/ent/word_info_table.h"
#include "../src/impl/browser_game.h"
#include "../src/impl/cmd_api.h"
#include "../src/impl/cmd_api_wmp.h"
#include "../src/impl/exec.h"
#include "../src/impl/wmp_maker.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

static Magpie *wasm_magpie = NULL;

int wasm_magpie_init(const char *data_paths) {
  if (wasm_magpie) {
    magpie_destroy(wasm_magpie);
  }
  wasm_magpie = magpie_create(data_paths);
  return wasm_magpie && magpie_run_sync(wasm_magpie, "set -pegjson true") ==
                            MAGPIE_SUCCESS
             ? 0
             : 1;
}

void wasm_magpie_destroy(void) {
  if (wasm_magpie) {
    magpie_destroy(wasm_magpie);
    wasm_magpie = NULL;
  }
  browser_game_cache_destroy();
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
int wasm_run_command_async(const char *command) {
  if (!wasm_magpie) {
    return MAGPIE_DID_NOT_RUN;
  }
  return magpie_run_async(wasm_magpie, command);
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

// WMP construction/parsing runs on a coordinator pthread. The JS worker can
// continue pumping messages and publishing progress while its child threads
// run.
typedef enum {
  WORD_MAP_BUILDING = 0,
  WORD_MAP_SAVING_WMP = 5,
  WORD_MAP_BUILDING_WIT = 6,
  WORD_MAP_LOADING_WIT = 7,
  WORD_MAP_SAVING_WIT = 8,
} WordMapPhase;
typedef struct {
  _Atomic int wmp_phase;

  cpthread_t wmp_thread;
  bool wmp_joinable;
  _Atomic bool wmp_done;
  WMPBuildProgress *wmp_progress;
  WMP *prepared_wmp;
  WordInfoTable *prepared_wit;
  int wmp_threads;
  const char *wmp_name;
  bool wmp_save;
  char *wmp_export;
  size_t wmp_export_size;
  unsigned char *wmp_bytes;
  size_t wmp_size;
  unsigned char *wit_bytes;
  size_t wit_size;
  char *wit_export;
  size_t wit_export_size;

} WordMapPreparation;

static WordMapPreparation word_maps;

static WordInfoTable *load_cached_wit(void) {
  ErrorStack *error_stack = error_stack_create();
  WordInfoTable *wit = NULL;
  FILE *stream = fmemopen(word_maps.wit_bytes, word_maps.wit_size, "rb");
  if (stream) {
    wit = calloc_or_die(1, sizeof(WordInfoTable));
    word_info_table_load_from_stream(wit, word_maps.wmp_name, stream,
                                     error_stack);
    const bool closed = fclose(stream) == 0;
    if (!closed || !error_stack_is_empty(error_stack)) {
      word_info_table_destroy(wit);
      wit = NULL;
    }
  }
  error_stack_destroy(error_stack);
  return wit;
}

static void export_prepared_wit(void) {
  ErrorStack *error_stack = error_stack_create();
  FILE *stream =
      open_memstream(&word_maps.wit_export, &word_maps.wit_export_size);
  if (stream) {
    word_info_table_write_to_stream(word_maps.prepared_wit, stream,
                                    error_stack);
    const bool closed = fclose(stream) == 0;
    if (!closed || !error_stack_is_empty(error_stack)) {
      free(word_maps.wit_export);
      word_maps.wit_export = NULL;
      word_maps.wit_export_size = 0;
    }
  }
  error_stack_destroy(error_stack);
}

static void *prepare_wmp(void *unused) {
  (void)unused;
  if (word_maps.wmp_bytes) {
    FILE *stream = fmemopen(word_maps.wmp_bytes, word_maps.wmp_size, "rb");
    if (stream) {
      ErrorStack *error_stack = error_stack_create();
      word_maps.prepared_wmp = calloc_or_die(1, sizeof(WMP));
      wmp_load_from_filename_with_stream(word_maps.prepared_wmp,
                                         word_maps.wmp_name, word_maps.wmp_name,
                                         stream, error_stack);
      const bool closed = fclose(stream) == 0;
      if (!closed || !error_stack_is_empty(error_stack)) {
        wmp_destroy(word_maps.prepared_wmp);
        word_maps.prepared_wmp = NULL;
      }
      error_stack_destroy(error_stack);
    }
  } else {
    word_maps.prepared_wmp = cmd_api_build_wmp(
        wasm_magpie, word_maps.wmp_threads, word_maps.wmp_progress);
  }
  if (word_maps.prepared_wmp && !word_maps.wmp_bytes && word_maps.wmp_save &&
      !wmp_build_progress_is_cancelled(word_maps.wmp_progress)) {
    atomic_store(&word_maps.wmp_phase, WORD_MAP_SAVING_WMP);
    FILE *stream =
        open_memstream(&word_maps.wmp_export, &word_maps.wmp_export_size);
    if (stream) {
      wmp_write_to_stream(word_maps.prepared_wmp, stream);
      if (fclose(stream) != 0) {
        free(word_maps.wmp_export);
        word_maps.wmp_export = NULL;
        word_maps.wmp_export_size = 0;
      }
    }
  }
  if (word_maps.prepared_wmp &&
      !wmp_build_progress_is_cancelled(word_maps.wmp_progress)) {
    if (word_maps.wit_bytes) {
      atomic_store(&word_maps.wmp_phase, WORD_MAP_LOADING_WIT);
      word_maps.prepared_wit = load_cached_wit();
    }
    if (!word_maps.prepared_wit) {
      atomic_store(&word_maps.wmp_phase, WORD_MAP_BUILDING_WIT);
      word_maps.prepared_wit = cmd_api_build_wit(wasm_magpie);
    }
    if (word_maps.prepared_wit && word_maps.wmp_save &&
        !wmp_build_progress_is_cancelled(word_maps.wmp_progress)) {
      atomic_store(&word_maps.wmp_phase, WORD_MAP_SAVING_WIT);
      export_prepared_wit();
    }
  }
  atomic_store(&word_maps.wmp_done, true);
  return NULL;
}

// name remains borrowed until finish; bytes until stage 6 or later.
// Cached WIT bytes remain borrowed until finish. Downloads are validated by the
// worker before entering the native binary reader.
int wasm_prepare_wmp(const char *name, unsigned char *bytes, int size,
                     int threads, int save, unsigned char *cached_wit,
                     int cached_wit_size) {
  if (!wasm_magpie || word_maps.wmp_joinable ||
      magpie_get_thread_status(wasm_magpie) == MAGPIE_THREAD_STATUS_STARTED) {
    return 1;
  }
  word_maps.wmp_name = name;
  word_maps.wmp_save = save != 0;
  word_maps.wit_bytes = cached_wit;
  word_maps.wit_size = (size_t)cached_wit_size;
  word_maps.wit_export = NULL;
  word_maps.wit_export_size = 0;
  word_maps.wmp_export = NULL;
  word_maps.wmp_export_size = 0;
  word_maps.wmp_bytes = bytes;
  word_maps.wmp_size = (size_t)size;
  word_maps.wmp_threads = threads;
  word_maps.prepared_wmp = NULL;
  word_maps.prepared_wit = NULL;
  word_maps.wmp_progress = wmp_build_progress_create();
  atomic_store(&word_maps.wmp_done, false);
  atomic_store(&word_maps.wmp_phase, WORD_MAP_BUILDING);
  word_maps.wmp_joinable = true;
  cpthread_create(&word_maps.wmp_thread, prepare_wmp, NULL);
  return 0;
}

void wasm_cancel_wmp(void) {
  wmp_build_progress_cancel(word_maps.wmp_progress);
}

// Caller frees the returned JSON string. Counts have no decimal packing limit.
char *wasm_wmp_progress(void) {
  if (atomic_load(&word_maps.wmp_done)) {
    return string_duplicate("{\"done\":true}");
  }
  wmp_build_stage_t stage = WMP_BUILD_WORD_LIST;
  int completed = 0;
  int total = 0;
  const int phase = atomic_load(&word_maps.wmp_phase);
  if (phase == WORD_MAP_BUILDING && word_maps.wmp_progress) {
    wmp_build_progress_snapshot(word_maps.wmp_progress, &stage, &completed,
                                &total);
  }
  return get_formatted_string(
      "{\"done\":false,\"stage\":%d,\"completed\":%d,\"total\":%d}",
      phase == WORD_MAP_BUILDING ? (int)stage : phase, completed, total);
}

int wasm_finish_wmp(int install) {
  if (!word_maps.wmp_joinable || !atomic_load(&word_maps.wmp_done)) {
    return 1;
  }
  cpthread_join(word_maps.wmp_thread);
  word_maps.wmp_joinable = false;
  wmp_build_progress_destroy(word_maps.wmp_progress);
  word_maps.wmp_progress = NULL;
  const bool success =
      install && cmd_api_install_wmp(wasm_magpie, word_maps.prepared_wmp,
                                     word_maps.prepared_wit);
  if (!success) {
    wmp_destroy(word_maps.prepared_wmp);
    word_info_table_destroy(word_maps.prepared_wit);
  }
  word_maps.prepared_wmp = NULL;
  word_maps.prepared_wit = NULL;
  free(word_maps.wmp_export);
  word_maps.wmp_export = NULL;
  free(word_maps.wit_export);
  word_maps.wit_export = NULL;
  return success ? 0 : 1;
}

const char *wasm_wmp_data(void) {
  return atomic_load(&word_maps.wmp_done) ? word_maps.wmp_export : NULL;
}

int wasm_wmp_data_size(void) {
  return atomic_load(&word_maps.wmp_done) ? (int)word_maps.wmp_export_size : 0;
}

const char *wasm_wit_data(void) {
  return atomic_load(&word_maps.wmp_done) ? word_maps.wit_export : NULL;
}
int wasm_wit_data_size(void) {
  return atomic_load(&word_maps.wmp_done) ? (int)word_maps.wit_export_size : 0;
}
