#include "../src/alphabet.h"
#include "../src/bag.h"
#include "alphabet_print.h"
#include "bag_print.h"

void write_bag_to_end_of_buffer(char * dest, Bag * bag, Alphabet * alphabet) {
    uint8_t sorted_bag[BAG_SIZE];
    for (int i = 0; i <= bag->last_tile_index; i++) {
        sorted_bag[i] = bag->tiles[i];
    }

    int x;
	int i = 1;
    int k;
	while (i < bag->last_tile_index + 1) {
		x = sorted_bag[i];
		k = i - 1;
		while (k >= 0 && x < sorted_bag[k]) {
			sorted_bag[k+1] = sorted_bag[k];
			k--;
		}
		sorted_bag[k+1] = x;
		i++;
	}

    for (int i = 0; i <= bag->last_tile_index; i++) {
        write_user_visible_letter_to_end_of_buffer(dest, alphabet, sorted_bag[i]);
    }
}