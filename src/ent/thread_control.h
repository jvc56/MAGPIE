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

// The status, or USER_INTERRUPT whenever the parent's (see
// thread_control_set_parent) is USER_INTERRUPT.
thread_control_status_t
thread_control_get_status(ThreadControl *thread_control);
// Links `thread_control` to `parent` (NULL for none), so a stop requested on
// the parent -- the user's, or a contribute task's time limit -- stops whatever
// runs on `thread_control` too: thread_control_get_status reads it as
// USER_INTERRUPT. Only reads look through: setting the status, or waiting on
// it, is this thread control's own and never touches the parent's. Set it
// before anything reads the status, and keep the parent alive while it does.
void thread_control_set_parent(ThreadControl *thread_control,
                               ThreadControl *parent);
bool thread_control_set_status(ThreadControl *thread_control,
                               thread_control_status_t exit_status);
void thread_control_wait_for_status_change(ThreadControl *thread_control);
void thread_control_print(ThreadControl *thread_control, const char *content);
void thread_control_print_formatted(ThreadControl *thread_control,
                                    const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
// The same, to the error stream: for what a user has to see even when the
// output is redirected or ignored.
void thread_control_print_err(ThreadControl *thread_control,
                              const char *content);
void thread_control_print_formatted_err(ThreadControl *thread_control,
                                        const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#endif