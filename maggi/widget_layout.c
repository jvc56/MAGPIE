#include "widget_layout.h"

#include "../src/def/board_defs.h"

#define BOARD_FRACTION_OF_PANEL 0.8
#define BOARD_H_OFFSET_FRACTION ((1 - BOARD_FRACTION_OF_PANEL) / 2)
#define BOARD_V_OFFSET_FRACTION (BOARD_H_OFFSET_FRACTION / 2)

void update_widget_layout(struct WidgetLayout *layout, int window_width,
                          int window_height) {
  layout->maggi_window.x = 0;
  layout->maggi_window.y = 0;
  layout->maggi_window.width = window_width;
  layout->maggi_window.height = window_height;

  layout->board_panel.x = 0;
  layout->board_panel.y = 0;
  layout->board_panel.width = window_height;
  layout->board_panel.height = window_height;

  const int squares_v_offset = BOARD_V_OFFSET_FRACTION * layout->board_panel.width;
  const int squares_h_offset = BOARD_H_OFFSET_FRACTION * layout->board_panel.height;
  const int square_size =
      BOARD_FRACTION_OF_PANEL * layout->board_panel.width / BOARD_DIM;
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      layout->square[row][col].x =
          layout->board_panel.x + squares_h_offset + col * square_size;
      layout->square[row][col].y =
          layout->board_panel.y + squares_v_offset + row * square_size;
      layout->square[row][col].width = square_size;
      layout->square[row][col].height = square_size;
    }
  }

  layout->console_panel.x = layout->console_panel.width;
  layout->console_panel.y = 0;
  layout->console_panel.width = window_width - layout->console_panel.x;
  layout->console_panel.height = window_height;
}