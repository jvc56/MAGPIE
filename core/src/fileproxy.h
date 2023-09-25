#ifndef FILEPROXY_H
#define FILEPROXY_H

#include <stdio.h>

FILE *stream_from_filename(const char *filename);
void precache_file(const char *filename);
void destroy_cache();

#endif