#include "render_common.h"

#include "../src/def/board_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/ent/bonus_square.h"
#include "../src/ent/letter_distribution.h"
#include "../src/util/string_util.h"
#include "config.h"
#include "theme.h"
#include "tile_input.h"
#include <dirent.h>
#include <notcurses/notcurses.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Empty / non-premium cells render an ideographic space so the cell still
// claims its full 2 columns of board_bg. Premium cells use either 2-char
// ASCII labels ("TW"/"tw"/etc.), or that same ideographic space when the
// user asked for color-only premiums.
#define PREMIUM_EMPTY_GLYPH "\xe3\x80\x80"
static const char *premium_glyph(const char *upper, const char *lower,
                                 const char *punct, TuiPremiumLabels labels) {
  switch (labels) {
  case TUI_PREMIUM_LABELS_LOWERCASE:
    return lower;
  case TUI_PREMIUM_LABELS_PUNCT:
    return punct;
  case TUI_PREMIUM_LABELS_NONE:
    return PREMIUM_EMPTY_GLYPH;
  case TUI_PREMIUM_LABELS_UPPERCASE:
  case TUI_PREMIUM_LABELS_COUNT:
  default:
    return upper;
  }
}
// Halfwidth premium markers collapse 2-char labels (TW / DW / TL / DL)
// to single punctuation glyphs that fit in one terminal column. The
// labels=NONE setting still suppresses them in favor of color-only.
static const char *halfwidth_premium_glyph(const char *punct,
                                           TuiPremiumLabels labels) {
  return labels == TUI_PREMIUM_LABELS_NONE ? " " : punct;
}
PremiumMarker premium_marker_for_cell(const Theme *theme, BonusSquare bs,
                                      int row, int col, TuiPremiumLabels labels,
                                      int cell_w) {
  const bool halfwidth = (cell_w == 1);
  const int center = BOARD_DIM / 2;
  if (row == center && col == center) {
    // The center is mechanically a DW; macondo paints it with the DW tint
    // and no separate symbol, so we follow suit — the tint identifies it.
    // Fullwidth punct mode: a ＊ asterisk marks the centre, matching the
    // basic string_builder_add_game printer's convention.
    const char *glyph =
        halfwidth ? halfwidth_premium_glyph("*", labels)
                  : premium_glyph(PREMIUM_EMPTY_GLYPH, PREMIUM_EMPTY_GLYPH,
                                  "\xef\xbc\x8a", labels); // ＊ U+FF0A
    return (PremiumMarker){glyph, theme->premium_center_fg,
                           theme->premium_center_bg};
  }
  const uint8_t word_mult = bonus_square_get_word_multiplier(bs);
  const uint8_t letter_mult = bonus_square_get_letter_multiplier(bs);
  if (word_mult == 3) {
    const char *glyph = halfwidth ? halfwidth_premium_glyph("=", labels)
                                  : premium_glyph("TW", "tw", "\xef\xbc\x9d",
                                                  labels); // ＝ U+FF1D
    return (PremiumMarker){glyph, theme->premium_tws_fg, theme->premium_tws_bg};
  }
  if (word_mult == 2) {
    const char *glyph = halfwidth ? halfwidth_premium_glyph("-", labels)
                                  : premium_glyph("DW", "dw", "\xef\xbc\x8d",
                                                  labels); // － U+FF0D
    return (PremiumMarker){glyph, theme->premium_dws_fg, theme->premium_dws_bg};
  }
  if (letter_mult == 3) {
    const char *glyph = halfwidth ? halfwidth_premium_glyph("\"", labels)
                                  : premium_glyph("TL", "tl", "\xef\xbc\x82",
                                                  labels); // ＂ U+FF02
    return (PremiumMarker){glyph, theme->premium_tls_fg, theme->premium_tls_bg};
  }
  if (letter_mult == 2) {
    const char *glyph = halfwidth ? halfwidth_premium_glyph("'", labels)
                                  : premium_glyph("DL", "dl", "\xef\xbc\x87",
                                                  labels); // ＇ U+FF07
    return (PremiumMarker){glyph, theme->premium_dls_fg, theme->premium_dls_bg};
  }
  return (PremiumMarker){halfwidth ? " " : PREMIUM_EMPTY_GLYPH, theme->dim_fg,
                         theme->board_bg};
}
// ── Lexicon → language label ──────────────────────────────────────────────
// Mirrors the prefix table in tui/lexicon_picker.c. Keep in sync if a new
// lexicon prefix is added. Note the engine's has_iprefix signature:
// `has_iprefix(prefix, str)` — prefix first.
const char *language_for_lexicon(const char *name) {
  if (name == NULL) {
    return "Unknown";
  }
  if (has_iprefix("CSW", name) || has_iprefix("NWL", name) ||
      has_iprefix("OSPD", name) || has_iprefix("OSW", name) ||
      has_iprefix("TWL", name) || has_iprefix("America", name) ||
      has_iprefix("CEL", name)) {
    return "English";
  }
  if (has_iprefix("FRA", name)) {
    return "French";
  }
  if (has_iprefix("OSPS", name)) {
    return "Polish";
  }
  if (has_iprefix("DISC", name)) {
    return "Catalan";
  }
  if (has_iprefix("DSW", name)) {
    return "Dutch";
  }
  if (has_iprefix("NSF", name)) {
    return "Norwegian";
  }
  if (has_iprefix("RD", name)) {
    return "German";
  }
  return "Other";
}
void draw_box_styled(struct ncplane *plane, const Theme *theme, int top_row,
                     int left_col, int height, int width, const char *title,
                     int hotkey, bool focused) {
  draw_box_styled_ex(plane, theme, top_row, left_col, height, width, title,
                     hotkey, focused, /*badge_secondary=*/false);
}
// ── Generic helpers ───────────────────────────────────────────────────────
// Render a panel border. When `focused` is true the box uses double-
// line glyphs and the border row/col cells paint with
// theme->panel_focus_border_bg so the frame lifts off the void. The
// `hotkey` digit (1..5; 0 = none) renders as "[N] " just before the
// title in the top border, dim-grey when unfocused and bold + bright
// when focused. The title itself is drawn in theme->fg on whichever
// bg the border row is using.
// Three-state badge rendering for panels with internal sub-focus:
//   - unfocused:                 dim "[N]" on theme->bg
//   - focused + badge primary:   inverted "[N>" chip (bg-on-fg, bold)
//   - focused + badge secondary: bright "[N]" on the focused border
//     bg (bold) — visible as the focused panel without claiming the
//     chevron-cursor that's drawn on whichever entry/row the user
//     is currently navigating.
// `badge_secondary` is ignored when `focused` is false.
void draw_box_styled_ex(struct ncplane *plane, const Theme *theme, int top_row,
                        int left_col, int height, int width, const char *title,
                        int hotkey, bool focused, bool badge_secondary) {
  const ThemeRgb border_fg = focused ? theme->fg : theme->dim_fg;
  const ThemeRgb border_bg = focused ? theme->panel_focus_border_bg : theme->bg;
  const char *tl = focused ? BOX2_TL : BOX_TL;
  const char *tr = focused ? BOX2_TR : BOX_TR;
  const char *bl = focused ? BOX2_BL : BOX_BL;
  const char *br = focused ? BOX2_BR : BOX_BR;
  const char *hz = focused ? BOX2_HZ : BOX_HZ;
  const char *vt = focused ? BOX2_VT : BOX_VT;
  theme_apply_fg(plane, border_fg);
  theme_apply_bg(plane, border_bg);
  const int right_col = left_col + width - 1;
  const int bottom_row = top_row + height - 1;

  ncplane_putstr_yx(plane, top_row, left_col, tl);
  for (int col = left_col + 1; col < right_col; col++) {
    ncplane_putstr_yx(plane, top_row, col, hz);
  }
  ncplane_putstr_yx(plane, top_row, right_col, tr);

  for (int row = top_row + 1; row < bottom_row; row++) {
    ncplane_putstr_yx(plane, row, left_col, vt);
    ncplane_putstr_yx(plane, row, right_col, vt);
  }

  ncplane_putstr_yx(plane, bottom_row, left_col, bl);
  for (int col = left_col + 1; col < right_col; col++) {
    ncplane_putstr_yx(plane, bottom_row, col, hz);
  }
  ncplane_putstr_yx(plane, bottom_row, right_col, br);

  if (title != NULL && title[0] != '\0') {
    // Title block flush against the top-left corner (one cell in
    // from the corner glyph). The previous two-cell inset wasted
    // a column on every panel; with N panels stacked, that adds up.
    int col = left_col + 1;
    // [N] hotkey indicator. Dim-grey when unfocused; bold + bright
    // when focused. Always renders if hotkey > 0.
    if (hotkey > 0) {
      char buf[8];
      // Three badge states:
      //  - focused + badge primary: inverted "[N>" chip — chevron
      //    indicates "this is where arrow keys move from".
      //  - focused + badge secondary: bright "[N]" on the focused
      //    border bg (bold, no chevron). The user has navigated
      //    deeper into the panel; the badge stays prominent so
      //    you can see WHICH panel is focused, but the chevron
      //    moves with the cursor onto a sub-element row.
      //  - unfocused: dim "[N]" hint.
      if (focused && !badge_secondary) {
        (void)snprintf(buf, sizeof(buf), "[%d>", hotkey);
        theme_apply_fg(plane, theme->bg);
        theme_apply_bg(plane, theme->fg);
        ncplane_set_styles(plane, NCSTYLE_BOLD);
      } else if (focused) {
        (void)snprintf(buf, sizeof(buf), "[%d]", hotkey);
        theme_apply_fg(plane, theme->fg);
        theme_apply_bg(plane, border_bg);
        ncplane_set_styles(plane, NCSTYLE_BOLD);
      } else {
        (void)snprintf(buf, sizeof(buf), "[%d]", hotkey);
        theme_apply_fg(plane, theme->modal_shortcut_fg);
        theme_apply_bg(plane, border_bg);
      }
      ncplane_putstr_yx(plane, top_row, col, buf);
      col += (int)strlen(buf);
      ncplane_set_styles(plane, 0);
      // Space between badge and title.
      theme_apply_fg(plane, theme->fg);
      theme_apply_bg(plane, border_bg);
      ncplane_putstr_yx(plane, top_row, col++, " ");
    }
    // Title text.
    theme_apply_fg(plane, theme->fg);
    theme_apply_bg(plane, border_bg);
    ncplane_putstr_yx(plane, top_row, col, title);
    col += (int)strlen(title);
    // Trailing pad.
    ncplane_putstr_yx(plane, top_row, col, " ");
  }
}
// Back-compat wrapper for callers that don't care about focus/hotkey.
void draw_box(struct ncplane *plane, const Theme *theme, int top_row,
              int left_col, int height, int width, const char *title) {
  draw_box_styled(plane, theme, top_row, left_col, height, width, title,
                  /*hotkey=*/0, /*focused=*/false);
}
// Negative values render with a leading "-" ("-1:23") — the overtime
// display for play-vs-computer games whose rule allows the clock to
// run past 0:00.
void format_clock(int seconds, char *buf, size_t buf_size) {
  const bool negative = seconds < 0;
  const int magnitude = negative ? -seconds : seconds;
  const int minutes = magnitude / 60;
  const int secs = magnitude % 60;
  (void)snprintf(buf, buf_size, "%s%d:%02d", negative ? "-" : "", minutes,
                 secs);
}
// Reorder an alphagram-style string (e.g. "AEHIIRV" or "?AAEIN") so
// it reads in the user's chosen rack-sort order. Used for the
// rack + leave columns inside history entries — those strings are
// pre-built engine-side as alphabetical-with-? alphagrams; we
// re-bucket them at render time so the user sees rack/leave
// orderings consistent with the live rack panel.
void tui_format_alphagram_for_sort(const char *in,
                                   const struct LetterDistribution *ld,
                                   TuiRackSort sort, char *out,
                                   size_t out_size) {
  // Forward to the in-file static implementation. Keeping the
  // static version means existing internal call sites don't have
  // to be touched.
  format_alphagram_for_sort(in, (const LetterDistribution *)ld, sort, out,
                            out_size);
}
// Copies up to `max_chars` UTF-8 characters of `text` into `out`,
// reading "L·L" / "l·l" as "ĿL" / "ŀl" so the geminated L fits two
// cells. Returns the characters copied.
static int compact_face(const char *text, int max_chars, char *out,
                        size_t out_size) {
  static const char middle_dot[] = "\xc2\xb7";
  size_t used = 0;
  int chars = 0;
  out[0] = '\0';
  for (const char *pos = text; *pos != '\0' && chars < max_chars;) {
    const char *piece = pos;
    size_t piece_len = (size_t)tui_tile_token_len(pos);
    const bool dotted_l =
        (*pos == 'L' || *pos == 'l') &&
        strncmp(pos + 1, middle_dot, sizeof(middle_dot) - 1) == 0;
    if (dotted_l) {
      piece = *pos == 'L' ? "\xc4\xbf" : "\xc5\x80"; // Ŀ / ŀ
      piece_len = 2;
      pos += 1 + sizeof(middle_dot) - 1;
    } else {
      pos += piece_len;
    }
    if (used + piece_len + 1 > out_size) {
      break;
    }
    memcpy(out + used, piece, piece_len);
    used += piece_len;
    out[used] = '\0';
    chars++;
  }
  return chars;
}

