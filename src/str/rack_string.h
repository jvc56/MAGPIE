#ifndef RACK_STRING_H
#define RACK_STRING_H

#include "../ent/letter_distribution.h"
#include "../ent/rack.h"
#include "../util/string_util.h"
#include <stddef.h>

void string_builder_add_rack(StringBuilder *string_builder, const Rack *rack,
                             const LetterDistribution *ld, bool blanks_first);

// Writes the same representation as string_builder_add_rack into dest
// instead of a StringBuilder, truncating if it does not fit. dest_size must
// include room for the terminating null.
void rack_get_string(const Rack *rack, const LetterDistribution *ld,
                     bool blanks_first, char *dest, size_t dest_size);

#endif
