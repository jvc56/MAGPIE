#ifndef MAGGI_BOARD_VIEW_H
#define MAGGI_BOARD_VIEW_H

#include "../src/ent/game.h"

#include "graphic_assets.h"
#include "widget_layout.h"

void draw_board_view(const WidgetLayout *widget_layout,
                     const GraphicAssets *graphic_assets, const Game *game);

#endif // MAGGI_BOARD_VIEW_H