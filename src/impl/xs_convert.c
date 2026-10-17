#include "xs_convert.h"

#include "../compat/xs_compress.h"
#include "../def/convert_defs.h"
#include "../ent/conversion_results.h"
#include "../util/io_util.h"
#include "../util/xs_bits.h"
#include "xs_codebook.h"
#include "xs_game.h"
#include "xs_position.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void xs_convert(const char *input, const char *output, const char *book_name,
                const char *budget, const char *paths, conversion_type_t type,
                ConversionResults *results, ErrorStack *errors) {
  if (!input || !output || !*output || !strcmp(input, output)) {
    xs_error(errors, "specify distinct input and output file paths");
    return;
  }
  const char *book = book_name ? book_name : "CSW24";
  unsigned profile = 3;
  if (!strcmp(book, "CSW24")) {
    profile = 1;
  } else if (!strcmp(book, "NWL23")) {
    profile = 2;
  } else if (!strcmp(book, "none")) {
    profile = 0;
  }
  int32_t search_ms = 20;
  if (profile > 2 || (budget && (!xs_int32(budget, &search_ms) ||
                                 search_ms < 0 || search_ms > 60000))) {
    xs_error(errors,
             "unknown codebook/profile or invalid search budget (0..60000 ms)");
    return;
  }
  if ((type == CONVERT_XSP2CGP || type == CONVERT_XSG2GCG) &&
      (book_name || budget)) {
    xs_error(errors, "decoders take only input and output paths");
    return;
  }
  if (type != CONVERT_CGP2XSP && budget) {
    xs_error(errors, "only cgp2xsp accepts a search budget");
    return;
  }
  if (type == CONVERT_COUNTS2XSW) {
    if (!profile) {
      xs_error(errors, "codebook creation requires CSW24 or NWL23");
      return;
    }
    xs_codebook_build(input, output, book, errors);
    return;
  }
  size_t length = 0;
  char *text = xs_read_file(input, &length, errors);
  if (!text) {
    return;
  }
  if (type == CONVERT_GCG2XSG && length >= 2 && (unsigned char)text[0] == 31 &&
      (unsigned char)text[1] == 139) {
    char *expanded = xs_gunzip(text, length, &length);
    free(text);
    text = expanded;
  }
  if (!text || strlen(text) != length) {
    free(text);
    xs_error(errors, "invalid input text or gzip");
    return;
  }
  while (length && isspace((unsigned char)text[length - 1])) {
    text[--length] = '\0';
  }
  char *value = NULL;
  XSGameStats stats = {0};
  if (type == CONVERT_CGP2XSP) {
    value =
        xs_position_encode(text, paths, profile, (unsigned)search_ms, errors);
  } else if (type == CONVERT_XSP2CGP) {
    value = xs_position_decode(text, paths, errors);
  } else if (type == CONVERT_GCG2XSG) {
    value = xs_game_encode(text, paths, profile, &stats, errors);
  } else if (type == CONVERT_XSG2GCG) {
    value = xs_game_decode(text, paths, errors);
  }
  free(text);
  if (value) {
    char *with_newline = get_formatted_string(
        "%s%s", value, *value && value[strlen(value) - 1] == '\n' ? "" : "\n");
    if (xs_write_file(output, with_newline, strlen(with_newline), errors)) {
      conversion_results_set_report(
          results, get_formatted_string("XS: wrote %zu bytes to %s%s",
                                        strlen(with_newline), output, "\n"));
      if (type == CONVERT_GCG2XSG) {
        conversion_results_set_report(
            results, get_formatted_string(
                         "XS: %zu characters; %u ranked moves, %u explicit "
                         "moves, %u probability-ranked draws\n",
                         strlen(value), stats.ranked, stats.explicit_moves,
                         stats.ranked_draws));
      }
    }
    free(with_newline);
    free(value);
  }
}
