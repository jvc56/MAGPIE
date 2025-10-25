// C wrapper for MAGPIE functions
// This avoids C++ compilation issues with void* pointer conversions

#include "magpie_wrapper.h"
#include "../src/ent/game.h"
#include "../src/ent/board.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/validated_move.h"
#include "../src/ent/move.h"
#include "../src/impl/config.h"
#include "../src/impl/cgp.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "../src/str/game_string.h"
#include "../src/str/rack_string.h"
#include "../src/str/move_string.h"
#include "../src/str/letter_distribution_string.h"
#include <stdio.h>
#include <stdlib.h>

// Create a config with CSW24, English, standard layout, WMP
Config* magpie_create_config(const char *data_path) {
    ErrorStack *error_stack = error_stack_create();

    // Create config with data path
    Config *config = config_create_default_with_data_paths(error_stack, data_path);

    if (!error_stack_is_empty(error_stack)) {
        fprintf(stderr, "Error creating config:\n");
        error_stack_print_and_reset(error_stack);
        error_stack_destroy(error_stack);
        return NULL;
    }

    // Set lexicon to CSW24 with pretty printing
    config_load_command(config, "set -lex CSW24 -ld english -bdn standard15 -pretty true", error_stack);
    config_execute_command(config, error_stack);

    if (!error_stack_is_empty(error_stack)) {
        fprintf(stderr, "Error setting lexicon:\n");
        error_stack_print_and_reset(error_stack);
        config_destroy(config);
        error_stack_destroy(error_stack);
        return NULL;
    }

    error_stack_destroy(error_stack);
    return config;
}

void magpie_destroy_config(Config *config) {
    if (config) {
        config_destroy(config);
    }
}

Game* magpie_get_game_from_config(Config *config) {
    if (config == NULL) {
        return NULL;
    }

    // Create a new game with the config's board layout and settings
    Game *game = config_game_create(config);
    return game;
}

Board* magpie_get_board_from_game(Game *game) {
    if (game == NULL) {
        return NULL;
    }
    return game_get_board(game);
}

MagpieBonusSquare magpie_get_bonus_square(Board *board, int row, int col) {
    if (board == NULL) {
        return MAGPIE_BONUS_NONE;
    }

    // Get the square at the specified position
    // Use dir=0 (horizontal) and ci=0 (cross index 0) to get the square
    const Square *square = board_get_readonly_square(board, row, col, 0, 0);
    if (square == NULL) {
        return MAGPIE_BONUS_NONE;
    }

    // Get the bonus square and extract its raw value
    BonusSquare bonus = square_get_bonus_square(square);
    uint8_t raw = bonus.raw;

    // Map raw value to our enum
    // 0x11 = none, 0x12 = DL, 0x13 = TL, 0x21 = DW, 0x31 = TW
    switch (raw) {
        case 0x12:
            return MAGPIE_DOUBLE_LETTER_SCORE;
        case 0x13:
            return MAGPIE_TRIPLE_LETTER_SCORE;
        case 0x21:
            return MAGPIE_DOUBLE_WORD_SCORE;
        case 0x31:
            return MAGPIE_TRIPLE_WORD_SCORE;
        default:
            return MAGPIE_BONUS_NONE;
    }
}

int magpie_board_is_square_empty(Board *board, int row, int col) {
    if (board == NULL) {
        return 0;
    }

    // Get the letter at the specified position
    // Empty squares have letter value of 0 (ALPHABET_EMPTY_SQUARE_MARKER)
    MachineLetter letter = board_get_letter(board, row, col);
    return letter == 0;  // 0 is ALPHABET_EMPTY_SQUARE_MARKER
}

char* magpie_game_to_string(const Config *config, const Game *game) {
    if (game == NULL) {
        return NULL;
    }

    // Create game string options without color but with box drawing characters
    GameStringOptions options = {
        .board_color = GAME_STRING_BOARD_COLOR_NONE,
        .board_tile_glyphs = GAME_STRING_BOARD_TILE_GLYPHS_PRIMARY,
        .board_border = GAME_STRING_BOARD_BORDER_BOX_DRAWING,
        .board_column_label = GAME_STRING_BOARD_COLUMN_LABEL_ASCII,
        .on_turn_marker = GAME_STRING_ON_TURN_MARKER_ARROWHEAD,
        .on_turn_color = GAME_STRING_ON_TURN_COLOR_NONE,
        .on_turn_score_style = GAME_STRING_ON_TURN_SCORE_NORMAL
    };

    // Create string builder
    StringBuilder *sb = string_builder_create();

    // Add game to string builder (pass NULL for move_list to just show board)
    string_builder_add_game(game, NULL, &options, sb);

    // Get the C string and destroy the string builder
    char *result = string_builder_dump(sb, NULL);
    string_builder_destroy(sb);

    return result;
}

