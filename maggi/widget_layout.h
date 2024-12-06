#ifndef MAGGI_WIDGET_LAYOUT_H
#define MAGGI_WIDGET_LAYOUT_H

#include "raylib/src/raylib.h"

#include "../src/def/board_defs.h"

typedef struct WidgetLayout {
  Rectangle maggi_window;
  Rectangle board_panel;
  Rectangle square[BOARD_DIM][BOARD_DIM];
  Rectangle console_panel;
} WidgetLayout;

void update_widget_layout(struct WidgetLayout *layout, int window_width, int window_height);

#endif // MAGGI_WIDGET_LAYOUT_H