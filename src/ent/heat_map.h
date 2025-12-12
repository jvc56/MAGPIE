#ifndef HEAT_MAP_H
#define HEAT_MAP_H

#include "../def/board_defs.h"
#include "../def/game_defs.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "move.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

typedef enum {
  HEAT_MAP_TYPE_ALL,
  HEAT_MAP_TYPE_BINGO,
  NUM_HEAT_MAP_TYPES,
} heat_map_t;

enum {
  NUM_HEAT_MAP_COUNTS = NUM_HEAT_MAP_TYPES * BOARD_DIM * BOARD_DIM,
};

typedef struct HeatMap {
  uint64_t board_count_maxes[NUM_HEAT_MAP_TYPES];
  uint64_t counts[NUM_HEAT_MAP_COUNTS];
} HeatMap;

enum {
  HEAT_MAP_NUM_BACKGROUND_COLORS = 7,
};

static const int
    heat_map_ascending_color_codes[HEAT_MAP_NUM_BACKGROUND_COLORS] = {
        49, // Default background color
        // 100, // Dark gray
        46, // Cyan
        44, // Blue
        42, // Green
        45, // Magenta
        43, // Yellow
        41, // Red
            // 107, // White
};

#define HEAT_MAP_FRAC_DELIMITER (1.0 / (double)HEAT_MAP_NUM_BACKGROUND_COLORS)

static inline HeatMap *heat_map_create(void) {
  return (HeatMap *)calloc_or_die(1, sizeof(HeatMap));
}

static inline void heat_map_destroy(HeatMap *hm) {
  if (!hm) {
    return;
  }
  free(hm);
}

static inline void heat_map_reset(HeatMap *hm) {
  memset(hm->board_count_maxes, 0, sizeof(hm->board_count_maxes));
  memset(hm->counts, 0, sizeof(hm->counts));
}

static inline size_t heat_map_get_index(int row, int col, heat_map_t type) {
  return ((((size_t)row * BOARD_DIM + col) * NUM_HEAT_MAP_TYPES) + type);
}

static inline uint64_t heat_map_get_count(const HeatMap *hm, int row, int col,
                                          heat_map_t type) {
  return hm->counts[heat_map_get_index(row, col, type)];
}

static inline void heat_map_add_move(HeatMap *hm, const Move *move) {
  if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return;
  }
  const bool is_bingo = move_get_tiles_played(move) == RACK_SIZE;
  const int row_inc = move_get_dir(move) == BOARD_VERTICAL_DIRECTION;
  const int col_inc = move_get_dir(move) == BOARD_HORIZONTAL_DIRECTION;
  size_t current_offset = heat_map_get_index(
      move_get_row_start(move), move_get_col_start(move), (heat_map_t)0);
  const int move_len = move_get_tiles_length(move);
  for (int i = 0; i < move_len; i++) {
    const MachineLetter letter = move_get_tile(move, i);
    if (letter != PLAYED_THROUGH_MARKER) {
      const size_t current_all_index = current_offset + HEAT_MAP_TYPE_ALL;
      hm->counts[current_all_index]++;
      if (hm->counts[current_all_index] >
          hm->board_count_maxes[HEAT_MAP_TYPE_ALL]) {
        hm->board_count_maxes[HEAT_MAP_TYPE_ALL] =
            hm->counts[current_all_index];
      }
      if (is_bingo) {
        const size_t current_bingo_index = current_offset + HEAT_MAP_TYPE_BINGO;
        hm->counts[current_bingo_index]++;
        if (hm->counts[current_bingo_index] >
            hm->board_count_maxes[HEAT_MAP_TYPE_BINGO]) {
          hm->board_count_maxes[HEAT_MAP_TYPE_BINGO] =
              hm->counts[current_bingo_index];
        }
      }
    }
    current_offset += heat_map_get_index(row_inc, col_inc, (heat_map_t)0);
  }
}

static inline uint64_t heat_map_get_total_count(const HeatMap *hm) {
  uint64_t sum = 0;
  for (size_t i = 0; i < NUM_HEAT_MAP_COUNTS; i++) {
    sum += hm->counts[i];
  }
  return sum;
}

static inline uint64_t heat_map_get_board_count_max(const HeatMap *hm,
                                                    heat_map_t heat_map_type) {
  return hm->board_count_maxes[heat_map_type];
}

typedef struct HeatMapSquare {
  int row;
  int col;
  uint64_t count;
} HeatMapSquare;

static inline int compare_heat_map_squares(const void *a, const void *b) {
  HeatMapSquare *sa = (HeatMapSquare *)a;
  HeatMapSquare *sb = (HeatMapSquare *)b;
  return sb->count > sa->count;
}

static inline void string_builder_add_heat_map(StringBuilder *sb,
                                               const HeatMap *hm,
                                               heat_map_t heat_map_type,
                                               int max_squares_to_display) {
  HeatMapSquare squares[BOARD_DIM * BOARD_DIM];

  int current_square_index = 0;
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      HeatMapSquare *square = &squares[current_square_index++];
      square->row = row;
      square->col = col;
      square->count = heat_map_get_count(hm, row, col, heat_map_type);
    }
  }

  qsort(squares, BOARD_DIM * BOARD_DIM, sizeof(HeatMapSquare),
        compare_heat_map_squares);

  int squares_to_display = BOARD_DIM * BOARD_DIM;
  if (squares_to_display > max_squares_to_display) {
    squares_to_display = max_squares_to_display;
  }

  StringGrid *sg = string_grid_create(squares_to_display + 1, 2, 1);
  string_grid_set_cell(sg, 0, 0, string_duplicate(""));
  string_grid_set_cell(sg, 0, 1, string_duplicate("Count"));
  for (int i = 0; i < BOARD_DIM * BOARD_DIM && i < max_squares_to_display;
       i++) {
    int curr_col = 0;
    int curr_row = i + 1;
    string_grid_set_cell(
        sg, curr_row, curr_col++,
        get_formatted_string("%d%c", squares[i].row + 1,
                             squares[i].col + ASCII_UPPERCASE_A));
    string_grid_set_cell(sg, curr_row, curr_col++,
                         get_formatted_string("%lu", squares[i].count));
  }
  string_builder_add_string_grid(sb, sg, false);
  string_grid_destroy(sg);
}

#endif