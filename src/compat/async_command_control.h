#ifndef ASYNC_COMMAND_CONTROL_H
#define ASYNC_COMMAND_CONTROL_H

#include "../util/io_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/poll.h>
#include <unistd.h>

typedef struct AsyncCommandControl {
  int pipefds[2];
  struct pollfd fds[2];
} AsyncCommandControl;

static inline AsyncCommandControl *async_command_control_create(void) {
  AsyncCommandControl *acc =
      (AsyncCommandControl *)malloc_or_die(sizeof(AsyncCommandControl));
  if (pipe(acc->pipefds) == -1) {
    perror("pipe");
    log_fatal("failed to create pipe for async command control");
  }
  acc->fds[0].fd = fileno(get_stream_in());
  acc->fds[0].events = POLLIN;
  acc->fds[1].fd = acc->pipefds[0];
  acc->fds[1].events = POLLIN;
  return acc;
}

static inline void async_command_control_destroy(AsyncCommandControl *acc) {
  close(acc->pipefds[0]);
  close(acc->pipefds[1]);
  free(acc);
}

static inline void
async_command_control_send_finished_signal(AsyncCommandControl *acc) {
  if (write(acc->pipefds[1], "x", 1) == -1) {
    log_fatal("failed to write to finished signal");
  }
}

// Returns an alloc'ed char* if there was input detected or NULL if the finished
// signal was detected
static inline char *async_command_control_wait_for_input_or_finished_signal(
    AsyncCommandControl *acc) {
  int ret = poll(acc->fds, 2, -1); // wait indefinitely
  if (ret == -1) {
    perror("poll");
    log_fatal("unexpected error while polling for input or finished signal");
  }
  char *input = NULL;
  if (acc->fds[0].revents) {
    input = read_line_from_stream_in();
  }
  return input;
}

#endif