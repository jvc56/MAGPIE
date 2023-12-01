#include "../board.h"
#include "../letter_distribution.h"

int traverse_backwards_for_score(const Board *board,
                                 const LetterDistribution *letter_distribution,
                                 int row, int col) {
  int score = 0;
  while (pos_exists(row, col)) {
    uint8_t ml = get_letter(board, row, col);
    if (ml == ALPHABET_EMPTY_SQUARE_MARKER) {
      break;
    }
    if (is_blanked(ml)) {
      score += letter_distribution->scores[BLANK_MACHINE_LETTER];
    } else {
      score += letter_distribution->scores[ml];
    }
    col--;
  }
  return score;
}