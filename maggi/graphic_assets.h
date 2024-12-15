#ifndef MAGGI_GRAPHIC_ASSETS_H
#define MAGGI_GRAPHIC_ASSETS_H

#include "raylib/src/raylib.h"
#include "widget_layout.h"

typedef struct GraphicAssets {
    Font tile_font;
    Font tile_score_font;
} GraphicAssets;

void graphic_assets_load(GraphicAssets *graphic_assets);

void graphic_assets_unload(GraphicAssets *graphic_assets);

#endif // MAGGI_GRAPHIC_ASSETS_H