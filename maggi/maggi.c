#include "raylib/src/raylib.h"

#include "../src/ent/move.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"

#include "../src/str/game_string.h"

#include "../test/tsrc/test_util.h"

#include "board_view.h"
#include "colors.h"
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
  InitWindow(initial_window_width, initial_window_height, "maggi");
  int codepoints[512] = {0};
  for (int i = 0; i < 96; i++) {
    codepoints[i] = 32 + i; // ASCII
  }
  Font tile_font =
      LoadFontEx("maggi/fonts/ClearSans-Bold.ttf", 128, codepoints, 512);
  Font tile_score_font =
      LoadFontEx("maggi/fonts/Roboto-Bold.ttf", 64, codepoints, 512);
  int frame_count = 0;
  SetTargetFPS(60);
  while (!WindowShouldClose()) {
    if (frame_count % 6 == 5) {
      /*
            StringBuilder *sb = string_builder_create();
            string_builder_add_game(sb, game, NULL);
            printf("%s\n", string_builder_peek(sb));
            string_builder_destroy(sb);
      */
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
    const Board *board = game_get_board(game);
    draw_board_view(&widget_layout, &tile_font, &tile_score_font,
                    game_get_ld(game), board);
    EndDrawing();
    frame_count++;
  }
  CloseWindow();
  UnloadFont(tile_font);
  return 0;
}