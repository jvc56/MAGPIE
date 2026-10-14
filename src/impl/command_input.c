#include "command_input.h"

#include "../compat/cpthread.h"
#include "../compat/cpthread_defs.h"
#include "../compat/linenoise.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include <stdbool.h>
#include <stdlib.h>

typedef struct CommandInput {
  cpthread_mutex_t mutex;
  // Signaled when a read is requested, when its result arrives, and by
  // command_input_wake.
  cpthread_cond_t changed;
  bool reader_started;
  // A read is wanted: requested and not yet finished.
  bool read_requested;
  // The reader has started the requested read, so its prompt is fixed.
  bool read_started;
  // The prompt for the requested read, or NULL for a plain read (an
  // asynchronous command's input, which shows no prompt).
  char *prompt;
  // The finished read's line, waiting for a caller; NULL at the end of input.
  bool has_result;
  char *result;
} CommandInput;

static CommandInput command_input = {
    .mutex = CPTHREAD_MUTEX_INITIALIZER,
    .changed = CPTHREAD_COND_INITIALIZER,
};

static void *command_input_reader(void *unused) {
  (void)unused;
  CommandInput *input = &command_input;
  cpthread_mutex_lock(&input->mutex);
  while (true) {
    while (!input->read_requested || input->read_started) {
      cpthread_cond_wait(&input->changed, &input->mutex);
    }
    input->read_started = true;
    char *prompt = input->prompt;
    input->prompt = NULL;
    cpthread_mutex_unlock(&input->mutex);
    char *line = prompt ? linenoise(prompt) : read_line_from_stream_in();
    free(prompt);
    cpthread_mutex_lock(&input->mutex);
    input->result = line;
    input->has_result = true;
    input->read_requested = false;
    input->read_started = false;
    cpthread_cond_broadcast(&input->changed);
  }
  return NULL;
}

// Starts the reader if it is not running. Called with the lock held.
static void command_input_start_reader(CommandInput *input) {
  if (input->reader_started) {
    return;
  }
  cpthread_t reader;
  cpthread_create(&reader, command_input_reader, NULL);
  // It runs for the life of the process, blocked reading most of the time,
  // so it is never joined.
  cpthread_detach(reader);
  input->reader_started = true;
}

// Takes the waiting result. Called with the lock held.
static char *command_input_take_result(CommandInput *input) {
  char *line = input->result;
  input->result = NULL;
  input->has_result = false;
  return line;
}

char *command_input_read_line(const char *prompt) {
  CommandInput *input = &command_input;
  cpthread_mutex_lock(&input->mutex);
  command_input_start_reader(input);
  if (!input->has_result) {
    if (!input->read_requested) {
      input->read_requested = true;
      input->prompt = string_duplicate(prompt ? prompt : "");
      cpthread_cond_broadcast(&input->changed);
    } else if (!input->read_started) {
      // A plain read not yet begun: make it this caller's.
      free(input->prompt);
      input->prompt = string_duplicate(prompt ? prompt : "");
    } else if (prompt && prompt[0] && input->prompt == NULL) {
      // A plain read is already waiting on the input, begun during a command
      // that has since finished: its line is this caller's, so show the
      // prompt that read could not.
      write_to_stream_out("%s", prompt);
    }
  }
  while (!input->has_result) {
    cpthread_cond_wait(&input->changed, &input->mutex);
  }
  char *line = command_input_take_result(input);
  cpthread_mutex_unlock(&input->mutex);
  return line;
}

char *command_input_read_line_until(bool (*done)(void *), void *done_arg,
                                    bool *at_end) {
  CommandInput *input = &command_input;
  *at_end = false;
  cpthread_mutex_lock(&input->mutex);
  command_input_start_reader(input);
  char *line = NULL;
  while (true) {
    if (input->has_result) {
      line = command_input_take_result(input);
      *at_end = line == NULL;
      break;
    }
    if (done(done_arg)) {
      break;
    }
    if (!input->read_requested) {
      input->read_requested = true;
      input->prompt = NULL;
      cpthread_cond_broadcast(&input->changed);
    }
    cpthread_cond_wait(&input->changed, &input->mutex);
  }
  cpthread_mutex_unlock(&input->mutex);
  return line;
}

void command_input_wake(void) {
  CommandInput *input = &command_input;
  cpthread_mutex_lock(&input->mutex);
  cpthread_cond_broadcast(&input->changed);
  cpthread_mutex_unlock(&input->mutex);
}
