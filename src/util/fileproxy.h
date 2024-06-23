#ifndef FILEPROXY_H
#define FILEPROXY_H

#include <stdio.h>

FILE *stream_from_filename(const char *filename);
void fileproxy_destroy_cache(void);

#endif