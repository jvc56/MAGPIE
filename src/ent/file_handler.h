#ifndef FILE_HANDLER_H
#define FILE_HANDLER_H

#include "../def/file_handler_defs.h"

struct FileHandler;
typedef struct FileHandler FileHandler;

FileHandler *
file_handler_create_from_filename(const char *filename,
                                  file_handler_mode_t file_handler_mode_type);
void file_handler_destroy(FileHandler *fh);

const char *file_handler_get_filename(const FileHandler *fh);
void file_handler_set_filename(FileHandler *fh, const char *filename,
                               file_handler_mode_t file_handler_mode_type);
void file_handler_write(FileHandler *fh, const char *content);
char *file_handler_get_line(FileHandler *fh);

#endif
