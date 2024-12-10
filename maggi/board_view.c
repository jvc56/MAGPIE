#include "board_view.h"

#include "raylib/src/raylib.h"

#include "../src/def/board_defs.h"

#include "colors.h"
#include "square_view.h"
#include "widget_layout.h"

#define LABEL_FONT_FRACTION 0.6
#define COLUMN_LABEL_MARGIN_FRACTION 0.2
#define ROW_LABEL_MARGIN_FRACTION 0.12

void draw_board_view(const WidgetLayout *widget_layout, const Font *tile_font,
                     const Font *tile_score_font, const LetterDistribution *ld,
                     const Board *board) {
  // Background for board panel (contains board and rack)
  DrawRectangle(widget_layout->board_panel.x, widget_layout->board_panel.y,
                widget_layout->board_panel.width,
                widget_layout->board_panel.height, GRAY16PERCENT);

  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      const Rectangle square_rect = widget_layout->square[row][col];
      const Square *board_square =
          board_get_readonly_square(board, row, col, BOARD_HORIZONTAL_DIRECTION,
                                    BOARD_HORIZONTAL_DIRECTION);
              draw_square_view(&square_rect, board_square, tile_font,
                               tile_score_font, ld);
    }
  }
  const int label_font_size =
      LABEL_FONT_FRACTION * widget_layout->square[0][0].height;
  for (int i = 0; i < BOARD_DIM; i++) {
    char label_str[2] = {i + 'A', '\0'};
    Vector2 label_text_size =
        MeasureTextEx(*tile_font, label_str, label_font_size, 0);
    const int x = widget_layout->square[0][i].x +
                  0.54 * widget_layout->square[0][i].width -
                  0.5 * label_text_size.x;
    const int y =
        widget_layout->square[0][i].y -
        COLUMN_LABEL_MARGIN_FRACTION * widget_layout->square[0][i].height -
        0.5 * label_text_size.y;
    const Vector2 pos = {x, y};
    DrawTextEx(*tile_font, label_str, pos, 20, 0, GRAY40PERCENT);
  }
  for (int i = 0; i < BOARD_DIM; i++) {
    char label_str[3];
    snprintf(label_str, sizeof(label_str), "%d", i + 1);
    Vector2 label_text_size =
        MeasureTextEx(*tile_font, label_str, label_font_size, 0);
    const int x =
        widget_layout->square[i][0].x -
        ROW_LABEL_MARGIN_FRACTION * widget_layout->square[i][0].width -
        0.75 * label_text_size.x;
    const int y = widget_layout->square[i][0].y +
                  0.5 * widget_layout->square[i][0].height -
                  0.5 * label_text_size.y;
    const Vector2 pos = {x, y};
    DrawTextEx(*tile_font, label_str, pos, 20, 0, GRAY40PERCENT);
  }
}