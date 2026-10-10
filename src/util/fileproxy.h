#ifndef FILEPROXY_H
#define FILEPROXY_H

#include "io_util.h"
#include <stdio.h>

FILE *stream_from_filename(const char *filename, ErrorStack *error_stack);
char *fileproxy_get_string_from_filename(const char *filename,
                                         ErrorStack *error_stack);
void precache_file_data(const char *filename, const char *raw_data,
                        int num_bytes);
// Call only after all cached streams have been closed and cache users joined.
// Replacement buffers remain alive until this teardown.
void fileproxy_destroy_cache(void);
bool fileproxy_file_exists(const char *filename);

#endif