#ifndef XS_CODEBOOK_H
#define XS_CODEBOOK_H

#include "../util/xs_bits.h"
#include <stdint.h>

typedef struct XSWord {
  char word[16];
  uint32_t code;
  unsigned length;
} XSWord;

typedef struct XSCodebook {
  XSWord *words;
  XSWord **ordered;
  int count;
  uint32_t first[33];
  uint32_t counts[33];
  uint32_t offsets[33];
} XSCodebook;

XSCodebook *xs_codebook_load(const char *data_paths, unsigned id,
                             ErrorStack *errors);
XSCodebook *xs_codebook_parse(const char *text, ErrorStack *errors);
void xs_codebook_destroy(XSCodebook *book);
const XSWord *xs_codebook_find(const XSCodebook *book, const char *word);
const char *xs_codebook_read(const XSCodebook *book, XSBuffer *bits);
bool xs_codebook_build(const char *input, const char *output,
                       const char *lexicon, ErrorStack *errors);

#endif
