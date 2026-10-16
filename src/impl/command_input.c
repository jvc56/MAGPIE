#include "command_input.h"

#include "../compat/cpthread.h"
#include "../compat/cpthread_defs.h"
#include "../compat/cterm.h"
#include "../compat/linenoise.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct CommandInput {
  cpthread_mutex_t mutex;
  // Signaled when a read is requested or changed, when its result arrives,
  // and by command_input_wake.
  cpthread_cond_t changed;
  bool reader_started;
  // Marks the reader thread, so that it never waits on itself.
  cpthread_key_t reader_key;
  // Ends the reader's wait for a terminal's line, where that is possible
  // (NULL elsewhere), so that a plain read not yet begun can become an edited
  // one.
  CTermLineWait *line_wait;
  // A read is wanted: requested and not yet finished.
  bool read_requested;
  // The stream the requested read is of: the input stream when it was asked
  // for.
  FILE *stream;
  // The prompt for the requested read, or NULL for a plain read (an
  // asynchronous command's input, which shows no prompt and is not edited).
  char *prompt;
  // The reader is waiting for a line of the requested plain read to arrive,
  // without having begun reading, so the read can still change.
  bool waiting;
  // The reader has begun the requested read, so it can no longer change.
  bool read_started;
  // The finished read's line, waiting for a caller, and the stream it came
  // from; the line is NULL at the end of input.
  bool has_result;
  char *result;
  FILE *result_stream;
} CommandInput;

static CommandInput command_input = {
    .mutex = CPTHREAD_MUTEX_INITIALIZER,
    .changed = CPTHREAD_COND_INITIALIZER,
};

static void *command_input_reader(void *unused) {
  (void)unused;
  CommandInput *input = &command_input;
  cpthread_setspecific(input->reader_key, input);
  cpthread_mutex_lock(&input->mutex);
  while (true) {
    while (!input->read_requested || input->read_started) {
      cpthread_cond_wait(&input->changed, &input->mutex);
    }
    FILE *stream = input->stream;
    bool line_typed = false;
    if (!input->prompt && input->line_wait && cterm_is_terminal(stream)) {
      // A plain read at a terminal waits for the line to be typed before
      // reading it, so that if the command loop wants an edited line first,
      // it can have one.
      input->waiting = true;
      cpthread_mutex_unlock(&input->mutex);
      const bool line_ready = cterm_line_wait(input->line_wait, stream);
      cpthread_mutex_lock(&input->mutex);
      input->waiting = false;
      if (!line_ready || !input->read_requested || input->stream != stream) {
        // The read changed or was withdrawn: look again.
        continue;
      }
      // Read as typed, even if the command loop has asked for an edited line
      // meanwhile: it is too late to edit, and the line editor would take
      // the typed newline for a character.
      line_typed = true;
    }
    input->read_started = true;
    char *prompt = input->prompt;
    input->prompt = NULL;
    cpthread_mutex_unlock(&input->mutex);
    char *line = prompt && !line_typed ? linenoise(prompt)
                                       : linenoise_read_plain_line(stream);
    free(prompt);
    cpthread_mutex_lock(&input->mutex);
    input->result = line;
    input->result_stream = stream;
    input->has_result = true;
    input->read_requested = false;
    input->read_started = false;
    cpthread_cond_broadcast(&input->changed);
  }
  return NULL;
}

// For the wait before an owned console closes (io_wait_before_close): the
// next line, which a read already under way delivers. On the reader thread
// itself, which would wait for itself, nothing.
static char *command_input_read_line_before_close(void) {
  CommandInput *input = &command_input;
  if (input->reader_started && cpthread_getspecific(input->reader_key)) {
    return NULL;
  }
  return command_input_read_line(NULL);
}

// Starts the reader if it is not running. Called with the lock held.
static void command_input_start_reader(CommandInput *input) {
  if (input->reader_started) {
    return;
  }
  cpthread_key_create(&input->reader_key, NULL);
  input->line_wait = cterm_line_wait_create();
  io_set_line_reader(command_input_read_line_before_close);
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
  input->result_stream = NULL;
  input->has_result = false;
  return line;
}

