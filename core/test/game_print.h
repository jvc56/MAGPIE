#ifndef GAME_PRINT_H
#define GAME_PRINT_H

#include "../src/alphabet.h"
#include "../src/board.h"
#include "../src/player.h"
#include "../src/game.h"

#include "game_print.h"

void print_game(Game * game);
void write_player_row_to_end_of_buffer(char * buf, Alphabet * alphabet, Player * player, char * marker);
void write_board_row_to_end_of_buffer(char * buf, Alphabet * alphabet, Board * board, int row);

#endif