#ifndef MAGGI_BOARD_VIEW_H
#define MAGGI_BOARD_VIEW_H

#include "../src/ent/board.h"
#include "widget_layout.h"

void draw_board_view(const WidgetLayout *widget_layout, const Font *tile_font,
                     const Font *tile_score_font, const LetterDistribution *ld,
                     const Board *board);

#endif // MAGGI_BOARD_VIEW_H