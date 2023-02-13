#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <endian.h>

#include "alphabet.h"
#include "config.h"
#include "constants.h"
#include "cross_set.h"
#include "gaddag.h"
#include "leaves.h"
#include "movegen.h"
#include "rack.h"

void go_on(Generator * gen, int current_col, uint8_t L, Rack * rack, Rack * opp_rack, uint32_t new_node_index, uint32_t oldnode_index, int leftstrip, int rightstrip, int unique_play);

void add_letter_to_rack_and_recalculate_leave_index(Generator * gen, Rack * rack, uint8_t letter) {
	add_letter_to_rack(rack, letter);
	traverse_add_edge(gen->laddag, letter);
}

void take_letter_from_rack_and_recalculate_leave_index(Generator * gen, Rack * rack, uint8_t letter) {
	take_letter_from_rack(rack, letter);
	traverse_take_edge(gen->laddag, letter);
}

void set_start_leave_index(Generator * gen, Rack * rack) {
	set_start_leave(gen->laddag, rack);
}

double placement_adjustment(Generator * gen, Move * move) {
	int start = move->col_start;
	int end = start + move->tiles_played;

	int j = start;
	double penalty = 0;
	double v_penalty = OPENING_HOTSPOT_PENALTY;

	while (j < end) {
		if (is_vowel(move->tiles[j-start], gen->gaddag->alphabet) && (j == 2 || j == 6 || j == 8 || j == 12)) {
			penalty += v_penalty;
		}
		j++;
	}
	return penalty;
}

double endgame_adjustment(Generator * gen, Rack * rack, Rack * opp_rack) {
	if (!rack->empty) {
		// This play is not going out. We should penalize it by our own score
		// plus some constant.
		return ((-(double)score_on_rack(gen->letter_distribution, rack)) * 2) - 10;
	}
	return 2 * ((double)score_on_rack(gen->letter_distribution, opp_rack));
}

double get_move_equity(Generator * gen, Rack * rack, Rack * opp_rack, Move * move) {
	double leave_adjustment = 0;
	double other_adjustments = 0;

	if (gen->board->tiles_played == 0 && move->move_type == MOVE_TYPE_PLAY) {
		other_adjustments = placement_adjustment(gen, move);
	}

	if (gen->bag->last_tile_index >= 0) {
		leave_adjustment = get_current_value(gen->laddag);
		int bag_plus_rack_size = (gen->bag->last_tile_index+1) - move->tiles_played + RACK_SIZE;
		if (bag_plus_rack_size < PREENDGAME_ADJUSTMENT_VALUES_LENGTH) {
			other_adjustments += gen->preendgame_adjustment_values[bag_plus_rack_size];
		}
	} else {
		other_adjustments += endgame_adjustment(gen, rack, opp_rack);
	}

	return ((double)move->score) + leave_adjustment + other_adjustments;
}

void record_play(Generator * gen, Rack * rack, Rack * opp_rack, int leftstrip, int rightstrip, int move_type) {
	int start_row = gen->current_row_index;
	int tiles_played = gen->tiles_played;

	int start_col = leftstrip;
	int row = start_row;
	int col = start_col;

	if (gen->vertical) {
		int temp = row;
		row = col;
		col = temp;
	}

	int score = 0;
	uint8_t * strip;

	if (move_type == MOVE_TYPE_PLAY) {
		score = score_move(gen->board, gen->strip, leftstrip, rightstrip, start_row, start_col, tiles_played, !gen->vertical, gen->letter_distribution);
		strip = gen->strip;
	} else if (move_type == MOVE_TYPE_EXCHANGE) {
		// ignore the empty exchange case
		if (rightstrip == 0) {
			return;
		}
		tiles_played = rightstrip;
		strip = gen->exchange_strip;
	}

	if (gen->play_recorder_type == PLAY_RECORDER_TYPE_ALL) {
		// Set the move to more easily handle equity calculations
		Move * move = new_move(gen->move_list);
		set_move_without_equity(move, strip, leftstrip, rightstrip, score, row, col, tiles_played, gen->vertical, move_type);
		double equity;
		if (gen->sorting_parameter == SORT_BY_EQUITY) {
			equity = get_move_equity(gen, rack, opp_rack, move);
		} else {
			equity = score;
		}
		set_move_equity(move, equity);
	} else {
		Move * move = new_top_equity_move(gen->move_list);
		set_move_without_equity(move, strip, leftstrip, rightstrip, score, row, col, tiles_played, gen->vertical, move_type);
		double equity = get_move_equity(gen, rack, opp_rack, move);
		if (gen->move_list->moves[0]->equity < equity) {
			set_move_equity(move, equity);
			set_top_equity_move(gen->move_list);
		}
	}
}

