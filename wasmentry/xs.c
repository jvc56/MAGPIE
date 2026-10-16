#include "../src/impl/xs_game.h"
#include "../src/impl/xs_position.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "../src/util/xs_bits.h"
#include <stdlib.h>

enum { XS_ENCODE_POSITION, XS_DECODE_POSITION, XS_ENCODE_GAME, XS_DECODE_GAME };

// The browser worker owns the call and frees the returned JSON. Codec state is
// independent of the interactive engine, including retained search results.
char *wasm_xs_convert(int operation, const char *text, unsigned profile) {
  ErrorStack *errors = error_stack_create();
  char *value = NULL;
  if (!text) {
    xs_error(errors, "missing link content");
  } else if (operation == XS_ENCODE_POSITION) {
    value = xs_position_encode(text, "data", profile, 20, errors);
  } else if (operation == XS_DECODE_POSITION) {
    value = xs_position_decode(text, "data", errors);
  } else if (operation == XS_ENCODE_GAME) {
    XSGameStats stats = {0};
    value = xs_game_encode(text, "data", profile, &stats, errors);
  } else if (operation == XS_DECODE_GAME) {
    value = xs_game_decode(text, "data", errors);
  } else {
    xs_error(errors, "unknown link operation");
  }
  StringBuilder *result = string_builder_create();
  if (value && error_stack_is_empty(errors)) {
    string_builder_add_string(result, "{\"value\":");
    string_builder_add_json_string(result, value);
  } else {
    char *message = error_stack_get_string_and_reset(errors);
    string_builder_add_string(result, "{\"error\":");
    string_builder_add_json_string(result, message);
    free(message);
  }
  string_builder_add_char(result, '}');
  char *json = string_builder_dump(result, NULL);
  string_builder_destroy(result);
  free(value);
  error_stack_destroy(errors);
  return json;
}
