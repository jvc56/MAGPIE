#ifndef CONSTANTS_H
#define CONSTANTS_H

#define GADDAG_MAGIC_STRING "cgdg"
#define KLV_MAGIC_STRING "cldg"
#define ALPHABET_MAGIC_STRING "clcv"
#define LETTER_DISTRIBUTION_MAGIC_STRING "clds"
#define MAX_ALPHABET_SIZE 50
#define MACHINE_LETTER_MAX_VALUE 255
#define ALPHABET_EMPTY_SQUARE_MARKER 0
#define PLAYED_THROUGH_MARKER 0
#define BLANK_MASK 0x80
#define UNBLANK_MASK (0x80 - 1)
#define INITIAL_LAST_ANCHOR_COL (BOARD_DIM)
#define BOARD_DIM 15
#define BINGO_BONUS 50
#define GADDAG_NUM_ARCS_BIT_LOC 24
#define GADDAG_LETTER_BIT_LOC 24
#define GADDAG_NODE_IDX_BIT_MASK (1 << GADDAG_LETTER_BIT_LOC) - 1
#define LETTER_SET_BIT_MASK (1 << GADDAG_NUM_ARCS_BIT_LOC) - 1
#define BLANK_OFFSET 100
#define BLANK_MACHINE_LETTER 0
#define SEPARATION_MACHINE_LETTER 0
#define TRIVIAL_CROSS_SET (uint64_t)((uint64_t)1 << MAX_ALPHABET_SIZE) - 1
#define WORD_DIRECTION_RIGHT 1
#define WORD_DIRECTION_LEFT -1
#define SEPARATION_TOKEN '^'
#define BLANK_TOKEN '?'
#define ASCII_PLAYED_THROUGH '.'
#define BAG_SIZE 100
#define RACK_SIZE 7
#define BOARD_HORIZONTAL_DIRECTION 0
#define BOARD_VERTICAL_DIRECTION 1
#define GAME_END_REASON_NONE 0
#define GAME_END_REASON_STANDARD 1
#define GAME_END_REASON_CONSECUTIVE_ZEROS 2
#define INFERENCE_EQUITY_EPSILON 0.000000001
#define INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW 0
#define INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE 1
#define MOVE_TYPE_PLAY 0
#define MOVE_TYPE_EXCHANGE 1
#define MOVE_TYPE_PASS 2
#define SORT_BY_SCORE 0
#define SORT_BY_EQUITY 1
#define PLAY_RECORDER_TYPE_ALL 0
#define PLAY_RECORDER_TYPE_TOP_EQUITY 1
#define PREENDGAME_ADJUSTMENT_VALUES_TYPE_ZERO 0
#define PREENDGAME_ADJUSTMENT_VALUES_TYPE_QUACKLE 1
#define INFERENCE_STATUS_SUCCESS 0
#define INFERENCE_STATUS_INITIALIZED 1
#define INFERENCE_STATUS_NO_TILES_PLAYED 2
#define INFERENCE_STATUS_RACK_OVERFLOW 3
#define INFERENCE_STATUS_TILES_PLAYED_NOT_IN_BAG 4
#define INFERENCE_STATUS_BOTH_PLAY_AND_EXCHANGE 5
#define INFERENCE_STATUS_EXCHANGE_SCORE_NOT_ZERO 6
#define INFERENCE_STATUS_EXCHANGE_NOT_ALLOWED 7
#define INFERENCE_STATUS_INVALID_NUMBER_OF_THREADS 8
#define NUMBER_OF_ROUNDED_EQUITY_VALUES 201
#define START_ROUNDED_EQUITY_VALUE -100
#define MOVE_LIST_CAPACITY 1000000
#define PASS_MOVE_EQUITY -10000
#define INITIAL_TOP_MOVE_EQUITY -100000
#define MAX_SCORELESS_TURNS 6
#define OPENING_HOTSPOT_PENALTY -0.7
#define PREENDGAME_ADJUSTMENT_VALUES_LENGTH 13
#define BONUS_TRIPLE_WORD_SCORE '='
#define BONUS_DOUBLE_WORD_SCORE '-'
#define BONUS_TRIPLE_LETTER_SCORE '"'
#define BONUS_DOUBLE_LETTER_SCORE '\''
#define DATA_DIRECTORY "data"
#define KLV_FILENAME_EXTENSION "lg"
#define MAX_ARG_LENGTH 300
#define SIM_STOPPING_CONDITION_NONE 0
#define SIM_STOPPING_CONDITION_95PCT 1
#define SIM_STOPPING_CONDITION_98PCT 2
#define SIM_STOPPING_CONDITION_99PCT 3
#define BACKUP_MODE_OFF 0
#define BACKUP_MODE_SIMULATION 1

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

// want superOMG board as well.

#endif