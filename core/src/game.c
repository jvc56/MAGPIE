#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alphabet.h"
#include "board.h"
#include "config.h"
#include "cross_set.h"
#include "gaddag.h"
#include "game.h"
#include "movegen.h"
#include "player.h"

char add_player_score(const char* cgp, int *cgp_index, Game * game, int player_index) {
	char cgp_char = cgp[*cgp_index];
	char score[10] = "";
	while (cgp_char != '/' && cgp_char != ' ') {
		sprintf(score + strlen(score), "%c", cgp_char);
		(*cgp_index)++;
		cgp_char = cgp[*cgp_index];
	}
	game->players[player_index]->score = atoi(score);
	return cgp_char;
}

void draw_letter_to_rack(Bag * bag, Rack * rack, uint8_t letter) {
    draw_letter(bag, letter);
    add_letter_to_rack(rack, letter, rack->number_of_nonzero_indexes);
}

char add_player_rack(const char* cgp, int *cgp_index, Game * game, int player_index) {
	char cgp_char = cgp[*cgp_index];
	while (cgp_char != '/' && cgp_char != ' ') {
		draw_letter_to_rack(game->gen->bag, game->players[player_index]->rack, val(game->gen->gaddag->alphabet, cgp_char));
		(*cgp_index)++;
		cgp_char = cgp[*cgp_index];
	}
	return cgp_char;
}

void load_cgp(Game * game, const char* cgp) {

	// Set all tiles:
	int cgp_index = 0;
	char cgp_char = cgp[cgp_index];
	int current_board_index = 0;
	int is_digit = 0;
	int previous_was_digit = 0;
	char current_digits[5] = "";
	while (cgp_char != ' ') {
		is_digit = isdigit(cgp_char);
		if (is_digit) {
		    sprintf(current_digits + strlen(current_digits), "%c", cgp_char);
		} else if (previous_was_digit) {
			current_board_index += atoi(current_digits);
			current_digits[0] = '\0';
		}
		if (isalpha(cgp_char)) {
			set_letter_by_index(game->gen->board, current_board_index, val(game->gen->gaddag->alphabet, cgp_char));
			draw_letter(game->gen->bag, get_letter_by_index(game->gen->board, current_board_index));
			current_board_index++;
            game->gen->board->tiles_played++;
		}
		cgp_index++;
		cgp_char = cgp[cgp_index];
		previous_was_digit = is_digit;
	}

	// Skip the whitespace
	while (cgp_char == ' ') {
		cgp_index++;
		cgp_char = cgp[cgp_index];
	}

	// Set the racks
	int player_index = 0;
	if (cgp_char == '/') {
		// player0 has an empty rack
		player_index = 1;
		// Advance the pointer
		cgp_index++;
	}

	cgp_char = add_player_rack(cgp, &cgp_index, game, player_index);

	if (cgp_char == '/') {
		player_index = 1;
		// Advance the pointer
		cgp_index++;
		cgp_char = add_player_rack(cgp, &cgp_index, game, 1);
	}

	// Skip the whitespace
	while (cgp_char == ' ') {
		cgp_index++;
		cgp_char = cgp[cgp_index];
	}

	add_player_score(cgp, &cgp_index, game, 0);
	cgp_index++;
	add_player_score(cgp, &cgp_index, game, 1);
    cgp_index++;

	cgp_char = cgp[cgp_index];
	// Skip the whitespace
	while (cgp_char == ' ') {
		cgp_index++;
		cgp_char = cgp[cgp_index];
	}

	// Set number of consecutive zeros
	game->consecutive_scoreless_turns = cgp_char - '0';
	game->player_on_turn_index = 0;

	generate_all_cross_sets(game->gen->board, game->gen->gaddag, game->gen->letter_distribution);
	update_all_anchors(game->gen->board);

	if (game->consecutive_scoreless_turns >= MAX_SCORELESS_TURNS) {
		game->game_end_reason = GAME_END_REASON_CONSECUTIVE_ZEROS;
	} else if (game->gen->bag->last_tile_index == -1 && (game->players[0]->rack->empty || game->players[1]->rack->empty)) {
		game->game_end_reason = GAME_END_REASON_STANDARD;
	} else {
		game->game_end_reason = GAME_END_REASON_NONE;
	}
}

void reset_game(Game *  game) {
	reset_generator(game->gen);
	reset_player(game->players[0]);
	reset_player(game->players[1]);
	game->player_on_turn_index = 0;
	game->consecutive_scoreless_turns = 0;
	game->game_end_reason = GAME_END_REASON_NONE;
}

Game * create_game(Config * config) {
	Game * game = malloc(sizeof(Game));
	game->gen = create_generator(config);
	game->players[0] = create_player("player_1");
	game->players[1] = create_player("player_2");
	game->players[0]->strategy_params = config->player_1_strategy_params;
	game->players[1]->strategy_params = config->player_2_strategy_params;
	game->player_on_turn_index = 0;
	game->consecutive_scoreless_turns = 0;
	game->game_end_reason = GAME_END_REASON_NONE;
	return game;
}

void destroy_game(Game * game) {
    destroy_generator(game->gen);
	destroy_player(game->players[0]);
	destroy_player(game->players[1]);
    free(game);
}
