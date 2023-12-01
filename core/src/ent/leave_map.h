#ifndef LEAVE_MAP_H
#define LEAVE_MAP_H

struct LeaveMap;
typedef struct LeaveMap LeaveMap;

LeaveMap *create_leave_map(int rack_array_size);
void destroy_leave_map(LeaveMap *LeaveMap);
void update_leave_map(LeaveMap *leave_map, int new_rack_array_size);
void set_current_value(LeaveMap *leave_map, double value);
double get_current_value(const LeaveMap *leave_map);

#endif