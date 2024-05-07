#include "words.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "../def/board_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/rack_defs.h"

#include "../ent/rack.h"

#include "../util/string_util.h"
#include "../util/util.h"

#include "board.h"
#include "kwg_alpha.h"
#include "letter_distribution.h"

typedef struct FormedWord {
  uint8_t word[BOARD_DIM];
  int word_length;
  bool valid;
} FormedWord;

struct FormedWords {
  int num_words;
  FormedWord words[RACK_SIZE + 1]; // max number of words we can form
};

FormedWords *formed_words_create(Board *board, Move *move) {
  int tiles_length = move_get_tiles_length(move);
  int row_start = move_get_row_start(move);
  int col_start = move_get_col_start(move);
  int dir = move_get_dir(move);

  bool board_was_transposed = false;

  if (!board_matches_dir(board, dir)) {
    board_transpose(board);
    board_was_transposed = true;
    int ph = col_start;
    col_start = row_start;
    row_start = ph;
  }

  FormedWords *ws = malloc_or_die(sizeof(FormedWords));
  int formed_words_idx = 0;
  uint8_t main_word[BOARD_DIM];
  int main_word_idx = 0;
  for (int idx = 0; idx < tiles_length; idx++) {
    uint8_t ml = move_get_tile(move, idx);
    bool fresh_tile = false;

    if (ml == PLAYED_THROUGH_MARKER) {
      ml = board_get_letter(board, row_start, col_start + idx);
    } else {
      fresh_tile = true;
    }
    ml = get_unblanked_machine_letter(ml);
    main_word[main_word_idx] = ml;
    main_word_idx++;

    bool actual_cross_word =
        (row_start > 0 &&
         !board_is_empty(board, row_start - 1, col_start + idx)) ||
        ((row_start < BOARD_DIM - 1) &&
         !board_is_empty(board, row_start + 1, col_start + idx));

    if (fresh_tile && actual_cross_word) {
      // Search for a word
      int rbegin, rend;
      for (rbegin = row_start - 1; rbegin >= 0; rbegin--) {
        if (board_is_empty(board, rbegin, col_start + idx)) {
          rbegin++;
          break;
        }
      }
      if (rbegin < 0) {
        rbegin = 0;
      }

      for (rend = rbegin; rend < BOARD_DIM; rend++) {
        if (rend != row_start && board_is_empty(board, rend, col_start + idx)) {
          rend--;
          break;
        }
      }
      int widx = 0;
      ws->words[formed_words_idx].word_length = rend - rbegin + 1;
      ws->words[formed_words_idx].valid = false; // we don't know validity yet.
      for (int r = rbegin; r <= rend; r++) {
        if (r != row_start) {
          uint8_t lt = get_unblanked_machine_letter(
              board_get_letter(board, r, col_start + idx));
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

  if (board_was_transposed) {
    board_transpose(board);
  }

  return ws;
}

void formed_words_destroy(FormedWords *fw) {
  if (!fw) {
    return;
  }
  free(fw);
}

int formed_words_get_num_words(const FormedWords *fw) { return fw->num_words; }

const uint8_t *formed_words_get_word(const FormedWords *fw, int word_index) {
  return fw->words[word_index].word;
}

int formed_words_get_word_length(const FormedWords *fw, int word_index) {
  return fw->words[word_index].word_length;
}

int formed_words_get_word_valid(const FormedWords *fw, int word_index) {
  return fw->words[word_index].valid;
}

int formed_words_get_word_letter(const FormedWords *fw, int word_index,
                                 int letter_index) {
  return fw->words[word_index].word[letter_index];
}

bool is_word_valid_standard(const FormedWord *w, const KWG *kwg) {
  int lidx = 0;
  uint32_t node_idx = kwg_get_dawg_root_node_index(kwg);
  uint32_t node = kwg_node(kwg, node_idx);
  do {
    if (lidx > w->word_length - 1) {
      // if we've gone too far the word is not found
      return false;
    }
    uint8_t ml = w->word[lidx];
    if (kwg_node_tile(node) == ml) {
      if (lidx == w->word_length - 1) {
        return kwg_node_accepts(node);
      }
      node_idx = kwg_node_arc_index_prefetch(node, kwg);
      node = kwg_node(kwg, node_idx);
      lidx++;
    } else {
      if (kwg_node_is_end(node)) {
        return false;
      }
      node_idx++;
      node = kwg_node(kwg, node_idx);
    }
  } while (1);
}

bool is_word_valid_alpha(const FormedWord *w, const KWG *kwg) {
  Rack *rack = rack_create(MAX_ALPHABET_SIZE);
  for (int i = 0; i < w->word_length; i++) {
    rack_add_letter(rack, w->word[i]);
  }
  bool is_valid = kwg_accepts_alpha(kwg, rack);
  rack_destroy(rack);
  return is_valid;
}

bool is_word_valid(const FormedWord *w, const KWG *kwg, bool is_wordsmog) {
  if (w->word_length < 2) {
    return false;
  }
  if (is_wordsmog) {
    return is_word_valid_alpha(w, kwg);
  } else {
    return is_word_valid_standard(w, kwg);
  }
}

void formed_words_populate_validities(const KWG *kwg, FormedWords *ws,
                                      bool is_wordsmog) {
  for (int i = 0; i < ws->num_words; i++) {
    ws->words[i].valid = is_word_valid(&ws->words[i], kwg, is_wordsmog);
  }
}