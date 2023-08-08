#ifndef THREAD_CONTROL_H
#define THREAD_CONTROL_H

#include <stdint.h>

#include "constants.h"

typedef struct ThreadControl {
  int halt;
  int print_info_interval;
} ThreadControl;

ThreadControl *create_thread_control(int print_info_interval);
void destroy_thread_control(ThreadControl *thread_control);
void halt(ThreadControl *thread_control);

#endif