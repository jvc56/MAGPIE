#ifndef XS_BITS_H
#define XS_BITS_H
#include "io_util.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { XS_MAX_BYTES = 8 * 1024 * 1024, XS_MAX_TEXT = 4096 };
typedef struct XSBuffer {
  uint8_t *bytes;
  size_t bits;
  size_t capacity;
  size_t cursor;
  bool valid;
} XSBuffer;
void xs_buffer_init(XSBuffer *buffer);
void xs_buffer_destroy(XSBuffer *buffer);
void xs_put(XSBuffer *buffer, uint32_t value, unsigned width);
uint32_t xs_get(XSBuffer *buffer, unsigned width);
void xs_append(XSBuffer *target, const XSBuffer *source);
void xs_put_var(XSBuffer *buffer, uint32_t value);
uint32_t xs_get_var(XSBuffer *buffer);
void xs_put_signed(XSBuffer *buffer, int32_t value);
int32_t xs_get_signed(XSBuffer *buffer);
void xs_put_text(XSBuffer *buffer, const char *text);
char *xs_get_text(XSBuffer *buffer);
void xs_put_rank(XSBuffer *buffer, unsigned order, uint32_t rank);
uint32_t xs_get_rank(XSBuffer *buffer, unsigned order);
bool xs_finish(XSBuffer *buffer);
char *xs_token(const XSBuffer *buffer, const char *magic);
bool xs_untoken(XSBuffer *buffer, const char *token, const char *magic);
void xs_error(ErrorStack *errors, const char *message);
char *xs_read_file(const char *path, size_t *length, ErrorStack *errors);
char *xs_asset_path(const char *data_paths, const char *name,
                    ErrorStack *errors);
bool xs_write_file(const char *path, const void *bytes, size_t length,
                   ErrorStack *errors);
bool xs_utf8(const char *text);
bool xs_int32(const char *text, int32_t *value);
#endif
