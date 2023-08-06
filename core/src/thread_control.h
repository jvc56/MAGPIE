#ifndef THREAD_CONTROL_H
#define THREAD_CONTROL_H

#include <stdint.h>

#include "constants.h"

typedef struct ThreadControl {
  int *halt;
  int print_info_interval;
} ThreadControl;

void halt(ThreadControl *thread_control);

#endif