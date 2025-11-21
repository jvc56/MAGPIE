#include "qt_bridge.h"

#include "../../ent/game_history.h"
#include "../../ent/game.h"
#include "../../ent/board.h"
#include "../../ent/board_layout.h"
#include "../../ent/letter_distribution.h"
#include "../../ent/players_data.h"
#include "../../ent/validated_move.h"
#include "../../ent/move.h"
#include "../../ent/rack.h"
#include "../../str/move_string.h"
#include "../../impl/gcg.h"
#include "../../impl/gameplay.h"
#include "../../util/io_util.h"
#include "../../util/string_util.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// Wrapper struct to manage lifetime of game and its dependencies
struct _BridgeGame {
    Game *game;
    LetterDistribution *ld;
    PlayersData *pd;
    BoardLayout *bl;
    int bingo_bonus;
    int game_variant;
};

// Cast opaque handles to actual types
#define TO_GH(x) ((GameHistory*)(x))
#define TO_GAME(x) (((BridgeGame*)(x))->game)

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
    BridgeGame *b_game = calloc(1, sizeof(BridgeGame));

    const char *lexiconName = game_history_get_lexicon_name(TO_GH(gh));
    if (!lexiconName) lexiconName = "CSW24"; // Default lexicon
    const char *ldName = game_history_get_ld_name(TO_GH(gh));
    
    b_game->ld = ld_create(data_path, ldName ? ldName : "CSW24", err);
    if (!error_stack_is_empty(err)) {
        bridge_game_destroy(b_game);
        error_stack_destroy(err);
        return NULL;
    }
    
    b_game->pd = players_data_create();
    players_data_set(b_game->pd, PLAYERS_DATA_TYPE_KWG, data_path, lexiconName, lexiconName, err);
    if (!error_stack_is_empty(err)) {
        bridge_game_destroy(b_game);
        error_stack_destroy(err);
        return NULL;
    }
    
    const char *layoutName = game_history_get_board_layout_name(TO_GH(gh));
    if (!layoutName) layoutName = board_layout_get_default_name();
    
    b_game->bl = board_layout_create();
    board_layout_load(b_game->bl, data_path, layoutName, err);
    if (!error_stack_is_empty(err)) {
        bridge_game_destroy(b_game);
        error_stack_destroy(err);
        return NULL;
    }
    
    b_game->bingo_bonus = 50;
    b_game->game_variant = (int)game_history_get_game_variant(TO_GH(gh));

    GameArgs gameArgs;
    memset(&gameArgs, 0, sizeof(GameArgs));
    gameArgs.players_data = b_game->pd;
    gameArgs.board_layout = b_game->bl;
    gameArgs.ld = b_game->ld;
    gameArgs.bingo_bonus = b_game->bingo_bonus;
    gameArgs.game_variant = (game_variant_t)b_game->game_variant;

    b_game->game = game_create(&gameArgs);
    
    error_stack_destroy(err);
    return b_game;
}

void bridge_game_destroy(BridgeGame* game) {
    if (!game) return;
    if(game->game) game_destroy(game->game);
    if(game->ld) ld_destroy(game->ld);
    if(game->pd) players_data_destroy(game->pd);
    if(game->bl) board_layout_destroy(game->bl);
    free(game);
}

