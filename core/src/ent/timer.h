#ifndef TIMER_H
#define TIMER_H

#include <time.h>

struct Timer;
typedef struct Timer Timer;

Timer *create_timer();
void destroy_timer(Timer *timer);
void timer_start(Timer *timer);
void timer_stop(Timer *timer);
void timer_reset(Timer *timer);
double timer_elapsed_seconds(Timer *timer);

#endif
