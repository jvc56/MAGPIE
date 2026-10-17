#ifndef XS_POSITION_H
#define XS_POSITION_H

#include "../util/xs_bits.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct XSPosition {
  char board[225];
  char racks[2][8];
  int32_t scores[2];
  unsigned zeros;
  char language[64];
  char lexicon[256];
} XSPosition;

void xs_write_metadata(XSBuffer *bits, const char *language,
                       const char *lexicon);
void xs_read_metadata(XSBuffer *bits, char language[64], char lexicon[256]);
bool xs_parse_cgp(const char *text, XSPosition *position);
char *xs_format_cgp(const XSPosition *position);
char *xs_position_encode(const char *cgp, const char *data_paths,
                         unsigned codebook, unsigned search_ms,
                         ErrorStack *errors);
char *xs_position_decode(const char *token, const char *data_paths,
                         ErrorStack *errors);
bool xs_letter(char letter);
char xs_upper(char letter);

#endif