// Ends the reader's wait for a line, if it is waiting, so that it looks at
// the requested read again. Called with the lock held.
static void command_input_end_wait(const CommandInput *input) {
  if (input->waiting) {
    cterm_line_wait_end(input->line_wait);
  }
}

// Drops what belongs to a stream other than `stream` -- a line read from it,
// and a read of it the reader has not begun -- so that it never reaches a
// caller reading `stream`. Called with the lock held.
static void command_input_forget_other_streams(CommandInput *input,
                                               const FILE *stream) {
  if (input->has_result && input->result_stream != stream) {
    free(command_input_take_result(input));
  }
  if (input->read_requested && !input->read_started &&
      input->stream != stream) {
    free(input->prompt);
    input->prompt = NULL;
    input->read_requested = false;
    command_input_end_wait(input);
  }
}

// Asks the reader for a line of `stream`, with `prompt` (taken) or none.
// Called with the lock held.
static void command_input_request(CommandInput *input, FILE *stream,
                                  char *prompt) {
  input->read_requested = true;
  input->stream = stream;
  input->prompt = prompt;
  cpthread_cond_broadcast(&input->changed);
}

char *command_input_read_line(const char *prompt) {
  CommandInput *input = &command_input;
  // Made before taking the lock, so that nothing under it can fail.
  char *own_prompt = string_duplicate(prompt ? prompt : "");
  FILE *stream = get_stream_in();
  cpthread_mutex_lock(&input->mutex);
  command_input_start_reader(input);
  bool prompt_shown = false;
  while (true) {
    command_input_forget_other_streams(input, stream);
    if (input->has_result) {
      break;
    }
    if (!input->read_requested) {
      command_input_request(input, stream, own_prompt);
      own_prompt = NULL;
    } else if (!input->read_started && input->stream == stream) {
      // A plain read not yet begun, or waiting for its line to be typed:
      // make it this caller's, edited.
      if (own_prompt) {
        free(input->prompt);
        input->prompt = own_prompt;
        own_prompt = NULL;
        command_input_end_wait(input);
        cpthread_cond_broadcast(&input->changed);
      }
    } else if (input->read_started && input->stream == stream &&
               !prompt_shown) {
      // A read already reading this stream, begun during a command that has
      // since finished, which nothing can end (a pipe, or a Windows console,
      // which edits lines itself): its line is this caller's, so show the
      // prompt it could not, at a terminal, as linenoise would.
      prompt_shown = true;
      if (own_prompt && own_prompt[0] && cterm_is_terminal(stream)) {
        write_to_stream_out("%s", own_prompt);
      }
    }
    // Otherwise a read of another stream is under way: once it ends, its
    // line is dropped and this stream's read requested.
    cpthread_cond_wait(&input->changed, &input->mutex);
  }
  char *line = command_input_take_result(input);
  cpthread_mutex_unlock(&input->mutex);
  free(own_prompt);
  return line;
}

char *command_input_read_line_until(bool (*done)(void *), void *done_arg,
                                    bool *at_end) {
  CommandInput *input = &command_input;
  *at_end = false;
  FILE *stream = get_stream_in();
  cpthread_mutex_lock(&input->mutex);
  command_input_start_reader(input);
  char *line = NULL;
  while (true) {
    command_input_forget_other_streams(input, stream);
    if (input->has_result) {
      line = command_input_take_result(input);
      *at_end = line == NULL;
      break;
    }
    if (done(done_arg)) {
      break;
    }
    if (!input->read_requested) {
      command_input_request(input, stream, NULL);
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

void command_input_release_stream(const FILE *stream) {
  CommandInput *input = &command_input;
  cpthread_mutex_lock(&input->mutex);
  while (input->read_requested && input->read_started &&
         input->stream == stream) {
    cpthread_cond_wait(&input->changed, &input->mutex);
  }
  if (input->has_result && input->result_stream == stream) {
    free(command_input_take_result(input));
  }
  if (input->read_requested && input->stream == stream) {
    free(input->prompt);
    input->prompt = NULL;
    input->read_requested = false;
    command_input_end_wait(input);
  }
  cpthread_mutex_unlock(&input->mutex);
}
