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
  ErrorStack *errors = error_stack_create();
  for (int entry_idx = 0; entry_idx < 300; entry_idx++) {
    FILE *stream = stream_from_filename("concurrent-cache", errors);
    char *text =
        get_string_from_file_handle(stream, "concurrent-cache", errors);
    assert(error_stack_is_empty(errors));
    assert(strings_equal(text, "first") || strings_equal(text, "replacement"));
    free(text);
  }
  error_stack_destroy(errors);
  return NULL;
}

void test_embedding_lifecycle(void) {
  ThreadControl *control = thread_control_create();
  const thread_control_status_t idle[] = {THREAD_CONTROL_STATUS_UNINITIALIZED,
                                          THREAD_CONTROL_STATUS_FINISHED};
  for (int index = 0; index < 2; index++) {
    thread_control_set_status(control, idle[index]);
    thread_control_stop(control);
    assert(thread_control_get_status(control) == idle[index]);
  }
  thread_control_set_status(control, THREAD_CONTROL_STATUS_STARTED);
  thread_control_stop(control);
  thread_control_stop(control);
  assert(thread_control_get_status(control) ==
         THREAD_CONTROL_STATUS_USER_INTERRUPT);
  thread_control_destroy(control);

  // Growth past the old 32-entry limit, duplicate replacement, long owned
  // filenames, owned data, and reuse after destruction.
  ErrorStack *errors = error_stack_create();
  for (int pass = 0; pass < 2; pass++) {
    for (int index = 0; index < 100; index++) {
      char *name = get_formatted_string("%080d/cache-entry-%d", index, index);
      char bytes[] = "first";
      precache_file_data(name, bytes, 5);
      memset(bytes, 'x', 5);
      char *text = fileproxy_get_string_from_filename(name, errors);
      assert(strings_equal(text, "first"));
      free(text);
      FILE *borrowed = stream_from_filename(name, errors);
      precache_file_data(name, "replacement", 11);
      char *original = get_string_from_file_handle(borrowed, name, errors);
      assert(strings_equal(original, "first"));
      free(original);
      text = fileproxy_get_string_from_filename(name, errors);
      assert(error_stack_is_empty(errors));
      assert(strings_equal(text, "replacement"));
      free(text);
      free(name);
    }
    fileproxy_destroy_cache();
    fileproxy_destroy_cache();
  }
  precache_file_data("concurrent-cache", "first", 5);
  cpthread_t reader;
  cpthread_create(&reader, read_cached_files, NULL);
  for (int entry_idx = 0; entry_idx < 300; entry_idx++) {
    precache_file_data("concurrent-cache", "replacement", 11);
    char *name = get_formatted_string("concurrent-new-%d", entry_idx);
    precache_file_data(name, "first", 5);
    free(name);
  }
  cpthread_join(reader);
  fileproxy_destroy_cache();
  error_stack_destroy(errors);
}
