#ifndef THREAD_CONTROL_H
#define THREAD_CONTROL_H

#include "../compat/ctime.h"
#include "../def/thread_control_defs.h"
#include "xoshiro.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct ThreadControl ThreadControl;

ThreadControl *thread_control_create(void);
void thread_control_destroy(ThreadControl *thread_control);

thread_control_status_t
thread_control_get_status(ThreadControl *thread_control);
bool thread_control_set_status(ThreadControl *thread_control,
                               thread_control_status_t exit_status);
void thread_control_wait_for_status_change(ThreadControl *thread_control);
void thread_control_print(ThreadControl *thread_control, const char *content);

#endif