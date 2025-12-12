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

typedef struct HeatMap {
  size_t num_plays;
  size_t num_plies;
  size_t num_counts;
  uint64_t *total_board_moves;
  uint64_t *board_count_maxes;
  uint64_t *counts;
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

static inline size_t heat_map_get_total_count(HeatMap *hm) {
  return hm->num_plays * hm->num_plies * BOARD_DIM * BOARD_DIM *
         NUM_HEAT_MAP_TYPES;
}

static inline HeatMap *heat_map_create(void) {
  return (HeatMap *)calloc_or_die(1, sizeof(HeatMap));
}

static inline void heat_map_destroy(HeatMap *hm) {
  if (!hm) {
    return;
  }
  free(hm->counts);
  free(hm->total_board_moves);
  free(hm->board_count_maxes);
  free(hm);
}

static inline void heat_map_reset(HeatMap *hm, const int num_plays,
                                  const int num_plies) {
  const size_t old_num_boards =
      hm->num_plays * hm->num_plies * NUM_HEAT_MAP_TYPES;
  const size_t new_num_boards = num_plays * num_plies * NUM_HEAT_MAP_TYPES;
  if (!hm->total_board_moves) {
    hm->total_board_moves =
        (uint64_t *)calloc_or_die(new_num_boards, sizeof(uint64_t));
    hm->board_count_maxes =
        (uint64_t *)calloc_or_die(new_num_boards, sizeof(uint64_t));
  } else if (new_num_boards > old_num_boards) {
    hm->total_board_moves = (uint64_t *)realloc_or_die(
        hm->total_board_moves, new_num_boards * sizeof(uint64_t));
    hm->board_count_maxes = (uint64_t *)realloc_or_die(
        hm->board_count_maxes, new_num_boards * sizeof(uint64_t));
    memset(hm->total_board_moves, 0, new_num_boards * sizeof(uint64_t));
    memset(hm->board_count_maxes, 0, new_num_boards * sizeof(uint64_t));
  } else {
    memset(hm->total_board_moves, 0, new_num_boards * sizeof(uint64_t));
    memset(hm->board_count_maxes, 0, new_num_boards * sizeof(uint64_t));
  }

  hm->num_plays = num_plays;
  hm->num_plies = num_plies;

  size_t old_num_counts = hm->num_counts;
  hm->num_counts = heat_map_get_total_count(hm);
  if (!hm->counts) {
    hm->counts = (uint64_t *)calloc_or_die(hm->num_counts, sizeof(uint64_t));
  } else if (hm->num_counts > old_num_counts) {
    hm->counts = (uint64_t *)realloc_or_die(hm->counts,
                                            hm->num_counts * sizeof(uint64_t));
    memset(hm->counts, 0, hm->num_counts * sizeof(uint64_t));
  } else {
    memset(hm->counts, 0, hm->num_counts * sizeof(uint64_t));
  }
}

static inline size_t heat_map_get_index(const HeatMap *hm, int play, int ply,
                                        int row, int col, heat_map_t type) {
  return (
      (((((size_t)play * hm->num_plies + ply) * BOARD_DIM + row) * BOARD_DIM +
        col) *
       NUM_HEAT_MAP_TYPES) +
      type);
}

static inline uint64_t heat_map_get_count(const HeatMap *hm, int play, int ply,
                                          int row, int col, heat_map_t type) {
  return hm->counts[heat_map_get_index(hm, play, ply, row, col, type)];
}

static inline size_t heat_map_get_board_index(const HeatMap *hm, int play,
                                              int ply, heat_map_t type) {
  return (size_t)play * hm->num_plies * NUM_HEAT_MAP_TYPES +
         (size_t)ply * NUM_HEAT_MAP_TYPES + type;
}

static inline void heat_map_add_move(HeatMap *hm, int play, int ply,
                                     const Move *move) {
  const size_t board_index_all =
      heat_map_get_board_index(hm, play, ply, HEAT_MAP_TYPE_ALL);
  hm->total_board_moves[board_index_all]++;
  if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return;
  }
  const bool is_bingo = move_get_tiles_played(move) == RACK_SIZE;
  const size_t board_index_bingo =
      heat_map_get_board_index(hm, play, ply, HEAT_MAP_TYPE_BINGO);
  if (is_bingo) {
    hm->total_board_moves[board_index_bingo]++;
  }
  const int row_inc = move_get_dir(move) == BOARD_VERTICAL_DIRECTION;
  const int col_inc = move_get_dir(move) == BOARD_HORIZONTAL_DIRECTION;
  size_t current_offset = heat_map_get_index(
      hm, play, ply, move_get_row_start(move), move_get_col_start(move), 0);
  const int move_len = move_get_tiles_length(move);
  for (int i = 0; i < move_len; i++) {
    const MachineLetter letter = move_get_tile(move, i);
    if (letter != PLAYED_THROUGH_MARKER) {
      const size_t current_all_index = current_offset + HEAT_MAP_TYPE_ALL;
      hm->counts[current_all_index]++;
      if (hm->counts[current_all_index] >
          hm->board_count_maxes[board_index_all]) {
        hm->board_count_maxes[board_index_all] = hm->counts[current_all_index];
      }
      if (is_bingo) {
        const size_t current_bingo_index = current_offset + HEAT_MAP_TYPE_BINGO;
        hm->counts[current_bingo_index]++;
        if (hm->counts[current_bingo_index] >
            hm->board_count_maxes[board_index_bingo]) {
          hm->board_count_maxes[board_index_bingo] =
              hm->counts[current_bingo_index];
        }
      }
    }
    current_offset += heat_map_get_index(hm, 0, 0, row_inc, col_inc, 0);
  }
}