void bridge_game_play_to_index(BridgeGameHistory* gh, BridgeGame* game, int index) {
    if (!game) return;
    ErrorStack *err = error_stack_create();
    
    if (game->game) {
        game_destroy(game->game);
    }

    GameArgs gameArgs;
    memset(&gameArgs, 0, sizeof(GameArgs));
    gameArgs.players_data = game->pd;
    gameArgs.board_layout = game->bl;
    gameArgs.ld = game->ld;
    gameArgs.bingo_bonus = game->bingo_bonus;
    gameArgs.game_variant = (game_variant_t)game->game_variant;

    game->game = game_create(&gameArgs);

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

static char* internal_format_rack(const Rack *r, const LetterDistribution *ld) {
    if (!r || !ld) return string_duplicate("");
    char buffer[64] = {0};
    int pos = 0;
    for (int i = 0; i < ld_get_size(ld); i++) {
        int count = rack_get_letter(r, i);
        if (count > 0) {
            char *hl = ld_ml_to_hl(ld, i);
            for (int c = 0; c < count; c++) {
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

char* bridge_get_current_rack(BridgeGame* game) {
    if (!game) return string_duplicate("");
    
    int playerIdx = game_get_player_on_turn_index(TO_GAME(game));
    Player *p = game_get_player(TO_GAME(game), playerIdx);
    Rack *r = player_get_rack(p);
    const LetterDistribution *ld = game_get_ld(TO_GAME(game));
    
    return internal_format_rack(r, ld);
}

// Get number of tiles in bag
int bridge_get_bag_count(BridgeGame* game) {
    if (!game) return 0;
    Bag *bag = game_get_bag(TO_GAME(game));
    int actual_bag = bag_get_letters(bag);
    
    // Correction for incomplete racks in annotated games.
    // Ghost tiles (tiles belonging to opponent but not in their rack) reside in the bag.
    // We subtract them to show the "True" bag count.
    
    int playerIdx = game_get_player_on_turn_index(TO_GAME(game));
    int opponentIdx = 1 - playerIdx;
    Player *p1 = game_get_player(TO_GAME(game), playerIdx);
    Player *p2 = game_get_player(TO_GAME(game), opponentIdx);
    Board *board = game_get_board(TO_GAME(game));
    const LetterDistribution *ld = game_get_ld(TO_GAME(game));
    
    int total_tiles = ld_get_total_tiles(ld);
    int tiles_on_board = board_get_tiles_played(board);
    int p1_tiles = rack_get_total_letters(player_get_rack(p1));
    int p2_tiles = rack_get_total_letters(player_get_rack(p2)); // Actual opponent rack
    
    int remaining_off_board_p1 = total_tiles - tiles_on_board - p1_tiles;
    
    // Expected opponent tiles is usually 7, but capped by what's remaining
    int expected_p2_tiles = 7;
    if (expected_p2_tiles > remaining_off_board_p1) {
        expected_p2_tiles = remaining_off_board_p1;
    }
    
    int missing_p2_tiles = expected_p2_tiles - p2_tiles;
    if (missing_p2_tiles < 0) missing_p2_tiles = 0;
    
    int corrected_bag = actual_bag - missing_p2_tiles;
    if (corrected_bag < 0) corrected_bag = 0;
    
    printf("BAG_DEBUG: Actual=%d, P1=%d, P2_Actual=%d, Board=%d, Total=%d, Expected_P2=%d, Missing_P2=%d, Corrected=%d\n", 
           actual_bag, p1_tiles, p2_tiles, tiles_on_board, total_tiles, expected_p2_tiles, missing_p2_tiles, corrected_bag);

    return corrected_bag;
}

// Get unseen tiles (bag + opponent rack)
// Returns string in *tiles (caller must free), and counts
void bridge_get_unseen_tiles(BridgeGame* game, char** tiles, int* vowel_count, int* consonant_count, int* blank_count) {
    if (!game) return;
    
    int playerIdx = game_get_player_on_turn_index(TO_GAME(game));
    int opponentIdx = 1 - playerIdx;
    
    Bag *bag = game_get_bag(TO_GAME(game));
    Player *opponent = game_get_player(TO_GAME(game), opponentIdx);
    Rack *opponentRack = player_get_rack(opponent);
    const LetterDistribution *ld = game_get_ld(TO_GAME(game));
    Board *board = game_get_board(TO_GAME(game));

    int unseen_counts[MAX_ALPHABET_SIZE];
    memset(unseen_counts, 0, sizeof(unseen_counts));
    
    // Add bag tiles
    bag_increment_unseen_count(bag, unseen_counts);
    
    // Add opponent rack tiles
    for (int i = 0; i < ld_get_size(ld); i++) {
        unseen_counts[i] += rack_get_letter(opponentRack, i);
    }
    
    StringBuilder *sb = string_builder_create();
    int v = 0;
    int c = 0;
    int b = 0;
    
    for (int i = 0; i < ld_get_size(ld); i++) {
        if (unseen_counts[i] > 0) {
            // Check if blank
            if (bridge_is_blank((uint8_t)i) || i == BLANK_MACHINE_LETTER) {
                b += unseen_counts[i];
            }
            else if (ld_get_is_vowel(ld, i)) {
                v += unseen_counts[i];
            } else {
                c += unseen_counts[i];
            }
            
            char *hl = ld_ml_to_hl(ld, i);
            for (int k = 0; k < unseen_counts[i]; k++) {
                string_builder_add_string(sb, hl);
            }
            free(hl);
            
            string_builder_add_string(sb, " ");
        }
    }
    
    if (tiles) *tiles = string_duplicate(string_builder_peek(sb));
    string_builder_destroy(sb);

    // Invariant Calculation for Unknowns
    int total_tiles = ld_get_total_tiles(ld);
    int tiles_on_board = board_get_tiles_played(board);
    int p1_tiles = rack_get_total_letters(player_get_rack(game_get_player(TO_GAME(game), playerIdx)));
    int bag_tiles = bag_get_letters(bag);
    int opp_tiles = rack_get_total_letters(opponentRack);

    int true_unseen = total_tiles - tiles_on_board - p1_tiles;
    int visible_unseen = bag_tiles + opp_tiles;
    int unknown = true_unseen - visible_unseen;
    
    if (unknown < 0) unknown = 0; 
    
    printf("UNSEEN_DEBUG: Total=%d, Board=%d, P1=%d, Bag=%d, Opp=%d, True_Unseen=%d, Visible=%d, Unknown=%d\n",
           total_tiles, tiles_on_board, p1_tiles, bag_tiles, opp_tiles, true_unseen, visible_unseen, unknown);

    if (vowel_count) *vowel_count = v;
    if (consonant_count) *consonant_count = c;
    if (blank_count) *blank_count = b + unknown; 

}

void bridge_get_event_details(BridgeGameHistory* gh, BridgeGame* game, int index,
                              int* player_index, int* type, char** move_str, char** rack_str,
                              int* score, int* cumulative_score) {
    if (!gh || !game) return;

    GameEvent *event = game_history_get_event(TO_GH(gh), index);
    if (!event) return;

    if (player_index) *player_index = game_event_get_player_index(event);
    if (type) *type = (int)game_event_get_type(event);
    if (score) *score = equity_to_int(game_event_get_move_score(event));
    if (cumulative_score) *cumulative_score = equity_to_int(game_event_get_cumulative_score(event));
    
    if (move_str) {
        char *human_readable = NULL;
        
        ValidatedMoves *vms = game_event_get_vms(event);
        if (vms && validated_moves_get_number_of_moves(vms) > 0) {
            const Move *move = validated_moves_get_move(vms, 0);
            game_event_t type = move_get_type(move);

            if (type == GAME_EVENT_TILE_PLACEMENT_MOVE || 
                type == GAME_EVENT_EXCHANGE || 
                type == GAME_EVENT_PASS) {
                
                // Temporarily rewind game to state before this move to get proper board state
                // This is inefficient (O(N^2)) but ensures accurate notation generation
                Game *temp_game = TO_GAME(game);
                ErrorStack *err = error_stack_create();
                game_reset(temp_game);
                game_play_n_events(TO_GH(gh), temp_game, index, true, err);
                error_stack_destroy(err);
                
                Board *board = game_get_board(temp_game);
                const LetterDistribution *ld = game_get_ld(temp_game);

                StringBuilder *sb = string_builder_create();
                string_builder_add_human_readable_move(sb, move, board, ld);
                
                human_readable = string_duplicate(string_builder_peek(sb));
                string_builder_destroy(sb);
            }
        }
        
        if (!human_readable) {
            game_event_t type = game_event_get_type(event);
            switch (type) {
                case GAME_EVENT_CHALLENGE_BONUS:
                    human_readable = string_duplicate("challenged");
                    break;
                case GAME_EVENT_PHONY_TILES_RETURNED:
                    human_readable = string_duplicate("phony");
                    break;
                case GAME_EVENT_TIME_PENALTY:
                    human_readable = string_duplicate("time");
                    break;
                case GAME_EVENT_END_RACK_POINTS: {
                    const Rack *r = game_event_get_const_rack(event);
                    char *tiles = internal_format_rack(r, game_get_ld(TO_GAME(game)));
                    char buf[128];
                    snprintf(buf, sizeof(buf), "2x %s", tiles);
                    free(tiles);
                    human_readable = string_duplicate(buf);
                    break;
                }
                case GAME_EVENT_END_RACK_PENALTY:
                    human_readable = string_duplicate("rack penalty");
                    break;
                default:
                    break;
            }
        }

        if (human_readable) {
            *move_str = human_readable;
        } else {
            const char *s = game_event_get_cgp_move_string(event);
            *move_str = s ? string_duplicate(s) : string_duplicate("");
        }
    }
    
    if (rack_str) {
        const Rack *r = game_event_get_const_rack(event);
        if (r) {
            *rack_str = internal_format_rack(r, game_get_ld(TO_GAME(game)));
        } else {
            *rack_str = string_duplicate("");
        }
    }
}

int bridge_get_last_move_tiles(BridgeGameHistory* gh, int index, int* rows, int* cols, int max_count) {
    if (!gh) return 0;
    
    GameEvent *event = game_history_get_event(TO_GH(gh), index);
    if (!event) return 0;
    
    ValidatedMoves *vms = game_event_get_vms(event);
    if (!vms || validated_moves_get_number_of_moves(vms) == 0) return 0;
    
    const Move *move = validated_moves_get_move(vms, 0);
    if (!move) return 0;
    
    if (move->move_type != GAME_EVENT_TILE_PLACEMENT_MOVE) return 0;
    
    int count = 0;
    int r = move->row_start;
    int c = move->col_start;
    int ri = (move->dir == BOARD_VERTICAL_DIRECTION) ? 1 : 0;
    int ci = (move->dir == BOARD_HORIZONTAL_DIRECTION) ? 1 : 0;
    
    for (int i = 0; i < move->tiles_length; i++) {
        // Check if tile is placed. Assuming 0 means existing tile.
        if (move->tiles[i] != 0) {
            if (rows && cols && count < max_count) {
                rows[count] = r;
                cols[count] = c;
            }
            count++;
        }
        r += ri;
        c += ci;
    }
    
    return count;
}