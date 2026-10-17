#include "move.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

void small_move_list_grow(MoveList *ml) {
  const int old_capacity = ml->capacity;
  const int new_capacity = old_capacity > 0 ? old_capacity * 2 : 1;
  SmallMove **old_slots = ml->small_moves;
  const SmallMove *old_storage =
      small_move_list_storage(old_slots, old_capacity);
  SmallMove **new_slots = small_move_list_alloc_slots(new_capacity);
  SmallMove *new_storage = small_move_list_storage(new_slots, new_capacity);
  // Each move keeps its storage index, so the old slots and the spare map
  // onto the first old_capacity + 1 moves and the new slots take the rest.
  memcpy(new_storage, old_storage,
         sizeof(SmallMove) * ((size_t)old_capacity + 1));
  for (int move_idx = 0; move_idx < old_capacity; move_idx++) {
    new_slots[move_idx] = new_storage + (old_slots[move_idx] - old_storage);
  }
  for (int move_idx = old_capacity; move_idx < new_capacity; move_idx++) {
    new_slots[move_idx] = &new_storage[move_idx + 1];
  }
  ml->spare_small_move = new_storage + (ml->spare_small_move - old_storage);
  free(old_slots);
  ml->small_moves = new_slots;
  ml->capacity = new_capacity;
}
