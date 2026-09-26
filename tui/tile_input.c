#include "tile_input.h"

#include "../src/def/letter_distribution_defs.h"
#include "../src/ent/letter_distribution.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum {
  ALIAS_MAX = 64,
  ALIAS_LD_MAX = 32,
  ALIAS_TYPED_MAX = 8,
  ALIAS_LINE_MAX = 256,
};

// One row of the tile alias file: in letter distribution `ld` (and its
// variants), `typed` reads as `letters`.
typedef struct {
  char ld[ALIAS_LD_MAX];
  char typed[ALIAS_TYPED_MAX];
  char letters[TUI_TILE_TEXT_MAX];
} TileAlias;

// Loaded once at startup, before other threads run; read-only after.
static TileAlias aliases[ALIAS_MAX];
static int alias_count;

bool tui_tile_aliases_find_path(char *out, size_t out_size) {
  // Probed like the bundled font, so the binary finds it from the repo
  // root or from bin/.
  static const char *candidates[] = {
      "tui/assets/tile_aliases.csv",
      "../tui/assets/tile_aliases.csv",
  };
  for (size_t idx = 0; idx < sizeof(candidates) / sizeof(candidates[0]);
       idx++) {
    if (access(candidates[idx], R_OK) == 0) {
      (void)snprintf(out, out_size, "%s", candidates[idx]);
      return true;
    }
  }
  return false;
}

// Copies field `field_idx` of the comma-separated `line` into `out`.
// Returns false when the line has fewer fields or the field doesn't
// fit.
static bool csv_field(const char *line, int field_idx, char *out,
                      size_t out_size) {
  const char *start = line;
  for (int skip_idx = 0; skip_idx < field_idx; skip_idx++) {
    start = strchr(start, ',');
    if (start == NULL) {
      return false;
    }
    start++;
  }
  size_t len = strcspn(start, ",\r\n");
  if (len == 0 || len >= out_size) {
    return false;
  }
  memcpy(out, start, len);
  out[len] = '\0';
  return true;
}

int tui_tile_aliases_load(const char *path) {
  alias_count = 0;
  FILE *file = fopen(path, "re");
  if (file == NULL) {
    return -1;
  }
  char line[ALIAS_LINE_MAX];
  while (alias_count < ALIAS_MAX && fgets(line, sizeof(line), file) != NULL) {
    if (line[0] == '#' || line[0] == '\n' || line[0] == '\r' ||
        line[0] == '\0') {
      continue;
    }
    TileAlias *alias = &aliases[alias_count];
    if (csv_field(line, 0, alias->ld, sizeof(alias->ld)) &&
        csv_field(line, 1, alias->typed, sizeof(alias->typed)) &&
        csv_field(line, 2, alias->letters, sizeof(alias->letters))) {
      alias_count++;
    }
  }
  (void)fclose(file);
  return alias_count;
}

// Whether alias rows for `alias_ld` apply to letter distribution
// `ld_name`: the same name, or a variant of it ("catalan_super").
static bool alias_applies(const char *alias_ld, const char *ld_name) {
  const size_t len = strlen(alias_ld);
  return ld_name != NULL && strncmp(ld_name, alias_ld, len) == 0 &&
         (ld_name[len] == '\0' || ld_name[len] == '_');
}

// The longest alias of `ld` that `text` starts with, or NULL.
static const TileAlias *alias_at(const LetterDistribution *ld,
                                 const char *text) {
  const TileAlias *found = NULL;
  size_t found_len = 0;
  for (int alias_idx = 0; alias_idx < alias_count; alias_idx++) {
    const TileAlias *alias = &aliases[alias_idx];
    const size_t len = strlen(alias->typed);
    if (len > found_len && strncmp(text, alias->typed, len) == 0 &&
        alias_applies(alias->ld, ld->name)) {
      found = alias;
      found_len = len;
    }
  }
  return found;
}

void tui_tile_key_reset(TuiTileKeyState *state) {
  state->bracket = false;
  state->bracket_text[0] = '\0';
  state->skip[0] = '\0';
}

// Copies `text` into `out`, each alias of `ld` replaced by its letters
// and ASCII letters uppercased, so it compares with the tiles'
// uppercase faces.
static void fold_text(const LetterDistribution *ld, const char *text, char *out,
                      size_t out_size) {
  size_t used = 0;
  for (const char *ch = text; *ch != '\0' && used + 1 < out_size;) {
    const TileAlias *alias = alias_at(ld, ch);
    if (alias != NULL) {
      const size_t letters_len = strlen(alias->letters);
      if (used + letters_len + 1 > out_size) {
        break;
      }
      memcpy(out + used, alias->letters, letters_len);
      used += letters_len;
      ch += strlen(alias->typed);
      continue;
    }
    char folded = *ch;
    if (folded >= 'a' && folded <= 'z') {
      folded = (char)(folded - 'a' + 'A');
    }
    out[used++] = folded;
    ch++;
  }
  out[used] = '\0';
}

