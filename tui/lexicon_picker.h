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

// Opaque list of lexica discovered under data/lexica/. Populated by
// tui_lexicon_list_load; freed by tui_lexicon_list_destroy. Used by
// the modal-style picker (separate from the full-screen startup
// picker above) so main.c can drive the picker's input loop.
typedef struct LexiconList LexiconList;

// Allocates and populates a list of lexica from data/lexica/. Returns
// NULL if no lexica were found.
LexiconList *tui_lexicon_list_load(void);
void tui_lexicon_list_destroy(LexiconList *list);

// Returns the number of entries (lexica) in the list.
int tui_lexicon_list_count(const LexiconList *list);

// Returns the index of the entry whose name matches `name`, or -1 if
// not found. Case-sensitive.
int tui_lexicon_list_find(const LexiconList *list, const char *name);

// Writes the name of entry `idx` to `out_buf`. Returns true on
// success, false if `idx` is out of range.
bool tui_lexicon_list_name(const LexiconList *list, int idx, char *out_buf,
                           size_t out_buf_size);

// Writes the display name of the language that owns entry `idx`
// (e.g. "English", "French", "Other"). Returns true on success.
bool tui_lexicon_list_language_name(const LexiconList *list, int idx,
                                    char *out_buf, size_t out_buf_size);

// Returns the index of the next/previous lexicon within the same
// language as entry `idx`. `dir` is +1 (next) or -1 (prev). Returns
// `idx` itself when no more entries exist in that direction (so the
// caller naturally clamps at the language's boundary).
int tui_lexicon_list_step_same_language(const LexiconList *list, int idx,
                                        int dir);

// Returns the index of the first lexicon in the next/previous
// language group from entry `idx`. `dir` is +1 (next language) or
// -1 (prev language). Returns `idx` itself if no further language
// exists in that direction. Entries are sorted so that all lexica
// belonging to the same language are contiguous.
int tui_lexicon_list_step_language(const LexiconList *list, int idx, int dir);

// Renders the modal-style lexicon picker into the shared modal plane.
// `focus_idx` is the lexicon index (not display row) that's selected.
// Only the language containing `focus_idx` is expanded; other
// languages render as a single collapsed header row.
void tui_game_render_lexicon_picker(struct ncplane *plane, const Theme *theme,
                                    const LexiconList *list, int focus_idx);

#endif
