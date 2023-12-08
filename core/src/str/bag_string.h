#ifndef BAG_STRING_H
#define BAG_STRING_H

#include "../ent/bag.h"
#include "../ent/letter_distribution.h"

#include "../util/string_util.h"

void string_builder_add_bag(const Bag *bag,
                            const LetterDistribution *letter_distribution,
                            StringBuilder *bag_string_builder);

#endif
