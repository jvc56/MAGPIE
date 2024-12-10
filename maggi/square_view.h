#ifndef MAGGI_SQUARE_VIEW_H
#define MAGGI_SQUARE_VIEW_H

#include "raylib/src/raylib.h"

#include "../src/ent/board.h"

void draw_square_view(const Rectangle *square_rect, const Square *board_square,
                      const Font *tile_font,
                      const Font *tile_score_font,
                      const LetterDistribution *ld);

#endif // MAGGI_SQUARE_VIEW_H