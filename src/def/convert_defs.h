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

typedef enum {
  CONVERT_STATUS_SUCCESS,
  CONVERT_STATUS_MISSING_LETTER_DISTRIBUTION,
  CONVERT_STATUS_INPUT_FILE_ERROR,
  CONVERT_STATUS_OUTPUT_FILE_NOT_WRITABLE,
  CONVERT_STATUS_KWG_TOO_LARGE_FOR_FORMAT,
  CONVERT_STATUS_TEXT_CONTAINS_INVALID_LETTER,
  CONVERT_STATUS_TEXT_CONTAINS_WORD_TOO_LONG,
  CONVERT_STATUS_TEXT_CONTAINS_WORD_TOO_SHORT,
  CONVERT_STATUS_MALFORMED_KWG,
  CONVERT_STATUS_UNRECOGNIZED_CONVERSION_TYPE,
  CONVERT_STATUS_UNIMPLEMENTED_CONVERSION_TYPE,
} conversion_status_t;

#endif
