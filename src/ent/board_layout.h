#ifndef BOARD_LAYOUT_H
#define BOARD_LAYOUT_H

#include "../def/board_layout_defs.h"

typedef struct BoardLayout BoardLayout;

BoardLayout *board_layout_create();
board_layout_load_status_t board_layout_load(BoardLayout *bl,
                                             const char *layout_filename);
char *board_layout_get_default_name();
BoardLayout *board_layout_create_default();
void board_layout_destroy(BoardLayout *bl);

#endif
