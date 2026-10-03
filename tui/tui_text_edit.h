#ifndef TUI_TUI_TEXT_EDIT_H
#define TUI_TUI_TEXT_EDIT_H

#include <notcurses/notcurses.h>
#include <stdbool.h>
#include <stdint.h>

bool tui_text_readline_key(uint32_t key, char *buf, int *cursor, int *len,
                           bool *dirty);

#endif
