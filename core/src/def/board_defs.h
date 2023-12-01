#ifndef BOARD_DEFS_H
#define BOARD_DEFS_H

#define BOARD_DIM 15
// Use 2 * 2 for
// vert and horizontal sets and
// player 1 and player 2 sets, when using different lexica
#define NUMBER_OF_CROSSES BOARD_DIM *BOARD_DIM * 2 * 2
#define BOARD_HORIZONTAL_DIRECTION 0
#define BOARD_VERTICAL_DIRECTION 1

// TODO: read this from file to make it easier to configure custom boards
#define CROSSWORD_GAME_BOARD                                                   \
  "=  '   =   '  ="                                                            \
  " -   \"   \"   - "                                                          \
  "  -   ' '   -  "                                                            \
  "'  -   '   -  '"                                                            \
  "    -     -    "                                                            \
  " \"   \"   \"   \" "                                                        \
  "  '   ' '   '  "                                                            \
  "=  '   -   '  ="                                                            \
  "  '   ' '   '  "                                                            \
  " \"   \"   \"   \" "                                                        \
  "    -     -    "                                                            \
  "'  -   '   -  '"                                                            \
  "  -   ' '   -  "                                                            \
  " -   \"   \"   - "                                                          \
  "=  '   =   '  ="

#define BONUS_TRIPLE_WORD_SCORE '='
#define BONUS_DOUBLE_WORD_SCORE '-'
#define BONUS_TRIPLE_LETTER_SCORE '"'
#define BONUS_DOUBLE_LETTER_SCORE '\''

#define BOARD_LAYOUT_CROSSWORD_GAME_NAME "CrosswordGame"
#define BOARD_LAYOUT_SUPER_CROSSWORD_GAME_NAME "SuperCrosswordGame"

typedef enum {
  BOARD_LAYOUT_UNKNOWN,
  BOARD_LAYOUT_CROSSWORD_GAME,
  BOARD_LAYOUT_SUPER_CROSSWORD_GAME,
} board_layout_t;

#endif