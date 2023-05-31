#ifndef BAG_PRINT_H
#define BAG_PRINT_H

#include "../src/bag.h"
#include "../src/letter_distribution.h"

void write_bag_to_end_of_buffer(char * dest, Bag * bag, LetterDistribution * letter_distribution);

#endif