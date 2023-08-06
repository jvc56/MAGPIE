#include "thread_control.h"

void halt(ThreadControl *thread_control) { *thread_control->halt = 1; }
