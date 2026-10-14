#ifndef XS_CONVERT_H
#define XS_CONVERT_H

#include "../def/convert_defs.h"
#include "../ent/conversion_results.h"
#include "../util/io_util.h"

void xs_convert(const char *input, const char *output, const char *book_name,
                const char *budget, const char *paths, conversion_type_t type,
                ConversionResults *results, ErrorStack *errors);

#endif
