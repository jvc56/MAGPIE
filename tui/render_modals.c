#include "render_modals.h"

#include "../src/util/string_util.h"
#include "render_common.h"
#include "render_hit_test.h"
#include "render_layout.h"
#include "render_planes.h"
#include "time_picker.h"
#include <stdio.h>
#include <string.h>

// ── Modal helpers ─────────────────────────────────────────────────────────
//
// A modal is a centered box on top of the game frame. We render the items
// vertically with the focused row using accent_fg as a highlight stripe.

// shortcuts[i] is an optional right-aligned hint shown in a mid-grey
// next to items[i] (e.g. "N", "Esc"). Pass NULL to omit shortcuts
// entirely; individual entries may also be NULL/"" for items with no
// hint. The modal uses its own clinical-grey palette (theme->modal_*)
// rather than the game-content green/amber, so menus read as system
// chrome distinct from the game surface.
// An "input-field zone" decoration for a modal row: a fixed-width,
// right-anchored-within-the-row rectangle painted in a darker bg
// so the user can see exactly where typing lands. zone_starts[i]
// is the modal-local items[]-string offset of the zone's left
// edge; zone_widths[i] is the zone width in cells. Pass NULL for
// either to disable zones for the whole modal.
static void render_modal_ex(struct ncplane *plane, const Theme *theme,
                            const char *title, const char *const *items,
                            const char *const *shortcuts, const bool *disabled,
                            const int *cursor_cols, const int *zone_starts,
                            const int *zone_widths, int item_count, int focus,
                            int width) {
  TuiGridPlanes *planes = tui_grid_planes();
  TuiHitMaps *hit = tui_hit_maps();
  unsigned plane_rows = 0;
  unsigned plane_cols = 0;
  ncplane_dim_yx(plane, &plane_rows, &plane_cols);
  // Items now sit flush against the top/bottom borders — no blank
  // padding row under the title — so height is exactly 2 + items.
  const int height = 2 + item_count;
  // Plane is height+1 × width+1 so we can paint a 1-cell drop shadow
  // along the bottom row and right column. The shadow uses half-block
  // glyphs (▀ ▌) with transparent bg, so the row under and column
  // right of the modal show through except for the thin shadow strip
  // hugging the modal's edge.
  const int plane_h = height + 1;
  const int plane_w = width + 1;
  if ((unsigned)plane_w >= plane_cols || (unsigned)plane_h >= plane_rows) {
    return;
  }
  const int top = (int)(plane_rows - plane_h) / 2;
  const int left = (int)(plane_cols - plane_w) / 2;

  // Publish hit-test data for mouse-click handling. Items live at
  // rows [top+1 .. top+item_count] within columns [left+1 .. left+width-2]
  // (the 1-cell border on each side is non-clickable chrome). The
  // shadow row/col are not part of the clickable surface.
  hit->modal_hit_map.valid = true;
  hit->modal_hit_map.outer_top = top;
  hit->modal_hit_map.outer_bottom = top + height - 1;
  hit->modal_hit_map.outer_left = left;
  hit->modal_hit_map.outer_right = left + width - 1;
  hit->modal_hit_map.top = top + 1; // first item row
  hit->modal_hit_map.left = left + 1;
  hit->modal_hit_map.right = left + width - 2;
  hit->modal_hit_map.item_count =
      item_count < MODAL_MAX_ITEMS ? item_count : MODAL_MAX_ITEMS;
  for (int i = 0; i < hit->modal_hit_map.item_count; i++) {
    hit->modal_hit_map.disabled[i] = disabled != NULL && disabled[i];
    hit->modal_hit_map.left_chev_col[i] = -1;
    hit->modal_hit_map.right_chev_col[i] = -1;
    // Scan the item text for ◀ (E2 97 80) and ▶ (E2 96 B6).
    // Each chevron occupies 1 display column. Item text renders
    // starting at modal-interior col 3, so the screen column is
    // (left + 3 + display_offset).
    if (items != NULL && items[i] != NULL) {
      const unsigned char *s = (const unsigned char *)items[i];
      int disp = 0;
      while (*s != '\0') {
        if (s[0] == 0xe2 && s[1] == 0x97 && s[2] == 0x80) {
          hit->modal_hit_map.left_chev_col[i] = left + 3 + disp;
          s += 3;
          disp++;
        } else if (s[0] == 0xe2 && s[1] == 0x96 && s[2] == 0xb6) {
          hit->modal_hit_map.right_chev_col[i] = left + 3 + disp;
          s += 3;
          disp++;
        } else if (s[0] >= 0x80) {
          // Other multi-byte UTF-8 glyph; advance bytes by the
          // UTF-8 length and column by 1 (assumes BMP narrow,
          // which holds for the strings the modals build).
          int len = 1;
          if ((s[0] & 0xe0) == 0xc0) {
            len = 2;
          } else if ((s[0] & 0xf0) == 0xe0) {
            len = 3;
          } else if ((s[0] & 0xf8) == 0xf0) {
            len = 4;
          }
          s += len;
          disp++;
        } else {
          s++;
          disp++;
        }
      }
    }
  }

  // Modal lives on its own child plane that always sits on top of the
  // z-stack. Otherwise the 2x pixel composite (also a child of std)
  // sits above the modal, occluding it. Box-local coords run (0,0) to
  // (plane_h-1, plane_w-1); the modal proper occupies (0..height-1,
  // 0..width-1) and the shadow occupies (height, 1..width) plus
  // (1..height, width).
  if (planes->modal == NULL) {
    ncplane_options opts = {0};
    opts.y = top;
    opts.x = left;
    opts.rows = (unsigned)plane_h;
    opts.cols = (unsigned)plane_w;
    opts.name = "modal";
    planes->modal = ncplane_create(plane, &opts);
    if (planes->modal == NULL) {
      return;
    }
  } else {
    unsigned cur_rows = 0;
    unsigned cur_cols = 0;
    ncplane_dim_yx(planes->modal, &cur_rows, &cur_cols);
    if ((int)cur_rows != plane_h || (int)cur_cols != plane_w) {
      ncplane_resize_simple(planes->modal, (unsigned)plane_h,
                            (unsigned)plane_w);
    }
    ncplane_move_yx(planes->modal, top, left);
  }
  struct ncplane *mp = planes->modal;
  // Plane base is transparent: cells outside the modal proper and
  // outside the shadow strips let the underlying game frame show
  // through. The modal area fills explicitly below.
  uint64_t base_ch = 0;
  ncchannels_set_fg_alpha(&base_ch, NCALPHA_TRANSPARENT);
  ncchannels_set_bg_alpha(&base_ch, NCALPHA_TRANSPARENT);
  ncplane_set_base(mp, " ", 0, base_ch);
  ncplane_erase(mp);
  ncplane_move_top(mp);
  // Reset the plane's current channels to fully-opaque defaults. The
  // shadow-pass at the end of this function leaves bg alpha set to
  // TRANSPARENT, and notcurses' ncplane_set_{fg,bg}_rgb8 only touches
  // the RGB bits — without this reset, every frame after the first
  // would inherit the transparent bg from the previous shadow pass
  // and paint the modal interior as see-through.
  ncplane_set_channels(mp, 0);

  theme_apply_fg(mp, theme->modal_fg);
  theme_apply_bg(mp, theme->modal_bg);
  for (int r = 0; r < height; r++) {
    for (int c = 0; c < width; c++) {
      ncplane_putstr_yx(mp, r, c, " ");
    }
  }

  // Frame chrome: top/bottom rows + left/right columns paint with
  // modal_border_bg (a hair lighter than the interior modal_bg) so
  // the edge reads as a defined trim — the macOS-style hairline.
  // Box-drawing glyphs sit on this trim in modal_border_fg.
  const int right_col = width - 1;
  const int bottom_row = height - 1;
  theme_apply_fg(mp, theme->modal_border_fg);
  theme_apply_bg(mp, theme->modal_border_bg);
  ncplane_putstr_yx(mp, 0, 0, BOX_TL);
  for (int col = 1; col < right_col; col++) {
    ncplane_putstr_yx(mp, 0, col, BOX_HZ);
  }
  ncplane_putstr_yx(mp, 0, right_col, BOX_TR);
  for (int row = 1; row < bottom_row; row++) {
    ncplane_putstr_yx(mp, row, 0, BOX_VT);
    ncplane_putstr_yx(mp, row, right_col, BOX_VT);
  }
  ncplane_putstr_yx(mp, bottom_row, 0, BOX_BL);
  for (int col = 1; col < right_col; col++) {
    ncplane_putstr_yx(mp, bottom_row, col, BOX_HZ);
  }
  ncplane_putstr_yx(mp, bottom_row, right_col, BOX_BR);

  if (title != NULL && title[0] != '\0') {
    // Title sits on the top frame strip, so its bg is modal_border_bg
    // to match the surrounding chrome (otherwise the " Title " label
    // appears in a darker pocket cut out of the lighter strip).
    theme_apply_fg(mp, theme->modal_fg);
    theme_apply_bg(mp, theme->modal_border_bg);
    ncplane_putstr_yx(mp, 0, 2, " ");
    ncplane_putstr(mp, title);
    ncplane_putstr(mp, " ");
  }

  // Item rows. Layout per row:
  //   [ space ][ space ][ label ............... ][ shortcut ][ space ]
  // Focused row gets a full-width selection bar (modal_focus_bg) so
  // the highlight reads as a coherent strip, not just a colored label.
  for (int i = 0; i < item_count; i++) {
    const int item_row = 1 + i;
    const bool focused = (i == focus);
    const bool item_disabled = disabled != NULL && disabled[i];
    // Disabled items never use the focus highlight — they paint
    // dim text on the unfocused row background so they read as
    // "informational only, not selectable".
    const ThemeRgb row_fg = item_disabled ? theme->modal_shortcut_fg
                            : focused     ? theme->modal_focus_fg
                                          : theme->modal_fg;
    const ThemeRgb row_bg =
        (focused && !item_disabled) ? theme->modal_focus_bg : theme->modal_bg;
    const ThemeRgb shortcut_fg = focused && !item_disabled
                                     ? theme->modal_focus_fg
                                     : theme->modal_shortcut_fg;

    // Fill the row background (between the side borders).
    theme_apply_fg(mp, row_fg);
    theme_apply_bg(mp, row_bg);
    for (int c = 1; c <= right_col - 1; c++) {
      ncplane_putstr_yx(mp, item_row, c, " ");
    }

    // Per-row input-field zone (e.g. annotate-setup name field).
    // Paint a darker bg over the zone before the items text so
    // the editable region reads as a recessed input rectangle.
    const int z_start = (zone_starts != NULL) ? zone_starts[i] : -1;
    const int z_width = (zone_widths != NULL) ? zone_widths[i] : 0;
    const bool has_zone = z_start >= 0 && z_width > 0;
    if (has_zone) {
      theme_apply_fg(mp, row_fg);
      theme_apply_bg(mp, theme->bg);
      for (int z = 0; z < z_width; z++) {
        ncplane_putstr_yx(mp, item_row, 3 + z_start + z, " ");
      }
    }

    // Label / value text. When a zone is set we split the paint
    // so the zone keeps its darker bg: label region uses row_bg,
    // zone region uses theme->bg.
    if (items[i] != NULL) {
      if (has_zone) {
        const int items_len = (int)strlen(items[i]);
        // Region before the zone.
        if (z_start > 0 && z_start <= items_len) {
          char before[96];
          int n = z_start;
          if (n > (int)sizeof(before) - 1) {
            n = sizeof(before) - 1;
          }
          memcpy(before, items[i], (size_t)n);
          before[n] = '\0';
          theme_apply_fg(mp, row_fg);
          theme_apply_bg(mp, row_bg);
          ncplane_putstr_yx(mp, item_row, 3, before);
        }
        // Region inside the zone.
        if (z_start < items_len) {
          char inside[96];
          int end = z_start + z_width;
          if (end > items_len) {
            end = items_len;
          }
          int n = end - z_start;
          if (n > (int)sizeof(inside) - 1) {
            n = sizeof(inside) - 1;
          }
          memcpy(inside, items[i] + z_start, (size_t)n);
          inside[n] = '\0';
          theme_apply_fg(mp, row_fg);
          theme_apply_bg(mp, theme->bg);
          ncplane_putstr_yx(mp, item_row, 3 + z_start, inside);
        }
        // Region after the zone, if any.
        const int after_off = z_start + z_width;
        if (after_off < items_len) {
          theme_apply_fg(mp, row_fg);
          theme_apply_bg(mp, row_bg);
          ncplane_putstr_yx(mp, item_row, 3 + after_off, items[i] + after_off);
        }
      } else {
        ncplane_putstr_yx(mp, item_row, 3, items[i]);
      }
    }

    // Right-aligned shortcut hint, 2-space right padding.
    if (shortcuts != NULL && shortcuts[i] != NULL && shortcuts[i][0] != '\0') {
      const int sc_len = (int)strlen(shortcuts[i]);
      const int sc_col = right_col - 2 - sc_len + 1;
      if (sc_col >= 3) {
        theme_apply_fg(mp, shortcut_fg);
        theme_apply_bg(mp, row_bg);
        ncplane_putstr_yx(mp, item_row, sc_col, shortcuts[i]);
      }
    }

    // Optional block cursor for this row. cursor_cols[i] is the
    // BYTE OFFSET into items[i] where the caret sits (-1 = no
    // cursor on this row). The cell repaints with inverted
    // colors — when the caret sits inside a zone we invert the
    // zone's darker bg, otherwise we invert the row bg. When the
    // caret is past the end of the string we draw a space, same
    // color treatment.
    if (cursor_cols != NULL && items[i] != NULL && cursor_cols[i] >= 0) {
      const int items_len = (int)strlen(items[i]);
      const int co = cursor_cols[i];
      const int screen_col = 3 + co;
      if (screen_col >= 1 && screen_col <= right_col - 1) {
        char ch[2] = {' ', '\0'};
        if (co < items_len) {
          ch[0] = items[i][co];
        }
        const bool over_zone =
            has_zone && co >= z_start && co < z_start + z_width;
        const ThemeRgb cursor_fg = over_zone ? theme->bg : row_bg;
        // Invert: cursor cell bg = row_fg, cursor cell fg = the
        // bg we'd otherwise have at this cell.
        theme_apply_fg(mp, cursor_fg);
        theme_apply_bg(mp, row_fg);
        ncplane_set_styles(mp, NCSTYLE_BOLD);
        ncplane_putstr_yx(mp, item_row, screen_col, ch);
        ncplane_set_styles(mp, 0);
      }
    }
  }

  // Drop shadow: offset 1 cell right / 1 row down. Bottom strip uses
  // ▀ (upper half block) so only the half-row immediately touching
  // the modal renders shadow color; the lower half stays transparent
  // and lets whatever's underneath show through. Right strip uses ▌
  // (left half block) symmetrically. The shadow skips the top-left
  // corner cells so it visibly comes from a top-left light source.
  // Glyphs paint with bg-alpha transparent so the uncovered half-cell
  // composes with the game plane behind.
  {
    uint64_t shadow_ch = 0;
    ncchannels_set_fg_rgb8(&shadow_ch, theme->modal_shadow_fg.r,
                           theme->modal_shadow_fg.g, theme->modal_shadow_fg.b);
    ncchannels_set_bg_alpha(&shadow_ch, NCALPHA_TRANSPARENT);
    ncplane_set_channels(mp, shadow_ch);
    const int shadow_row = height; // first row past modal's bottom
    const int shadow_col = width;  // first col past modal's right
    // Half-cell offset shadow: the right strip's ▌ paints the LEFT
    // half of col=width, so its right edge sits at the middle of
    // that cell. For a sharp bottom-right corner the bottom strip
    // has to end at that same middle. Symmetrically on the left,
    // the bottom strip starts at the middle of col=0 (a half-cell
    // offset from the modal's left edge, implying light from
    // upper-left). Quadrant glyphs handle the two end caps:
    //   col=0:       ▝ (upper-right quadrant)  — right-half + top-half
    //   1..width-1:  ▀ (upper half, full width)
    //   col=width:   ▘ (upper-left quadrant)   — left-half + top-half
    ncplane_putstr_yx(mp, shadow_row, 0, "\xe2\x96\x9d"); // ▝
    for (int c = 1; c < shadow_col; c++) {
      ncplane_putstr_yx(mp, shadow_row, c, "\xe2\x96\x80"); // ▀
    }
    ncplane_putstr_yx(mp, shadow_row, shadow_col, "\xe2\x96\x98"); // ▘
    for (int r = 1; r < shadow_row; r++) {
      ncplane_putstr_yx(mp, r, shadow_col, "\xe2\x96\x8c"); // ▌
    }
  }
}
// Backwards-compatible wrapper: existing callers (menu, settings,
// pickers) never gray out items, so they pass NULL for the
// disabled mask and reach the same paint code path.
static void render_modal(struct ncplane *plane, const Theme *theme,
                         const char *title, const char *const *items,
                         const char *const *shortcuts, int item_count,
                         int focus, int width) {
  render_modal_ex(plane, theme, title, items, shortcuts, /*disabled=*/NULL,
                  /*cursor_cols=*/NULL, /*zone_starts=*/NULL,
                  /*zone_widths=*/NULL, item_count, focus, width);
}
// Forward declaration so tui_game_render_watch_setup can format
// adjustable rows using the same arrow-marker convention as the
// Settings modal. Defined a few hundred lines below.
static void format_setting_row(char *out, size_t out_size, const char *label,
                               const char *value, bool focused);
