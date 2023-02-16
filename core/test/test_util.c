#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../src/config.h"
#include "../src/constants.h"
#include "../src/game.h"
#include "../src/leaves.h"
#include "../src/move.h"
#include "../src/rack.h"

#include "alphabet_print.h"
#include "test_constants.h"

void write_string_to_end_of_buffer(char * buffer, char * s) {
    sprintf(buffer + strlen(buffer), "%s", s);
}

void write_spaces_to_end_of_buffer(char * buffer, int n) {
    sprintf(buffer + strlen(buffer), "%*s", n, "");
}

void write_int_to_end_of_buffer(char * buffer, int n) {
    sprintf(buffer + strlen(buffer), "%d", n);
}

void write_char_to_end_of_buffer(char * buffer, char c) {
    sprintf(buffer + strlen(buffer), "%c", c);
}

void write_double_to_end_of_buffer(char * buffer, double d) {
    sprintf(buffer + strlen(buffer), "%0.2f", d);
}

void reset_string(char * string) {
    memset(string, 0, sizeof(*string));
}

int within_epsilon(double a, double b) {
    return fabs(a - b) < EPSILON;
}

double get_leave_value_for_move(Laddag * laddag, Move * move, Rack * rack) {
    int valid_tiles = move->tiles_length;
    if (move->move_type == MOVE_TYPE_EXCHANGE) {
        valid_tiles = move->tiles_played;
    }
    for (int i = 0; i < valid_tiles; i++) {
        if (move->tiles[i] != PLAYED_THROUGH_MARKER) {
            if (move->tiles[i] >= BLANK_OFFSET) {
                take_letter_from_rack(rack, BLANK_MACHINE_LETTER);
            } else {
                take_letter_from_rack(rack, move->tiles[i]);
            }
        }
    }
    go_to_leave(laddag, rack);
    return get_current_value(laddag);
}

void generate_moves_for_game(Game * game) {
    generate_moves(game->gen, game->players[game->player_on_turn_index], game->players[1 - game->player_on_turn_index]->rack, game->gen->bag->last_tile_index + 1 >= RACK_SIZE);
}

void write_rack_to_end_of_buffer(char * dest, Alphabet * alphabet, Rack * rack) {
    for (int i = 0; i < (RACK_ARRAY_SIZE); i++) {
        for (int k = 0; k < rack->array[i]; k++) {
			write_user_visible_letter_to_end_of_buffer(dest, alphabet, i);
        }
    }
}
