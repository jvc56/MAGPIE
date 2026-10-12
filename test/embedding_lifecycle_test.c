#include "embedding_lifecycle_test.h"

#include "../src/compat/cpthread.h"
#include "../src/def/cpthread_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/thread_control.h"
#include "../src/util/fileproxy.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *read_cached_files(void *unused) {
  (void)unused;
  ErrorStack *error_stack = error_stack_create();
  for (int entry_idx = 0; entry_idx < 300; entry_idx++) {
    FILE *stream = stream_from_filename("concurrent-cache", error_stack);
    char *text =
        get_string_from_file_handle(stream, "concurrent-cache", error_stack);
    assert(error_stack_is_empty(error_stack));
    assert(strings_equal(text, "first") || strings_equal(text, "replacement"));
    free(text);
  }
  error_stack_destroy(error_stack);
  return NULL;
}

void test_embedding_lifecycle(void) {
  ThreadControl *thread_control = thread_control_create();
  const thread_control_status_t idle[] = {THREAD_CONTROL_STATUS_UNINITIALIZED,
                                          THREAD_CONTROL_STATUS_FINISHED};
  for (int status_idx = 0; status_idx < 2; status_idx++) {
    thread_control_set_status(thread_control, idle[status_idx]);
    thread_control_stop(thread_control);
    assert(thread_control_get_status(thread_control) == idle[status_idx]);
  }
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  thread_control_stop(thread_control);
  thread_control_stop(thread_control);
  assert(thread_control_get_status(thread_control) ==
         THREAD_CONTROL_STATUS_USER_INTERRUPT);
  thread_control_destroy(thread_control);
}

void test_fileproxy_cache(void) {
  ErrorStack *path_errors = error_stack_create();
  assert(fileproxy_file_exists(".//test/fixtures/leavegen_CSW21_ab_gen_2.csv"));
  FILE *path_stream = stream_from_filename(
      ".//test/fixtures/leavegen_CSW21_ab_gen_2.csv", path_errors);
  assert(path_stream != NULL);
  assert(error_stack_is_empty(path_errors));
  fclose_or_die(path_stream);
  error_stack_destroy(path_errors);
  // Exercise owned filenames/data, replacement during reads, and reuse.
  ErrorStack *error_stack = error_stack_create();
  for (int cycle_idx = 0; cycle_idx < 2; cycle_idx++) {
    for (int status_idx = 0; status_idx < 100; status_idx++) {
      char *name =
          get_formatted_string("%080d/cache-entry-%d", status_idx, status_idx);
      char bytes[] = "first";
      precache_file_data(name, bytes, sizeof("first") - 1);
      memset(bytes, 'x', sizeof("first") - 1);
      char *text = fileproxy_get_string_from_filename(name, error_stack);
      assert(strings_equal(text, "first"));
      free(text);
      FILE *borrowed = stream_from_filename(name, error_stack);
      precache_file_data(name, "replacement", sizeof("replacement") - 1);
      char *original = get_string_from_file_handle(borrowed, name, error_stack);
      assert(strings_equal(original, "first"));
      free(original);
      text = fileproxy_get_string_from_filename(name, error_stack);
      assert(error_stack_is_empty(error_stack));
      assert(strings_equal(text, "replacement"));
      free(text);
      free(name);
    }
    fileproxy_destroy_cache();
    fileproxy_destroy_cache();
  }
  precache_file_data("./concurrent-cache", "first", sizeof("first") - 1);
  cpthread_t reader;
  cpthread_create(&reader, read_cached_files, NULL);
  for (int entry_idx = 0; entry_idx < 300; entry_idx++) {
    precache_file_data("concurrent-cache", "replacement",
                       sizeof("replacement") - 1);
    char *name = get_formatted_string("concurrent-new-%d", entry_idx);
    precache_file_data(name, "first", sizeof("first") - 1);
    free(name);
  }
  cpthread_join(reader);
  fileproxy_destroy_cache();
  error_stack_destroy(error_stack);
}
