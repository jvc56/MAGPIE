#include "board_layout.h"

#include <stdint.h>
#include <stdio.h>

#include "../def/board_defs.h"
#include "../def/board_layout_defs.h"

#include "../util/log.h"
#include "../util/string_util.h"
#include "../util/util.h"

struct BoardLayout {
  int start_row;
  int start_col;
  uint8_t bonus_squares[BOARD_DIM * BOARD_DIM];
};

BoardLayout *board_layout_create() {
  return malloc_or_die(sizeof(BoardLayout));
}

board_layout_load_status_t board_layout_load(BoardLayout *bl,
                                             const char *layout_filename) {
  printf("%p %p", bl, layout_filename);
  return 0;
}

char *board_layout_get_default_name() {
  return get_formatted_string("standard%d", BOARD_DIM);
}

BoardLayout *board_layout_create_default() {
  BoardLayout *bl = malloc_or_die(sizeof(BoardLayout));
  char *default_layout_filename = board_layout_get_default_name();
  board_layout_load_status_t status =
      board_layout_load(bl, default_layout_filename);
  free(default_layout_filename);
  if (status != BOARD_LAYOUT_LOAD_STATUS_SUCCESS) {
    log_fatal("standard board with dim %d failed to load", BOARD_DIM);
  }
  return bl;
}

void board_layout_destroy(BoardLayout *bl) { free(bl); }