// Tests for tui/tile_input: typing multi-letter tiles (Catalan QU, NY, L·L)
// into a move. Run from the repository root (reads data/letterdistributions).
// Build and run: make magpie_tui_test && ./bin/magpie_tui_test

#include "../../src/ent/letter_distribution.h"
#include "../../src/util/io_util.h"
#include "../tile_input.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum { TYPED_MAX = 128 };

static int failures = 0;

static void check(bool ok, const char *what) {
  if (!ok) {
    (void)fprintf(stderr, "FAIL: %s\n", what);
    failures++;
  }
}

static void check_str(const char *got, const char *want, const char *what) {
  if (strcmp(got, want) != 0) {
    (void)fprintf(stderr, "FAIL: %s: got \"%s\", want \"%s\"\n", what, got,
                  want);
    failures++;
  }
}

// Types `keys` (NULL-terminated) into an empty move the way board entry
// does, and writes the move text it builds to `out`.
static void type_keys(const LetterDistribution *ld, const char *const *keys,
                      char *out, size_t out_size) {
  TuiTileKeyState state;
  tui_tile_key_reset(&state);
  char faces[TYPED_MAX][TUI_TILE_TEXT_MAX];
  int count = 0;
  for (int key_idx = 0; keys[key_idx] != NULL; key_idx++) {
    int ml = -1;
    const TuiTileKeyAction action = tui_tile_key(
        ld, &state, keys[key_idx], count > 0 ? faces[count - 1] : NULL, &ml);
    if (action == TUI_TILE_KEY_PLACE && count < TYPED_MAX) {
      (void)snprintf(faces[count++], TUI_TILE_TEXT_MAX, "%s",
                     ld->ld_ml_to_hl[ml]);
    } else if (action == TUI_TILE_KEY_REPLACE_LAST && count > 0) {
      (void)snprintf(faces[count - 1], TUI_TILE_TEXT_MAX, "%s",
                     ld->ld_ml_to_hl[ml]);
    }
  }
  size_t used = 0;
  out[0] = '\0';
  for (int face_idx = 0; face_idx < count; face_idx++) {
    char token[TUI_TILE_TEXT_MAX + 2];
    tui_tile_token(ld, tui_tile_for_text(ld, faces[face_idx]), false, token,
                   sizeof(token));
    used += (size_t)snprintf(out + used, out_size - used, "%s", token);
  }
}

static void check_typed(const LetterDistribution *ld, const char *const *keys,
                        const char *want, const char *what) {
  char got[TYPED_MAX];
  type_keys(ld, keys, got, sizeof(got));
  check_str(got, want, what);
}

static void test_catalan(const LetterDistribution *ld) {
  const int qu = tui_tile_for_text(ld, "QU");
  const int ll = tui_tile_for_text(ld, "L\xc2\xb7L");
  check(qu > 0, "QU is a Catalan tile");
  check(ll > 0, "L·L is a Catalan tile");
  check(tui_tile_for_text(ld, "NY") > 0, "NY is a Catalan tile");
  check(tui_tile_for_text(ld, "qu") == qu, "lowercase qu is QU");
  check(tui_tile_for_text(ld, "l.l") == ll, "l.l is L·L");
  check(tui_tile_for_text(ld, "L-L") == ll, "L-L is L·L");
  check(tui_tile_for_text(ld, "\xc3\x87") > 0, "Ç is a tile");
  check(tui_tile_for_text(ld, "\xc3\xa7") == tui_tile_for_text(ld, "\xc3\x87"),
        "ç is Ç");
  check(tui_tile_for_text(ld, "Q") < 0, "Catalan has no plain Q");
  check(tui_tile_for_text(ld, "Y") < 0, "Catalan has no plain Y");

  const char *const que[] = {"q", "u", "e", NULL};
  check_typed(ld, que, "[QU]E", "Q starts QU and absorbs the U");
  const char *const qe[] = {"q", "e", NULL};
  check_typed(ld, qe, "[QU]E", "Q alone is still QU");
  const char *const nya[] = {"n", "y", "a", NULL};
  check_typed(ld, nya, "[NY]A", "N then Y joins into NY");
  const char *const nana[] = {"n", "a", NULL};
  check_typed(ld, nana, "NA", "N then another letter stays N");
  const char *const lla_dot[] = {"l", ".", "l", "a", NULL};
  check_typed(ld, lla_dot, "[L\xc2\xb7L]A", "L . L joins into L·L");
  const char *const ll_dash[] = {"l", "-", "l", NULL};
  check_typed(ld, ll_dash, "[L\xc2\xb7L]", "L - L joins into L·L");
  const char *const ll_dot[] = {"l", "\xc2\xb7", "l", NULL};
  check_typed(ld, ll_dot, "[L\xc2\xb7L]", "L · L joins into L·L");
  const char *const two_l[] = {"l", "l", NULL};
  check_typed(ld, two_l, "LL", "LL stays two L tiles");
  const char *const bracket_ny[] = {"[", "n", "y", "]", NULL};
  check_typed(ld, bracket_ny, "[NY]", "brackets choose NY");
  const char *const bracket_qu[] = {"[", "Q", "U", "]", "i", NULL};
  check_typed(ld, bracket_qu, "[QU]I", "brackets choose QU");
  const char *const bracket_ll[] = {"[", "l", ".", "l", "]", NULL};
  check_typed(ld, bracket_ll, "[L\xc2\xb7L]", "brackets choose L·L");
  const char *const bracket_bad[] = {"[", "l", "l", "]", NULL};
  check_typed(ld, bracket_bad, "", "[LL] is no tile");
  const char *const cedilla[] = {"\xc3\xa7", "a", NULL};
  check_typed(ld, cedilla,
              "\xc3\x87"
              "A",
              "Ç types directly");
  const char *const lone_y[] = {"y", NULL};
  check_typed(ld, lone_y, "", "a lone Y is rejected");
  const char *const ay[] = {"a", "y", NULL};
  check_typed(ld, ay, "A", "Y after A is rejected");

  char token[TUI_TILE_TEXT_MAX];
  tui_tile_token(ld, qu, true, token, sizeof(token));
  check_str(token, "[qu]", "a blank QU");
  tui_tile_token(ld, ll, false, token, sizeof(token));
  check_str(token, "[L\xc2\xb7L]", "L·L token");
}

