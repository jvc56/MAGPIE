#ifndef AUTOPLAY_H
#define AUTOPLAY_H

#include "../def/autoplay_defs.h"

#include "../ent/autoplay_results.h"
#include "../ent/config.h"

autoplay_status_t autoplay(const Config *config,
                           AutoplayResults **autoplay_results);

#endif
