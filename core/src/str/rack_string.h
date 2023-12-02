#ifndef RACK_STRING_H
#define RACK_STRING_H

#include "../ent/letter_distribution.h"
#include "../ent/rack.h"

#include "string_util.h"

void string_builder_add_rack(const Rack *rack,
                             const LetterDistribution *letter_distribution,
                             StringBuilder *string_builder);

#endif
