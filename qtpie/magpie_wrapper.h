#ifndef MAGPIE_WRAPPER_H
#define MAGPIE_WRAPPER_H

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations - avoids including MAGPIE headers in C++ code
typedef struct Game Game;
typedef struct Board Board;
typedef struct Config Config;

// Bonus square types (matching MAGPIE's BonusSquare enum)
typedef enum {
    MAGPIE_BONUS_NONE = 0,
    MAGPIE_DOUBLE_LETTER_SCORE = 1,
    MAGPIE_TRIPLE_LETTER_SCORE = 2,
    MAGPIE_DOUBLE_WORD_SCORE = 3,
    MAGPIE_TRIPLE_WORD_SCORE = 4
} MagpieBonusSquare;

// Config and game creation
Config* magpie_create_config(const char *data_path);
void magpie_destroy_config(Config *config);
Game* magpie_get_game_from_config(Config *config);

// Wrapper functions
Board* magpie_get_board_from_game(Game *game);
MagpieBonusSquare magpie_get_bonus_square(Board *board, int row, int col);
int magpie_board_is_square_empty(Board *board, int row, int col);

// Game string printing
char* magpie_game_to_string(const Config *config, const Game *game);

// CGP loading
void magpie_load_cgp(Game *game, const char *cgp);

// Config command loading
void magpie_config_load_command(Config *config, const char *cmd);

// Get player rack as string
char* magpie_get_player_rack_string(Game *game, int player_index);

// Draw starting racks for all players
void magpie_draw_starting_racks(Game *game);

// Get CGP string for current game state
char* magpie_get_cgp(const Game *game);

#ifdef __cplusplus
}
#endif

#endif // MAGPIE_WRAPPER_H
