#ifndef TUI_LEXICON_PICKER_H
#define TUI_LEXICON_PICKER_H

#include "theme.h"
#include <notcurses/notcurses.h>
#include <stdbool.h>
#include <stddef.h>

// Renders a lexicon picker by scanning data/lexica/*.kwg in the current
// working directory. The chosen lexicon basename (e.g. "CSW21") is written
// to out_buf (NUL-terminated). Returns true on confirm, false on cancel
// or if no lexica are available.
//
// `initial` is the lexicon to focus when opening; pass NULL or "" to focus
// the first entry.
bool tui_lexicon_picker_run(struct notcurses *nc, const Theme *theme,
                            const char *initial, char *out_buf,
                            size_t out_buf_size);

#endif