void generate_exchange_moves(Generator * gen, Rack * rack, uint8_t ml, int stripidx) {
	while (ml < (RACK_ARRAY_SIZE) && rack->array[ml] == 0) {
		ml++;
	}
	if (ml == (RACK_ARRAY_SIZE)) {
		// The recording of an exchange should never require
		// the opponent's rack.
		record_play(gen, rack, NULL, 0, stripidx, MOVE_TYPE_EXCHANGE);
	} else {
		generate_exchange_moves(gen, rack, ml+1, stripidx);
		int num_this = rack->array[ml];
		for (int i = 0; i < num_this; i++) {
			gen->exchange_strip[stripidx] = ml;
			stripidx += 1;
			take_letter_from_rack_and_recalculate_leave_index(gen, rack, ml);
			generate_exchange_moves(gen, rack, ml+1, stripidx);
		}
		for (int i = 0; i < num_this; i++) {
			add_letter_to_rack_and_recalculate_leave_index(gen, rack, ml);
		}
	}
}

void recursive_gen(Generator * gen, int col, Rack * rack, Rack * opp_rack, uint32_t node_index, int leftstrip, int rightstrip, int unique_play) {
	int cs_direction;
	uint8_t current_letter = get_letter(gen->board, gen->current_row_index, col);
	if (gen->vertical) {
		cs_direction = BOARD_HORIZONTAL_DIRECTION;
	} else {
		cs_direction = BOARD_VERTICAL_DIRECTION;
	}
	uint64_t cross_set = get_cross_set(gen->board, gen->current_row_index, col, cs_direction);
	if (current_letter != ALPHABET_EMPTY_SQUARE_MARKER) {
		uint32_t next_node_index = get_next_node_index(gen->gaddag, node_index, get_unblanked_machine_letter(current_letter));
		go_on(gen, col, current_letter, rack, opp_rack, next_node_index, node_index, leftstrip, rightstrip, unique_play);
	} else if (!rack->empty) {
		for (uint8_t ml = 0; ml < gen->number_of_possible_letters; ml++)
		{
			if (rack->array[ml] == 0) {
				continue;
			}
			if (allowed(cross_set, ml)) {
				uint32_t next_node_index = get_next_node_index(gen->gaddag, node_index, ml);
				take_letter_from_rack_and_recalculate_leave_index(gen, rack, ml);
				gen->tiles_played++;
				go_on(gen, col, ml, rack, opp_rack, next_node_index, node_index, leftstrip, rightstrip, unique_play);
				add_letter_to_rack_and_recalculate_leave_index(gen, rack, ml);
				gen->tiles_played--;
			}
		}
		if (rack->array[BLANK_MACHINE_LETTER] > 0) {
			for (uint8_t i = 0; i < gen->number_of_possible_letters; i++) {
				if (allowed(cross_set, i)) {
					uint32_t next_node_index = get_next_node_index(gen->gaddag, node_index, i);
					take_letter_from_rack_and_recalculate_leave_index(gen, rack, BLANK_MACHINE_LETTER);
					gen->tiles_played++;
					go_on(gen, col, get_blanked_machine_letter(i), rack, opp_rack, next_node_index, node_index, leftstrip, rightstrip, unique_play);
					add_letter_to_rack_and_recalculate_leave_index(gen, rack, BLANK_MACHINE_LETTER);
					gen->tiles_played--;
				}
			}
		}
	}
}

void go_on(Generator * gen, int current_col, uint8_t L, Rack * rack, Rack * opp_rack, uint32_t new_node_index, uint32_t oldnode_index, int leftstrip, int rightstrip, int unique_play) {
	if (current_col <= gen->current_anchor_col) {
		if (!is_empty(gen->board, gen->current_row_index, current_col)) {
			gen->strip[current_col] = PLAYED_THROUGH_MARKER;
		} else {
			gen->strip[current_col] = L;
			if (gen->vertical && (get_cross_set(gen->board, gen->current_row_index, current_col, BOARD_HORIZONTAL_DIRECTION) == TRIVIAL_CROSS_SET)) {
				unique_play = 1;
			}
		}
		leftstrip = current_col;
		int no_letter_directly_left = (current_col == 0) || is_empty(gen->board, gen->current_row_index, current_col - 1);

		if (in_letter_set(gen->gaddag, L, oldnode_index) && no_letter_directly_left && gen->tiles_played > 0) {
			if (unique_play || gen->tiles_played > 1) {
				record_play(gen, rack, opp_rack, leftstrip, rightstrip, MOVE_TYPE_PLAY);
			}
		}

		if (new_node_index == 0) {
			return;
		}

		if (current_col > 0 && current_col - 1 != gen->last_anchor_col) {
			recursive_gen(gen, current_col - 1, rack, opp_rack, new_node_index, leftstrip, rightstrip, unique_play);
		}
		
		uint32_t separation_node_index = get_next_node_index(gen->gaddag, new_node_index, SEPARATION_MACHINE_LETTER);
		if (separation_node_index != 0 && no_letter_directly_left && gen->current_anchor_col < BOARD_DIM - 1) {
			recursive_gen(gen, gen->current_anchor_col+1, rack, opp_rack, separation_node_index, leftstrip, rightstrip, unique_play);
		}
	} else {
		if (!is_empty(gen->board, gen->current_row_index, current_col)) {
			gen->strip[current_col] = PLAYED_THROUGH_MARKER;
		} else {
			gen->strip[current_col] = L;
			if (gen->vertical && (get_cross_set(gen->board, gen->current_row_index, current_col, BOARD_HORIZONTAL_DIRECTION) == TRIVIAL_CROSS_SET)) {
				unique_play = 1;
			}
		}
		rightstrip = current_col;
		int no_letter_directly_right = (current_col == BOARD_DIM - 1) || is_empty(gen->board, gen->current_row_index, current_col + 1);

		if (in_letter_set(gen->gaddag, L, oldnode_index) && no_letter_directly_right && gen->tiles_played > 0) {
			if (unique_play || gen->tiles_played > 1) {
				record_play(gen, rack, opp_rack, leftstrip, rightstrip, MOVE_TYPE_PLAY);
			}
		}

		if (new_node_index != 0 && current_col < BOARD_DIM - 1) {
			recursive_gen(gen, current_col+1, rack, opp_rack, new_node_index, leftstrip, rightstrip, unique_play);
		}
	}
}

