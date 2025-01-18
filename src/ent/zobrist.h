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

// A Zobrist hash implementation for our fun game.
typedef struct Zobrist {
  uint64_t their_turn;
  uint64_t **pos_table;
  uint64_t **our_rack_table;
  uint64_t **their_rack_table;
  uint64_t scoreless_turns[3];

  XoshiroPRNG *prng;
  int board_dim;
} Zobrist;

Zobrist *zobrist_create(uint64_t seed) {
  Zobrist *z = malloc_or_die(sizeof(Zobrist));
  z->prng = prng_create(seed);

  // pos_table is a BOARD_DIM x BOARD_DIM 2d array of random integers
  z->pos_table = malloc_or_die(BOARD_DIM * BOARD_DIM * sizeof(uint64_t *));
  for (size_t i = 0; i < BOARD_DIM * BOARD_DIM; i++) {
    // * 2 for the blank
    z->pos_table[i] = malloc_or_die(ZOBRIST_MAX_LETTERS * 2 * sizeof(uint64_t));
    for (int j = 0; j < ZOBRIST_MAX_LETTERS * 2; j++) {
      z->pos_table[i][j] = prng_get_random_number(z->prng, UINT64_MAX);
    }
  }
  // rack tables are ZOBRIST_MAX_LETTERS * RACK_SIZE 2D arrays of random
  // integers
  z->our_rack_table = malloc_or_die(ZOBRIST_MAX_LETTERS * sizeof(uint64_t *));
  for (int i = 0; i < ZOBRIST_MAX_LETTERS; i++) {
    z->our_rack_table[i] = malloc_or_die(RACK_SIZE * sizeof(uint64_t));
    for (int j = 0; j < RACK_SIZE; j++) {
      z->our_rack_table[i][j] = prng_get_random_number(z->prng, UINT64_MAX);
    }
  }

  z->their_rack_table = malloc_or_die(ZOBRIST_MAX_LETTERS * sizeof(uint64_t *));
  for (int i = 0; i < ZOBRIST_MAX_LETTERS; i++) {
    z->their_rack_table[i] = malloc_or_die(RACK_SIZE * sizeof(uint64_t));
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

void zobrist_destroy(Zobrist *z) {
  for (size_t i = 0; i < BOARD_DIM * BOARD_DIM; i++) {
    free(z->pos_table[i]);
  }
  free(z->pos_table);
  for (int i = 0; i < ZOBRIST_MAX_LETTERS; i++) {
    free(z->our_rack_table[i]);
    free(z->their_rack_table[i]);
  }
  free(z->our_rack_table);
  free(z->their_rack_table);
  prng_destroy(z->prng);
  free(z);
}

uint64_t zobrist_calculate_hash(const Zobrist *z, const Board *game_board,
                                const Rack *our_rack, const Rack *their_rack,
                                bool their_turn, int scoreless_turns) {

  // Calculate the hash of a specific position/situation.
  uint64_t key = 0;

  for (int i = 0; i < BOARD_DIM * BOARD_DIM; i++) {
    const Square *sq = &(game_board->squares[i]);
    uint8_t letter = sq->letter;
    if (letter == 0) {
      continue;
    }

    if (get_is_blanked(letter)) {
      letter = get_unblanked_machine_letter(letter) + ZOBRIST_MAX_LETTERS;
    }
    key ^= z->pos_table[i][letter];
  }

  for (int i = 0; i < our_rack->dist_size; i++) {
    int ct = our_rack->array[i];
    key ^= z->our_rack_table[i][ct];
  }
  for (int i = 0; i < their_rack->dist_size; i++) {
    int ct = their_rack->array[i];
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

  // For every letter in the move (assume it's only a tile placement or
  // a pass for now):
  // - XOR with its position on the board
  // - XOR with the "position" on the rack hash.
  // Then XOR with p2ToMove since we always alternate.
  uint64_t **rack_table = z->our_rack_table;
  if (!was_our_move) {
    rack_table = z->their_rack_table;
  }
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
      key ^= z->pos_table[new_row * BOARD_DIM + new_col][board_tile];
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
    for (int idx = 0; idx < move->tiles_played; idx++) {
      uint8_t tile = move->tiles[idx];
      if (tile == PLAYED_THROUGH_MARKER) {
        continue;
      }
      int tile_idx = get_is_blanked(tile) ? 0 : tile;
      key ^= rack_table[tile_idx][placeholder_rack[tile_idx]];
      placeholder_rack[tile_idx]--;
      key ^= rack_table[tile_idx][placeholder_rack[tile_idx]];
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