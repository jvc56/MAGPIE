#include "qt_bridge.h"

#include "../../ent/equity.h"
#include "../../ent/game_history.h"
#include "../../ent/game.h"
#include "../../ent/board.h"
#include "../../ent/board_layout.h"
#include "../../ent/letter_distribution.h"
#include "../../ent/players_data.h"
#include "../../impl/gcg.h"
#include "../../impl/gameplay.h"
#include "../../util/io_util.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// Wrapper struct to manage lifetime of game and its dependencies
typedef struct _BridgeGame {
    Game *game;
    LetterDistribution *ld;
    PlayersData *pd;
    BoardLayout *bl;
} _BridgeGame;


// Cast opaque handles to actual types
#define TO_GH(x) ((GameHistory*)(x))
#define TO_GAME(x) (((_BridgeGame*)(x))->game)

BridgeGameHistory* bridge_game_history_create(void) {
    return (BridgeGameHistory*)game_history_create();
}

void bridge_game_history_destroy(BridgeGameHistory* gh) {
    game_history_destroy(TO_GH(gh));
}

int bridge_load_gcg(BridgeGameHistory* gh, const char* gcg_content, const char* data_path, char* error_msg, int error_msg_len) {
    ErrorStack *err = error_stack_create();
    
    GCGParser *parser = gcg_parser_create(gcg_content, TO_GH(gh), "CSW21", err);
    if (!error_stack_is_empty(err)) {
        char *msg = error_stack_get_string_and_reset(err);
        snprintf(error_msg, error_msg_len, "Parser create failed: %s", msg);
        free(msg);
        error_stack_destroy(err);
        return 1;
    }

    parse_gcg_settings(parser, err);
    if (!error_stack_is_empty(err)) {
        char *msg = error_stack_get_string_and_reset(err);
        snprintf(error_msg, error_msg_len, "Settings parse failed: %s", msg);
        free(msg);
        gcg_parser_destroy(parser);
        error_stack_destroy(err);
        return 1;
    }

    // We need to temporarily create a game to parse events, even if we destroy it later.
    // But we need dependencies.
    const char *lexiconName = game_history_get_lexicon_name(TO_GH(gh));
    if (!lexiconName) lexiconName = "CSW24"; // Default lexicon
    const char *ldName = game_history_get_ld_name(TO_GH(gh));
    
    LetterDistribution *ld = ld_create(data_path, ldName ? ldName : "CSW24", err);
    if (!error_stack_is_empty(err)) {
        char *msg = error_stack_get_string_and_reset(err);
        snprintf(error_msg, error_msg_len, "LD load failed: %s", msg);
        free(msg);
        gcg_parser_destroy(parser);
        error_stack_destroy(err);
        return 1;
    }

    PlayersData *pd = players_data_create();
    players_data_set(pd, PLAYERS_DATA_TYPE_KWG, data_path, lexiconName, lexiconName, err);
    if (!error_stack_is_empty(err)) {
        char *msg = error_stack_get_string_and_reset(err);
        snprintf(error_msg, error_msg_len, "KWG load failed: %s", msg);
        free(msg);
        ld_destroy(ld);
        gcg_parser_destroy(parser);
        error_stack_destroy(err);
        return 1;
    }
    
    const char *layoutName = game_history_get_board_layout_name(TO_GH(gh));
    if (!layoutName) layoutName = board_layout_get_default_name();
    
    BoardLayout *bl = board_layout_create();
    board_layout_load(bl, data_path, layoutName, err);
    
    GameArgs gameArgs;
    memset(&gameArgs, 0, sizeof(GameArgs));
    gameArgs.players_data = pd;
    gameArgs.board_layout = bl;
    gameArgs.ld = ld;
    gameArgs.bingo_bonus = 50;
    gameArgs.game_variant = game_history_get_game_variant(TO_GH(gh));

    Game *game = game_create(&gameArgs);

    parse_gcg_events(parser, game, err);
    if (!error_stack_is_empty(err)) {
        char *msg = error_stack_get_string_and_reset(err);
        snprintf(error_msg, error_msg_len, "Event parse failed: %s", msg);
        free(msg);
        // cleanup
    }

    game_destroy(game);
    players_data_destroy(pd);
    board_layout_destroy(bl);
    ld_destroy(ld);

    gcg_parser_destroy(parser);
    
    bool failed = !error_stack_is_empty(err); // Should be empty if we handled it above, but check again
    error_stack_destroy(err);
    return failed ? 1 : 0;
}

