#ifndef LEAVE_BLEND_H
#define LEAVE_BLEND_H

#include "../util/io_util.h"

// Mixes two leave tables leave by leave into output_name, taking `weight` of
// the second and 1 - weight of the first. A leave only one table holds keeps
// that table's value: blending it against a zero the other never claimed
// would invent a number. wolges measured the census and self-play tables
// mixed at 0.5 beating both parents by about a tenth of a point of win
// percentage.
void leave_blend(const char *data_paths, const char *first_name,
                 const char *second_name, double weight,
                 const char *output_name, ErrorStack *error_stack);

#endif
