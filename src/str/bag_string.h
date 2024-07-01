#ifndef BAG_STRING_H
#define BAG_STRING_H

#include "../ent/bag.h"
#include "../ent/letter_distribution.h"

#include "../util/string_util.h"

void string_builder_add_bag(StringBuilder *bag_string_builder, const Bag *bag,
                            const LetterDistribution *ld);

#endif
