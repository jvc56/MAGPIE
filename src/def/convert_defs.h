#ifndef CONVERT_DEFS_H
#define CONVERT_DEFS_H

typedef enum {
  CONVERT_TEXT2DAWG,
  CONVERT_TEXT2GADDAG,
  CONVERT_TEXT2KWG,
  CONVERT_DAWG2TEXT,
  CONVERT_GADDAG2TEXT,
  CONVERT_CSV2KLV,
  CONVERT_KLV2CSV,
  CONVERT_TEXT2WORDMAP,
  CONVERT_UNKNOWN,
} conversion_type_t;

#endif
