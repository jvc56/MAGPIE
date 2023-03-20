#include <stdlib.h>

#include "bag.h"

void shuffle(Bag * bag)
{
    if (bag->last_tile_index + 1 > 1) 
    {
        int i;
        for (i = 0; i < bag->last_tile_index + 1 - 1; i++) 
        {
          int j = i + rand() / (RAND_MAX / (bag->last_tile_index + 1 - i) + 1);
          int t = bag->tiles[j];
          bag->tiles[j] = bag->tiles[i];
          bag->tiles[i] = t;
        }
    }
}

void reset_bag(Bag * bag, LetterDistribution * letter_distribution) {
    int idx = 0;
    for (uint32_t i = 0; i < (letter_distribution->size); i++) {
        for (uint32_t k = 0; k < letter_distribution->distribution[i]; k++) {
            bag->tiles[idx] = i;
            idx++;
        }
    }
    bag->last_tile_index = sizeof(bag->tiles) - 1;
    shuffle(bag);
}

Bag * create_bag(LetterDistribution * letter_distribution) {
	Bag * bag = malloc(sizeof(Bag));
    reset_bag(bag, letter_distribution);
	return bag;
}

void destroy_bag(Bag * bag) {
	free(bag);
}

// This assumes the letter is in the bag
void draw_letter(Bag * bag, uint8_t letter) {
    if (is_blanked(letter)) {
        letter = BLANK_MACHINE_LETTER;
    }
    for (int i = 0; i <= bag->last_tile_index; i++) {
        if (bag->tiles[i] == letter) {
            bag->tiles[i] = bag->tiles[bag->last_tile_index];
            bag->last_tile_index--;
            return;
        }
    }
}

void add_letter(Bag * bag, uint8_t letter) {
    if (is_blanked(letter)) {
        letter = BLANK_MACHINE_LETTER;
    }
    int insert_index = 0;
    if (bag->last_tile_index >= 0 ) {
        insert_index = rand()%(bag->last_tile_index+1);
    }
    bag->tiles[bag->last_tile_index+1] = bag->tiles[insert_index];
    bag->tiles[insert_index] = letter;
    bag->last_tile_index++;
}