void magpie_load_cgp(Game *game, const char *cgp) {
    if (game == NULL || cgp == NULL) {
        return;
    }

    ErrorStack *error_stack = error_stack_create();
    game_load_cgp(game, cgp, error_stack);

    if (!error_stack_is_empty(error_stack)) {
        fprintf(stderr, "Error loading CGP:\\n");
        error_stack_print_and_reset(error_stack);
    }

    error_stack_destroy(error_stack);
}

void magpie_config_load_command(Config *config, const char *cmd) {
    if (config == NULL || cmd == NULL) {
        return;
    }

    ErrorStack *error_stack = error_stack_create();
    config_load_command(config, cmd, error_stack);
    config_execute_command(config, error_stack);

    if (!error_stack_is_empty(error_stack)) {
        fprintf(stderr, "Error executing command: %s\n", cmd);
        error_stack_print_and_reset(error_stack);
    }

    error_stack_destroy(error_stack);
}

char* magpie_get_player_rack_string(Game *game, int player_index) {
    if (game == NULL) {
        return NULL;
    }

    Player *player = game_get_player(game, player_index);
    if (player == NULL) {
        return NULL;
    }

    const Rack *rack = player_get_rack(player);
    if (rack == NULL) {
        return NULL;
    }

    const LetterDistribution *ld = game_get_ld(game);
    if (ld == NULL) {
        return NULL;
    }

    // Convert rack to string
    StringBuilder *sb = string_builder_create();
    string_builder_add_rack(sb, rack, ld, false);  // blanks_first = false
    char *result = string_builder_dump(sb, NULL);
    string_builder_destroy(sb);

    return result;
}

void magpie_draw_starting_racks(Game *game) {
    if (game == NULL) {
        return;
    }
    draw_starting_racks(game);
}

char* magpie_get_cgp(const Game *game) {
    if (game == NULL) {
        return NULL;
    }
    // Get CGP with player on turn first = false (standard order)
    return game_get_cgp(game, false);
}

char* magpie_validate_move(Game *game, int player_index, const char *ucgi_move_string) {
    if (game == NULL || ucgi_move_string == NULL) {
        return string_duplicate("Invalid game or move string");
    }

    ErrorStack *error_stack = error_stack_create();

    // Create validated move with allow_phonies=true (we're only checking well-formedness)
    ValidatedMoves *vms = validated_moves_create(
        game,
        player_index,
        ucgi_move_string,
        true,  // allow_phonies (we don't care about dictionary validation)
        false, // allow_unknown_exchanges
        true,  // allow_playthrough
        error_stack
    );

    // Check if validation failed
    char *error_msg = NULL;
    if (!error_stack_is_empty(error_stack)) {
        error_msg = error_stack_get_string_and_reset(error_stack);
    }

    validated_moves_destroy(vms);
    error_stack_destroy(error_stack);

    return error_msg;  // NULL if valid, error message if invalid
}

int magpie_get_player_score(Game *game, int player_index) {
    if (game == NULL) {
        return 0;
    }

    Player *player = game_get_player(game, player_index);
    if (player == NULL) {
        return 0;
    }

    Equity score_equity = player_get_score(player);
    return equity_to_int(score_equity);
}

int magpie_get_player_on_turn_index(Game *game) {
    if (game == NULL) {
        return 0;
    }

    return game_get_player_on_turn_index(game);
}

