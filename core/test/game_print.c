#include <stdio.h>
#include <string.h>

#include "../src/alphabet.h"
#include "../src/rack.h"

#include "alphabet_print.h"
#include "bag_print.h"
#include "game_print.h"
#include "move_print.h"
#include "test_util.h"

void write_player_row_to_end_of_buffer(char * buf, Alphabet * alphabet, Player * player, char * marker) {
	write_string_to_end_of_buffer(buf, marker);
	write_string_to_end_of_buffer(buf, player->name);
	write_spaces_to_end_of_buffer(buf, 25 - strlen(player->name));
	write_rack_to_end_of_buffer(buf, alphabet, player->rack);
	write_spaces_to_end_of_buffer(buf, 10 - player->rack->number_of_letters);
	write_int_to_end_of_buffer(buf, player->score);
}

void write_board_row_to_end_of_buffer(char * buf, Alphabet * alphabet, Board * board, int row) {
	sprintf(buf + strlen(buf), "%2d|", row + 1);
	for (int i = 0; i < BOARD_DIM; i++) {
		uint8_t current_letter = get_letter(board, row, i);
		if (current_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
			write_char_to_end_of_buffer(buf, CROSSWORD_GAME_BOARD[(row*BOARD_DIM) + i]);
		} else {
			write_user_visible_letter_to_end_of_buffer(buf, alphabet, current_letter);
		}
		write_string_to_end_of_buffer(buf, " ");
	}
	write_string_to_end_of_buffer(buf, "|");
}

void print_game(Game * game) {
	char gs[1440] = "";

	char * player_0_on_turn_marker = "-> ";
	char * player_1_on_turn_marker = "   ";

	if (game->player_on_turn_index == 1) {
		char * temp = player_0_on_turn_marker;
		player_0_on_turn_marker = player_1_on_turn_marker;
		player_1_on_turn_marker = temp;
	}

	write_string_to_end_of_buffer(gs, "   A B C D E F G H I J K L M N O   ");
	write_player_row_to_end_of_buffer(gs, game->gen->kwg->alphabet, game->players[0], player_0_on_turn_marker);
	write_string_to_end_of_buffer(gs, "\n   ------------------------------  ");
	write_player_row_to_end_of_buffer(gs, game->gen->kwg->alphabet, game->players[1], player_1_on_turn_marker);
	write_string_to_end_of_buffer(gs, "\n");

	for (int i = 0; i < BOARD_DIM; i++) {
		write_board_row_to_end_of_buffer(gs, game->gen->kwg->alphabet, game->gen->board, i);
		if (i == 0) {
			write_string_to_end_of_buffer(gs, " --Tracking-----------------------------------");
		} else if (i == 1) {
			write_string_to_end_of_buffer(gs, " ");
			write_bag_to_end_of_buffer(gs, game->gen->bag, game->gen->kwg->alphabet);
			write_string_to_end_of_buffer(gs, "  ");
			write_int_to_end_of_buffer(gs, game->gen->bag->last_tile_index + 1);
		} else if (i - 2 < game->gen->move_list->count) {
			char move_string[24] = "";
			write_user_visible_move_to_end_of_buffer(move_string, game->gen->board, game->gen->move_list->moves[i-2], game->gen->kwg->alphabet);
			sprintf(gs + strlen(gs), " %-3d %-24s %0.2f", i-1, move_string, game->gen->move_list->moves[i-2]->equity);
		}
		write_string_to_end_of_buffer(gs, "\n");
	}

	write_string_to_end_of_buffer(gs, "   ------------------------------\n");
	printf("%s", gs);
}