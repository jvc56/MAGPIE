#include "browser_game.h"
#include "../src/compat/cpthread.h"
#include "../src/def/board_defs.h"
#include "../src/def/cpthread_defs.h"
#include "../src/ent/wmp.h"
#include "../src/ent/word_info_table.h"
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
#include <unistd.h>

static Magpie *wasm_magpie = NULL;

int wasm_magpie_init(const char *data_paths) {
  if (wasm_magpie) {
    magpie_destroy(wasm_magpie);
  }
  wasm_magpie = magpie_create(data_paths);
  magpie_set_peg_json(wasm_magpie, true);
  return wasm_magpie ? 0 : 1;
}

void wasm_magpie_destroy(void) {
  if (wasm_magpie) {
    magpie_destroy(wasm_magpie);
    wasm_magpie = NULL;
  }
  wasm_game_cache_destroy();
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
static cpthread_t wmp_thread;
static bool wmp_joinable = false;
static _Atomic bool wmp_done = false;
static WMPBuildProgress wmp_progress;
static WMP *prepared_wmp = NULL;
static WordInfoTable *prepared_wit = NULL;
static int wmp_threads;
static const char *wmp_name;
static bool wmp_save;
static char *wmp_export;
static size_t wmp_export_size;
static unsigned char *wmp_bytes;
static size_t wmp_size;
static unsigned char *wit_bytes;
static size_t wit_size;
static char *wit_export;
static size_t wit_export_size;

// Use a short-lived MEMFS file with the existing validated WIT reader/writer.
// The coordinator is the sole owner; no persistent or network file is involved.
static WordInfoTable *load_cached_wit(void) {
  ErrorStack *errors = error_stack_create();
  WordInfoTable *wit = NULL;
  FILE *stream = fopen("/tmp/magpie-cache.wit", "wb");
  if (stream) {
    const bool written = fwrite(wit_bytes, 1, wit_size, stream) == wit_size;
    const bool closed = fclose(stream) == 0;
    if (written && closed) {
      wit = calloc_or_die(1, sizeof(WordInfoTable));
      word_info_table_load(wit, wmp_name, "/tmp/magpie-cache.wit", errors);
      if (!error_stack_is_empty(errors)) {
        word_info_table_destroy(wit);
        wit = NULL;
      }
    }
  }
  unlink("/tmp/magpie-cache.wit");
  error_stack_destroy(errors);
  return wit;
}

static void export_prepared_wit(void) {
  ErrorStack *errors = error_stack_create();
  word_info_table_write_to_file(prepared_wit, "/tmp/magpie-cache.wit", errors);
  FILE *stream = error_stack_is_empty(errors)
                     ? fopen("/tmp/magpie-cache.wit", "rb")
                     : NULL;
  if (stream) {
    if (fseek(stream, 0, SEEK_END) == 0) {
      const long size = ftell(stream);
      if (size > 0 && fseek(stream, 0, SEEK_SET) == 0) {
        wit_export = malloc_or_die((size_t)size);
        if (fread(wit_export, 1, (size_t)size, stream) == (size_t)size) {
          wit_export_size = (size_t)size;
        } else {
          free(wit_export);
          wit_export = NULL;
        }
      }
    }
    fclose(stream);
  }
  unlink("/tmp/magpie-cache.wit");
  error_stack_destroy(errors);
}

static void *prepare_wmp(void *unused) {
  (void)unused;
  if (wmp_bytes) {
    FILE *stream = fmemopen(wmp_bytes, wmp_size, "rb");
    if (stream) {
      ErrorStack *errors = error_stack_create();
      prepared_wmp = calloc_or_die(1, sizeof(WMP));
      wmp_load_from_filename_with_stream(prepared_wmp, wmp_name, wmp_name,
                                         stream, errors);
      fclose(stream);
      if (!error_stack_is_empty(errors)) {
        wmp_destroy(prepared_wmp);
        prepared_wmp = NULL;
      }
      error_stack_destroy(errors);
    }
  } else {
    prepared_wmp = magpie_build_wmp(wasm_magpie, wmp_threads, &wmp_progress);
  }
  if (prepared_wmp && !wmp_bytes && wmp_save && !atomic_load(&wmp_progress.cancelled)) {
    wmp_build_progress_set_stage(&wmp_progress, 5, 0);
    FILE *stream = open_memstream(&wmp_export, &wmp_export_size);
    if (stream) {
      write_byte_to_stream_or_die(prepared_wmp->version, stream, "wmp version");
      write_byte_to_stream_or_die(prepared_wmp->board_dim, stream,
                                  "wmp board dim");
      write_uint32_to_stream_or_die(prepared_wmp->max_word_lookup_bytes, stream,
                                    "wmp max lookup bytes");
      for (int length = 2; length <= BOARD_DIM; length++) {
        write_wfl_to_stream(length, &prepared_wmp->wfls[length], stream);
      }
      fclose(stream);
    }
  }
  if (prepared_wmp && !atomic_load(&wmp_progress.cancelled)) {
    if (wit_bytes) {
      wmp_build_progress_set_stage(&wmp_progress, 7, 0);
      prepared_wit = load_cached_wit();
    }
    if (!prepared_wit) {
      wmp_build_progress_set_stage(&wmp_progress, 6, 0);
      prepared_wit = magpie_build_wit(wasm_magpie);
    }
    if (prepared_wit && wmp_save && !atomic_load(&wmp_progress.cancelled)) {
      wmp_build_progress_set_stage(&wmp_progress, 8, 0);
      export_prepared_wit();
    }
  }
  atomic_store(&wmp_done, true);
  return NULL;
}

// name remains borrowed until finish; bytes until stage 6 or later.
// Cached WIT bytes remain borrowed until finish. Downloads are validated by the
// worker before entering the native binary reader.
int wasm_prepare_wmp(const char *name, unsigned char *bytes, int size,
                     int threads, int save, unsigned char *cached_wit,
                     int cached_wit_size) {
  if (!wasm_magpie || wmp_joinable ||
      magpie_get_thread_status(wasm_magpie) == MAGPIE_THREAD_STATUS_STARTED) {
    return 1;
  }
  wmp_name = name;
  wmp_save = save != 0;
  wit_bytes = cached_wit;
  wit_size = (size_t)cached_wit_size;
  wit_export = NULL;
  wit_export_size = 0;
  wmp_export = NULL;
  wmp_export_size = 0;
  wmp_bytes = bytes;
  wmp_size = (size_t)size;
  wmp_threads = threads;
  prepared_wmp = NULL;
  prepared_wit = NULL;
  atomic_store(&wmp_progress.cancelled, false);
  atomic_store(&wmp_done, false);
  wmp_build_progress_set_stage(&wmp_progress, 0, 0);
  wmp_joinable = true;
  cpthread_create(&wmp_thread, prepare_wmp, NULL);
  return 0;
}

void wasm_cancel_wmp(void) { atomic_store(&wmp_progress.cancelled, true); }

int wasm_wmp_progress(void) {
  if (atomic_load(&wmp_done)) {
    return -1;
  }
  int stage;
  int completed;
  int total;
  wmp_build_progress_snapshot(&wmp_progress, &stage, &completed, &total);
  return stage * 10000 + total * 100 + completed;
}

int wasm_finish_wmp(int install) {
  if (!wmp_joinable || !atomic_load(&wmp_done)) {
    return 1;
  }
  cpthread_join(wmp_thread);
  wmp_joinable = false;
  const bool success =
      install && magpie_install_wmp(wasm_magpie, prepared_wmp, prepared_wit);
  if (!success) {
    wmp_destroy(prepared_wmp);
    word_info_table_destroy(prepared_wit);
  }
  prepared_wmp = NULL;
  prepared_wit = NULL;
  free(wmp_export);
  wmp_export = NULL;
  free(wit_export);
  wit_export = NULL;
  return success ? 0 : 1;
}

const char *wasm_wmp_data(void) {
  return atomic_load(&wmp_done) ? wmp_export : NULL;
}

int wasm_wmp_data_size(void) {
  return atomic_load(&wmp_done) ? (int)wmp_export_size : 0;
}

const char *wasm_wit_data(void) {
  return atomic_load(&wmp_done) ? wit_export : NULL;
}
int wasm_wit_data_size(void) {
  return atomic_load(&wmp_done) ? (int)wit_export_size : 0;
}
