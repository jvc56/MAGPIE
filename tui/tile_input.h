#ifndef TUI_TILE_INPUT_H
#define TUI_TILE_INPUT_H

#include "../src/ent/letter_distribution.h"
#include <stdbool.h>
#include <stddef.h>

// Typing tiles whose face has more than one letter (Catalan QU, NY, L·L;
// Spanish CH, LL, RR) into a move, one key at a time. Move text writes
// such a tile in brackets, as the engine does ("8H [QU]E"). The typist
// can type the brackets too, or leave them out when the letters can only
// mean that tile:
//   - a letter with no tile of its own that starts exactly one tile
//     becomes that tile, and its remaining letters are skipped when typed
//     next (Catalan Q is QU; the U that follows is absorbed);
//   - a key with no tile of its own that joins the previous tile into
//     one becomes that tile (Catalan N then Y is NY; L, then "." or "-"
//     or "·", then L is L·L).
// Letters that also spell separate tiles stay separate (Catalan LL is
// two Ls); brackets choose the joined tile.

enum { TUI_TILE_TEXT_MAX = 16 };

typedef struct {
  bool bracket;                         // typing inside [ ... ]
  char bracket_text[TUI_TILE_TEXT_MAX]; // what's been typed inside
  char skip[TUI_TILE_TEXT_MAX]; // letters to absorb: a guessed tile's rest
} TuiTileKeyState;

typedef enum {
  TUI_TILE_KEY_PLACE,        // add tile *out_ml
  TUI_TILE_KEY_REPLACE_LAST, // the previous tile becomes *out_ml
  TUI_TILE_KEY_WAIT,         // part of a bracketed tile; nothing yet
  TUI_TILE_KEY_ABSORBED,     // an expected letter of the last tile
  TUI_TILE_KEY_REJECT,       // no tile
} TuiTileKeyAction;

void tui_tile_key_reset(TuiTileKeyState *state);

// The unblanked machine letter of the tile whose face is `text` (any
// case), or -1 when none.
int tui_tile_for_text(const LetterDistribution *ld, const char *text);

// One key (a UTF-8 character) typed into a move. `last_tile` is the face
// of the tile the typist placed last, when it's the last thing in the
// move and could still be joined; NULL otherwise.
TuiTileKeyAction tui_tile_key(const LetterDistribution *ld,
                              TuiTileKeyState *state, const char *key,
                              const char *last_tile, int *out_ml);

// Bytes in the move-text token at `text`: a bracketed tile through its
// "]", else one UTF-8 character; 0 at the end.
int tui_tile_token_len(const char *text);

// The token boundaries around byte `pos` of `text`: where the token
// ending at `pos` starts (0 at the start), and where the token at `pos`
// ends (`pos` at the end). Cursor movement and deletion use them so a
// "[QU]" or a two-byte "Ç" moves and deletes as one.
int tui_tile_prev_boundary(const char *text, int pos);
int tui_tile_next_boundary(const char *text, int pos);

// Tiles in the first `len` bytes of a move's word ("[QU]E" is 2);
// spaces and parentheses don't count.
int tui_tile_count(const char *text, int len);

// What a tile string may hold: a rack's tiles (uppercase) and blanks
// ("?"), or a move word's tiles (uppercase, or lowercase as blanks) and
// played-through squares (".").
typedef enum {
  TUI_TILES_RACK,
  TUI_TILES_WORD,
} TuiTilesKind;

// Whether each token of the first `len` bytes of `text` is a tile (or,
// by `kind`, a blank or a played-through square).
bool tui_tiles_valid(const LetterDistribution *ld, const char *text, int len,
                     TuiTilesKind kind);

// `text`'s tiles in the letter distribution's order, blanks ("?") first;
// anything unrecognized keeps its order at the end.
void tui_tiles_sort(const LetterDistribution *ld, const char *text, char *out,
                    size_t out_size);

// The rack tiles the first `len` bytes of a move word use: its tiles, a
// lowercase (blank) tile as "?", played-through "." skipped; sorted.
void tui_tiles_from_word(const LetterDistribution *ld, const char *word,
                         int len, char *out, size_t out_size);

// `rack` minus `played` (both tile strings), sorted, into `out`. Returns
// false (with `out` empty) when `played` has a tile `rack` doesn't.
bool tui_tiles_subtract(const LetterDistribution *ld, const char *rack,
                        const char *played, char *out, size_t out_size);

// Tile `ml` (unblanked) as move text: "E", "Ç", "[QU]", or lowercase as a
// blank: "e", "ç", "[qu]".
void tui_tile_token(const LetterDistribution *ld, int ml, bool blank, char *out,
                    size_t out_size);

// The face of the move-text token `token` (its first `len` bytes) without
// brackets: "QU" for "[QU]".
void tui_tile_token_face(const char *token, int len, char *out,
                         size_t out_size);

#endif
