#include <stdio.h>
#include <stdlib.h>

#include "board.h"
#include "constants.h"
#include "move.h"

MoveList *create_move_list() {
  MoveList *ml = malloc(sizeof(MoveList));
  ml->count = 0;
  ml->spare_move = malloc(sizeof(Move));
  ml->moves = malloc((sizeof(Move *)) * (MOVE_LIST_CAPACITY));
  for (int i = 0; i < MOVE_LIST_CAPACITY; i++) {
    ml->moves[i] = malloc(sizeof(Move));
  }
  ml->moves[0]->equity = INITIAL_TOP_MOVE_EQUITY;
  return ml;
}

void destroy_move(Move *move) { free(move); }

void destroy_move_list(MoveList *ml) {
  for (int i = 0; i < MOVE_LIST_CAPACITY; i++) {
    destroy_move(ml->moves[i]);
  }
  destroy_move(ml->spare_move);
  free(ml->moves);
  free(ml);
}

void reset_move_list(MoveList *ml) {
  ml->count = 0;
  ml->moves[0]->equity = INITIAL_TOP_MOVE_EQUITY;
}

void up_heapify(MoveList *ml, int index) {
  Move *temp;
  int parent_node = (index - 1) / 2;

  if (ml->moves[parent_node]->equity > ml->moves[index]->equity) {
    temp = ml->moves[parent_node];
    ml->moves[parent_node] = ml->moves[index];
    ml->moves[index] = temp;
    up_heapify(ml, parent_node);
  }
}

void down_heapify(MoveList *ml, int parent_node) {
  int left = parent_node * 2 + 1;
  int right = parent_node * 2 + 2;
  int min;
  Move *temp;

  if (left >= ml->count || left < 0)
    left = -1;
  if (right >= ml->count || right < 0)
    right = -1;

  if (left != -1 && ml->moves[left]->equity < ml->moves[parent_node]->equity)
    min = left;
  else
    min = parent_node;
  if (right != -1 && ml->moves[right]->equity < ml->moves[min]->equity)
    min = right;

  if (min != parent_node) {
    temp = ml->moves[min];
    ml->moves[min] = ml->moves[parent_node];
    ml->moves[parent_node] = temp;
    down_heapify(ml, min);
  }
}

void set_move(Move *move, uint8_t strip[], int leftstrip, int rightstrip,
              int score, int row_start, int col_start, int tiles_played,
              int vertical, int move_type) {
  move->score = score;
  move->row_start = row_start;
  move->col_start = col_start;
  move->tiles_played = tiles_played;
  move->vertical = vertical;
  move->move_type = move_type;
  move->tiles_length = rightstrip - leftstrip + 1;
  for (int i = 0; i < move->tiles_length; i++) {
    move->tiles[i] = strip[leftstrip + i];
  }
}

void set_spare_move(MoveList *ml, uint8_t strip[], int leftstrip,
                    int rightstrip, int score, int row_start, int col_start,
                    int tiles_played, int vertical, int move_type) {
  set_move(ml->spare_move, strip, leftstrip, rightstrip, score, row_start,
           col_start, tiles_played, vertical, move_type);
}

void insert_spare_move(MoveList *ml, float equity) {
  ml->spare_move->equity = equity;

  Move *swap = ml->moves[ml->count];
  ml->moves[ml->count] = ml->spare_move;
  ml->spare_move = swap;

  up_heapify(ml, ml->count);
  ml->count++;

  if (ml->count == MOVE_LIST_CAPACITY) {
    pop_move(ml);
  }
}

void insert_spare_move_top_equity(MoveList *ml, float equity) {
  if (equity > ml->moves[0]->equity) {
    ml->spare_move->equity = equity;
    Move *swap = ml->moves[0];
    ml->moves[0] = ml->spare_move;
    ml->spare_move = swap;
  }
}

Move *pop_move(MoveList *ml) {
  if (ml->count == 1) {
    ml->count--;
    return ml->moves[0];
  }
  Move *swap = ml->spare_move;
  ml->spare_move = ml->moves[0];
  ml->moves[0] = ml->moves[ml->count - 1];
  ml->moves[ml->count - 1] = swap;

  ml->count--;
  down_heapify(ml, 0);
  return ml->spare_move;
}
