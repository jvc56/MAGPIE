#ifndef BOARD_LAYOUT_H
#define BOARD_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

#include "../def/board_layout_defs.h"

typedef struct BoardLayout BoardLayout;

BoardLayout *board_layout_create();
board_layout_load_status_t board_layout_load(BoardLayout *bl,
                                             const char *data_dir,
                                             const char *board_layout_name);
char *board_layout_get_default_name();
const char *board_layout_get_name(const BoardLayout *bl);
bool board_layout_is_name_default(const char *board_layout_name);
BoardLayout *board_layout_create_default(const char *data_dir);
void board_layout_destroy(BoardLayout *bl);

char bonus_square_value_to_char(uint8_t bonus_square_value);
uint8_t bonus_square_char_to_value(char bonus_square_char);
int board_layout_get_index(int row, int col);
uint8_t board_layout_get_bonus_square(const BoardLayout *bl, int row, int col);
int board_layout_get_start_coord(const BoardLayout *bl, int index);

#endif
