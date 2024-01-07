#ifndef WORD_PRUNE_H
#define WORD_PRUNE_H

#include "board.h"
#include "constants.h"
#include "game.h"
#include "kwg.h"
#include "rack.h"

typedef struct PossibleWord {
  uint8_t word[(BOARD_DIM)];
  int word_length;
} PossibleWord;

typedef struct PossibleWordList {
  PossibleWord* possible_words;
  int num_words;
  int capacity;
} PossibleWordList;

typedef struct BoardRow {
  uint8_t letters[BOARD_DIM];
} BoardRow;

typedef struct BoardRows {
  BoardRow rows[BOARD_DIM * 2];
  int num_rows;
} BoardRows;

BoardRows* create_board_rows(const Game* game);
void destroy_board_rows(BoardRows* board_rows);

int max_nonplaythrough_spaces_in_row(BoardRow* board_row);

void add_words_without_playthrough(const KWG* kwg, uint32_t node_index,
                                   Rack* bag_as_rack, int max_nonplaythrough,
                                   uint8_t* word, int tiles_played,
                                   bool accepts,
                                   PossibleWordList* possible_word_list);
                                   
PossibleWordList* create_empty_possible_word_list();
PossibleWordList* create_possible_word_list(const Game* game,
                                            const KWG* override_kwg);
void destroy_possible_word_list(PossibleWordList* possible_word_list);

#endif  // WORD_PRUNE_H