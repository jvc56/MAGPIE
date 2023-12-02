#ifndef MOVE_H
#define MOVE_H

#include <stdint.h>

#include "../def/game_history_defs.h"

struct Move;
typedef struct Move Move;

struct MoveList;
typedef struct MoveList MoveList;

game_event_t get_move_type(const struct Move *move);
int get_score(const struct Move *move);
int get_row_start(const struct Move *move);
int get_col_start(const struct Move *move);
int get_tiles_played(const struct Move *move);
int get_tiles_length(const struct Move *move);
double get_equity(const struct Move *move);
int get_dir(const struct Move *move);
uint8_t get_tile(const struct Move *move, int index);
Move *get_spare_move(MoveList *ml);
int *move_list_get_count(MoveList *ml);
int *move_list_get_capacity(MoveList *ml);
Move *move_list_get_move(MoveList *ml, int move_index);

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