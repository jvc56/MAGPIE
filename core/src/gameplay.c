#include <stdio.h>

#include "board.h"
#include "cross_set.h"
#include "game.h"
#include "gameplay.h"
#include "move.h"
#include "movegen.h"
#include "rack.h"

void draw_at_most_to_rack(Bag * bag, Rack * rack, int n) {
    while (n > 0 && bag->last_tile_index >= 0) {
        add_letter_to_rack(rack, bag->tiles[bag->last_tile_index], rack->number_of_nonzero_indexes);
        bag->last_tile_index--;
        n--;
    }
}

void play_move_on_board(Game * game, Move * move) {
    // PlaceMoveTiles
    for (int idx = 0; idx < move->tiles_length; idx++) {
        uint8_t letter = move->tiles[idx];
        if (letter == PLAYED_THROUGH_MARKER) {
            continue;
        }
        set_letter(game->gen->board, move->row_start + (move->vertical*idx), move->col_start + ((1-move->vertical)*idx), letter);
        if (letter >= BLANK_OFFSET) {
            letter = BLANK_MACHINE_LETTER;
        }
        take_letter_from_rack(game->players[game->player_on_turn_index]->rack, letter);
    }
    game->gen->board->tiles_played += move->tiles_played;

    // updateAnchorsForMove
    int row = move->row_start;
    int col = move->col_start;
    if (move->vertical) {
        row = move->col_start;
        col = move->row_start;
    }

    for (int i = col; i < move->tiles_length+col; i++) {
        update_anchors(game->gen->board, row, i, move->vertical);
        if (row > 0) {
            update_anchors(game->gen->board, row-1, i, move->vertical);
        }
        if (row < BOARD_DIM - 1) {
            update_anchors(game->gen->board, row+1, i, move->vertical);
        }
    }
    if (col-1 >= 0) {
        update_anchors(game->gen->board, row, col - 1, move->vertical);
    }
    if (move->tiles_length+col < BOARD_DIM) {
        update_anchors(game->gen->board, row, move->tiles_length+col, move->vertical);
    }
}

void calc_for_across(int row_start, int col_start, int csd, Game * game, Move * move) {
    for (int row = row_start; row < move->tiles_length + row_start; row++) {
        if (move->tiles[row-row_start] == PLAYED_THROUGH_MARKER) {
            continue;
        }

        int right_col = word_edge(game->gen->board, row, col_start, WORD_DIRECTION_RIGHT);
        int left_col = word_edge(game->gen->board, row, col_start, WORD_DIRECTION_LEFT);
        gen_cross_set(game->gen->board, row, right_col+1, csd, game->gen->gaddag, game->gen->letter_distribution);
        gen_cross_set(game->gen->board, row, left_col-1, csd, game->gen->gaddag, game->gen->letter_distribution);
        gen_cross_set(game->gen->board, row, col_start, csd, game->gen->gaddag, game->gen->letter_distribution);
    }   
}

void calc_for_self(int row_start, int col_start, int csd, Game * game, Move * move) {
    for (int col = col_start - 1; col <= col_start + move->tiles_length; col++) {
        gen_cross_set(game->gen->board, row_start, col, csd, game->gen->gaddag, game->gen->letter_distribution);
    }
}

void update_cross_set_for_move(Game * game, Move * move) {
    if (move->vertical) {
        calc_for_across(move->row_start, move->col_start, BOARD_HORIZONTAL_DIRECTION, game, move);
        transpose(game->gen->board);
        calc_for_self(move->col_start, move->row_start, BOARD_VERTICAL_DIRECTION, game, move);
        transpose(game->gen->board);
    } else {
        calc_for_self(move->row_start, move->col_start, BOARD_HORIZONTAL_DIRECTION, game, move);
        transpose(game->gen->board);
        calc_for_across(move->col_start, move->row_start, BOARD_VERTICAL_DIRECTION, game, move);
        transpose(game->gen->board);
    }
}

void execute_exchange_move(Game * game, Move * move) {
    for (int i = 0; i < move->tiles_played; i++) {
        take_letter_from_rack(game->players[game->player_on_turn_index]->rack, move->tiles[i]);
    }
    draw_at_most_to_rack(game->gen->bag, game->players[game->player_on_turn_index]->rack, move->tiles_played);
    for (int i = 0; i < move->tiles_played; i++) {
        add_letter(game->gen->bag, move->tiles[i]);
    }
}

void standard_end_of_game_calculations(Game * game) {
    game->players[game->player_on_turn_index]->score += 2 * score_on_rack(game->gen->letter_distribution, game->players[1 - game->player_on_turn_index]->rack);
    game->game_end_reason = GAME_END_REASON_STANDARD;
}

void play_move(Game *  game, Move * move) {
    if (move->move_type == MOVE_TYPE_PLAY) {
        play_move_on_board(game, move);
        update_cross_set_for_move(game, move);
        game->consecutive_scoreless_turns = 0;
        game->players[game->player_on_turn_index]->score += move->score;
        draw_at_most_to_rack(game->gen->bag, game->players[game->player_on_turn_index]->rack, move->tiles_played);
        if (game->players[game->player_on_turn_index]->rack->empty) {
            standard_end_of_game_calculations(game);
        }
    } else if (move->move_type == MOVE_TYPE_PASS) {
        game->consecutive_scoreless_turns++;
    } else if (move->move_type == MOVE_TYPE_EXCHANGE) {
        execute_exchange_move(game, move);
        game->consecutive_scoreless_turns++;
    }

    if (game->consecutive_scoreless_turns == MAX_SCORELESS_TURNS) {
        game->players[0]->score -= score_on_rack(game->gen->letter_distribution, game->players[0]->rack);
        game->players[1]->score -= score_on_rack(game->gen->letter_distribution, game->players[1]->rack);
        game->game_end_reason = GAME_END_REASON_CONSECUTIVE_ZEROS;
    }

    if (game->game_end_reason == GAME_END_REASON_NONE) {
        game->player_on_turn_index = 1 - game->player_on_turn_index;
    }
}