BridgeGame* bridge_game_create_from_history(BridgeGameHistory* gh, const char* data_path) {
    ErrorStack *err = error_stack_create();
    _BridgeGame *b_game = calloc(1, sizeof(_BridgeGame));

    const char *lexiconName = game_history_get_lexicon_name(TO_GH(gh));
    if (!lexiconName) lexiconName = "CSW24"; // Default lexicon
    const char *ldName = game_history_get_ld_name(TO_GH(gh));
    
    b_game->ld = ld_create(data_path, ldName ? ldName : "CSW24", err);
    if (!error_stack_is_empty(err)) {
        bridge_game_destroy((BridgeGame*)b_game);
        error_stack_destroy(err);
        return NULL;
    }
    
    b_game->pd = players_data_create();
    players_data_set(b_game->pd, PLAYERS_DATA_TYPE_KWG, data_path, lexiconName, lexiconName, err);
    if (!error_stack_is_empty(err)) {
        bridge_game_destroy((BridgeGame*)b_game);
        error_stack_destroy(err);
        return NULL;
    }
    
    const char *layoutName = game_history_get_board_layout_name(TO_GH(gh));
    if (!layoutName) layoutName = board_layout_get_default_name();
    
    b_game->bl = board_layout_create();
    board_layout_load(b_game->bl, data_path, layoutName, err);
    if (!error_stack_is_empty(err)) {
        bridge_game_destroy((BridgeGame*)b_game);
        error_stack_destroy(err);
        return NULL;
    }
    
    GameArgs gameArgs;
    memset(&gameArgs, 0, sizeof(GameArgs));
    gameArgs.players_data = b_game->pd;
    gameArgs.board_layout = b_game->bl;
    gameArgs.ld = b_game->ld;
    gameArgs.bingo_bonus = 50;
    gameArgs.game_variant = game_history_get_game_variant(TO_GH(gh));

    b_game->game = game_create(&gameArgs);
    
    error_stack_destroy(err);
    return (BridgeGame*)b_game;
}

void bridge_game_destroy(BridgeGame* game) {
    if (!game) return;
    _BridgeGame *b_game = (_BridgeGame*)game;
    if(b_game->game) game_destroy(b_game->game);
    if(b_game->ld) ld_destroy(b_game->ld);
    if(b_game->pd) players_data_destroy(b_game->pd);
    if(b_game->bl) board_layout_destroy(b_game->bl);
    free(b_game);
}

void bridge_game_play_to_index(BridgeGameHistory* gh, BridgeGame* game, int index) {
    if (!game) return;
    ErrorStack *err = error_stack_create();
    game_reset(TO_GAME(game));
    game_play_n_events(TO_GH(gh), TO_GAME(game), index, true, err);
    error_stack_destroy(err);
}

const char* bridge_get_player_name(BridgeGameHistory* gh, int player_index) {
    return game_history_player_get_name(TO_GH(gh), player_index);
}

int bridge_get_player_score(BridgeGame* game, int player_index) {
    if (!game) return 0;
    return equity_to_int(player_get_score(game_get_player(TO_GAME(game), player_index)));
}

int bridge_get_player_on_turn_index(BridgeGame* game) {
    if (!game) return 0;
    return game_get_player_on_turn_index(TO_GAME(game));
}

int bridge_get_num_events(BridgeGameHistory* gh) {
    if (!gh) return 0;
    return game_history_get_num_events(TO_GH(gh));
}

char* bridge_get_board_square_string(BridgeGame* game, int row, int col) {
    if (!game) return string_duplicate("");
    Board *b = game_get_board(TO_GAME(game));
    MachineLetter ml = board_get_letter(b, row, col);
    if (ml == ALPHABET_EMPTY_SQUARE_MARKER) {
        return string_duplicate("");
    }
    const LetterDistribution *ld = game_get_ld(TO_GAME(game));
    return ld_ml_to_hl(ld, ml);
}

uint8_t bridge_get_board_bonus(BridgeGame* game, int row, int col) {
    if (!game) return 0x11; // Default to normal square
    Board *b = game_get_board(TO_GAME(game));
    BonusSquare bonus = board_get_bonus_square(b, row, col);
    return bonus.raw;
}

uint8_t bridge_get_machine_letter(BridgeGame* game, int row, int col) {
    if (!game) return ALPHABET_EMPTY_SQUARE_MARKER;
    Board *b = game_get_board(TO_GAME(game));
    return board_get_letter(b, row, col);
}

int bridge_get_letter_score(BridgeGame* game, uint8_t ml) {
    if (!game) return 0;
    const LetterDistribution *ld = game_get_ld(TO_GAME(game));
    if (bridge_is_blank(ml)) {
        return equity_to_int(ld_get_score(ld, BLANK_MACHINE_LETTER));
    }
    return equity_to_int(ld_get_score(ld, ml));
}

bool bridge_is_blank(uint8_t ml) {
    return get_is_blanked(ml);
}

char* bridge_get_current_rack(BridgeGame* game) {
    if (!game) return string_duplicate("");
    
    int playerIdx = game_get_player_on_turn_index(TO_GAME(game));
    Player *p = game_get_player(TO_GAME(game), playerIdx);
    Rack *r = player_get_rack(p);
    const LetterDistribution *ld = game_get_ld(TO_GAME(game));
    
    // Estimate buffer size: max 7 tiles * max 4 bytes per char + null
    char buffer[64] = {0}; 
    int pos = 0;
    
    for (int i = 0; i < ld_get_size(ld); i++) {
        int count = rack_get_letter(r, i);
        if (count > 0) {
            char *hl = ld_ml_to_hl(ld, i);
            for (int c = 0; c < count; c++) {
                // Check buffer safety
                if (pos + strlen(hl) < sizeof(buffer) - 1) {
                    strcpy(buffer + pos, hl);
                    pos += strlen(hl);
                }
            }
            free(hl);
        }
    }
    
    return string_duplicate(buffer);
}
