#ifndef FILEPROXY_H
#define FILEPROXY_H

#include "io_util.h"
#include <stdio.h>

FILE *stream_from_filename(const char *filename, ErrorStack *error_stack);
void fileproxy_destroy_cache(void);
bool fileproxy_file_exists(const char *filename);

#endif