int magpie_get_move_score(Game *game, int player_index, const char *ucgi_move_string) {
    if (game == NULL || ucgi_move_string == NULL) {
        return -1;
    }

    ErrorStack *error_stack = error_stack_create();

    ValidatedMoves *vms = validated_moves_create(
        game,
        player_index,
        ucgi_move_string,
        true,  // allow_phonies
        false, // allow_unknown_exchanges
        true,  // allow_playthrough
        error_stack
    );

    int score = -1;
    if (error_stack_is_empty(error_stack) && vms != NULL) {
        // Get the first validated move
        int num_moves = validated_moves_get_number_of_moves(vms);
        if (num_moves > 0) {
            const Move *move = validated_moves_get_move(vms, 0);
            if (move != NULL) {
                Equity score_equity = move_get_score(move);
                score = equity_to_int(score_equity);
            }
        }
    }

    validated_moves_destroy(vms);
    error_stack_destroy(error_stack);

    return score;
}

char* magpie_play_move(Game *game, int player_index, const char *ucgi_move_string) {
    if (game == NULL || ucgi_move_string == NULL) {
        return string_duplicate("Invalid game or move string");
    }

    ErrorStack *error_stack = error_stack_create();

    // Validate and play the move
    ValidatedMoves *vms = validated_moves_create(
        game,
        player_index,
        ucgi_move_string,
        true,  // allow_phonies
        false, // allow_unknown_exchanges
        true,  // allow_playthrough
        error_stack
    );

    char *error_msg = NULL;
    if (!error_stack_is_empty(error_stack) || vms == NULL) {
        error_msg = error_stack_get_string_and_reset(error_stack);
        validated_moves_destroy(vms);
        error_stack_destroy(error_stack);
        return error_msg;
    }

    // Get the first validated move and play it
    int num_moves = validated_moves_get_number_of_moves(vms);
    if (num_moves == 0) {
        validated_moves_destroy(vms);
        error_stack_destroy(error_stack);
        return string_duplicate("No valid move found");
    }

    const Move *move = validated_moves_get_move(vms, 0);
    if (move == NULL) {
        validated_moves_destroy(vms);
        error_stack_destroy(error_stack);
        return string_duplicate("Failed to get move");
    }

    // Play the move without drawing tiles
    play_move_without_drawing_tiles(move, game);

    // Draw tiles to refill the player's rack
    draw_to_full_rack(game, player_index);

    validated_moves_destroy(vms);
    error_stack_destroy(error_stack);

    return NULL;  // Success
}

char* magpie_get_top_equity_move(Game *game, int player_index) {
    if (game == NULL) {
        return NULL;
    }

    // Create move list to hold generated moves
    MoveList *move_list = move_list_create(1);  // Initial capacity of 1
    if (move_list == NULL) {
        return NULL;
    }

    // Get top equity move using the gameplay helper
    Move *top_move = get_top_equity_move(game, 0, move_list);  // thread_index = 0

    if (top_move == NULL) {
        move_list_destroy(move_list);
        return NULL;
    }

    // Build UCGI notation string using StringBuilder
    StringBuilder *sb = string_builder_create();
    Board *board = game_get_board(game);
    const LetterDistribution *ld = game_get_ld(game);
    string_builder_add_ucgi_move(sb, top_move, board, ld);
    char *move_string = string_builder_dump(sb, NULL);

    string_builder_destroy(sb);
    move_list_destroy(move_list);

    return move_string;  // Caller must free
}

char* magpie_get_rack(Game *game, int player_index) {
    return magpie_get_player_rack_string(game, player_index);
}

char* magpie_board_get_letter_at(Board *board, Game *game, int row, int col) {
    if (board == NULL || game == NULL) {
        return NULL;
    }

    // Check bounds
    if (row < 0 || row >= BOARD_DIM || col < 0 || col >= BOARD_DIM) {
        return NULL;
    }

    // Get the machine letter at this position
    MachineLetter ml = board_get_letter(board, row, col);

    // Check if square is empty
    if (ml == 0) {  // 0 is ALPHABET_EMPTY_SQUARE_MARKER
        return NULL;
    }

    // Convert to user-visible letter using the game's letter distribution
    const LetterDistribution *ld = game_get_ld(game);
    if (ld == NULL) {
        return NULL;
    }

    // Use StringBuilder to get the human-readable letter
    StringBuilder *sb = string_builder_create();
    string_builder_add_user_visible_letter(sb, ld, ml);
    char *result = string_builder_dump(sb, NULL);
    string_builder_destroy(sb);

    return result;
}
