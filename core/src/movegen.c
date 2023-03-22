#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "alphabet.h"
#include "config.h"
#include "constants.h"
#include "cross_set.h"
#include "kwg.h"
#include "leaves.h"
#include "movegen.h"
#include "player.h"
#include "rack.h"

void go_on(Generator * gen, int current_col, uint8_t L, Player * player, Rack * opp_rack, uint32_t new_node_index, int accepts, int leftstrip, int rightstrip, int unique_play);

void add_letter_to_rack_and_recalculate_leave_index(Player * player, uint8_t letter) {
	add_letter_to_rack(player->rack, letter);
	traverse_add_edge(player->strategy_params->laddag, letter);
}

void take_letter_from_rack_and_recalculate_leave_index(Player * player, uint8_t letter) {
	take_letter_from_rack(player->rack, letter);
	traverse_take_edge(player->strategy_params->laddag, letter);
}

void set_start_leave_index(Player * player) {
	set_start_leave(player->strategy_params->laddag, player->rack);
}

double placement_adjustment(Generator * gen, Move * move) {
	int start = move->col_start;
	int end = start + move->tiles_played;

	int j = start;
	double penalty = 0;
	double v_penalty = OPENING_HOTSPOT_PENALTY;

	while (j < end) {
		if (is_vowel(move->tiles[j-start], gen->kwg->alphabet) && (j == 2 || j == 6 || j == 8 || j == 12)) {
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

double get_spare_move_equity(Generator * gen, Player * player, Rack * opp_rack) {
	double leave_adjustment = 0;
	double other_adjustments = 0;

	if (gen->board->tiles_played == 0 && gen->move_list->spare_move->move_type == MOVE_TYPE_PLAY) {
		other_adjustments = placement_adjustment(gen, gen->move_list->spare_move);
	}

	if (gen->bag->last_tile_index >= 0) {
		leave_adjustment = get_current_value(player->strategy_params->laddag);
		int bag_plus_rack_size = (gen->bag->last_tile_index+1) - gen->move_list->spare_move->tiles_played + RACK_SIZE;
		if (bag_plus_rack_size < PREENDGAME_ADJUSTMENT_VALUES_LENGTH) {
			other_adjustments += gen->preendgame_adjustment_values[bag_plus_rack_size];
		}
	} else {
		other_adjustments += endgame_adjustment(gen, player->rack, opp_rack);
	}

	return ((double)gen->move_list->spare_move->score) + leave_adjustment + other_adjustments;
}

void record_play(Generator * gen, Player * player, Rack * opp_rack, int leftstrip, int rightstrip, int move_type, int score) {
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

	uint8_t * strip = NULL;

	if (move_type == MOVE_TYPE_PLAY) {
		strip = gen->strip;
	} else if (move_type == MOVE_TYPE_EXCHANGE) {
		// ignore the empty exchange case
		if (rightstrip == 0) {
			return;
		}
		tiles_played = rightstrip;
		strip = gen->exchange_strip;
	}

	// Set the move to more easily handle equity calculations
	set_spare_move(gen->move_list, strip, leftstrip, rightstrip, score, row, col, tiles_played, gen->vertical, move_type);

	if (player->strategy_params->play_recorder_type == PLAY_RECORDER_TYPE_ALL) {
		double equity;
		if (player->strategy_params->move_sorting == SORT_BY_EQUITY) {
			equity = get_spare_move_equity(gen, player, opp_rack);
		} else {
			equity = score;
		}
		insert_spare_move(gen->move_list, equity);
	} else {
		insert_spare_move_top_equity(gen->move_list, get_spare_move_equity(gen, player, opp_rack));
	}
}

void generate_exchange_moves(Generator * gen, Player * player, uint8_t ml, int stripidx) {
	while (ml < (gen->letter_distribution->size) && player->rack->array[ml] == 0) {
		ml++;
	}
	if (ml == (gen->letter_distribution->size)) {
		// The recording of an exchange should never require
		// the opponent's rack.
		record_play(gen, player, NULL, 0, stripidx, MOVE_TYPE_EXCHANGE, 0);
	} else {
		generate_exchange_moves(gen, player, ml+1, stripidx);
		int num_this = player->rack->array[ml];
		for (int i = 0; i < num_this; i++) {
			gen->exchange_strip[stripidx] = ml;
			stripidx += 1;
			take_letter_from_rack_and_recalculate_leave_index(player, ml);
			generate_exchange_moves(gen, player, ml+1, stripidx);
		}
		for (int i = 0; i < num_this; i++) {
			add_letter_to_rack_and_recalculate_leave_index(player, ml);
		}
	}
}

void recursive_gen(Generator * gen, int col, Player * player, Rack * opp_rack, uint32_t node_index, int leftstrip, int rightstrip, int unique_play) {
	int cs_direction;
	uint8_t current_letter = get_letter(gen->board, gen->current_row_index, col);
	if (gen->vertical) {
		cs_direction = BOARD_HORIZONTAL_DIRECTION;
	} else {
		cs_direction = BOARD_VERTICAL_DIRECTION;
	}
	uint64_t cross_set = get_cross_set(gen->board, gen->current_row_index, col, cs_direction);
	if (current_letter != ALPHABET_EMPTY_SQUARE_MARKER) {
		int raw = get_unblanked_machine_letter(current_letter);
		int next_node_index = 0;
		int accepts = 0;
		int i = node_index;
		while(1) {
			if (kwg_tile(gen->kwg, i) == raw) {
				next_node_index = kwg_arc_index(gen->kwg, i);
				accepts = kwg_accepts(gen->kwg, i);
				break;
			}
			if (kwg_is_end(gen->kwg, i)) {
				break;
			}
			i++;
		}
		// printf("calling go_on with: %d\n", current_letter);
		go_on(gen, col, current_letter, player, opp_rack, next_node_index, accepts, leftstrip, rightstrip, unique_play);
	} else if (!player->rack->empty) {
		int i = node_index;
		while(1) {
			int ml = kwg_tile(gen->kwg, i);
			if (ml != 0 && (player->rack->array[ml] != 0 || player->rack->array[0] != 0) && allowed(cross_set, ml)) {
				int next_node_index = kwg_arc_index(gen->kwg, i);
				int accepts = kwg_accepts(gen->kwg, i);
				if (player->rack->array[ml] > 0) {
					take_letter_from_rack_and_recalculate_leave_index(player, ml);
					gen->tiles_played++;
					// printf("calling go_on with: %d\n", ml);
					go_on(gen, col, ml, player, opp_rack, next_node_index, accepts, leftstrip, rightstrip, unique_play);
					gen->tiles_played--;
					// printf("adding back: %d\n", ml);
					add_letter_to_rack_and_recalculate_leave_index(player, ml);
				}
				// check blank
				if (player->rack->array[0] > 0) {
					take_letter_from_rack_and_recalculate_leave_index(player, BLANK_MACHINE_LETTER);
					gen->tiles_played++;
					// printf("calling go_on with: %d\n", get_blanked_machine_letter(ml));
					go_on(gen, col, get_blanked_machine_letter(ml), player, opp_rack, next_node_index, accepts, leftstrip, rightstrip, unique_play);
					gen->tiles_played--;
					add_letter_to_rack_and_recalculate_leave_index(player, BLANK_MACHINE_LETTER);
				}
			}
			if (kwg_is_end(gen->kwg, i)) {
				// printf("REACHED END\n");
				break;
			}
			i++;
		}
	}
	// printf("DONE WITH RECUR GEN\n");
}

void go_on(Generator * gen, int current_col, uint8_t L, Player * player, Rack * opp_rack, uint32_t new_node_index, int accepts, int leftstrip, int rightstrip, int unique_play) {	
	// Set the incremental score
	gen->incremental_score_index++;
	uint8_t bonus_square = get_bonus_square(gen->board, gen->current_row_index, current_col);
	int letter_multiplier = 1;
	int this_word_multiplier = 1;
	int square_is_empty = is_empty(gen->board, gen->current_row_index, current_col);
	if (square_is_empty) {
		this_word_multiplier = bonus_square / 16;
		letter_multiplier = bonus_square % 16;
		// printf("setting inc word mult: %d: %d * %d\n", gen->incremental_score_index - 1, this_word_multiplier, gen->incremental_word_multiplier[gen->incremental_score_index - 1]);
		gen->incremental_word_multiplier[gen->incremental_score_index] = this_word_multiplier * gen->incremental_word_multiplier[gen->incremental_score_index - 1];
	} else {
		gen->incremental_word_multiplier[gen->incremental_score_index] = gen->incremental_word_multiplier[gen->incremental_score_index - 1];
	}
	int cs = get_cross_score(gen->board, gen->current_row_index, current_col, !gen->vertical);
	int letter_score;
	if (is_blanked(L)) {
		letter_score = 0;
	} else {
		letter_score = gen->letter_distribution->scores[L];
	}
	gen->incremental_main_word_score[gen->incremental_score_index] = (letter_score * letter_multiplier) + gen->incremental_main_word_score[gen->incremental_score_index - 1];
	int actual_cross_word = (gen->current_row_index > 0 && !is_empty(gen->board, gen->current_row_index-1, current_col)) || ((gen->current_row_index < BOARD_DIM - 1) && !is_empty(gen->board, gen->current_row_index+1, current_col));
	if (square_is_empty && actual_cross_word) {
		gen->incremental_cross_scores[gen->incremental_score_index] = letter_score*letter_multiplier*this_word_multiplier + cs*this_word_multiplier + gen->incremental_cross_scores[gen->incremental_score_index - 1];
	} else {
		gen->incremental_cross_scores[gen->incremental_score_index] = gen->incremental_cross_scores[gen->incremental_score_index - 1];
	}
	int bingo_bonus = 0;
	if (gen->tiles_played == RACK_SIZE) {
		bingo_bonus = BINGO_BONUS;
	}
	int score = gen->incremental_main_word_score[gen->incremental_score_index]*gen->incremental_word_multiplier[gen->incremental_score_index] +
	            gen->incremental_cross_scores[gen->incremental_score_index] + bingo_bonus;

	// printf("go_on %d: %d, %d, %d\n", gen->incremental_score_index, current_col, L, square_is_empty);
	for (int i = 0; i <= gen->incremental_score_index; i++) {
		// printf("inc %d: %d * %d + %d + bingo = score\n", i, gen->incremental_main_word_score[i], gen->incremental_word_multiplier[i], gen->incremental_cross_scores[i]);
	}
	// printf("final %d: %d * %d + %d + %d = %d\n", gen->incremental_score_index, gen->incremental_main_word_score[gen->incremental_score_index], gen->incremental_word_multiplier[gen->incremental_score_index], gen->incremental_cross_scores[gen->incremental_score_index], bingo_bonus, score);
	
	if (current_col <= gen->current_anchor_col) {
		if (!square_is_empty) {
			gen->strip[current_col] = PLAYED_THROUGH_MARKER;
		} else {
			gen->strip[current_col] = L;
			if (gen->vertical && (get_cross_set(gen->board, gen->current_row_index, current_col, BOARD_HORIZONTAL_DIRECTION) == TRIVIAL_CROSS_SET)) {
				unique_play = 1;
			}
		}
		leftstrip = current_col;
		int no_letter_directly_left = (current_col == 0) || is_empty(gen->board, gen->current_row_index, current_col - 1);

		if (accepts && no_letter_directly_left && gen->tiles_played > 0 && (unique_play || gen->tiles_played > 1)) {
			// printf("RECORDING with score of %d\n", score);
			record_play(gen, player, opp_rack, leftstrip, rightstrip, MOVE_TYPE_PLAY, score);
		}

		if (new_node_index == 0) {
			gen->incremental_score_index--;
			return;
		}

		if (current_col > 0 && current_col - 1 != gen->last_anchor_col) {
			// printf("recurring with %d\n", current_col - 1);
			recursive_gen(gen, current_col - 1, player, opp_rack, new_node_index, leftstrip, rightstrip, unique_play);
			// printf("done recurring with %d\n", current_col - 1);
		}

		uint32_t separation_node_index = kwg_get_next_node_index(gen->kwg, new_node_index, SEPARATION_MACHINE_LETTER);
		if (separation_node_index != 0 && no_letter_directly_left && gen->current_anchor_col < BOARD_DIM - 1) {
			recursive_gen(gen, gen->current_anchor_col+1, player, opp_rack, separation_node_index, leftstrip, rightstrip, unique_play);
		}
	} else {
		if (!square_is_empty) {
			gen->strip[current_col] = PLAYED_THROUGH_MARKER;
		} else {
			gen->strip[current_col] = L;
			if (gen->vertical && (get_cross_set(gen->board, gen->current_row_index, current_col, BOARD_HORIZONTAL_DIRECTION) == TRIVIAL_CROSS_SET)) {
				unique_play = 1;
			}
		}
		rightstrip = current_col;
		int no_letter_directly_right = (current_col == BOARD_DIM - 1) || is_empty(gen->board, gen->current_row_index, current_col + 1);

		if (accepts && no_letter_directly_right && gen->tiles_played > 0 && (unique_play || gen->tiles_played > 1)) {
			// printf("RECORDING with score of %d\n", score);
			record_play(gen, player, opp_rack, leftstrip, rightstrip, MOVE_TYPE_PLAY, score);
		}

		if (new_node_index != 0 && current_col < BOARD_DIM - 1) {
			// printf("recurring with %d\n", current_col + 1);
			recursive_gen(gen, current_col+1, player, opp_rack, new_node_index, leftstrip, rightstrip, unique_play);
			// printf("done recurring with %d\n", current_col + 1);
		}
	}
	gen->incremental_score_index--;
	// printf("decremented score index, current value is: %d\n", gen->incremental_score_index);
}

void gen_by_orientation(Generator * gen, Player * player, Rack * opp_rack, int dir) {
	// genByOrientation
	for (int row = 0; row < BOARD_DIM; row++)
	{
		gen->current_row_index = row;
		gen->last_anchor_col = 100;
		for (int col = 0; col < BOARD_DIM; col++)
		{
			if (get_anchor(gen->board, row, col, dir)) {
				gen->current_anchor_col = col;
				recursive_gen(gen, col, player, opp_rack, kwg_get_root_node_index(gen->kwg), col, col, !gen->vertical);
				gen->last_anchor_col = col;
			}
		}
	}
}

void generate_moves(Generator * gen, Player * player, Rack * opp_rack, int add_exchange) {
	// Add plays
	set_start_leave_index(player);
	for (int dir = 0; dir < 2; dir++)
	{
		gen->vertical = dir%2 != 0;
		gen_by_orientation(gen, player, opp_rack, dir);
		transpose(gen->board);
	}

	// Add exchanges
	if (add_exchange) {
		generate_exchange_moves(gen, player, 0, 0);
	}

	// Add the pass move
	if (player->strategy_params->play_recorder_type == PLAY_RECORDER_TYPE_ALL || gen->move_list->moves[0]->equity < PASS_MOVE_EQUITY) {
		set_spare_move(gen->move_list, gen->strip, 0, 0, 0, 0, 0, 0, 0, MOVE_TYPE_PASS);
		insert_spare_move(gen->move_list, PASS_MOVE_EQUITY);
	}
}

void reset_incremental_move_scores(Generator * gen) {
    gen->incremental_score_index = 0;
    gen->incremental_main_word_score[0] = 0;
    gen->incremental_cross_scores[0] = 0;
    gen->incremental_word_multiplier[0] = 1;
}

void reset_generator(Generator * gen) {
	reset_bag(gen->bag, gen->letter_distribution);
	reset_board(gen->board);
	reset_move_list(gen->move_list);
	reset_incremental_move_scores(gen);
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
    generator->kwg = config->kwg;
    generator->letter_distribution = config->letter_distribution;
	generator->tiles_played = 0;
	generator->vertical = 0;
	generator->last_anchor_col = 0;

	generator->exchange_strip = (uint8_t *) malloc(config->letter_distribution->size*sizeof(uint8_t));
	// Just load the zero values for now
	load_zero_preendgame_adjustment_values(generator);

	reset_incremental_move_scores(generator);
	return generator;
}

void destroy_generator(Generator * gen) {
	destroy_bag(gen->bag);
	destroy_board(gen->board);
	destroy_move_list(gen->move_list);
	free(gen->exchange_strip);
	free(gen);
}