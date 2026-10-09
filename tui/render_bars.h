#ifndef TUI_RENDER_BARS_H
#define TUI_RENDER_BARS_H

#include "game_state.h"
#include "render_layout.h"
#include "theme.h"
#include "tui_ui_types.h"
#include <notcurses/notcurses.h>
#include <stdint.h>

void render_command_bar(struct ncplane *plane, const Theme *theme,
                        const TuiGameState *state, const Layout *L,
                        TuiModalState modal, const char *modal_help);
void render_command_palette(struct ncplane *plane, const Theme *theme,
                            const TuiGameState *state, const Layout *L);
void render_pending_bar(struct ncplane *plane, const Theme *theme,
                        const TuiGameState *state, const Layout *L);
void render_status_bar(struct ncplane *plane, const Theme *theme,
                       const TuiGameState *state, const Layout *L,
                       TuiModalState modal);
void render_too_small(struct ncplane *plane, const Theme *theme);
// Debug / perf instrumentation (see MAGPIE_FPS_DEBUG in main.c). The UI
// thread records per-frame measurements; the status bar reads them back.
void tui_debug_record_frame_us(long frame_us);
void tui_debug_record_sprixel_stats(uint64_t emits, uint64_t elides);
void tui_debug_set_input_lag_us(long us);

#endif