static inline uint64_t heat_map_get_total_counts_sum(const HeatMap *hm) {
  uint64_t sum = 0;
  for (size_t i = 0; i < hm->num_counts; i++) {
    sum += hm->counts[i];
  }
  return sum;
}

static inline size_t heat_map_get_num_plays(const HeatMap *hm) {
  return hm->num_plays;
}

static inline size_t heat_map_get_num_plies(const HeatMap *hm) {
  return hm->num_plies;
}

static inline uint64_t
heat_map_get_board_total_moves(const HeatMap *hm, const int play, const int ply,
                               heat_map_t heat_map_type) {
  return hm->total_board_moves[heat_map_get_board_index(hm, play, ply,
                                                        heat_map_type)];
}

static inline uint64_t heat_map_get_board_count_max(const HeatMap *hm,
                                                    const int play,
                                                    const int ply,
                                                    heat_map_t heat_map_type) {
  return hm->board_count_maxes[heat_map_get_board_index(hm, play, ply,
                                                        heat_map_type)];
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
                                               const HeatMap *hm, int play,
                                               int ply,
                                               heat_map_t heat_map_type,
                                               int max_squares_to_display) {
  HeatMapSquare squares[BOARD_DIM * BOARD_DIM];

  for (int i = 0; i < BOARD_DIM; i++) {
    for (int j = 0; j < BOARD_DIM; j++) {
      HeatMapSquare square;
      square.row = i;
      square.col = j;
      square.count = heat_map_get_count(hm, play, ply, i, j, heat_map_type);
      squares[i * BOARD_DIM + j] = square;
    }
  }

  qsort(squares, BOARD_DIM * BOARD_DIM, sizeof(HeatMapSquare),
        compare_heat_map_squares);

  int squares_to_display = BOARD_DIM * BOARD_DIM;
  if (squares_to_display > max_squares_to_display) {
    squares_to_display = max_squares_to_display;
  }

  StringGrid *sg = string_grid_create(squares_to_display + 1, 3, 1);
  string_grid_set_cell(sg, 0, 0, string_duplicate(""));
  string_grid_set_cell(sg, 0, 1, string_duplicate("Count"));
  string_grid_set_cell(sg, 0, 2, string_duplicate("Total"));
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
    string_grid_set_cell(
        sg, curr_row, curr_col++,
        get_formatted_string("%lu", heat_map_get_board_total_moves(
                                        hm, play, ply, heat_map_type)));
  }
  string_builder_add_string_grid(sb, sg, false);
  string_grid_destroy(sg);
}

#endif
