#include "board_view.h"

#include "raylib/src/raylib.h"

#include "../src/def/board_defs.h"
#include "../src/ent/board.h"

#include "colors.h"
#include "widget_layout.h"

#define DRAWN_SQUARE_FRACTION_OF_WIDGET 0.85
#define DRAWN_SQUARE_OFFSET_FRACTION ((1 - DRAWN_SQUARE_FRACTION_OF_WIDGET) / 2)
#define DRAWN_SQUARE_GRADIENT_FRACTION 0.98
#define DRAWN_SQUARE_GRADIENT_OFFSET_FRACTION                                  \
  ((1 - DRAWN_SQUARE_GRADIENT_FRACTION) / 2)
#define TILE_TEXT_VERTICAL_OFFSET_FRACTION 0.05
#define BLANK_TILE_SIZE_FRACTION 0.6667
#define BLANK_TILE_SIZE_OFFSET_FRACTION ((1 - BLANK_TILE_SIZE_FRACTION) / 2)
#define TILE_SCORE_SIZE_FRACTION 0.42
#define TWO_DIGIT_SCORE_SIZE_FRACTION 0.35
#define TILE_SCORE_HORIZONTAL_OFFSET_FRACTION 0.87
#define TILE_SCORE_VERTICAL_OFFSET_FRACTION 0.8
#define LABEL_FONT_FRACTION 0.6
#define COLUMN_LABEL_MARGIN_FRACTION 0.2
#define ROW_LABEL_MARGIN_FRACTION 0.12

void draw_board_view(const WidgetLayout *widget_layout, const Font *tile_font,
                     const Font *tile_score_font, const LetterDistribution *ld,
                     const Board *board) {
  DrawRectangle(widget_layout->board_panel.x, widget_layout->board_panel.y,
                widget_layout->board_panel.width,
                widget_layout->board_panel.height, GRAY16PERCENT);

  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      const int x =
          widget_layout->square[row][col].x +
          DRAWN_SQUARE_OFFSET_FRACTION * widget_layout->square[row][col].width;
      const int y =
          widget_layout->square[row][col].y +
          DRAWN_SQUARE_OFFSET_FRACTION * widget_layout->square[row][col].height;
      const int width = DRAWN_SQUARE_FRACTION_OF_WIDGET *
                        widget_layout->square[row][col].width;
      const int height = DRAWN_SQUARE_FRACTION_OF_WIDGET *
                         widget_layout->square[row][col].height;
      const Square *square =
          board_get_readonly_square(board, row, col, BOARD_HORIZONTAL_DIRECTION,
                                    BOARD_HORIZONTAL_DIRECTION);
      const uint8_t ml = square_get_letter(square);
      const uint8_t bonus_square = square_get_bonus_square(square);
      const Color square_bg_color =
          get_square_background_color(ml, bonus_square);
      DrawRectangle(x, y, width, height, square_bg_color);
      const Rectangle square_rect = {x, y, width, height};
      DrawRectangleRoundedLinesEx(square_rect, 0.25, 10, 1.5, square_bg_color);
      // Empty square, draw gradient for concave effect
      Color gradient_top = Fade(BLACK, 0.04);
      Color gradient_bottom = Fade(WHITE, 0.04);
      if (ml != ALPHABET_EMPTY_SQUARE_MARKER) {
        // Tile square, draw gradient for convex effect
        gradient_top = Fade(WHITE, 0.18);
        gradient_bottom = Fade(BLACK, 0.18);
      }
      const Rectangle gradient_rect = {
          x + DRAWN_SQUARE_GRADIENT_OFFSET_FRACTION * width,
          y + DRAWN_SQUARE_GRADIENT_OFFSET_FRACTION * height,
          DRAWN_SQUARE_GRADIENT_FRACTION * width,
          DRAWN_SQUARE_GRADIENT_FRACTION * height};
      DrawRectangleGradientV(gradient_rect.x, gradient_rect.y,
                             gradient_rect.width, gradient_rect.height,
                             gradient_top, gradient_bottom);
      if (ml != ALPHABET_EMPTY_SQUARE_MARKER) {
        const bool is_blank = get_is_blanked(ml);
        const int font_size =
            (is_blank ? BLANK_TILE_SIZE_FRACTION : 1) * gradient_rect.height;
        char *human_readable_letter =
            ld_ml_to_hl(ld, get_unblanked_machine_letter(ml));
        Vector2 text_size =
            MeasureTextEx(*tile_font, human_readable_letter, font_size, 0);
        Vector2 text_position = {
            gradient_rect.x + 0.5 * gradient_rect.width - 0.5 * text_size.x,
            gradient_rect.y +
                (0.5 - TILE_TEXT_VERTICAL_OFFSET_FRACTION) *
                    gradient_rect.height -
                0.5 * text_size.y};
        if (is_blank) {
          const Rectangle blank_outline_rect = {
              gradient_rect.x +
                  BLANK_TILE_SIZE_OFFSET_FRACTION * gradient_rect.width,
              gradient_rect.y +
                  BLANK_TILE_SIZE_OFFSET_FRACTION * gradient_rect.height,
              BLANK_TILE_SIZE_FRACTION * gradient_rect.width,
              BLANK_TILE_SIZE_FRACTION * gradient_rect.width};
          DrawRectangleRoundedLinesEx(blank_outline_rect, 0.15, 10, 1.5,
                                      Fade(DARKRED, 0.9));
        } else {
          char score_str[20];
          snprintf(score_str, sizeof(score_str), "%d", ld_get_score(ld, ml));
          const int score_font_size =
              ((strlen(score_str) == 1) ? TILE_SCORE_SIZE_FRACTION
                                        : TWO_DIGIT_SCORE_SIZE_FRACTION) *
              gradient_rect.height;
          Vector2 score_text_size =
              MeasureTextEx(*tile_score_font, score_str, score_font_size, 0);
          Vector2 score_text_position = {
              gradient_rect.x +
                  TILE_SCORE_HORIZONTAL_OFFSET_FRACTION * gradient_rect.width -
                  0.5 * score_text_size.x,
              gradient_rect.y +
                  TILE_SCORE_VERTICAL_OFFSET_FRACTION * gradient_rect.height -
                  0.5 * score_text_size.y};
          DrawTextEx(*tile_score_font, score_str, score_text_position,
                     score_font_size, 0, BLACK);
        }
        DrawTextEx(*tile_font, human_readable_letter, text_position, font_size,
                   0, BLACK);
        free(human_readable_letter);
      }
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