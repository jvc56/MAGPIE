/*
 * Implements algorithms described in
 *
 * Dealing with Unknown Variances in Best-Arm Identification
 * (https://arxiv.org/pdf/2210.00974)
 *
 * with Julia source code kindly provided by Marc Jourdan.
 */
#ifndef BAI_LOGGER_H
#define BAI_LOGGER_H

#include "../util/io_util.h"
#include "../util/string_util.h"
#include <math.h>
#include <stdio.h>

#define BAI_LOGGER_NUM_DECIMALS 15

typedef struct BAILogger {
  FILE *fh;
  StringBuilder *buffer;
} BAILogger;

static inline BAILogger *bai_logger_create(const char *log_filename) {
  BAILogger *bai_logger = (BAILogger *)malloc_or_die(sizeof(BAILogger));
  bai_logger->fh = fopen_or_die(log_filename, "w");
  bai_logger->buffer = string_builder_create();
  return bai_logger;
}

static inline void bai_logger_destroy(BAILogger *bai_logger) {
  if (!bai_logger) {
    return;
  }
  fclose_or_die(bai_logger->fh);
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

static inline void bai_logger_log_bool(BAILogger *bai_logger,
                                       const char *bool_name, const bool x) {
  if (!bai_logger) {
    return;
  }
  string_builder_add_formatted_string(bai_logger->buffer, "%s = %s\n",
                                      bool_name, x ? "true" : "false");
}

static inline void bai_logger_string_builder_add_double(StringBuilder *sb,
                                                        const double x) {
  if (isinf(x)) {
    string_builder_add_string(sb, "Inf");
  } else if (isnan(x)) {
    string_builder_add_string(sb, "NaN");
  } else {
    string_builder_add_formatted_string(sb, "%.*f", BAI_LOGGER_NUM_DECIMALS, x);
  }
}

static inline void bai_logger_log_double(BAILogger *bai_logger,
                                         const char *double_name,
                                         const double x) {
  if (!bai_logger) {
    return;
  }
  string_builder_add_formatted_string(bai_logger->buffer, "%s = ", double_name);
  bai_logger_string_builder_add_double(bai_logger->buffer, x);
  string_builder_add_string(bai_logger->buffer, "\n");
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
    bai_logger_string_builder_add_double(bai_logger->buffer, x[i]);
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

static inline void bai_logger_log_bool_array(BAILogger *bai_logger,
                                             const char *double_array_name,
                                             const bool *x, int size) {
  if (!bai_logger) {
    return;
  }
  string_builder_add_formatted_string(bai_logger->buffer,
                                      "%s = ", double_array_name);
  for (int i = 0; i < size; i++) {
    if (x[i]) {
      string_builder_add_string(bai_logger->buffer, "true");
    } else {
      string_builder_add_string(bai_logger->buffer, "false");
    }
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
  write_to_stream(bai_logger->fh, "%s",
                  string_builder_peek(bai_logger->buffer));
  string_builder_clear(bai_logger->buffer);
}

#endif