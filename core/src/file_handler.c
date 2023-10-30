#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "file_handler.h"
#include "log.h"
#include "string_util.h"
#include "util.h"

struct FileHandler {
  FILE *file;
  char *filename;
  file_handler_mode_t type;
  pthread_mutex_t mutex;
};

void file_handler_clear_file_and_filename(FileHandler *fh) {
  if (fh->file && !strings_equal(fh->filename, STDOUT_FILENAME) &&
      !strings_equal(fh->filename, STDIN_FILENAME) &&
      !strings_equal(fh->filename, STDERR_FILENAME)) {
    fclose(fh->file);
    fh->file = NULL;
  }
  if (fh->filename) {
    free(fh->filename);
    fh->filename = NULL;
  }
}

void set_file_handler(FileHandler *fh, const char *filename,
                      file_handler_mode_t file_handler_mode_type) {
  pthread_mutex_lock(&fh->mutex);
  file_handler_clear_file_and_filename(fh);
  fh->filename = get_formatted_string("%s", filename);
  fh->type = file_handler_mode_type;
  // Handle reserved filenames specially
  if (strings_equal(filename, STDOUT_FILENAME)) {
    if (fh->type != FILE_HANDLER_MODE_WRITE) {
      log_fatal("file handler for stdout must be in write mode\n",
                file_handler_mode_type);
    }
    fh->file = stdout;
  } else if (strings_equal(filename, STDIN_FILENAME)) {
    if (fh->type != FILE_HANDLER_MODE_READ) {
      log_fatal("file handler for stderr must be in read mode\n");
    }
    fh->file = stdin;
  } else if (strings_equal(filename, STDERR_FILENAME)) {
    if (fh->type != FILE_HANDLER_MODE_WRITE) {
      log_fatal("file handler for stderr must be in write mode\n");
    }
    fh->file = stderr;
    log_set_error_out(fh->file);
  } else {
    switch (fh->type) {
    case FILE_HANDLER_MODE_READ:
      fh->file = fopen(filename, "r");
      break;
    case FILE_HANDLER_MODE_WRITE:
      fh->file = fopen(filename, "a");
      break;
    }
  }
  pthread_mutex_unlock(&fh->mutex);
}

FileHandler *
create_file_handler_from_filename(const char *filename,
                                  file_handler_mode_t file_handler_mode_type) {
  FileHandler *fh = (FileHandler *)malloc_or_die(sizeof(FileHandler));
  fh->file = NULL;
  fh->filename = NULL;
  pthread_mutex_init(&fh->mutex, NULL);
  set_file_handler(fh, filename, file_handler_mode_type);
  return fh;
}

void destroy_file_handler(FileHandler *fh) {
  file_handler_clear_file_and_filename(fh);
  free(fh);
}

void write_to_file(FileHandler *fh, const char *content) {
  pthread_mutex_lock(&fh->mutex);
  if (!fh) {
    log_fatal("cannot write to null file handler\n");
  }
  if (fh->type != FILE_HANDLER_MODE_WRITE) {
    log_fatal("cannot write to nonwrite file handler\n");
  }
  if (!fh->file) {
    log_fatal("cannot write to null file\n");
  }
  if (!content) {
    return;
  }
  fprintf(fh->file, "%s", content);
  fflush(fh->file);
  pthread_mutex_unlock(&fh->mutex);
}

char *getline_from_file(FileHandler *fh) {
  pthread_mutex_lock(&fh->mutex);
  if (!fh) {
    log_fatal("cannot getline from null file handler\n");
  }
  if (fh->type != FILE_HANDLER_MODE_READ) {
    log_fatal("cannot getline from nonread file handler\n");
  }
  if (!fh->file) {
    log_fatal("cannot getline from null file\n");
  }

  char *line = NULL;
  size_t len = 0;
  ssize_t read;

  read = getline(&line, &len, fh->file);
  if (read == -1) {
    free(line);
    return NULL;
  }

  if (read > 0 && line[read - 1] == '\n') {
    line[read - 1] = '\0';
  }
  pthread_mutex_unlock(&fh->mutex);

  return line;
}