// Whether tile `ml`'s face, uppercase or as a blank (lowercase), equals
// `text` (exact) or, when `prefix`, strictly extends it.
static bool face_matches(const LetterDistribution *ld, int ml, const char *raw,
                         const char *folded, bool prefix) {
  const char *faces[2] = {
      ld->ld_ml_to_hl[ml],
      ld->ld_ml_to_hl[get_blanked_machine_letter((MachineLetter)ml)]};
  const char *texts[2] = {folded, raw};
  for (int face_idx = 0; face_idx < 2; face_idx++) {
    const char *face = faces[face_idx];
    const char *text = texts[face_idx];
    const size_t text_len = strlen(text);
    if (text_len == 0) {
      continue;
    }
    if (prefix ? strlen(face) > text_len && strncmp(face, text, text_len) == 0
               : strcmp(face, text) == 0) {
      return true;
    }
  }
  return false;
}

int tui_tile_for_text(const LetterDistribution *ld, const char *text) {
  char folded[TUI_TILE_TEXT_MAX * 2];
  fold_text(ld, text, folded, sizeof(folded));
  for (int ml = 1; ml < ld_get_size(ld); ml++) {
    if (face_matches(ld, ml, text, folded, false)) {
      return ml;
    }
  }
  return -1;
}

// The only tile whose face strictly extends `text`, or -1 when none or
// several.
static int only_extension(const LetterDistribution *ld, const char *text) {
  char folded[TUI_TILE_TEXT_MAX * 2];
  fold_text(ld, text, folded, sizeof(folded));
  int found = -1;
  for (int ml = 1; ml < ld_get_size(ld); ml++) {
    if (face_matches(ld, ml, text, folded, true)) {
      if (found >= 0) {
        return -1;
      }
      found = ml;
    }
  }
  return found;
}

// Sets the letters of tile `ml`'s face after the first `typed_len`
// folded bytes as the ones to absorb next.
static void set_skip(const LetterDistribution *ld, TuiTileKeyState *state,
                     int ml, const char *typed) {
  char folded[TUI_TILE_TEXT_MAX * 2];
  fold_text(ld, typed, folded, sizeof(folded));
  const char *face = ld->ld_ml_to_hl[ml];
  const size_t typed_len = strlen(folded);
  (void)snprintf(state->skip, sizeof(state->skip), "%s",
                 strlen(face) > typed_len ? face + typed_len : "");
}

TuiTileKeyAction tui_tile_key(const LetterDistribution *ld,
                              TuiTileKeyState *state, const char *key,
                              const char *last_tile, int *out_ml) {
  char folded_key[TUI_TILE_TEXT_MAX];
  fold_text(ld, key, folded_key, sizeof(folded_key));
  if (state->skip[0] != '\0') {
    const size_t key_len = strlen(folded_key);
    if (key_len > 0 && strncmp(state->skip, folded_key, key_len) == 0) {
      memmove(state->skip, state->skip + key_len,
              strlen(state->skip + key_len) + 1);
      return TUI_TILE_KEY_ABSORBED;
    }
    state->skip[0] = '\0';
  }
  if (strcmp(key, "[") == 0 && !state->bracket) {
    state->bracket = true;
    state->bracket_text[0] = '\0';
    return TUI_TILE_KEY_WAIT;
  }
  if (state->bracket) {
    if (strcmp(key, "]") == 0) {
      const int ml = tui_tile_for_text(ld, state->bracket_text);
      tui_tile_key_reset(state);
      if (ml < 0) {
        return TUI_TILE_KEY_REJECT;
      }
      *out_ml = ml;
      return TUI_TILE_KEY_PLACE;
    }
    const size_t used = strlen(state->bracket_text);
    if (used + strlen(key) >= sizeof(state->bracket_text)) {
      return TUI_TILE_KEY_REJECT;
    }
    memcpy(state->bracket_text + used, key, strlen(key) + 1);
    return TUI_TILE_KEY_WAIT;
  }
  const int ml = tui_tile_for_text(ld, key);
  if (ml >= 0) {
    *out_ml = ml;
    return TUI_TILE_KEY_PLACE;
  }
  if (last_tile != NULL && last_tile[0] != '\0') {
    char joined[TUI_TILE_TEXT_MAX * 2];
    (void)snprintf(joined, sizeof(joined), "%s%s", last_tile, key);
    const int joined_ml = tui_tile_for_text(ld, joined);
    if (joined_ml >= 0) {
      *out_ml = joined_ml;
      return TUI_TILE_KEY_REPLACE_LAST;
    }
    const int extended_ml = only_extension(ld, joined);
    if (extended_ml >= 0) {
      set_skip(ld, state, extended_ml, joined);
      *out_ml = extended_ml;
      return TUI_TILE_KEY_REPLACE_LAST;
    }
  }
  const int extended_ml = only_extension(ld, key);
  if (extended_ml >= 0) {
    set_skip(ld, state, extended_ml, key);
    *out_ml = extended_ml;
    return TUI_TILE_KEY_PLACE;
  }
  return TUI_TILE_KEY_REJECT;
}

