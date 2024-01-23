#ifndef MOVE_H
#define MOVE_H

#include <stdint.h>

#include "../def/game_history_defs.h"

typedef struct Move Move;

Move *move_create();
void move_destroy(Move *move);

game_event_t move_get_type(const Move *move);
int move_get_score(const Move *move);
int move_get_row_start(const Move *move);
int move_get_col_start(const Move *move);
int move_get_tiles_played(const Move *move);
int move_get_tiles_length(const Move *move);
double move_get_equity(const Move *move);
int move_get_dir(const Move *move);
uint8_t move_get_tile(const Move *move, int index);

void move_set_type(Move *move, game_event_t move_type);
void move_set_score(Move *move, int score);
void move_set_row_start(Move *move, int row_start);
void move_set_col_start(Move *move, int col_start);
void move_set_tiles_played(Move *move, int tiles_played);
void move_set_tiles_length(Move *move, int tiles_length);
void move_set_dir(Move *move, int dir);
void move_set_tile(Move *move, uint8_t tile, int index);
void move_set_all_except_equity(Move *move, uint8_t strip[], int leftstrip,
                                int rightstrip, int score, int row_start,
                                int col_start, int tiles_played, int dir,
                                game_event_t move_type);
void move_set_all(Move *move, uint8_t strip[], int leftstrip, int rightstrip,
                  int score, int row_start, int col_start, int tiles_played,
                  int dir, game_event_t move_type, double leave_value);
void move_set_as_pass(Move *move);
void move_copy(Move *dest_move, const Move *src_move);

typedef struct MoveList MoveList;

MoveList *move_list_create(int capacity);
void move_list_destroy(MoveList *ml);

Move *move_list_get_spare_move(const MoveList *ml);
int move_list_get_count(const MoveList *ml);
int move_list_get_capacity(const MoveList *ml);
Move *move_list_get_move(const MoveList *ml, int move_index);

void move_list_set_spare_move(MoveList *ml, uint8_t strip[], int leftstrip,
                              int rightstrip, int score, int row_start,
                              int col_start, int tiles_played, int dir,
                              game_event_t move_type);
void move_list_set_spare_move_as_pass(MoveList *ml);

void move_list_insert_spare_move(MoveList *ml, double equity);
void move_list_insert_spare_move_top_equity(MoveList *ml, double equity);
Move *move_list_pop_move(MoveList *ml);
void move_list_sort_moves(MoveList *ml);
void move_list_reset(MoveList *ml);

#endif