#ifndef FILEPROXY_H
#define FILEPROXY_H

#include <stdio.h>

#include "io_util.h"

FILE *stream_from_filename(const char *filename, ErrorStack *error_stack);
void fileproxy_destroy_cache(void);

#endif