static void test_english(const LetterDistribution *ld) {
  const char *const qi[] = {"q", "i", NULL};
  check_typed(ld, qi, "QI", "English Q is Q");
  const char *const bracket_qu[] = {"[", "q", "u", "]", NULL};
  check_typed(ld, bracket_qu, "", "English has no QU tile");
}

static void test_tokens(void) {
  check(tui_tile_count("[QU]E", 5) == 2, "[QU]E is two tiles");
  check(tui_tile_count("(A)B", 4) == 2, "parentheses aren't tiles");
  check(tui_tile_count("[L\xc2\xb7L]A", 7) == 2, "[L·L]A is two tiles");
  check(tui_tile_count("\xc3\x87"
                       "A",
                       3) == 2,
        "ÇA is two tiles");
  check(tui_tile_token_len("[L\xc2\xb7L]x") == 6, "[L·L] is 6 bytes");
  check(tui_tile_token_len("\xc3\x87x") == 2, "Ç is 2 bytes");
  // "8H [QU]ÇA": tokens start at 3, 7, 9.
  const char *word = "8H [QU]\xc3\x87"
                     "A";
  check(tui_tile_prev_boundary(word, 10) == 9, "back over A");
  check(tui_tile_prev_boundary(word, 9) == 7, "back over Ç");
  check(tui_tile_prev_boundary(word, 7) == 3, "back over [QU]");
  check(tui_tile_next_boundary(word, 3) == 7, "forward over [QU]");
  check(tui_tile_next_boundary(word, 7) == 9, "forward over Ç");
  check(tui_tile_next_boundary(word, 10) == 10, "forward at the end");
  char face[TUI_TILE_TEXT_MAX];
  tui_tile_token_face("[NY]", 4, face, sizeof(face));
  check_str(face, "NY", "[NY] face");
}

static void test_tile_strings(const LetterDistribution *ld) {
  char out[64];
  tui_tiles_sort(ld, "E[QU]?A\xc3\x87", out, sizeof(out));
  check_str(out,
            "?A\xc3\x87"
            "E[QU]",
            "Catalan rack sorts blank first, Ç after A");
  tui_tiles_from_word(ld, "[QU].[ny]A", 10, out, sizeof(out));
  check_str(out, "?A[QU]", "word to rack: blank NY, played-through skipped");
  check(tui_tiles_subtract(ld, "AE[QU][NY]?", "[QU]A", out, sizeof(out)),
        "subtract QU and A");
  check_str(out, "?E[NY]", "leave after [QU]A");
  check(!tui_tiles_subtract(ld, "AE", "[QU]", out, sizeof(out)),
        "can't subtract a tile the rack lacks");
  check(tui_tiles_valid(ld, "A[QU]?", 6, TUI_TILES_RACK), "rack is valid");
  check(!tui_tiles_valid(ld, "A[LL]", 5, TUI_TILES_RACK), "[LL] isn't a tile");
  check(!tui_tiles_valid(ld, "Aq", 2, TUI_TILES_RACK),
        "lowercase isn't a rack tile");
  check(tui_tiles_valid(ld, "[qu].E", 6, TUI_TILES_WORD), "word is valid");
}

static LetterDistribution *load_ld(const char *name) {
  ErrorStack *err = error_stack_create();
  LetterDistribution *ld = ld_create("data", name, err);
  if (!error_stack_is_empty(err)) {
    (void)fprintf(stderr, "could not load %s (run from the repo root)\n", name);
    ld = NULL;
  }
  error_stack_destroy(err);
  return ld;
}

int main(void) {
  LetterDistribution *catalan = load_ld("catalan");
  LetterDistribution *english = load_ld("english");
  if (catalan == NULL || english == NULL) {
    return 1;
  }
  test_catalan(catalan);
  test_tile_strings(catalan);
  test_english(english);
  test_tokens();
  ld_destroy(catalan);
  ld_destroy(english);
  if (failures > 0) {
    (void)fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  (void)printf("tile_input: all tests passed\n");
  return 0;
}