// Bytes in the UTF-8 character starting with `lead`.
static int utf8_len(unsigned char lead) {
  if (lead < 0x80) {
    return 1;
  }
  if ((lead & 0xe0) == 0xc0) {
    return 2;
  }
  if ((lead & 0xf0) == 0xe0) {
    return 3;
  }
  return (lead & 0xf8) == 0xf0 ? 4 : 1;
}

int tui_tile_token_len(const char *text) {
  if (text[0] == '\0') {
    return 0;
  }
  if (text[0] == '[') {
    const char *close = strchr(text, ']');
    return close != NULL ? (int)(close - text) + 1 : (int)strlen(text);
  }
  const int len = utf8_len((unsigned char)text[0]);
  int available = 0;
  while (available < len && text[available] != '\0') {
    available++;
  }
  return available;
}

int tui_tile_prev_boundary(const char *text, int pos) {
  int start = 0;
  for (int at = 0; at < pos && text[at] != '\0';) {
    start = at;
    at += tui_tile_token_len(text + at);
  }
  return start;
}

int tui_tile_next_boundary(const char *text, int pos) {
  for (int at = 0; text[at] != '\0';) {
    const int end = at + tui_tile_token_len(text + at);
    if (end > pos) {
      return end;
    }
    at = end;
  }
  return pos;
}

int tui_tile_count(const char *text, int len) {
  int tiles = 0;
  int pos = 0;
  while (pos < len && text[pos] != '\0') {
    const int token_len = tui_tile_token_len(text + pos);
    const char ch = text[pos];
    if (ch != ' ' && ch != '(' && ch != ')') {
      tiles++;
    }
    pos += token_len;
  }
  return tiles;
}

void tui_tile_token(const LetterDistribution *ld, int ml, bool blank, char *out,
                    size_t out_size) {
  const char *face =
      ld->ld_ml_to_hl[blank ? get_blanked_machine_letter((MachineLetter)ml)
                            : (MachineLetter)ml];
  if (is_human_readable_letter_multichar(face)) {
    (void)snprintf(out, out_size, "[%s]", face);
  } else {
    (void)snprintf(out, out_size, "%s", face);
  }
}

void tui_tile_token_face(const char *token, int len, char *out,
                         size_t out_size) {
  if (len >= 2 && token[0] == '[' && token[len - 1] == ']') {
    (void)snprintf(out, out_size, "%.*s", len - 2, token + 1);
  } else {
    (void)snprintf(out, out_size, "%.*s", len, token);
  }
}

// The machine letter of the token at `token` (`len` bytes), -1 when it
// isn't a tile; *out_blank says whether it's written as a blank.
static int token_tile(const LetterDistribution *ld, const char *token, int len,
                      bool *out_blank) {
  char face[TUI_TILE_TEXT_MAX];
  tui_tile_token_face(token, len, face, sizeof(face));
  const int ml = tui_tile_for_text(ld, face);
  *out_blank = false;
  if (ml > 0) {
    char blank_token[TUI_TILE_TEXT_MAX + 2];
    tui_tile_token(ld, ml, true, blank_token, sizeof(blank_token));
    *out_blank = (int)strlen(blank_token) == len &&
                 strncmp(token, blank_token, (size_t)len) == 0;
  }
  return ml;
}

bool tui_tiles_valid(const LetterDistribution *ld, const char *text, int len,
                     TuiTilesKind kind) {
  for (int pos = 0; pos < len && text[pos] != '\0';) {
    const int token_len = tui_tile_token_len(text + pos);
    bool blank = false;
    const bool ok =
        kind == TUI_TILES_RACK
            ? (token_len == 1 && text[pos] == '?') ||
                  (token_tile(ld, text + pos, token_len, &blank) > 0 && !blank)
            : (token_len == 1 && text[pos] == '.') ||
                  token_tile(ld, text + pos, token_len, &blank) > 0;
    if (!ok) {
      return false;
    }
    pos += token_len;
  }
  return true;
}

