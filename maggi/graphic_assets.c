#include "graphic_assets.h"
#include <stdio.h>

void graphic_assets_load(GraphicAssets *graphic_assets) {
  int codepoints[512] = {0};
  for (int i = 0; i < 96; i++) {
    codepoints[i] = 32 + i; // ASCII
  }
  graphic_assets->tile_font =
      LoadFontEx("maggi/fonts/ClearSans-Bold.ttf", 128, codepoints, 512);
  graphic_assets->tile_score_font =
      LoadFontEx("maggi/fonts/Roboto-Bold.ttf", 64, codepoints, 512);
}

void graphic_assets_unload(GraphicAssets *graphic_assets) {
  UnloadFont(graphic_assets->tile_font);
  UnloadFont(graphic_assets->tile_score_font);
}