#include "tui_text_edit.h"

#include <string.h>

// Apply readline / emacs-style cursor and kill bindings to a
// multi-line text buffer. Returns true if the key was consumed,
// in which case the caller should continue past the rest of its
// input handling. "Line" means visual newline-bounded segment —
// for a single-line buffer (CGP) Ctrl-A goes to buffer start and
// Ctrl-E to buffer end. *dirty flips to true on any edit so the
// caller's live-parse trigger fires.
bool tui_text_readline_key(uint32_t key, char *buf, int *cursor, int *len,
                           bool *dirty) {
  switch (key) {
  case 0x01: { // Ctrl-A: beginning of current line
    int c = *cursor;
    while (c > 0 && buf[c - 1] != '\n') {
      c--;
    }
    *cursor = c;
    return true;
  }
  case 0x05: { // Ctrl-E: end of current line
    int c = *cursor;
    while (c < *len && buf[c] != '\n') {
      c++;
    }
    *cursor = c;
    return true;
  }
  case 0x02: // Ctrl-B: back one char
    if (*cursor > 0) {
      (*cursor)--;
    }
    return true;
  case 0x06: // Ctrl-F: forward one char
    if (*cursor < *len) {
      (*cursor)++;
    }
    return true;
  case 0x04: // Ctrl-D: delete forward
    if (*cursor < *len) {
      memmove(&buf[*cursor], &buf[*cursor + 1], (size_t)(*len - *cursor));
      (*len)--;
      *dirty = true;
    }
    return true;
  case 0x0b: { // Ctrl-K: kill from cursor to end of line
    int end = *cursor;
    while (end < *len && buf[end] != '\n') {
      end++;
    }
    if (end > *cursor) {
      memmove(&buf[*cursor], &buf[end], (size_t)(*len - end + 1));
      *len -= (end - *cursor);
      *dirty = true;
    }
    return true;
  }
  case 0x15: { // Ctrl-U: kill from beginning of line to cursor
    int beg = *cursor;
    while (beg > 0 && buf[beg - 1] != '\n') {
      beg--;
    }
    if (beg < *cursor) {
      const int drop = *cursor - beg;
      memmove(&buf[beg], &buf[*cursor], (size_t)(*len - *cursor + 1));
      *len -= drop;
      *cursor = beg;
      *dirty = true;
    }
    return true;
  }
  case 0x17: { // Ctrl-W: delete word backward
    int end = *cursor;
    int beg = end;
    while (beg > 0 && (buf[beg - 1] == ' ' || buf[beg - 1] == '\t' ||
                       buf[beg - 1] == '\n')) {
      beg--;
    }
    while (beg > 0 && buf[beg - 1] != ' ' && buf[beg - 1] != '\t' &&
           buf[beg - 1] != '\n') {
      beg--;
    }
    if (beg < end) {
      memmove(&buf[beg], &buf[end], (size_t)(*len - end + 1));
      *len -= (end - beg);
      *cursor = beg;
      *dirty = true;
    }
    return true;
  }
  default:
    return false;
  }
}
