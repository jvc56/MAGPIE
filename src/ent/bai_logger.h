#ifndef BAI_LOGGER_H
#define BAI_LOGGER_H

#include <stdio.h>

#include "../util/string_util.h"
#include "../util/util.h"

#define BAI_LOGGER_NUM_DECIMALS 4

typedef struct BAILogger {
  FILE *fh;
  StringBuilder *buffer;
} BAILogger;

static inline BAILogger *bai_logger_create(const char *log_filename) {
  BAILogger *bai_logger = (BAILogger *)malloc_or_die(sizeof(BAILogger));
  bai_logger->fh = fopen(log_filename, "w");
  bai_logger->buffer = string_builder_create();
  return bai_logger;
}

static inline void bai_logger_destroy(BAILogger *bai_logger) {
  if (!bai_logger) {
    return;
  }
  fclose(bai_logger->fh);
  string_builder_destroy(bai_logger->buffer);
  free(bai_logger);
}

static inline void bai_logger_log_title(BAILogger *bai_logger,
                                        const char *title) {
  if (!bai_logger) {
    return;
  }
  string_builder_add_formatted_string(bai_logger->buffer, "%s\n", title);
}

static inline void bai_logger_log_int(BAILogger *bai_logger,
                                      const char *int_name, const int x) {
  if (!bai_logger) {
    return;
  }
  string_builder_add_formatted_string(bai_logger->buffer, "%s = %d\n", int_name,
                                      x);
}

static inline void bai_logger_log_double(BAILogger *bai_logger,
                                         const char *double_name,
                                         const double x) {
  if (!bai_logger) {
    return;
  }
  string_builder_add_formatted_string(bai_logger->buffer, "%s = %.*f\n",
                                      double_name, BAI_LOGGER_NUM_DECIMALS, x);
}

static inline void bai_logger_log_double_array(BAILogger *bai_logger,
                                               const char *double_array_name,
                                               const double *x, int size) {
  if (!bai_logger) {
    return;
  }
  string_builder_add_formatted_string(bai_logger->buffer,
                                      "%s = ", double_array_name);
  for (int i = 0; i < size; i++) {
    string_builder_add_formatted_string(bai_logger->buffer, "%.*f",
                                        BAI_LOGGER_NUM_DECIMALS, x[i]);
    if (i != size - 1) {
      string_builder_add_string(bai_logger->buffer, " ");
    }
  }
  string_builder_add_string(bai_logger->buffer, "\n");
}

static inline void bai_logger_log_int_array(BAILogger *bai_logger,
                                            const char *double_array_name,
                                            const int *x, int size) {
  if (!bai_logger) {
    return;
  }
  string_builder_add_formatted_string(bai_logger->buffer,
                                      "%s = ", double_array_name);
  for (int i = 0; i < size; i++) {
    string_builder_add_formatted_string(bai_logger->buffer, "%d", x[i]);
    if (i != size - 1) {
      string_builder_add_string(bai_logger->buffer, " ");
    }
  }
  string_builder_add_string(bai_logger->buffer, "\n");
}

static inline void bai_logger_flush(BAILogger *bai_logger) {
  if (!bai_logger) {
    return;
  }
  if (string_builder_length(bai_logger->buffer) == 0) {
    return;
  }
  fprintf(bai_logger->fh, "%s", string_builder_peek(bai_logger->buffer));
  fflush(bai_logger->fh);
  string_builder_clear(bai_logger->buffer);
}

#endif
