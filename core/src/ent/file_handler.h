#ifndef FILE_HANDLER_H
#define FILE_HANDLER_H

#include "../def/file_handler_defs.h"

struct FileHandler;
typedef struct FileHandler FileHandler;

const char *get_file_handler_filename(const FileHandler *fh);
void set_file_handler(FileHandler *fh, const char *filename,
                      file_handler_mode_t file_handler_mode_type);
FileHandler *
create_file_handler_from_filename(const char *filename,
                                  file_handler_mode_t file_handler_mode_type);
void destroy_file_handler(FileHandler *fh);
void write_to_file(FileHandler *fh, const char *content);
char *getline_from_file(FileHandler *fh);

#endif