void tui_game_render_menu(struct ncplane *plane, const Theme *theme,
                          int focus) {
  if (plane == NULL || theme == NULL) {
    return;
  }
  const char *items[TUI_MENU_ITEM_COUNT];
  const char *shortcuts[TUI_MENU_ITEM_COUNT];
  items[TUI_MENU_NEW_GAME] = "New game";
  shortcuts[TUI_MENU_NEW_GAME] = "N";
  items[TUI_MENU_SETTINGS] = "Settings";
  shortcuts[TUI_MENU_SETTINGS] = "S";
  items[TUI_MENU_BACK] = "Back";
  shortcuts[TUI_MENU_BACK] = "Esc";
  items[TUI_MENU_QUIT] = "Quit";
  shortcuts[TUI_MENU_QUIT] = "Q";
  render_modal(plane, theme, "Menu", items, shortcuts, TUI_MENU_ITEM_COUNT,
               focus, 28);
}
void tui_game_render_startup_menu(struct ncplane *plane, const Theme *theme,
                                  int focus) {
  if (plane == NULL || theme == NULL) {
    return;
  }
  // Per-row buffers so we can append "(coming soon)" to unbuilt
  // modes without separate string literals for each variant.
  enum { ROW_BUF = 48 };
  static char buf[TUI_STARTUP_ITEM_COUNT][ROW_BUF];
  const char *items[TUI_STARTUP_ITEM_COUNT];
  const char *shortcuts[TUI_STARTUP_ITEM_COUNT];
  bool disabled[TUI_STARTUP_ITEM_COUNT];
  const char *labels[TUI_STARTUP_ITEM_COUNT] = {
      "Watch computer play",  "Load a position",           "Load a game",
      "Annotate a live game", "Play against the computer",
  };
  const char *shortcut_chars[TUI_STARTUP_ITEM_COUNT] = {"W", "P", "G", "A",
                                                        "C"};
  // All modes are wired up. Any future unbuilt mode would render
  // dimmed with a "(coming soon)" tag and the cursor would skip past
  // it — flip its entry to true to do so.
  const bool item_disabled[TUI_STARTUP_ITEM_COUNT] = {
      false, false, false, false, false,
  };
  for (int i = 0; i < TUI_STARTUP_ITEM_COUNT; i++) {
    if (item_disabled[i]) {
      snprintf(buf[i], ROW_BUF, "%s (coming soon)", labels[i]);
    } else {
      snprintf(buf[i], ROW_BUF, "%s", labels[i]);
    }
    items[i] = buf[i];
    shortcuts[i] = item_disabled[i] ? NULL : shortcut_chars[i];
    disabled[i] = item_disabled[i];
  }
  render_modal_ex(plane, theme, "MAGPIE", items, shortcuts, disabled,
                  /*cursor_cols=*/NULL, /*zone_starts=*/NULL,
                  /*zone_widths=*/NULL, TUI_STARTUP_ITEM_COUNT, focus, 44);
}
// Format a Watch-setup row as "Label" left-aligned + "value"
// right-aligned within `content_w` display columns. When `focused`
// is true, the value is wrapped in ◀ ▶ markers to signal that
// Left/Right arrows will adjust it. `content_w` is the cell width
// available between the modal's 2-space left padding and the
// right border padding — the caller picks a value that matches
// the modal's `width - 4`.
static void format_setup_row(char *out, size_t out_size, int content_w,
                             const char *label, const char *value,
                             bool focused) {
  // Display width of the value, including arrow decorations when
  // focused. Each arrow is one display column despite being 3
  // bytes of UTF-8 (◀ = U+25C0, ▶ = U+25B6).
  const int value_disp = (int)strlen(value);
  const int decorated_disp = focused ? value_disp + 4 : value_disp;
  const int label_disp = (int)strlen(label);
  int pad = content_w - label_disp - decorated_disp;
  if (pad < 1) {
    pad = 1;
  }
  if (focused) {
    snprintf(out, out_size, "%s%*s\xe2\x97\x80 %s \xe2\x96\xb6", label, pad, "",
             value);
  } else {
    snprintf(out, out_size, "%s%*s%s", label, pad, "", value);
  }
}
void tui_game_render_watch_setup(struct ncplane *plane, const Theme *theme,
                                 int focus, int time_seconds,
                                 const char *language, const char *lexicon,
                                 int sim_plies, int sim_candidates) {
  if (plane == NULL || theme == NULL) {
    return;
  }
  // Resolve the time-control display string from whichever preset
  // currently matches. Falls back to a "Ns" form so a custom value
  // (e.g., loaded from config) renders sensibly.
  const int preset_idx = tui_time_picker_closest_index(time_seconds);
  const char *time_label =
      tui_time_picker_preset_seconds(preset_idx) == time_seconds
          ? tui_time_picker_preset_label(preset_idx)
          : NULL;
  char time_value[24];
  if (time_label != NULL) {
    snprintf(time_value, sizeof(time_value), "%s", time_label);
  } else if (time_seconds <= 0) {
    snprintf(time_value, sizeof(time_value), "untimed");
  } else if (time_seconds % 60 == 0) {
    snprintf(time_value, sizeof(time_value), "%d min", time_seconds / 60);
  } else {
    snprintf(time_value, sizeof(time_value), "%ds", time_seconds);
  }

  // Modal width chosen to comfortably fit the widest row. "Sim
  // candidates" + 4 digits + ◀ ▶ markers needs ~30 cols of
  // content; 56 keeps the value column visually anchored to the
  // right edge for every row.
  enum { MODAL_WIDTH = 56, CONTENT_W = MODAL_WIDTH - 4, ROW_BUF = 96 };
  static char buf[TUI_WATCH_SETUP_ITEM_COUNT][ROW_BUF];
  const char *items[TUI_WATCH_SETUP_ITEM_COUNT];
  const bool focus_time = (focus == TUI_WATCH_SETUP_TIME);
  const bool focus_lang = (focus == TUI_WATCH_SETUP_LANGUAGE);
  const bool focus_lex = (focus == TUI_WATCH_SETUP_LEXICON);
  const bool focus_plies = (focus == TUI_WATCH_SETUP_SIM_PLIES);
  const bool focus_cands = (focus == TUI_WATCH_SETUP_SIM_CANDIDATES);
  format_setup_row(buf[TUI_WATCH_SETUP_TIME], ROW_BUF, CONTENT_W, "Time",
                   time_value, focus_time);
  format_setup_row(
      buf[TUI_WATCH_SETUP_LANGUAGE], ROW_BUF, CONTENT_W, "Language",
      language != NULL && language[0] != '\0' ? language : "(none)",
      focus_lang);
  format_setup_row(buf[TUI_WATCH_SETUP_LEXICON], ROW_BUF, CONTENT_W, "Lexicon",
                   lexicon != NULL && lexicon[0] != '\0' ? lexicon : "(none)",
                   focus_lex);
  char plies_str[8];
  snprintf(plies_str, sizeof(plies_str), "%d", sim_plies);
  format_setup_row(buf[TUI_WATCH_SETUP_SIM_PLIES], ROW_BUF, CONTENT_W,
                   "Sim plies", plies_str, focus_plies);
  char cands_str[8];
  snprintf(cands_str, sizeof(cands_str), "%d", sim_candidates);
  format_setup_row(buf[TUI_WATCH_SETUP_SIM_CANDIDATES], ROW_BUF, CONTENT_W,
                   "Sim candidates", cands_str, focus_cands);
  snprintf(buf[TUI_WATCH_SETUP_START], ROW_BUF, "Start game");
  for (int i = 0; i < TUI_WATCH_SETUP_ITEM_COUNT; i++) {
    items[i] = buf[i];
  }
  render_modal(plane, theme, "Watch setup", items, NULL,
               TUI_WATCH_SETUP_ITEM_COUNT, focus, MODAL_WIDTH);
}
// Format a row with a right-anchored fixed-width input zone.
// Layout:
//   [label][   space-padding   ][   input zone   ]
// The input zone is `zone_width` cells wide and ends at column
// content_w-1. The value (possibly empty) sits left-justified
// inside the zone, padded with spaces so the entire zone is
// covered by characters — the renderer paints a darker bg on
// the zone, and characters here keep that bg.
static void format_setup_text_row(char *out, size_t out_size, int content_w,
                                  int zone_width, const char *label,
                                  const char *value) {
  if (out_size == 0) {
    return;
  }
  for (size_t i = 0; i < out_size - 1; i++) {
    out[i] = ' ';
  }
  out[out_size - 1] = '\0';
  const int label_disp = label != NULL ? (int)strlen(label) : 0;
  int li = 0;
  while (label != NULL && label[li] != '\0' && li < content_w &&
         (size_t)li < out_size - 1) {
    out[li] = label[li];
    li++;
  }
  const int zone_start = content_w - zone_width;
  (void)label_disp;
  if (value != NULL && zone_width > 0 && zone_start >= 0) {
    const int max_chars = zone_width - 1; // leave a trailing cell for the
                                          // end-of-text caret
    for (int i = 0; value[i] != '\0' && i < max_chars &&
                    (size_t)(zone_start + i) < out_size - 1;
         i++) {
      out[zone_start + i] = value[i];
    }
  }
  if ((size_t)content_w < out_size) {
    out[content_w] = '\0';
  }
}
void tui_game_render_annotate_setup(struct ncplane *plane, const Theme *theme,
                                    int focus, const char *lexicon,
                                    const char *p1_name, const char *p2_name,
                                    int name_edit_pos) {
  if (plane == NULL || theme == NULL) {
    return;
  }
  enum {
    MODAL_WIDTH = 56,
    CONTENT_W = MODAL_WIDTH - 4,
    ROW_BUF = 96,
    // Right-anchored input zone for the player-name rows. Width
    // is the visual size of the rectangle the renderer paints
    // in a darker bg; the value sits left-justified inside, and
    // the trailing cell is reserved for the block cursor when
    // the caret is at end-of-text. 24 cells fits "PlayerNameHere "
    // — plenty for tournament-style nicknames.
    NAME_ZONE_W = 24,
    NAME_ZONE_START = CONTENT_W - NAME_ZONE_W,
  };
  static char buf[TUI_ANNOTATE_SETUP_ITEM_COUNT][ROW_BUF];
  const char *items[TUI_ANNOTATE_SETUP_ITEM_COUNT];
  int cursor_cols[TUI_ANNOTATE_SETUP_ITEM_COUNT];
  int zone_starts[TUI_ANNOTATE_SETUP_ITEM_COUNT];
  int zone_widths[TUI_ANNOTATE_SETUP_ITEM_COUNT];
  const bool focus_lex = (focus == TUI_ANNOTATE_SETUP_LEXICON);
  const bool focus_p1 = (focus == TUI_ANNOTATE_SETUP_P1_NAME);
  const bool focus_p2 = (focus == TUI_ANNOTATE_SETUP_P2_NAME);

  format_setup_row(
      buf[TUI_ANNOTATE_SETUP_LEXICON], ROW_BUF, CONTENT_W, "Lexicon",
      lexicon != NULL && lexicon[0] != '\0' ? lexicon : "(none)", focus_lex);
  format_setup_text_row(buf[TUI_ANNOTATE_SETUP_P1_NAME], ROW_BUF, CONTENT_W,
                        NAME_ZONE_W, "Player 1",
                        p1_name != NULL ? p1_name : "");
  format_setup_text_row(buf[TUI_ANNOTATE_SETUP_P2_NAME], ROW_BUF, CONTENT_W,
                        NAME_ZONE_W, "Player 2",
                        p2_name != NULL ? p2_name : "");
  snprintf(buf[TUI_ANNOTATE_SETUP_START], ROW_BUF, "Start");
  for (int i = 0; i < TUI_ANNOTATE_SETUP_ITEM_COUNT; i++) {
    items[i] = buf[i];
    cursor_cols[i] = -1;
    zone_starts[i] = -1;
    zone_widths[i] = 0;
  }
  // Both name rows always show the input zone (so the user can
  // see where typing will land before they focus the row). The
  // block cursor is only painted on the focused row.
  zone_starts[TUI_ANNOTATE_SETUP_P1_NAME] = NAME_ZONE_START;
  zone_widths[TUI_ANNOTATE_SETUP_P1_NAME] = NAME_ZONE_W;
  zone_starts[TUI_ANNOTATE_SETUP_P2_NAME] = NAME_ZONE_START;
  zone_widths[TUI_ANNOTATE_SETUP_P2_NAME] = NAME_ZONE_W;
  if (focus_p1) {
    cursor_cols[TUI_ANNOTATE_SETUP_P1_NAME] = NAME_ZONE_START + name_edit_pos;
  }
  if (focus_p2) {
    cursor_cols[TUI_ANNOTATE_SETUP_P2_NAME] = NAME_ZONE_START + name_edit_pos;
  }
  render_modal_ex(plane, theme, "Annotate setup", items, /*shortcuts=*/NULL,
                  /*disabled=*/NULL, cursor_cols, zone_starts, zone_widths,
                  TUI_ANNOTATE_SETUP_ITEM_COUNT, focus, MODAL_WIDTH);
}
// Render a load-style modal (text input + Enter-to-load + error
// line). Both Load-position (CGP) and Load-game (GCG) use this
// with just title + prompt differing.
static void render_load_text_modal(struct ncplane *plane, const Theme *theme,
                                   const char *title, const char *prompt,
                                   const char *buf, int cursor,
                                   const char *error);
