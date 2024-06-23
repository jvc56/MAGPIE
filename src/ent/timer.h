#ifndef TIMER_H
#define TIMER_H

typedef struct Timer Timer;

Timer *mtimer_create(void);
void mtimer_destroy(Timer *timer);
void mtimer_start(Timer *timer);
void mtimer_stop(Timer *timer);
void mtimer_reset(Timer *timer);
double mtimer_elapsed_seconds(const Timer *timer);

#endif
