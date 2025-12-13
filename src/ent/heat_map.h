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
  uint64_t board_count_total;
  uint64_t board_count_maxes[NUM_HEAT_MAP_TYPES];
  uint64_t counts[NUM_HEAT_MAP_COUNTS];
} HeatMap;

enum {
  HEAT_MAP_NUM_BACKGROUND_COLORS = 7,
};

// static const char
//     *heat_map_ascending_color_codes[HEAT_MAP_NUM_BACKGROUND_COLORS] = {
//         "\x1b[49m", // Default background color
//         "\x1b[46m", // Cyan
//         "\x1b[44m", // Blue
//         "\x1b[42m", // Green
//         "\x1b[45m", // Magenta
//         "\x1b[43m", // Yellow
//         "\x1b[41m", // Red
// };

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
  hm->board_count_total = 0;
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
  hm->board_count_total++;
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

// FIXME: test
static inline uint64_t heat_map_get_board_count_total(const HeatMap *hm) {
  return hm->board_count_total;
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

typedef struct ColorSegment {
  int r_start;
  int g_start;
  int b_start;
  int r_end;
  int g_end;
  int b_end;
} ColorSegment;

static const ColorSegment heat_map_segments[] = {
    // Blank -> Green
    {0, 0, 0, 0, 255, 0},
    // Green -> Yellow
    {0, 255, 0, 255, 255, 0},
    // Yellow -> Red
    {255, 255, 0, 255, 0, 0}};

static const size_t NUM_SEGMENTS =
    sizeof(heat_map_segments) / sizeof(heat_map_segments[0]);

static inline int lerp_color(int start, int end, double fraction) {
  double interp_double =
      (double)start + fraction * ((double)end - (double)start);
  return (int)round(interp_double);
}

static inline char *
heat_map_format_color_escape_string(const int r, const int g, const int b) {
  return get_formatted_string("\x1b[48;2;%d;%d;%dm", r, g, b);
}

static inline char *interpolate_rgb(double fraction) {
  if (fraction < 0.0) {
    fraction = 0.0;
  } else if (fraction > 1.0) {
    fraction = 1.0;
  }

  if (fraction == 1.0) {
    const ColorSegment *last_segment = &heat_map_segments[NUM_SEGMENTS - 1];
    return heat_map_format_color_escape_string(
        last_segment->r_end, last_segment->g_end, last_segment->b_end);
  }

  const double segment_size = 1.0 / (double)NUM_SEGMENTS;

  size_t segment_index = (size_t)floor(fraction / segment_size);

  if (segment_index >= NUM_SEGMENTS) {
    segment_index = NUM_SEGMENTS - 1;
  }

  const ColorSegment *segment = &heat_map_segments[segment_index];

  double start_fraction = (double)segment_index * segment_size;
  double f = (fraction - start_fraction) / segment_size;

  int r = lerp_color(segment->r_start, segment->r_end, f);
  int g = lerp_color(segment->g_start, segment->g_end, f);
  int b = lerp_color(segment->b_start, segment->b_end, f);

  r = (r > 255) ? 255 : ((r < 0) ? 0 : r);
  g = (g > 255) ? 255 : ((g < 0) ? 0 : g);
  b = (b > 255) ? 255 : ((b < 0) ? 0 : b);

  return heat_map_format_color_escape_string(r, g, b);
}

static inline char *heat_map_get_color_escape_code(const HeatMap *hm,
                                                   const int row, const int col,
                                                   const heat_map_t type) {
  // Add background color
  const uint64_t square_count = heat_map_get_count(hm, row, col, type);
  // FIXME: make this optional between max and total
  const uint64_t total = heat_map_get_board_count_max(hm, type);
  double frac = 0;
  if (total > 0) {
    frac = (double)square_count / (double)total;
  }
  return interpolate_rgb(frac);
}

#endif