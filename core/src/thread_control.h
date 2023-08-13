#ifndef THREAD_CONTROL_H
#define THREAD_CONTROL_H

#include <pthread.h>

#define MODE_STOPPED 0
#define MODE_SEARCHING 1

#define HALT_STATUS_NONE 0
#define HALT_STATUS_PROBABILISTIC 1
#define HALT_STATUS_MAX_ITERATIONS 2
#define HALT_STATUS_USER_INTERRUPT 3

typedef struct ThreadControl {
  pthread_mutex_t halt_status_mutex;
  int halt_status;
  pthread_mutex_t current_mode_mutex;
  int current_mode;
  int print_info_interval;
  int check_stopping_condition_interval;
} ThreadControl;

ThreadControl *create_thread_control();
void destroy_thread_control(ThreadControl *thread_control);
int halt(ThreadControl *thread_control, int halt_status);
int unhalt(ThreadControl *thread_control);
int is_halted(ThreadControl *thread_control);
int get_halt_status(ThreadControl *thread_control);
void set_print_info_interval(ThreadControl *thread_control,
                             int print_info_interval);
void set_check_stopping_condition_interval(
    ThreadControl *thread_control, int check_stopping_condition_interval);
int set_mode_searching(ThreadControl *thread_control);
int set_mode_stopped(ThreadControl *thread_control);
int get_mode(ThreadControl *thread_control);

#endif