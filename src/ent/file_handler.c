#include "file_handler.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

#include "../def/file_handler_defs.h"

#include "../util/log.h"
#include "../util/string_util.h"
#include "../util/util.h"

struct FileHandler {
  FILE *file;
  char *filename;
  file_handler_mode_t mode;
  pthread_mutex_t mutex;
};

void file_handler_clear_file_and_filename(FileHandler *fh) {
  if (fh->file && !strings_equal(fh->filename, STDOUT_FILENAME) &&
      !strings_equal(fh->filename, STDIN_FILENAME)) {
    fclose(fh->file);
    fh->file = NULL;
  }
  free(fh->filename);
  fh->filename = NULL;
}

const char *file_handler_get_filename(const FileHandler *fh) {
  return fh->filename;
}

void file_handler_set_while_locked(FileHandler *fh, const char *filename,
                                   file_handler_mode_t mode) {
  if (strings_equal(filename, fh->filename) && fh->mode == mode) {
    return;
  }
  file_handler_clear_file_and_filename(fh);
  fh->filename = string_duplicate(filename);
  fh->mode = mode;
  // Handle reserved filenames specially
  if (strings_equal(filename, STDOUT_FILENAME)) {
    if (fh->mode != FILE_HANDLER_MODE_WRITE) {
      log_fatal("file handler for stdout must be in write mode\n", mode);
    }
    fh->file = stdout;
  } else if (strings_equal(filename, STDIN_FILENAME)) {
    if (fh->mode != FILE_HANDLER_MODE_READ) {
      log_fatal("file handler for stderr must be in read mode\n");
    }
    fh->file = stdin;
  } else {
    switch (fh->mode) {
    case FILE_HANDLER_MODE_READ:
      fh->file = fopen(filename, "r");
      if (!fh->file) {
        log_fatal("failed to open fail handle %s for reading", filename);
      }
      break;
    case FILE_HANDLER_MODE_WRITE:
      fh->file = fopen(filename, "a");
      if (!fh->file) {
        log_fatal("failed to open fail handle %s for appending", fh->filename);
      }
      break;
    }
  }
}

// No-op if the filename matches the existing filename
void file_handler_set_filename(FileHandler *fh, const char *filename,
                               file_handler_mode_t mode) {
  pthread_mutex_lock(&fh->mutex);
  file_handler_set_while_locked(fh, filename, mode);
  pthread_mutex_unlock(&fh->mutex);
}

FileHandler *file_handler_create_from_filename(const char *filename,
                                               file_handler_mode_t mode) {
  FileHandler *fh = (FileHandler *)malloc_or_die(sizeof(FileHandler));
  fh->file = NULL;
  fh->filename = NULL;
  pthread_mutex_init(&fh->mutex, NULL);
  file_handler_set_filename(fh, filename, mode);
  return fh;
}

void file_handler_destroy(FileHandler *fh) {
  if (!fh) {
    return;
  }
  file_handler_clear_file_and_filename(fh);
  free(fh);
}

void file_handler_write_with_lock(const FileHandler *fh, const char *content) {
  if (!fh) {
    log_fatal("cannot write to null file handler\n");
  }
  if (fh->mode != FILE_HANDLER_MODE_WRITE) {
    log_fatal("cannot write to nonwrite file handler\n");
  }
  if (!fh->file) {
    log_fatal("cannot write to null file\n");
  }
  if (!content) {
    return;
  }

  int fprintf_result = fprintf(fh->file, "%s", content);
  if (fprintf_result < 0) {
    log_fatal("fprintf failed with error code %d\n", fprintf_result);
  }

  int fflush_result = fflush(fh->file);
  if (fflush_result != 0) {
    log_fatal("fflush failed with error code %d\n", fflush_result);
  }
}

void file_handler_write(FileHandler *fh, const char *content) {
  pthread_mutex_lock(&fh->mutex);
  file_handler_write_with_lock(fh, content);
  pthread_mutex_unlock(&fh->mutex);
}

char *file_handler_get_line_with_lock(FileHandler *fh) {
  if (!fh) {
    log_fatal("cannot getline from null file handler\n");
  }
  if (fh->mode != FILE_HANDLER_MODE_READ) {
    log_fatal("cannot getline from nonread file handler\n");
  }
  if (!fh->file) {
    log_fatal("cannot getline from null file\n");
  }

  char *line = NULL;
  size_t len = 0;
  ssize_t read;

  errno = 0;
  read = getline(&line, &len, fh->file);
  if (read == -1) {
    int error_number = errno;
    if (error_number) {
      log_fatal("getline for %s failed with code: %d\n", fh->filename,
                error_number);
    } else {
      free(line);
      line = NULL;
    }
  }

  if (read && read > 0 && line[read - 1] == '\n') {
    line[read - 1] = '\0';
  }

  return line;
}

char *file_handler_get_line(FileHandler *fh) {
  pthread_mutex_lock(&fh->mutex);
  char *line = file_handler_get_line_with_lock(fh);
  pthread_mutex_unlock(&fh->mutex);
  return line;
}