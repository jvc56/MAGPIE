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
// Links `thread_control` to `parent` (NULL for none), so a stop requested on
// the parent -- the user's, or a contribute task's time limit -- stops whatever
// runs on `thread_control` too: the parent's USER_INTERRUPT, whenever it is
// set, and at linking if it is set already, becomes the child's own, and
// stays when the parent's is lifted. Nothing passes the other way: the
// child's status is its own. Link after setting the child's status STARTED,
// keep the parent alive while the child is linked, and destroy (or unlink)
// the child before the parent.
void thread_control_set_parent(ThreadControl *thread_control,
                               ThreadControl *parent);
void thread_control_stop(ThreadControl *thread_control);
// Puts back `status` after an interrupt that was not the user's (a contribute
// task's time limit), unless thread_control_stop was called since the status
// last became STARTED: a stop that came while the interrupt was in place
// changed nothing, and must still stand. Returns whether it restored.
bool thread_control_restore_unless_stopped(ThreadControl *thread_control,
                                           thread_control_status_t status);
// Cooperative search pause. Returns false unless a command is running.
// Stop and terminal status transitions release every paused worker.
bool thread_control_set_paused(ThreadControl *thread_control, bool paused);
// Search workers only: waits until resumed, then returns the current status.
// Never call while holding a lock another search worker or status reader needs.
thread_control_status_t
thread_control_search_checkpoint(ThreadControl *thread_control);
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