#ifndef ZOBRIST_H
#define ZOBRIST_H

#include "../def/board_defs.h"
#include "../def/rack_defs.h"
#include "../util/io_util.h"
#include "board.h"
#include "letter_distribution.h"
#include "move.h"
#include "rack.h"
#include "xoshiro.h"
#include <stdint.h>

#define ZOBRIST_MAX_LETTERS 35 // For the purposes of Zobrist hashing.

// A Zobrist hash implementation for our fun game.
// Struct layout: hot fields (accessed every move) first for cache locality.
typedef struct Zobrist {
  // Hot: accessed on every move
  uint64_t their_turn;
  uint64_t scoreless_turns[3];
  uint64_t our_rack_table[ZOBRIST_MAX_LETTERS][RACK_SIZE];
  uint64_t their_rack_table[ZOBRIST_MAX_LETTERS][RACK_SIZE];
  // Cold: only accessed during initial hash calculation
  uint64_t pos_table[BOARD_DIM * BOARD_DIM][ZOBRIST_MAX_LETTERS * 2];
  // Very cold: only used during creation
  XoshiroPRNG *prng;
} Zobrist;

static Zobrist *zobrist_create(uint64_t seed) {
  Zobrist *z = malloc_or_die(sizeof(Zobrist));
  z->prng = prng_create(seed);

  for (int i = 0; i < BOARD_DIM * BOARD_DIM; i++) {
    for (int j = 0; j < ZOBRIST_MAX_LETTERS * 2; j++) {
      z->pos_table[i][j] = prng_get_random_number(z->prng, UINT64_MAX);
    }
  }

  for (int i = 0; i < ZOBRIST_MAX_LETTERS; i++) {
    for (int j = 0; j < RACK_SIZE; j++) {
      z->our_rack_table[i][j] = prng_get_random_number(z->prng, UINT64_MAX);
    }
  }

  for (int i = 0; i < ZOBRIST_MAX_LETTERS; i++) {
    for (int j = 0; j < RACK_SIZE; j++) {
      z->their_rack_table[i][j] = prng_get_random_number(z->prng, UINT64_MAX);
    }
  }

  for (int i = 0; i < 3; i++) {
    z->scoreless_turns[i] = prng_get_random_number(z->prng, UINT64_MAX);
  }

  z->their_turn = prng_get_random_number(z->prng, UINT64_MAX);

  return z;
}

static void zobrist_destroy(Zobrist *z) {
  prng_destroy(z->prng);
  free(z);
}

inline static uint64_t
zobrist_calculate_hash(const Zobrist *z, const Board *game_board,
                       const Rack *our_rack, const Rack *their_rack,
                       bool their_turn, int scoreless_turns) {

  // Calculate the hash of a specific position/situation.
  uint64_t key = 0;

  for (int i = 0; i < BOARD_DIM * BOARD_DIM; i++) {
    const Square *sq = &(game_board->squares[i]);
    MachineLetter letter = sq->letter;
    if (letter == 0) {
      continue;
    }

    if (get_is_blanked(letter)) {
      letter = get_unblanked_machine_letter(letter) + ZOBRIST_MAX_LETTERS;
    }
    key ^= z->pos_table[i][letter];
  }

  for (uint16_t i = 0; i < our_rack->dist_size; i++) {
    int8_t ct = our_rack->array[i];
    key ^= z->our_rack_table[i][ct];
  }
  for (uint16_t i = 0; i < their_rack->dist_size; i++) {
    int8_t ct = their_rack->array[i];
    key ^= z->their_rack_table[i][ct];
  }
  if (their_turn) {
    key ^= z->their_turn;
  }
  key ^= z->scoreless_turns[scoreless_turns];
  return key;
}

inline static uint64_t zobrist_add_move(const Zobrist *z, uint64_t key,
                                        const Move *move, const Rack *move_rack,
                                        bool was_our_move, int scoreless_turns,
                                        int last_scoreless_turns) {
  // Add a move to the Zobrist hash.
  // For tile placements: XOR board positions and rack hash changes.
  // Then XOR turn indicator since we always alternate.

  const uint64_t (*rack_table)[RACK_SIZE] =
      was_our_move ? z->our_rack_table : z->their_rack_table;

  if (move->move_type != GAME_EVENT_PASS) {
    // Count tiles played of each type (for rack hash update)
    int8_t tiles_played[ZOBRIST_MAX_LETTERS] = {0};

    const int row = move_get_row_start(move);
    const int col = move_get_col_start(move);
    const int row_inc = (move_get_dir(move) == BOARD_VERTICAL_DIRECTION) ? 1 : 0;
    const int col_inc = 1 - row_inc;

    // Single pass: XOR board positions and count tiles played
    int cur_row = row, cur_col = col;
    for (int idx = 0; idx < move->tiles_length; idx++) {
      const MachineLetter tile = move->tiles[idx];
      if (tile != PLAYED_THROUGH_MARKER) {
        // Board hash: XOR in the new tile position
        int board_tile = tile;
        if (get_is_blanked(tile)) {
          board_tile = get_unblanked_machine_letter(tile) + ZOBRIST_MAX_LETTERS;
        }
        key ^= z->pos_table[cur_row * BOARD_DIM + cur_col][board_tile];
        // Count this tile for rack hash update
        const int tile_idx = get_is_blanked(tile) ? 0 : tile;
        tiles_played[tile_idx]++;
      }
      cur_row += row_inc;
      cur_col += col_inc;
    }

    // Rack hash: for each tile type played, XOR transition from pre to post
    for (int tile_idx = 0; tile_idx < ZOBRIST_MAX_LETTERS; tile_idx++) {
      const int num_played = tiles_played[tile_idx];
      if (num_played > 0) {
        const int post_count =
            (tile_idx < move_rack->dist_size) ? move_rack->array[tile_idx] : 0;
        const int pre_count = post_count + num_played;
        key ^= rack_table[tile_idx][pre_count] ^ rack_table[tile_idx][post_count];
      }
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