void tile_face_cells(const LetterDistribution *ld, MachineLetter ml,
                     bool halfwidth, char *out, size_t out_size) {
  const char *face = ml == 0 ? "?" : ld->ld_ml_to_hl[ml];
  if (face == NULL || face[0] == '\0') {
    (void)snprintf(out, out_size, "%s", halfwidth ? " " : "  ");
    return;
  }
  if (is_human_readable_letter_multichar(face)) {
    const int chars = compact_face(face, halfwidth ? 1 : 2, out, out_size);
    if (!halfwidth && chars < 2) {
      (void)snprintf(out + strlen(out), out_size - strlen(out), " ");
    }
    return;
  }
  if (halfwidth) {
    (void)snprintf(out, out_size, "%s", face);
    return;
  }
  const char *fullwidth = ml == 0 ? "" : ld->ld_ml_to_alt_hl[ml];
  if (fullwidth[0] != '\0') {
    (void)snprintf(out, out_size, "%s", fullwidth);
  } else {
    (void)snprintf(out, out_size, " %s", face);
  }
}

// One tile of a rack being sorted for display: its move-text token
// ("E", "Ç", "[QU]", "?"), its place in the letter distribution, and
// whether it's a vowel or the blank.
typedef struct {
  char text[TUI_TILE_TEXT_MAX];
  int rank;
  bool vowel;
  bool blank;
} AlphagramTile;

