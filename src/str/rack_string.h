#ifndef RACK_STRING_H
#define RACK_STRING_H

#include "../ent/letter_distribution.h"
#include "../ent/rack.h"
#include "../util/string_util.h"

void string_builder_add_rack(StringBuilder *string_builder, const Rack *rack,
                             const LetterDistribution *ld, bool blanks_first);

#endif
