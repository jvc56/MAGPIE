#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "log.h"
#include "util.h"
#include "words.h"

FormedWords *words_played(Board *board, uint8_t word[], int word_start_index,
                          int word_end_index, int row, int col, int vertical) {

  if (vertical) {
    transpose(board);
    int ph = col;
    col = row;
    row = ph;
  }

  FormedWords *ws = malloc_or_die(sizeof(FormedWords));
  int formed_words_idx = 0;
  uint8_t main_word[BOARD_DIM];
  int main_word_idx = 0;
  for (int idx = 0; idx < word_end_index - word_start_index + 1; idx++) {
    uint8_t ml = word[idx + word_start_index];
    int fresh_tile = 0;

    if (ml == PLAYED_THROUGH_MARKER) {
      ml = get_letter(board, row, col + idx);
    } else {
      fresh_tile = 1;
    }
    ml = get_unblanked_machine_letter(ml);
    main_word[main_word_idx] = ml;
    main_word_idx++;

    int actual_cross_word =
        (row > 0 && !is_empty(board, row - 1, col + idx)) ||
        ((row < BOARD_DIM - 1) && !is_empty(board, row + 1, col + idx));

    if (fresh_tile && actual_cross_word) {
      // Search for a word
      int rbegin, rend;
      for (rbegin = row - 1; rbegin >= 0; rbegin--) {
        if (is_empty(board, rbegin, col + idx)) {
          rbegin++;
          break;
        }
      }
      if (rbegin < 0) {
        rbegin = 0;
      }

      for (rend = rbegin; rend < BOARD_DIM; rend++) {
        if (rend != row && is_empty(board, rend, col + idx)) {
          rend--;
          break;
        }
      }
      int widx = 0;
      ws->words[formed_words_idx].word_length = rend - rbegin + 1;
      ws->words[formed_words_idx].valid = 0; // we don't know validity yet.
      for (int r = rbegin; r <= rend; r++) {
        if (r != row) {
          uint8_t lt =
              get_unblanked_machine_letter(get_letter(board, r, col + idx));
          ws->words[formed_words_idx].word[widx] = lt;
        } else {
          ws->words[formed_words_idx].word[widx] = ml;
        }
        widx++;
      }
      formed_words_idx++;
    }
  }

  ws->words[formed_words_idx].word_length = main_word_idx;
  memory_copy(ws->words[formed_words_idx].word, main_word, main_word_idx);
  formed_words_idx++;
  ws->num_words = formed_words_idx;

  if (vertical) {
    transpose(board);
  }

  return ws;
}

int is_word_valid(FormedWord *w, KWG *kwg) {
  if (w->word_length < 2) {
    return 0;
  }

  int lidx = 0;
  int node_idx = kwg_arc_index(kwg, 0);
  do {
    if (lidx > w->word_length - 1) {
      // if we've gone too far the word is not found
      return 0;
    }
    uint8_t ml = w->word[lidx];
    if (kwg_tile(kwg, node_idx) == ml) {
      if (lidx == w->word_length - 1) {
        return kwg_accepts(kwg, node_idx);
      }
      node_idx = kwg_arc_index(kwg, node_idx);
      lidx++;
    } else {
      if (kwg_is_end(kwg, node_idx)) {
        return 0;
      }
      node_idx++;
    }
  } while (1);
}

void populate_word_validities(FormedWords *ws, KWG *kwg) {
  for (int i = 0; i < ws->num_words; i++) {
    ws->words[i].valid = is_word_valid(&ws->words[i], kwg);
  }
}