enum { ALPHAGRAM_MAX_TILES = 32, ALPHAGRAM_UNKNOWN_RANK = 1 << 20 };

// Groups a sort mode lists, in order: blanks, vowels, consonants, or all
// letters (the alphabetical modes don't split vowels).
enum { GROUP_BLANKS, GROUP_VOWELS, GROUP_CONSONANTS, GROUP_LETTERS };

static bool alphagram_in_group(const AlphagramTile *tile, int group) {
  if (group == GROUP_BLANKS || tile->blank) {
    return group == GROUP_BLANKS && tile->blank;
  }
  if (group == GROUP_LETTERS) {
    return true;
  }
  return group == GROUP_VOWELS ? tile->vowel : !tile->vowel;
}

void format_alphagram_for_sort(const char *in, const LetterDistribution *ld,
                               TuiRackSort sort, char *out, size_t out_size) {
  if (out == NULL || out_size == 0) {
    return;
  }
  out[0] = '\0';
  if (in == NULL || ld == NULL) {
    return;
  }
  // Split the rack into tiles (a bracketed multi-letter tile or a UTF-8
  // letter each) and order them as the letter distribution does, so Ç
  // follows C and [L·L] follows L. Unknown text keeps its input order at
  // the end.
  AlphagramTile tiles[ALPHAGRAM_MAX_TILES];
  int count = 0;
  for (const char *pos = in; *pos != '\0' && count < ALPHAGRAM_MAX_TILES;) {
    const int len = tui_tile_token_len(pos);
    AlphagramTile *tile = &tiles[count++];
    (void)snprintf(tile->text, sizeof(tile->text), "%.*s", len, pos);
    tile->blank = strcmp(tile->text, "?") == 0;
    char face[TUI_TILE_TEXT_MAX];
    tui_tile_token_face(pos, len, face, sizeof(face));
    const int ml = tile->blank ? -1 : tui_tile_for_text(ld, face);
    tile->rank = ml >= 0 ? ml : ALPHAGRAM_UNKNOWN_RANK + count;
    tile->vowel = ml >= 0 && ld_get_is_vowel(ld, (MachineLetter)ml);
    pos += len;
  }
  // Insertion sort: racks are a handful of tiles, and it keeps equal
  // tiles in input order.
  for (int idx = 1; idx < count; idx++) {
    const AlphagramTile moving = tiles[idx];
    int slot = idx;
    while (slot > 0 && tiles[slot - 1].rank > moving.rank) {
      tiles[slot] = tiles[slot - 1];
      slot--;
    }
    tiles[slot] = moving;
  }
  int order[3] = {GROUP_LETTERS, GROUP_BLANKS, -1};
  switch (sort) {
  case TUI_RACK_SORT_BLANKS_ALPHA:
    order[0] = GROUP_BLANKS;
    order[1] = GROUP_LETTERS;
    break;
  case TUI_RACK_SORT_VOWELS:
    order[0] = GROUP_VOWELS;
    order[1] = GROUP_CONSONANTS;
    order[2] = GROUP_BLANKS;
    break;
  case TUI_RACK_SORT_BLANKS_VOWELS:
    order[0] = GROUP_BLANKS;
    order[1] = GROUP_VOWELS;
    order[2] = GROUP_CONSONANTS;
    break;
  case TUI_RACK_SORT_ALPHA:
  case TUI_RACK_SORT_COUNT:
  default:
    break;
  }
  size_t used = 0;
  for (int group_idx = 0; group_idx < 3 && order[group_idx] >= 0; group_idx++) {
    for (int idx = 0; idx < count; idx++) {
      const size_t text_len = strlen(tiles[idx].text);
      if (!alphagram_in_group(&tiles[idx], order[group_idx]) ||
          used + text_len + 1 > out_size) {
        continue;
      }
      memcpy(out + used, tiles[idx].text, text_len);
      used += text_len;
    }
  }
  out[used] = '\0';
}
// Render a GCG-style move notation with played-through letters (the
// segments wrapped in parentheses) and the post-play leave (in square
// brackets) unbolded — everything outside the bracketed/parenthesized
// segments reads as bold. The history and analysis panels share this
// so segments look consistent in both. Caller positions the text;
// this routine handles segmenting and style changes only.
//
// `hide_parens` is true in the analysis panel: the parentheses
// themselves are skipped (so "T(H)OLOI" reads as "THOLOI", with the H
// rendered non-bold). History keeps the parens so a played-through
// tile is visually unmistakable in the move log. Square brackets
// (used for leaves in history) are always shown verbatim.
// hide_parens:               drop every `(…)` group's brackets
// hide_playthrough_parens:   drop only `(…)` groups whose content is all
//                            uppercase ASCII (playthrough); keep parens
//                            around lowercase content (blank designation).
//                            Ignored when hide_parens is true.
// Both modes still render parens content non-bold.
// Screen columns `text` takes: one per UTF-8 character (move text is
// letters, dots, and brackets — all narrow).
static int utf8_text_columns(const char *text) {
  int cols = 0;
  for (const unsigned char *ch = (const unsigned char *)text; *ch != '\0';
       ch++) {
    if ((*ch & 0xc0) != 0x80) {
      cols++;
    }
  }
  return cols;
}

