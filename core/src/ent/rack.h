#ifndef RACK_H
#define RACK_H

struct Rack;
typedef struct Rack Rack;

void add_letter_to_rack(Rack *rack, uint8_t letter);
Rack *create_rack(int array_size);
void update_or_create_rack(Rack **rack, int array_size);
Rack *rack_duplicate(const Rack *rack);
void rack_copy(Rack *dst, const Rack *src);
void destroy_rack(Rack *rack);
void reset_rack(Rack *rack);
void take_letter_from_rack(Rack *rack, uint8_t letter);
bool racks_are_equal(const Rack *rack1, const Rack *rack2);

#endif