#ifndef FILEPROXY_H
#define FILEPROXY_H

#include <stdbool.h>
#include <stdio.h>

// Forward declaration to avoid circular dependency
typedef struct ErrorStack ErrorStack;

FILE *stream_from_filename(const char *filename, ErrorStack *error_stack);
char *get_string_from_file(const char *filename, ErrorStack *error_stack);
void fileproxy_destroy_cache(void);
bool fileproxy_file_exists(const char *filename);

#endif