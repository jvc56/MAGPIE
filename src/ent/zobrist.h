#ifndef ZOBRIST_H
#define ZOBRIST_H

#include "../def/board_defs.h"
#include "../def/rack_defs.h"
#include "../util/util.h"
#include "board.h"
#include "letter_distribution.h"
#include "move.h"
#include "rack.h"
#include "xoshiro.h"
#include <stdint.h>

#define ZOBRIST_MAX_LETTERS 35 // For the purposes of Zobrist hashing.

#define POS_HASH_TABLE_SIZE (BOARD_DIM * BOARD_DIM * ZOBRIST_MAX_LETTERS * 2)
#define RACK_HASH_TABLE_SIZE (ZOBRIST_MAX_LETTERS * RACK_SIZE)

// A Zobrist hash implementation for our fun game.
typedef struct Zobrist {
  uint64_t their_turn;
  uint64_t pos_table[POS_HASH_TABLE_SIZE];
  uint64_t rack_tables[2 * RACK_HASH_TABLE_SIZE];
  uint64_t scoreless_turns[3];

  XoshiroPRNG *prng;
  int board_dim;
} Zobrist;

static inline int get_pos_table_index(int row, int col, int letter_idx) {
  return (row * BOARD_DIM + col) * ZOBRIST_MAX_LETTERS + letter_idx;
}

static inline int get_rack_table_index(int player_idx, int letter_idx,
                                       int rack_idx) {
  return player_idx * RACK_HASH_TABLE_SIZE + letter_idx * RACK_SIZE + rack_idx;
}

static inline Zobrist *zobrist_create(uint64_t seed) {
  Zobrist *z = (Zobrist *)malloc_or_die(sizeof(Zobrist));
  z->prng = prng_create(seed);

  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      for (int letter_idx = 0; letter_idx < ZOBRIST_MAX_LETTERS * 2;
           letter_idx++) {
        z->pos_table[get_pos_table_index(row, col, letter_idx)] =
            prng_get_random_number(z->prng, UINT64_MAX);
      }
    }
  }

  // rack tables are ZOBRIST_MAX_LETTERS * RACK_SIZE 2D arrays of random
  // integers
  for (int player_idx = 0; player_idx < 2; player_idx++) {
    for (int letter_idx = 0; letter_idx < ZOBRIST_MAX_LETTERS; letter_idx++) {
      for (int rack_idx = 0; rack_idx < RACK_SIZE; rack_idx++) {
        z->rack_tables[get_rack_table_index(player_idx, letter_idx, rack_idx)] =
            prng_get_random_number(z->prng, UINT64_MAX);
      }
    }
  }

  for (int i = 0; i < 3; i++) {
    z->scoreless_turns[i] = prng_get_random_number(z->prng, UINT64_MAX);
  }

  z->their_turn = prng_get_random_number(z->prng, UINT64_MAX);

  return z;
}

static inline void zobrist_destroy(Zobrist *z) {
  prng_destroy(z->prng);
  free(z);
}

inline static uint64_t zobrist_add_move(const Zobrist *z, uint64_t key,
                                        const Move *move, const Rack *move_rack,
                                        bool was_our_move, int scoreless_turns,
                                        int last_scoreless_turns) {
  // Add a move to the Zobrist hash.

  // For every letter in the move (assume it's only a tile placement or
  // a pass for now):
  // - XOR with its position on the board
  // - XOR with the "position" on the rack hash.
  // Then XOR with p2ToMove since we always alternate.
  const int player_idx = was_our_move ? 0 : 1;
  if (move->move_type != GAME_EVENT_PASS) {
    // create placeholder rack to keep track of what our rack would be
    // before we made the play.
    uint8_t placeholder_rack[ZOBRIST_MAX_LETTERS];
    memset(placeholder_rack, 0, ZOBRIST_MAX_LETTERS);

    int row = move_get_row_start(move);
    int col = move_get_col_start(move);
    bool vertical = move_get_dir(move) == BOARD_VERTICAL_DIRECTION;

    // XOR with the new board positions
    for (int idx = 0; idx < move->tiles_length; idx++) {
      int tile = move->tiles[idx];
      int new_row = row + (vertical ? idx : 0);
      int new_col = col + (vertical ? 0 : idx);
      if (tile == PLAYED_THROUGH_MARKER) {
        continue;
      }
      int board_tile = tile;
      if (get_is_blanked(tile)) {
        board_tile = get_unblanked_machine_letter(tile) + ZOBRIST_MAX_LETTERS;
      }
      const int pos_table_idx =
          get_pos_table_index(new_row, new_col, board_tile);
      key ^= z->pos_table[pos_table_idx];
      // Build up placeholder rack.
      int tile_idx = get_is_blanked(tile) ? 0 : tile;
      placeholder_rack[tile_idx]++;
    }
    // move_rack contains the left-over tiles
    for (int idx = 0; idx < move_rack->dist_size; idx++) {
      for (int j = 0; j < move_rack->array[idx]; j++) {
        placeholder_rack[idx]++;
      }
    }
    // now "play" all the tiles in the placeholder rack
    for (int idx = 0; idx < move->tiles_length; idx++) {
      uint8_t tile = move->tiles[idx];
      if (tile == PLAYED_THROUGH_MARKER) {
        continue;
      }
      const int tile_idx = get_is_blanked(tile) ? 0 : tile;
      placeholder_rack[tile_idx]--;
      const int new_rack_table_idx = get_rack_table_index(
          player_idx, tile_idx, placeholder_rack[tile_idx]);
      key ^= z->rack_tables[new_rack_table_idx];
      // This is equal to what old_rack_table_idx would have been.
      // It doesn't matter which order we xor in, so we do it sequentially.
      key ^= z->rack_tables[new_rack_table_idx + 1];
    }
  }

  if (last_scoreless_turns != scoreless_turns) {
    key ^= z->scoreless_turns[last_scoreless_turns];
    key ^= z->scoreless_turns[scoreless_turns];
  }
  key ^= z->their_turn;

  return key;
}

#endif