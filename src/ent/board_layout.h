#ifndef BOARD_LAYOUT_H
#define BOARD_LAYOUT_H

#include <stdint.h>

#include "../def/board_layout_defs.h"

typedef struct BoardLayout BoardLayout;

BoardLayout *board_layout_create();
char *board_layout_get_filepath(const char *layout_name);
board_layout_load_status_t board_layout_load(BoardLayout *bl,
                                             const char *layout_filename);
char *board_layout_get_default_name();
BoardLayout *board_layout_create_default();
void board_layout_destroy(BoardLayout *bl);

char bonus_square_value_to_char(uint8_t bonus_square_value);
const char *bonus_square_value_to_color_code(uint8_t bonus_square_value);
uint8_t bonus_square_char_to_value(char bonus_square_char);
int board_layout_get_index(int row, int col);
uint8_t board_layout_get_bonus_square(const BoardLayout *bl, int row, int col);
int board_layout_get_start_coord(const BoardLayout *bl, int index);

#endif