void gen_by_orientation(Generator * gen, Rack * rack, Rack * opp_rack, int dir) {
	// genByOrientation
	for (int row = 0; row < BOARD_DIM; row++)
	{
		gen->current_row_index = row;
		gen->last_anchor_col = 100;
		for (int col = 0; col < BOARD_DIM; col++)
		{
			if (get_anchor(gen->board, row, col, dir)) {
				gen->current_anchor_col = col;
				recursive_gen(gen, col, rack, opp_rack, 0, col, col, !gen->vertical);
				gen->last_anchor_col = col;
			}
		}
	}
}

void generate_moves(Generator * gen, Rack * rack, Rack * opp_rack, int add_exchange) {
	// Add plays
	set_start_leave_index(gen, rack);
	for (int dir = 0; dir < 2; dir++)
	{
		gen->vertical = dir%2 != 0;
		gen_by_orientation(gen, rack, opp_rack, dir);
		transpose(gen->board);
	}

	// Add exchanges
	if (add_exchange) {
		generate_exchange_moves(gen, rack, 0, 0);
	}

	// Add the pass move
	if (gen->play_recorder_type == PLAY_RECORDER_TYPE_ALL || gen->move_list->moves[0]->equity < PASS_MOVE_EQUITY) {
		set_move(gen->move_list->moves[gen->move_list->count], gen->strip, 0, 0, 0, PASS_MOVE_EQUITY, 0, 0, 0, 0, MOVE_TYPE_PASS);
		gen->move_list->count++;
	}

	if (gen->move_list->count > 1) {
		// If there is more than one move, we need to sort the
		// list by equity, which could be just the score
		// if the sorting parameter is SORT_BY_SCORE.
		sort_move_list(gen->move_list);
	}
}

void set_gen_sorting_parameter(Generator * gen, int move_sorting) {
	gen->sorting_parameter = move_sorting;
	return;
}

void set_gen_play_recorder_type(Generator * gen, int play_recorder_type) {
	gen->play_recorder_type = play_recorder_type;
	return;
}

void reset_generator(Generator * gen) {
	reset_bag(gen->bag, gen->letter_distribution);
	reset_board(gen->board);
	reset_move_list(gen->move_list);
}

void load_quackle_preendgame_adjustment_values(Generator * gen) {
	double values[] = {0, -8, 0, -0.5, -2, -3.5, -2, 2, 10, 7, 4, -1, -2};
	for (int i = 0; i < PREENDGAME_ADJUSTMENT_VALUES_LENGTH; i++) {
		gen->preendgame_adjustment_values[i] = values[i];
	}
}

void load_zero_preendgame_adjustment_values(Generator * gen) {
	for (int i = 0; i < PREENDGAME_ADJUSTMENT_VALUES_LENGTH; i++) {
		gen->preendgame_adjustment_values[i] = 0;
	}
}

Generator * create_generator(Config * config) {
    Generator * generator = malloc(sizeof(Generator));
	generator->bag = create_bag(config->letter_distribution);
    generator->board = create_board();
	generator->move_list = create_move_list();
    generator->gaddag = config->gaddag;
    generator->letter_distribution = config->letter_distribution;
	generator->laddag = config->laddag;
    generator->number_of_possible_letters = get_number_of_letters(generator->gaddag->alphabet);
    generator->sorting_parameter = config->move_sorting;
	generator->play_recorder_type = config->play_recorder_type;
	generator->tiles_played = 0;
	generator->vertical = 0;
	generator->last_anchor_col = 0;

	// Just load the zero values for now
	load_zero_preendgame_adjustment_values(generator);

	return generator;
}

void destroy_generator(Generator * gen) {
	destroy_bag(gen->bag);
	destroy_board(gen->board);
	destroy_move_list(gen->move_list);
	free(gen);
}