void render_move_styled(struct ncplane *plane, int row, int col,
                        const char *move_str, bool hide_parens,
                        bool hide_playthrough_parens) {
  if (move_str == NULL || *move_str == '\0') {
    return;
  }
  const char *p = move_str;
  const char *end = p + strlen(move_str);
  int x = col;
  // Compact-exchange marker: a leading "-" (used by the analysis
  // panel's "-ABCD" shorthand) renders non-bold so it reads as
  // punctuation rather than as part of the tile letters.
  if (*p == '-') {
    ncplane_set_styles(plane, 0);
    ncplane_putstr_yx(plane, row, x, "-");
    x += 1;
    p += 1;
  }
  while (p < end) {
    const char *seg_start = p;
    const char *seg_end;
    bool seg_bold;
    if (*p == '(') {
      seg_bold = false;
      seg_end = p;
      while (seg_end < end && *seg_end != ')') {
        seg_end++;
      }
      // Decide whether to drop this paren group's brackets. hide_parens
      // drops every group; hide_playthrough_parens only drops groups
      // whose content is all uppercase ASCII (the engine emits
      // lowercase inside parens for blank designation — N(a)P — and
      // uppercase for playthrough — CA(JO)N).
      bool drop_this = hide_parens;
      if (!drop_this && hide_playthrough_parens) {
        bool all_upper = (seg_end > p + 1);
        for (const char *q = p + 1; q < seg_end; q++) {
          const unsigned char ch = (unsigned char)*q;
          if (ch < 'A' || ch > 'Z') {
            all_upper = false;
            break;
          }
        }
        drop_this = all_upper;
      }
      if (drop_this) {
        seg_start = p + 1; // skip '('; ')' is dropped via seg_end below
      } else if (seg_end < end) {
        seg_end++; // include the close paren in the rendered segment
      }
    } else if (*p == '[') {
      seg_bold = false;
      seg_end = p;
      while (seg_end < end && *seg_end != ']') {
        seg_end++;
      }
      if (seg_end < end) {
        seg_end++; // include the close bracket in the rendered segment
      }
    } else {
      // Bold segment: a run of characters before the next paren or
      // bracket. We subdivide it so the '.' playthrough marker —
      // a fallback for moves whose position prefix couldn't be
      // parsed and so didn't get its dots resolved to (L) — still
      // renders non-bold. Lowercase letters in this segment are
      // newly-played blanks (the GCG convention is bare lowercase
      // for new blanks; played-through blanks come pre-wrapped as
      // (l)) and render bold like the rest. seg_bold is set
      // per-sub-run inside the inner loop.
      seg_end = p;
      while (seg_end < end && *seg_end != '(' && *seg_end != '[') {
        seg_end++;
      }
      const char *q = seg_start;
      while (q < seg_end) {
        const char *run_start = q;
        const bool run_dim = (*q == '.');
        while (q < seg_end && (*q == '.') == run_dim) {
          q++;
        }
        ncplane_set_styles(plane, run_dim ? 0 : NCSTYLE_BOLD);
        char rbuf[64];
        size_t rlen = (size_t)(q - run_start);
        if (rlen >= sizeof(rbuf)) {
          rlen = sizeof(rbuf) - 1;
        }
        memcpy(rbuf, run_start, rlen);
        rbuf[rlen] = '\0';
        ncplane_putstr_yx(plane, row, x, rbuf);
        x += utf8_text_columns(rbuf);
      }
      p = seg_end;
      continue;
    }
    ncplane_set_styles(plane, seg_bold ? NCSTYLE_BOLD : 0);
    char buf[64];
    size_t len = (size_t)(seg_end - seg_start);
    if (len >= sizeof(buf)) {
      len = sizeof(buf) - 1;
    }
    memcpy(buf, seg_start, len);
    buf[len] = '\0';
    ncplane_putstr_yx(plane, row, x, buf);
    x += utf8_text_columns(buf);
    // When we dropped the parens for this group we landed on ')'; skip
    // past it so it doesn't render as a stray bracket. (seg_end was
    // not advanced over ')' in the drop branch.)
    if (seg_end < end && *seg_end == ')' && seg_start > p) {
      p = seg_end + 1;
    } else {
      p = seg_end;
    }
  }
  ncplane_set_styles(plane, 0);
}
// EMA-smoothed FPS based on the interval between status-bar renders.
// Status-bar render happens once per top-level render call, which is
// what we actually care about — terminal frame rate after all the
// game/grid/composite work. alpha=0.1 gives a stable readout that
// still responds within a second or so when the rate genuinely shifts.
// Compact count formatter shared by the analysis-panel titles and the
// status bar's NPS readout. Same threshold ladder as a chess engine
// info line: bare integer up to 9999, then K / M / B / T with two
// decimal places past the first thousand-tier boundary so the digits
// roll smoothly rather than in big jumps.
void format_count_compact(uint64_t n, char *buf, size_t bufsz) {
  if (n >= 1000000000000ULL) {
    (void)snprintf(buf, bufsz, "%.2fT", (double)n / 1e12);
  } else if (n >= 1000000000ULL) {
    (void)snprintf(buf, bufsz, "%.2fB", (double)n / 1e9);
  } else if (n >= 1000000ULL) {
    (void)snprintf(buf, bufsz, "%.2fM", (double)n / 1e6);
  } else if (n >= 10000ULL) {
    (void)snprintf(buf, bufsz, "%lluK", n / 1000ULL);
  } else {
    (void)snprintf(buf, bufsz, "%llu", (unsigned long long)n);
  }
}
