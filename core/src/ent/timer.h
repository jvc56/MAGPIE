#ifndef TIMER_H
#define TIMER_H

#include <time.h>

struct Timer;
typedef struct Timer Timer;

Timer *mtimer_create();
void mtimer_destroy(Timer *timer);
void mtimer_start(Timer *timer);
void mtimer_stop(Timer *timer);
void mtimer_reset(Timer *timer);
double mtimer_elapsed_seconds(Timer *timer);

#endif
