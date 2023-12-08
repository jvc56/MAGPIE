#ifndef LEAVE_MAP_H
#define LEAVE_MAP_H

#include <stdint.h>

#include "rack.h"

struct LeaveMap;
typedef struct LeaveMap LeaveMap;

LeaveMap *create_leave_map(int rack_array_size);
void destroy_leave_map(LeaveMap *LeaveMap);
void update_leave_map(LeaveMap *leave_map, int new_rack_array_size);
void set_current_value(LeaveMap *leave_map, double value);
double get_current_value(const LeaveMap *leave_map);
int leave_map_get_current_index(const LeaveMap *leave_map);
void leave_map_take_letter(LeaveMap *leave_map, uint8_t letter,
                           int number_of_letter_on_rack);
void leave_map_add_letter(LeaveMap *leave_map, uint8_t letter,
                          int number_of_letter_on_rack);
void init_leave_map(const Rack *rack, LeaveMap *leave_map);

double *leave_map_get_leave_values(const LeaveMap *leave_map);
void take_letter_and_update_current_index(LeaveMap *leave_map, Rack *rack,
                                          uint8_t letter);
void add_letter_and_update_current_index(LeaveMap *leave_map, Rack *rack,
                                         uint8_t letter);

#endif