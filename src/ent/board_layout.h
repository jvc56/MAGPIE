#ifndef BOARD_LAYOUT_H
#define BOARD_LAYOUT_H

#include "../util/io_util.h"
#include "bonus_square.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct BoardLayout BoardLayout;

BoardLayout *board_layout_create(void);
void board_layout_load(BoardLayout *bl, const char *data_paths,
                       const char *board_layout_name, ErrorStack *error_stack);
char *board_layout_get_default_name(void);
const char *board_layout_get_name(const BoardLayout *bl);
bool board_layout_is_name_default(const char *board_layout_name);
BoardLayout *board_layout_create_default(const char *data_paths,
                                         ErrorStack *error_stack);
void board_layout_destroy(BoardLayout *bl);

int board_layout_get_index(int row, int col);
BonusSquare board_layout_get_bonus_square(const BoardLayout *bl, int row,
                                          int col);
int board_layout_get_start_coord(const BoardLayout *bl, int index);

#endif
