#ifndef TUI_RENDER_COMMON_H
#define TUI_RENDER_COMMON_H

#include "../src/ent/bonus_square.h"
#include "../src/ent/letter_distribution.h"
#include "config.h"
#include "theme.h"
#include <notcurses/notcurses.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ── Box drawing chars ─────────────────────────────────────────────────────
#define BOX_TL "\xe2\x94\x8c" // ┌
#define BOX_TR "\xe2\x94\x90" // ┐
#define BOX_BL "\xe2\x94\x94" // └
#define BOX_BR "\xe2\x94\x98" // ┘
#define BOX_HZ "\xe2\x94\x80" // ─
#define BOX_VT "\xe2\x94\x82" // │
// Double-line variants for the focused-panel border.
#define BOX2_TL "\xe2\x95\x94"     // ╔
#define BOX2_TR "\xe2\x95\x97"     // ╗
#define BOX2_BL "\xe2\x95\x9a"     // ╚
#define BOX2_BR "\xe2\x95\x9d"     // ╝
#define BOX2_HZ "\xe2\x95\x90"     // ═
#define BOX2_VT "\xe2\x95\x91"     // ║
#define BOX_T_DOWN "\xe2\x94\xac"  // ┬
#define BOX_T_UP "\xe2\x94\xb4"    // ┴
#define BOX_T_RIGHT "\xe2\x94\x9c" // ├
#define BOX_T_LEFT "\xe2\x94\xa4"  // ┤
#define BOX_CROSS "\xe2\x94\xbc"   // ┼
typedef struct {
  const char *glyph; // exactly 2 terminal columns wide
  ThemeRgb fg;
  ThemeRgb bg;
} PremiumMarker;

void draw_box(struct ncplane *plane, const Theme *theme, int top_row,
              int left_col, int height, int width, const char *title);
void draw_box_styled(struct ncplane *plane, const Theme *theme, int top_row,
                     int left_col, int height, int width, const char *title,
                     int hotkey, bool focused);
void draw_box_styled_ex(struct ncplane *plane, const Theme *theme, int top_row,
                        int left_col, int height, int width, const char *title,
                        int hotkey, bool focused, bool badge_secondary);
void format_alphagram_for_sort(const char *in, const LetterDistribution *ld,
                               TuiRackSort sort, char *out, size_t out_size);
void format_clock(int seconds, char *buf, size_t buf_size);
void format_count_compact(uint64_t n, char *buf, size_t bufsz);
const char *language_for_lexicon(const char *name);
PremiumMarker premium_marker_for_cell(const Theme *theme, BonusSquare bs,
                                      int row, int col, TuiPremiumLabels labels,
                                      int cell_w);
void render_move_styled(struct ncplane *plane, int row, int col,
                        const char *move_str, bool hide_parens,
                        bool hide_playthrough_parens);
// Alphagram-sort a rack-like input string according to the user's
// rack_sort preference (vowels-first, blanks-first, etc.). Output is
// the sorted form in a caller-owned buffer with a NUL terminator.
// Public form of the in-renderer helper so the annotation editor can
// canonicalize the rack buffer on focus-leave.
void tui_format_alphagram_for_sort(const char *in,
                                   const struct LetterDistribution *ld,
                                   TuiRackSort sort, char *out,
                                   size_t out_size);

#endif
