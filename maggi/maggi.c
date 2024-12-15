#include "raylib/src/raylib.h"

#include "../src/ent/move.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"

#include "../src/str/game_string.h"

#include "../test/tsrc/test_util.h"

#include "board_view.h"
#include "colors.h"
#include "graphic_assets.h"
#include "widget_layout.h"

#if defined(PLATFORM_WEB)
#include <emscripten/emscripten.h>
#endif

static const int initial_window_width = 1440;
static const int initial_window_height = 825;

int main(void) {
  SetConfigFlags(FLAG_WINDOW_HIGHDPI);
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  Game *game = config_game_create(config);
  draw_starting_racks(game);

  MoveList *move_list = move_list_create(10000);
  WidgetLayout widget_layout;
  GraphicAssets graphic_assets;
  // SetConfigFlags(FLAG_WINDOW_RESIZABLE);
  InitWindow(initial_window_width, initial_window_height, "maggi");
  graphic_assets_load(&graphic_assets);
  int frame_count = 0;
  SetTargetFPS(60);
  while (!WindowShouldClose()) {
    if (frame_count % 30 == 29) {
      // StringBuilder *sb = string_builder_create();
      // string_builder_add_game(sb, game, NULL);
      // printf("%s\n", string_builder_peek(sb));
      // string_builder_destroy(sb);
      if (game_over(game)) {
        game_destroy(game);
        game = config_game_create(config);
        game_seed(game, frame_count);
        draw_starting_racks(game);
      } else {
        const Move *best_move = get_top_equity_move(game, 0, move_list);
        play_move(best_move, game, NULL, NULL);
      }
    }
    update_widget_layout(&widget_layout, initial_window_width,
                         initial_window_height);
    BeginDrawing();
    ClearBackground(GRAY8PERCENT);
    draw_board_view(&widget_layout, &graphic_assets, game);
    EndDrawing();
    frame_count++;
  }
  CloseWindow();
  graphic_assets_unload(&graphic_assets);
  return 0;
}