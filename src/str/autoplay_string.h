#ifndef AUTOPLAY_STRING_H
#define AUTOPLAY_STRING_H

#include "../ent/autoplay_results.h"
#include "../ent/thread_control.h"

#include "../util/string_util.h"

void print_ucgi_autoplay_results(const AutoplayResults *autoplay_results,
                                 ThreadControl *thread_control);

#endif