void tui_game_render_load_position(struct ncplane *plane, const Theme *theme,
                                   const char *buf, int cursor,
                                   const char *error) {
  render_load_text_modal(plane, theme, " Load position ",
                         "Type or paste a CGP format position, or drag a .cgp "
                         "file to this window.",
                         buf, cursor, error);
}
void tui_game_render_load_game(struct ncplane *plane, const Theme *theme,
                               const char *buf, int cursor, const char *error) {
  render_load_text_modal(plane, theme, " Load game ",
                         "Drag a .gcg file to this window (or type its path).",
                         buf, cursor, error);
}
static void render_load_text_modal(struct ncplane *plane, const Theme *theme,
                                   const char *title, const char *prompt,
                                   const char *buf, int cursor,
                                   const char *error) {
  TuiGridPlanes *planes = tui_grid_planes();
  if (plane == NULL || theme == NULL) {
    return;
  }
  // Layout: prompt (1) + spacer (1) + INPUT_ROWS + spacer (1) +
  // Load button (1) + error line (1) inside the box, plus the
  // top/bottom borders. INPUT_ROWS includes a one-row pad below
  // the lowest line of text so the cursor doesn't sit flush
  // against the next row.
  enum {
    MODAL_WIDTH = 80,
    INPUT_ROWS = 8,
    INTERIOR_LEFT = 3,
  };
  const int interior_w = MODAL_WIDTH - 2 - INTERIOR_LEFT - 2;
  // Total content height: prompt (1) + blank (1) + INPUT_ROWS +
  // hint (1) + error (1) = INPUT_ROWS + 4.
  const int height = INPUT_ROWS + 4 + 2; // +2 for top/bottom borders
  const int width = MODAL_WIDTH;

  unsigned plane_rows = 0;
  unsigned plane_cols = 0;
  ncplane_dim_yx(plane, &plane_rows, &plane_cols);
  if ((unsigned)width >= plane_cols || (unsigned)height >= plane_rows) {
    return;
  }
  const int top = (int)(plane_rows - height) / 2;
  const int left = (int)(plane_cols - width) / 2;

  // Reuse the shared modal plane (created on first use by
  // render_modal_ex) — same z-order rules apply. Re-create if
  // not present and size to our dimensions.
  if (planes->modal == NULL) {
    ncplane_options opts = {0};
    opts.y = top;
    opts.x = left;
    opts.rows = (unsigned)height;
    opts.cols = (unsigned)width;
    opts.name = "modal";
    planes->modal = ncplane_create(plane, &opts);
    if (planes->modal == NULL) {
      return;
    }
  } else {
    unsigned cur_rows = 0;
    unsigned cur_cols = 0;
    ncplane_dim_yx(planes->modal, &cur_rows, &cur_cols);
    if ((int)cur_rows != height || (int)cur_cols != width) {
      ncplane_resize_simple(planes->modal, (unsigned)height, (unsigned)width);
    }
    ncplane_move_yx(planes->modal, top, left);
  }
  struct ncplane *mp = planes->modal;
  uint64_t base_ch = 0;
  ncchannels_set_fg_alpha(&base_ch, NCALPHA_TRANSPARENT);
  ncchannels_set_bg_alpha(&base_ch, NCALPHA_TRANSPARENT);
  ncplane_set_base(mp, " ", 0, base_ch);
  ncplane_erase(mp);
  ncplane_move_top(mp);
  ncplane_set_channels(mp, 0);

  // Background fill + border chrome.
  theme_apply_fg(mp, theme->modal_fg);
  theme_apply_bg(mp, theme->modal_bg);
  for (int r = 0; r < height; r++) {
    for (int c = 0; c < width; c++) {
      ncplane_putstr_yx(mp, r, c, " ");
    }
  }
  const int right_col = width - 1;
  const int bottom_row = height - 1;
  theme_apply_fg(mp, theme->modal_border_fg);
  theme_apply_bg(mp, theme->modal_border_bg);
  ncplane_putstr_yx(mp, 0, 0, BOX_TL);
  for (int col = 1; col < right_col; col++) {
    ncplane_putstr_yx(mp, 0, col, BOX_HZ);
  }
  ncplane_putstr_yx(mp, 0, right_col, BOX_TR);
  for (int row = 1; row < bottom_row; row++) {
    ncplane_putstr_yx(mp, row, 0, BOX_VT);
    ncplane_putstr_yx(mp, row, right_col, BOX_VT);
  }
  ncplane_putstr_yx(mp, bottom_row, 0, BOX_BL);
  for (int col = 1; col < right_col; col++) {
    ncplane_putstr_yx(mp, bottom_row, col, BOX_HZ);
  }
  ncplane_putstr_yx(mp, bottom_row, right_col, BOX_BR);

  // Title inset.
  theme_apply_fg(mp, theme->modal_fg);
  theme_apply_bg(mp, theme->modal_border_bg);
  ncplane_putstr_yx(mp, 0, 2, title != NULL ? title : " Load ");

  // Prompt row.
  theme_apply_fg(mp, theme->modal_shortcut_fg);
  theme_apply_bg(mp, theme->modal_bg);
  if (prompt != NULL) {
    ncplane_putstr_yx(mp, 1, INTERIOR_LEFT, prompt);
  }

  // Walk the buffer into (row, col) display coordinates.
  // For each character paint it within the input area; record
  // where the cursor lands so we can paint it inverted last.
  const int input_top = 3;
  const int input_left = INTERIOR_LEFT;
  int row_in = 0;
  int col_in = 0;
  int cursor_row = 0;
  int cursor_col = 0;
  theme_apply_fg(mp, theme->modal_fg);
  theme_apply_bg(mp, theme->modal_bg);
  for (int i = 0; buf != NULL && buf[i] != '\0'; i++) {
    if (i == cursor) {
      cursor_row = row_in;
      cursor_col = col_in;
    }
    const char ch = buf[i];
    if (ch == '\n') {
      row_in++;
      col_in = 0;
      continue;
    }
    if (row_in < INPUT_ROWS && col_in < interior_w) {
      char one[2] = {ch, '\0'};
      ncplane_putstr_yx(mp, input_top + row_in, input_left + col_in, one);
    }
    col_in++;
    if (col_in >= interior_w) {
      // Soft wrap so a long line keeps flowing into the next row.
      row_in++;
      col_in = 0;
    }
  }
  // Cursor at end-of-buffer case.
  if (buf == NULL || cursor >= (int)(buf != NULL ? strlen(buf) : 0)) {
    cursor_row = row_in;
    cursor_col = col_in;
  }
  // Render the cursor as an inverted cell so the user always
  // sees where the next inserted/deleted character will land.
  if (cursor_row < INPUT_ROWS) {
    const int cy = input_top + cursor_row;
    const int cx =
        input_left + (cursor_col < interior_w ? cursor_col : interior_w - 1);
    theme_apply_fg(mp, theme->modal_bg);
    theme_apply_bg(mp, theme->modal_fg);
    ncplane_putstr_yx(mp, cy, cx, " ");
  }

  // Hint row below the input area.
  const int hint_row = input_top + INPUT_ROWS;
  theme_apply_fg(mp, theme->modal_shortcut_fg);
  theme_apply_bg(mp, theme->modal_bg);
  ncplane_putstr_yx(mp, hint_row, INTERIOR_LEFT, "Enter: load  Esc: cancel");

  // Error line just below the hint, dim red.
  if (error != NULL && error[0] != '\0') {
    const int err_row = hint_row + 1;
    theme_apply_fg(mp, theme->error_fg);
    theme_apply_bg(mp, theme->modal_bg);
    char trunc[96];
    snprintf(trunc, sizeof(trunc), "%.*s", interior_w, error);
    ncplane_putstr_yx(mp, err_row, INTERIOR_LEFT, trunc);
  }
}
void tui_game_render_time_picker(struct ncplane *plane, const Theme *theme,
                                 int focus) {
  if (plane == NULL || theme == NULL) {
    return;
  }
  const int n = tui_time_picker_preset_count();
  // Format each row as "1 minute    ultra" — left-justified label
  // followed by a blurb. render_modal takes plain strings so we
  // pre-format into per-row buffers and pass pointers into items[].
  enum { ROW_BUF = 40 };
  static char buf[8][ROW_BUF];
  const char *items[8];
  const int rows = n < 8 ? n : 8;
  for (int i = 0; i < rows; i++) {
    snprintf(buf[i], ROW_BUF, "%-12s %s", tui_time_picker_preset_label(i),
             tui_time_picker_preset_blurb(i));
    items[i] = buf[i];
  }
  render_modal(plane, theme, "Time control", items, NULL, rows, focus, 28);
}
void tui_game_render_quit_confirm(struct ncplane *plane, const Theme *theme,
                                  int focus) {
  if (plane == NULL || theme == NULL) {
    return;
  }
  const char *items[2] = {"No", "Yes"};
  const char *shortcuts[2] = {"N", "Y"};
  render_modal(plane, theme, "Quit?", items, shortcuts, 2, focus, 24);
}
void tui_play_setup_enabled_rows(UiOvertimeRule overtime_rule, int time_seconds,
                                 UiChallengeRule challenge_rule,
                                 bool out_enabled[TUI_PLAY_SETUP_ITEM_COUNT]) {
  for (int item_idx = 0; item_idx < TUI_PLAY_SETUP_ITEM_COUNT; item_idx++) {
    out_enabled[item_idx] = true;
  }
  if (challenge_rule != UI_CHALLENGE_PENALTY) {
    out_enabled[TUI_PLAY_SETUP_CHALLENGE_PENALTY] = false;
  }
  if (time_seconds <= 0) {
    out_enabled[TUI_PLAY_SETUP_OVERTIME] = false;
    out_enabled[TUI_PLAY_SETUP_OVERTIME_CAP] = false;
    out_enabled[TUI_PLAY_SETUP_TIME_PENALTY] = false;
    return;
  }
  if (overtime_rule != UI_OVERTIME_MAX) {
    out_enabled[TUI_PLAY_SETUP_OVERTIME_CAP] = false;
  }
  if (overtime_rule == UI_OVERTIME_FLAG) {
    out_enabled[TUI_PLAY_SETUP_TIME_PENALTY] = false;
  }
}
void tui_game_render_play_setup(
    struct ncplane *plane, const Theme *theme, int focus,
    const char *human_name, const char *computer_name, int first_move,
    int name_edit_pos, int time_seconds, UiOvertimeRule overtime_rule,
    int overtime_cap_minutes, UiTimePenaltyRate time_penalty_rate,
    UiChallengeRule challenge_rule, UiChallengePenalty challenge_penalty,
    const char *language, const char *lexicon, int sim_plies,
    int sim_candidates) {
  if (plane == NULL || theme == NULL) {
    return;
  }
  enum {
    MODAL_WIDTH = 56,
    CONTENT_W = MODAL_WIDTH - 4,
    ROW_BUF = 96,
    NAME_ZONE_W = 24,
    NAME_ZONE_START = CONTENT_W - NAME_ZONE_W,
  };
  static char buf[TUI_PLAY_SETUP_ITEM_COUNT][ROW_BUF];
  const char *items[TUI_PLAY_SETUP_ITEM_COUNT];
  int cursor_cols[TUI_PLAY_SETUP_ITEM_COUNT];
  int zone_starts[TUI_PLAY_SETUP_ITEM_COUNT];
  int zone_widths[TUI_PLAY_SETUP_ITEM_COUNT];
  bool enabled[TUI_PLAY_SETUP_ITEM_COUNT];
  bool disabled[TUI_PLAY_SETUP_ITEM_COUNT];
  tui_play_setup_enabled_rows(overtime_rule, time_seconds, challenge_rule,
                              enabled);
  for (int item_idx = 0; item_idx < TUI_PLAY_SETUP_ITEM_COUNT; item_idx++) {
    disabled[item_idx] = !enabled[item_idx];
  }
  const bool focus_human = (focus == TUI_PLAY_SETUP_HUMAN_NAME);
  const bool focus_comp = (focus == TUI_PLAY_SETUP_COMPUTER_NAME);

  format_setup_text_row(buf[TUI_PLAY_SETUP_HUMAN_NAME], ROW_BUF, CONTENT_W,
                        NAME_ZONE_W, "Your name",
                        human_name != NULL ? human_name : "");
  format_setup_text_row(buf[TUI_PLAY_SETUP_COMPUTER_NAME], ROW_BUF, CONTENT_W,
                        NAME_ZONE_W, "Computer name",
                        computer_name != NULL ? computer_name : "");
  const char *first_value = first_move == TUI_PLAY_FIRST_HUMAN      ? "Human"
                            : first_move == TUI_PLAY_FIRST_COMPUTER ? "Computer"
                                                                    : "Random";
  format_setup_row(buf[TUI_PLAY_SETUP_FIRST_MOVE], ROW_BUF, CONTENT_W,
                   "First move", first_value,
                   focus == TUI_PLAY_SETUP_FIRST_MOVE);

  // Time control — same preset resolution as the Watch-setup modal.
  const int preset_idx = tui_time_picker_closest_index(time_seconds);
  const char *time_label =
      tui_time_picker_preset_seconds(preset_idx) == time_seconds
          ? tui_time_picker_preset_label(preset_idx)
          : NULL;
  char time_value[24];
  if (time_label != NULL) {
    snprintf(time_value, sizeof(time_value), "%s", time_label);
  } else if (time_seconds <= 0) {
    snprintf(time_value, sizeof(time_value), "untimed");
  } else if (time_seconds % 60 == 0) {
    snprintf(time_value, sizeof(time_value), "%d min", time_seconds / 60);
  } else {
    snprintf(time_value, sizeof(time_value), "%ds", time_seconds);
  }
  format_setup_row(buf[TUI_PLAY_SETUP_TIME], ROW_BUF, CONTENT_W, "Time",
                   time_value, focus == TUI_PLAY_SETUP_TIME);

  // Overtime rule + its dependents. Disabled rows render their value
  // dimmed without the ◀ ▶ adjusters (the cap only matters under
  // "max overtime"; penalties don't exist under "flag at 0:00").
  const char *overtime_value =
      overtime_rule == UI_OVERTIME_FLAG  ? "flag at 0:00"
      : overtime_rule == UI_OVERTIME_MAX ? "max overtime"
                                         : "unlimited";
  format_setup_row(buf[TUI_PLAY_SETUP_OVERTIME], ROW_BUF, CONTENT_W, "Overtime",
                   overtime_value,
                   focus == TUI_PLAY_SETUP_OVERTIME &&
                       enabled[TUI_PLAY_SETUP_OVERTIME]);
  // Disabled rows show a plain-ASCII "n/a" — format_setup_row pads by
  // byte length, so a multi-byte glyph (em dash) would right-align two
  // columns short.
  char cap_value[24];
  if (enabled[TUI_PLAY_SETUP_OVERTIME_CAP]) {
    snprintf(cap_value, sizeof(cap_value), "%d min", overtime_cap_minutes);
  } else {
    snprintf(cap_value, sizeof(cap_value), "n/a");
  }
  format_setup_row(buf[TUI_PLAY_SETUP_OVERTIME_CAP], ROW_BUF, CONTENT_W,
                   "Overtime cap", cap_value,
                   focus == TUI_PLAY_SETUP_OVERTIME_CAP &&
                       enabled[TUI_PLAY_SETUP_OVERTIME_CAP]);
  const char *penalty_value = "n/a";
  if (enabled[TUI_PLAY_SETUP_TIME_PENALTY]) {
    penalty_value = time_penalty_rate == UI_TIME_PENALTY_1_PER_SEC
                        ? "1 pt/sec"
                        : "10 pts/min";
  }
  format_setup_row(buf[TUI_PLAY_SETUP_TIME_PENALTY], ROW_BUF, CONTENT_W,
                   "Time penalty", penalty_value,
                   focus == TUI_PLAY_SETUP_TIME_PENALTY &&
                       enabled[TUI_PLAY_SETUP_TIME_PENALTY]);

  // Challenge rule + its penalty variant (the variant row only
  // applies under the "penalty" rule).
  const char *challenge_value =
      challenge_rule == UI_CHALLENGE_VOID     ? "void"
      : challenge_rule == UI_CHALLENGE_SINGLE ? "single"
      : challenge_rule == UI_CHALLENGE_DOUBLE ? "double"
                                              : "penalty";
  format_setup_row(buf[TUI_PLAY_SETUP_CHALLENGE], ROW_BUF, CONTENT_W,
                   "Challenge", challenge_value,
                   focus == TUI_PLAY_SETUP_CHALLENGE);
  const char *challenge_penalty_value = "n/a";
  if (enabled[TUI_PLAY_SETUP_CHALLENGE_PENALTY]) {
    challenge_penalty_value =
        challenge_penalty == UI_CHALLENGE_PENALTY_5_PER_PLAY    ? "5 pts/play"
        : challenge_penalty == UI_CHALLENGE_PENALTY_10_PER_PLAY ? "10 pts/play"
        : challenge_penalty == UI_CHALLENGE_PENALTY_5_PER_WORD  ? "5 pts/word"
                                                                : "10 pts/word";
  }
  format_setup_row(buf[TUI_PLAY_SETUP_CHALLENGE_PENALTY], ROW_BUF, CONTENT_W,
                   "Challenge penalty", challenge_penalty_value,
                   focus == TUI_PLAY_SETUP_CHALLENGE_PENALTY &&
                       enabled[TUI_PLAY_SETUP_CHALLENGE_PENALTY]);

  format_setup_row(buf[TUI_PLAY_SETUP_LANGUAGE], ROW_BUF, CONTENT_W, "Language",
                   language != NULL && language[0] != '\0' ? language
                                                           : "(none)",
                   focus == TUI_PLAY_SETUP_LANGUAGE);
  format_setup_row(buf[TUI_PLAY_SETUP_LEXICON], ROW_BUF, CONTENT_W, "Lexicon",
                   lexicon != NULL && lexicon[0] != '\0' ? lexicon : "(none)",
                   focus == TUI_PLAY_SETUP_LEXICON);
  char plies_str[8];
  snprintf(plies_str, sizeof(plies_str), "%d", sim_plies);
  format_setup_row(buf[TUI_PLAY_SETUP_SIM_PLIES], ROW_BUF, CONTENT_W,
                   "Sim plies", plies_str, focus == TUI_PLAY_SETUP_SIM_PLIES);
  char cands_str[8];
  snprintf(cands_str, sizeof(cands_str), "%d", sim_candidates);
  format_setup_row(buf[TUI_PLAY_SETUP_SIM_CANDIDATES], ROW_BUF, CONTENT_W,
                   "Sim candidates", cands_str,
                   focus == TUI_PLAY_SETUP_SIM_CANDIDATES);
  snprintf(buf[TUI_PLAY_SETUP_START], ROW_BUF, "Start");
  for (int i = 0; i < TUI_PLAY_SETUP_ITEM_COUNT; i++) {
    items[i] = buf[i];
    cursor_cols[i] = -1;
    zone_starts[i] = -1;
    zone_widths[i] = 0;
  }
  zone_starts[TUI_PLAY_SETUP_HUMAN_NAME] = NAME_ZONE_START;
  zone_widths[TUI_PLAY_SETUP_HUMAN_NAME] = NAME_ZONE_W;
  zone_starts[TUI_PLAY_SETUP_COMPUTER_NAME] = NAME_ZONE_START;
  zone_widths[TUI_PLAY_SETUP_COMPUTER_NAME] = NAME_ZONE_W;
  if (focus_human) {
    cursor_cols[TUI_PLAY_SETUP_HUMAN_NAME] = NAME_ZONE_START + name_edit_pos;
  }
  if (focus_comp) {
    cursor_cols[TUI_PLAY_SETUP_COMPUTER_NAME] = NAME_ZONE_START + name_edit_pos;
  }
  render_modal_ex(plane, theme, "Play vs computer", items, /*shortcuts=*/NULL,
                  disabled, cursor_cols, zone_starts, zone_widths,
                  TUI_PLAY_SETUP_ITEM_COUNT, focus, MODAL_WIDTH);
}
// Helper for an arrow-adjusted Settings row. Renders
//   "<label>   ◀ <value> ▶"   when focused
//   "<label>   <value>"       when not focused
// `value` may be a fixed string (e.g., "lowercase") or numeric.
static void format_setting_row(char *out, size_t out_size, const char *label,
                               const char *value, bool focused) {
  if (focused) {
    snprintf(out, out_size, "%-13s\xe2\x97\x80 %s \xe2\x96\xb6", label, value);
  } else {
    snprintf(out, out_size, "%-13s%s", label, value);
  }
}
static const char *premium_labels_value(TuiPremiumLabels labels) {
  switch (labels) {
  case TUI_PREMIUM_LABELS_LOWERCASE:
    return "lowercase";
  case TUI_PREMIUM_LABELS_PUNCT:
    return "punctuation";
  case TUI_PREMIUM_LABELS_NONE:
    return "none";
  case TUI_PREMIUM_LABELS_UPPERCASE:
  case TUI_PREMIUM_LABELS_COUNT:
  default:
    return "uppercase";
  }
}
static const char *score_subscripts_value(TuiScoreSubscripts mode) {
  switch (mode) {
  case TUI_SCORE_SUBSCRIPTS_NONZERO:
    return "nonzero";
  case TUI_SCORE_SUBSCRIPTS_ALL:
    return "all";
  case TUI_SCORE_SUBSCRIPTS_OFF:
  case TUI_SCORE_SUBSCRIPTS_COUNT:
  default:
    return "off";
  }
}
// Display label for a rack-sort enum value. Concise on purpose so it
// fits in the right-aligned value column of the Settings modal:
//   "?+alpha" / "alpha+?" / "?+vow+con" / "vow+con+?"
// Leading "?+" means blanks come first; the rest is the letter
// ordering ("alpha" = alphabetical, "vow+con" = vowels then
// consonants).
static const char *rack_sort_value(TuiRackSort sort) {
  switch (sort) {
  case TUI_RACK_SORT_BLANKS_ALPHA:
    return "?+alpha";
  case TUI_RACK_SORT_BLANKS_VOWELS:
    return "?+vow+con";
  case TUI_RACK_SORT_VOWELS:
    return "vow+con+?";
  case TUI_RACK_SORT_ALPHA:
  case TUI_RACK_SORT_COUNT:
  default:
    return "alpha+?";
  }
}
void tui_game_render_settings(struct ncplane *plane, const Theme *theme,
                              int focus, int board_scale, bool antialias,
                              TuiScoreSubscripts score_subscripts,
                              int border_thickness, bool pixel_supported,
                              bool font_available,
                              TuiPremiumLabels premium_labels,
                              bool blank_uppercase, TuiRackSort rack_sort,
                              const char *lexicon, bool load_rit) {
  if (plane == NULL || theme == NULL) {
    return;
  }
  // Lexicon row has been removed — lexicon is set only via the
  // New Game / Watch setup flow. Keep the param for signature
  // stability with existing callers.
  (void)lexicon;

  // Scale row. 2x needs both pixel graphics and a loaded font; if
  // either is missing, the row reports unavailable and arrow keys
  // become no-ops at this focus. Even when 2x is supported the
  // terminal may currently be too small to fit 2x cells — in that
  // case we still show the preference (the user may want to set 2x
  // and resize) but flag that it can't render right now.
  char scale_label[96];
  const bool scale_available = pixel_supported && font_available;
  if (!scale_available) {
    snprintf(scale_label, sizeof(scale_label), "Scale        unsupported here");
  } else {
    unsigned plane_rows = 0;
    unsigned plane_cols = 0;
    ncplane_dim_yx(plane, &plane_rows, &plane_cols);
    const bool layout_fits_2x =
        compute_effective_scale(2, plane_cols, plane_rows) >= 2;
    char value_buf[32];
    if (board_scale >= 2 && !layout_fits_2x) {
      // The setting stays editable so the user can step back to 1x
      // without resizing first, but the value spells out why the
      // board is still rendering as 1x.
      snprintf(value_buf, sizeof(value_buf), "2x \xc2\xb7 too small");
    } else {
      snprintf(value_buf, sizeof(value_buf), "%dx", board_scale);
    }
    format_setting_row(scale_label, sizeof(scale_label), "Scale", value_buf,
                       focus == TUI_SETTINGS_SCALE);
  }

  // Antialiasing row — only meaningful when 2x is engaged.
  char aa_label[96];
  if (!scale_available || board_scale < 2) {
    snprintf(aa_label, sizeof(aa_label), "Antialias    n/a at 1x");
  } else {
    format_setting_row(aa_label, sizeof(aa_label), "Antialias",
                       antialias ? "on" : "off", focus == TUI_SETTINGS_AA);
  }

  // Score subscripts row — also 2x-only.
  char sub_label[96];
  if (!scale_available || board_scale < 2) {
    snprintf(sub_label, sizeof(sub_label), "Subscript    n/a at 1x");
  } else {
    format_setting_row(sub_label, sizeof(sub_label), "Subscript",
                       score_subscripts_value(score_subscripts),
                       focus == TUI_SETTINGS_SUBSCRIPTS);
  }

  // Border row.
  char border_label[96];
  if (!pixel_supported) {
    snprintf(border_label, sizeof(border_label),
             "Border       unsupported here");
  } else {
    char value_buf[16];
    if (border_thickness <= 0) {
      snprintf(value_buf, sizeof(value_buf), "off");
    } else {
      snprintf(value_buf, sizeof(value_buf), "%dpx", border_thickness);
    }
    format_setting_row(border_label, sizeof(border_label), "Border", value_buf,
                       focus == TUI_SETTINGS_BORDER);
  }

  // Premium label row.
  char premium_label[96];
  format_setting_row(premium_label, sizeof(premium_label), "Premium",
                     premium_labels_value(premium_labels),
                     focus == TUI_SETTINGS_PREMIUM);

  // Blanks row.
  char blanks_label[96];
  format_setting_row(blanks_label, sizeof(blanks_label), "Blanks",
                     blank_uppercase ? "uppercase" : "lowercase",
                     focus == TUI_SETTINGS_BLANKS);

  // Rack-sort row.
  char rack_sort_label[96];
  format_setting_row(rack_sort_label, sizeof(rack_sort_label), "Rack sort",
                     rack_sort_value(rack_sort),
                     focus == TUI_SETTINGS_RACK_SORT);

  // RIT row. Plain on/off arrow toggle like Antialias.
  char rit_label[96];
  format_setting_row(rit_label, sizeof(rit_label), "RIT",
                     load_rit ? "on" : "off", focus == TUI_SETTINGS_RIT);

  // Antialias / Subscript / Border are only meaningful at 2x — hide
  // them entirely when the board isn't rendering at 2x rather than
  // showing greyed "n/a at 1x" placeholders. settings_visible() in
  // main.c mirrors this so arrow-key navigation skips them.
  const bool effective_2x = scale_available && board_scale >= 2;
  const char *items[TUI_SETTINGS_ITEM_COUNT];
  int n = 0;
  int display_focus = 0;
  // Walk enum order; append a row if visible, and translate the
  // caller's enum-valued focus into the corresponding display index.
  for (int idx = 0; idx < TUI_SETTINGS_ITEM_COUNT; idx++) {
    const bool is_2x_only =
        (idx == TUI_SETTINGS_AA || idx == TUI_SETTINGS_SUBSCRIPTS ||
         idx == TUI_SETTINGS_BORDER);
    if (is_2x_only && !effective_2x) {
      continue;
    }
    const char *label = NULL;
    switch (idx) {
    case TUI_SETTINGS_SCALE:
      label = scale_label;
      break;
    case TUI_SETTINGS_AA:
      label = aa_label;
      break;
    case TUI_SETTINGS_SUBSCRIPTS:
      label = sub_label;
      break;
    case TUI_SETTINGS_BORDER:
      label = border_label;
      break;
    case TUI_SETTINGS_PREMIUM:
      label = premium_label;
      break;
    case TUI_SETTINGS_BLANKS:
      label = blanks_label;
      break;
    case TUI_SETTINGS_RACK_SORT:
      label = rack_sort_label;
      break;
    case TUI_SETTINGS_RIT:
      label = rit_label;
      break;
    case TUI_SETTINGS_BACK:
      label = "Back";
      break;
    default:
      continue;
    }
    if (idx == focus) {
      display_focus = n;
    }
    items[n++] = label;
  }
  render_modal(plane, theme, "Settings", items, NULL, n, display_focus, 40);
}
