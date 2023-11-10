#ifndef FILE_HANDLER_H
#define FILE_HANDLER_H

#define STDOUT_FILENAME "stdout"
#define STDIN_FILENAME "stdin"

typedef enum {
  FILE_HANDLER_MODE_READ,
  FILE_HANDLER_MODE_WRITE,
} file_handler_mode_t;

struct FileHandler;
typedef struct FileHandler FileHandler;

const char *get_file_handler_filename(FileHandler *fh);
void set_file_handler(FileHandler *fh, const char *filename,
                      file_handler_mode_t file_handler_mode_type);
FileHandler *
create_file_handler_from_filename(const char *filename,
                                  file_handler_mode_t file_handler_mode_type);
void destroy_file_handler(FileHandler *fh);
void write_to_file(FileHandler *fh, const char *content);
char *getline_from_file(FileHandler *fh);

#endif
