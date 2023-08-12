#ifndef THREAD_CONTROL_H
#define THREAD_CONTROL_H

#include <pthread.h>

#define MODE_SEARCHING 1
#define MODE_STOPPED 0

typedef struct ThreadControl {
  pthread_mutex_t halt_mutex;
  int halt;
  int print_info_interval;
  int check_stopping_condition_interval;
  pthread_mutex_t current_mode_mutex;
  int current_mode;
} ThreadControl;

ThreadControl *create_thread_control();
void destroy_thread_control(ThreadControl *thread_control);
void halt(ThreadControl *thread_control);
int is_halted(ThreadControl *thread_control);
void set_print_info_interval(ThreadControl *thread_control,
                             int print_info_interval);
void set_check_stopping_condition_interval(
    ThreadControl *thread_control, int check_stopping_condition_interval);
void set_mode(ThreadControl *thread_control, int mode);
int get_mode(ThreadControl *thread_control);
#endif