void tui_tiles_canonical_rack(const LetterDistribution *ld, const char *text,
                              int len, char *out, size_t out_size) {
  size_t used = 0;
  out[0] = '\0';
  for (int pos = 0; pos < len && text[pos] != '\0';) {
    int token_len = tui_tile_token_len(text + pos);
    if (token_len > len - pos) {
      token_len = len - pos;
    }
    const bool open_bracket =
        text[pos] == '[' && text[pos + token_len - 1] != ']';
    char face[TUI_TILE_TEXT_MAX];
    tui_tile_token_face(text + pos, token_len, face, sizeof(face));
    const int ml = open_bracket ? -1 : tui_tile_for_text(ld, face);
    char token[TUI_TILE_TEXT_MAX + 2];
    if (ml > 0) {
      tui_tile_token(ld, ml, false, token, sizeof(token));
    } else {
      (void)snprintf(token, sizeof(token), "%.*s", token_len, text + pos);
    }
    const size_t token_bytes = strlen(token);
    if (used + token_bytes + 1 > out_size) {
      break;
    }
    memcpy(out + used, token, token_bytes + 1);
    used += token_bytes;
    pos += token_len;
  }
}

enum { SORT_MAX_TILES = 32, SORT_UNKNOWN_RANK = 1 << 20 };

typedef struct {
  char text[TUI_TILE_TEXT_MAX + 2];
  int rank;
} SortTile;

// Splits `text` into up to SORT_MAX_TILES tiles ranked for sorting:
// blanks first, then letter-distribution order, unknown text last.
static int split_tiles(const LetterDistribution *ld, const char *text,
                       SortTile *tiles) {
  int count = 0;
  for (const char *pos = text; *pos != '\0' && count < SORT_MAX_TILES;) {
    const int len = tui_tile_token_len(pos);
    SortTile *tile = &tiles[count++];
    (void)snprintf(tile->text, sizeof(tile->text), "%.*s", len, pos);
    bool blank = false;
    const int ml =
        strcmp(tile->text, "?") == 0 ? 0 : token_tile(ld, pos, len, &blank);
    tile->rank = ml >= 0 ? ml : SORT_UNKNOWN_RANK + count;
    pos += len;
  }
  return count;
}

// Insertion sort by rank (a handful of tiles; keeps ties in order), then
// concatenates into `out`.
static void join_sorted(SortTile *tiles, int count, char *out,
                        size_t out_size) {
  for (int idx = 1; idx < count; idx++) {
    const SortTile moving = tiles[idx];
    int slot = idx;
    while (slot > 0 && tiles[slot - 1].rank > moving.rank) {
      tiles[slot] = tiles[slot - 1];
      slot--;
    }
    tiles[slot] = moving;
  }
  size_t used = 0;
  out[0] = '\0';
  for (int idx = 0; idx < count; idx++) {
    const size_t len = strlen(tiles[idx].text);
    if (used + len + 1 > out_size) {
      break;
    }
    memcpy(out + used, tiles[idx].text, len);
    used += len;
  }
  out[used] = '\0';
}

void tui_tiles_sort(const LetterDistribution *ld, const char *text, char *out,
                    size_t out_size) {
  SortTile tiles[SORT_MAX_TILES];
  join_sorted(tiles, split_tiles(ld, text, tiles), out, out_size);
}

void tui_tiles_from_word(const LetterDistribution *ld, const char *word,
                         int len, char *out, size_t out_size) {
  SortTile tiles[SORT_MAX_TILES];
  int count = 0;
  for (int pos = 0; pos < len && word[pos] != '\0' && count < SORT_MAX_TILES;) {
    const int token_len = tui_tile_token_len(word + pos);
    bool blank = false;
    const int ml = token_tile(ld, word + pos, token_len, &blank);
    if (ml > 0) {
      SortTile *tile = &tiles[count++];
      if (blank) {
        (void)snprintf(tile->text, sizeof(tile->text), "?");
        tile->rank = 0;
      } else {
        tui_tile_token(ld, ml, false, tile->text, sizeof(tile->text));
        tile->rank = ml;
      }
    }
    pos += token_len;
  }
  join_sorted(tiles, count, out, out_size);
}

bool tui_tiles_subtract(const LetterDistribution *ld, const char *rack,
                        const char *played, char *out, size_t out_size) {
  out[0] = '\0';
  SortTile left[SORT_MAX_TILES];
  int left_count = split_tiles(ld, rack, left);
  SortTile used[SORT_MAX_TILES];
  const int used_count = split_tiles(ld, played, used);
  for (int used_idx = 0; used_idx < used_count; used_idx++) {
    int found = -1;
    for (int idx = 0; idx < left_count && found < 0; idx++) {
      if (left[idx].rank == used[used_idx].rank &&
          (left[idx].rank < SORT_UNKNOWN_RANK ||
           strcmp(left[idx].text, used[used_idx].text) == 0)) {
        found = idx;
      }
    }
    if (found < 0) {
      return false;
    }
    left[found] = left[--left_count];
  }
  join_sorted(left, left_count, out, out_size);
  return true;
}
