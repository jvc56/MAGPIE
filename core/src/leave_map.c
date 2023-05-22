#include <stdlib.h>
#include <stdio.h>

#include "rack.h"
#include "leave_map.h"

LeaveMap * create_leave_map(int rack_array_size) {
    int number_of_values = 1 << RACK_SIZE;
    LeaveMap * leave_map = malloc(sizeof(LeaveMap));
    leave_map->leave_values = (float *) malloc(number_of_values*sizeof(float));
    leave_map->letter_base_index_map = (int *) malloc(rack_array_size*sizeof(int));
    return leave_map;
}

void destroy_leave_map(LeaveMap * leave_map) {
    free(leave_map->leave_values);
    free(leave_map->letter_base_index_map);
    free(leave_map);
}

void take_letter_and_update_current_index(LeaveMap * leave_map, Rack * rack, uint8_t letter) {
	take_letter_from_rack(rack, letter);
    int base_index = leave_map->letter_base_index_map[letter];
    int offset = rack->array[letter];
    if (letter >= rack->array_size) {
        printf("ERROR: take invalid letter: %d\n", letter);
        abort();
    }
    int bit_index = base_index + offset;
    leave_map->current_index &= ~(1 << bit_index);
    if (leave_map->current_index > 127 || leave_map->current_index < 0) {
        printf("ERROR: take %d, %d = %d + %d, %d\n", leave_map->current_index, bit_index, base_index, offset, letter);
        abort();
    }
}

void add_letter_and_update_current_index(LeaveMap * leave_map, Rack * rack, uint8_t letter) {
	add_letter_to_rack(rack, letter);
    int base_index = leave_map->letter_base_index_map[letter];
    int offset = rack->array[letter] - 1;
    if (letter >= rack->array_size) {
        printf("ERROR: add invalid letter: %d\n", letter);
        abort();
    }
    int bit_index = base_index + offset;
    leave_map->current_index |= 1 << bit_index;
    if (leave_map->current_index > 127 || leave_map->current_index < 0) {
        printf("ERROR: add %d, %d = %d + %d, %d\n", leave_map->current_index, bit_index, base_index, offset, letter);
        abort();
    }
}

void set_current_value(LeaveMap * leave_map, float value) {
    if (leave_map->current_index > 127) {
        printf("ERROR: setting value %f at %d\n", value, leave_map->current_index);
        abort();
    }
    leave_map->leave_values[leave_map->current_index] = value;
}

float get_current_value(LeaveMap * leave_map) {
    if (leave_map->current_index > 127) {
        printf("ERROR: getting value at %d\n", leave_map->current_index);
        abort();
    }
    return leave_map->leave_values[leave_map->current_index];
}

void init_leave_map(LeaveMap * leave_map, Rack * rack) {
    int current_base_index = 0;
    for (int i = 0; i < rack->array_size; i++) {
        if (rack->array[i] > 0) {
            leave_map->letter_base_index_map[i] = current_base_index;
            current_base_index += rack->array[i];
        }
    }
    leave_map->current_index = (1 << current_base_index) - 1;
    if (leave_map->current_index > 127 || leave_map->current_index < 0) {
        printf("ERROR: init %d\n", leave_map->current_index);
    }
}
