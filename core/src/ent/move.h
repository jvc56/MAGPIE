#ifndef MOVE_H
#define MOVE_H

#include "../def/game_event_defs.h"

struct Move;
typedef struct Move Move;

struct MoveList;
typedef struct MoveList MoveList;

Move *create_move();
void move_copy(Move *dest_move, const Move *src_move);
void destroy_move(Move *move);
MoveList *create_move_list(int capacity);
void destroy_move_list(MoveList *ml);
void update_move_list(MoveList *ml, int new_capacity);
void sort_moves(MoveList *ml);
void set_spare_move(MoveList *ml, uint8_t strip[], int leftstrip,
                    int rightstrip, int score, int row_start, int col_start,
                    int tiles_played, int dir, game_event_t move_type);
void insert_spare_move(MoveList *ml, double equity);
void insert_spare_move_top_equity(MoveList *ml, double equity);
Move *pop_move(MoveList *ml);
void reset_move_list(MoveList *ml);
void set_move(Move *move, uint8_t strip[], int leftstrip, int rightstrip,
              int score, int row_start, int col_start, int tiles_played,
              int dir, game_event_t move_type);
void set_move_as_pass(Move *move);
void set_spare_move_as_pass(MoveList *ml);

#endif