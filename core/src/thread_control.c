#include <stdlib.h>

#include "thread_control.h"

ThreadControl *create_thread_control(int print_info_interval) {
  ThreadControl *thread_control = malloc(sizeof(ThreadControl));
  thread_control->print_info_interval = print_info_interval;
  thread_control->halt = 0;
  return thread_control;
}

void destroy_thread_control(ThreadControl *thread_control) {
  free(thread_control);
}

void halt(ThreadControl *thread_control) { thread_control->halt = 1; }
