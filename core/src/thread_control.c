#include <pthread.h>
#include <stdlib.h>

#include "thread_control.h"

ThreadControl *create_thread_control() {
  ThreadControl *thread_control = malloc(sizeof(ThreadControl));
  thread_control->print_info_interval = 0;
  pthread_mutex_init(&thread_control->halt_mutex, NULL);
  thread_control->halt = 0;
  thread_control->check_stopping_condition_interval = 0;
  thread_control->current_mode = MODE_STOPPED;
  return thread_control;
}

void destroy_thread_control(ThreadControl *thread_control) {
  free(thread_control);
}

void set_print_info_interval(ThreadControl *thread_control,
                             int print_info_interval) {
  thread_control->print_info_interval = print_info_interval;
}

void set_check_stopping_condition_interval(
    ThreadControl *thread_control, int check_stopping_condition_interval) {
  thread_control->check_stopping_condition_interval =
      check_stopping_condition_interval;
}

void halt(ThreadControl *thread_control) {
  pthread_mutex_lock(&thread_control->halt_mutex);
  thread_control->halt = 1;
  pthread_mutex_unlock(&thread_control->halt_mutex);
}

int is_halted(ThreadControl *thread_control) {
  int is_halted;
  pthread_mutex_lock(&thread_control->halt_mutex);
  is_halted = thread_control->halt;
  pthread_mutex_unlock(&thread_control->halt_mutex);
  return is_halted;
}

void set_mode(ThreadControl *thread_control, int mode) {
  pthread_mutex_lock(&thread_control->current_mode_mutex);
  thread_control->current_mode = mode;
  pthread_mutex_unlock(&thread_control->current_mode_mutex);
}

int get_mode(ThreadControl *thread_control) {
  int current_mode;
  pthread_mutex_lock(&thread_control->current_mode_mutex);
  current_mode = thread_control->current_mode;
  pthread_mutex_unlock(&thread_control->current_mode_mutex);
  return current_mode;
}