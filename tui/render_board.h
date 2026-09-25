#ifndef TUI_RENDER_BOARD_H
#define TUI_RENDER_BOARD_H

#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "config.h"
#include "game_state.h"
#include "render_layout.h"
#include "theme.h"
#include <notcurses/notcurses.h>
#include <stdbool.h>

// Destroy the 2x per-cell board tile planes and drop their cache.
void render_board_invalidate_tile_planes(void);
void render_board(struct ncplane *plane, const Theme *theme,
                  const TuiGameState *state, const Layout *L);
void render_board_grid_overlay(struct ncplane *parent, const Theme *theme,
                               const Layout *L, int thickness,
                               uint64_t render_version);
int tui_debug_last_tile_blits(void);
unsigned long tui_debug_tile_invalidations(void);
// Render just the board cells (no row/col labels) at (top, left). Each
// cell is 2 columns wide; the rendered region is BOARD_DIM rows tall and
// BOARD_DIM*2 columns wide. When the host terminal supports pixel
// graphics and `border_thickness` is positive, a grid overlay is drawn
// on top using a private cached child plane.
void tui_render_board_at(struct ncplane *plane, int top, int left,
                         const Theme *theme, const Game *game,
                         const LetterDistribution *ld, bool blank_uppercase,
                         TuiPremiumLabels premium_labels, int border_thickness);

// Mark the 2x board and label composites stale (next render re-blits).
void render_board_invalidate_blit_caches(void);

#endif
