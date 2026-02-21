#ifndef MOVE_STRING_H
#define MOVE_STRING_H

#include "../ent/board.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/thread_control.h"
#include "../util/string_util.h"

void string_builder_add_move_description(StringBuilder *move_string_builder,
                                         const Move *move,
                                         const LetterDistribution *ld);
void string_builder_add_move(StringBuilder *string_builder, const Board *board,
                             const Move *m, const LetterDistribution *ld,
                             bool add_score);

void string_builder_add_ucgi_move(StringBuilder *move_string_builder,
                                  const Move *move, const Board *board,
                                  const LetterDistribution *ld);
void string_builder_add_gcg_move(StringBuilder *move_string_builder,
                                 const Move *move,
                                 const LetterDistribution *ld);

void string_builder_add_move_leave(StringBuilder *sb, const Rack *rack,
                                   const Move *move,
                                   const LetterDistribution *ld);

bool move_matches_filters(const Move *move, int filter_row, int filter_col,
                          const MachineLetter *prefix_mls, int prefix_len,
                          bool exclude_tile_placement_moves,
                          const Board *board);

void string_builder_add_move_list(
    StringBuilder *string_builder, const MoveList *move_list,
    const Board *board, const LetterDistribution *ld, int max_num_display_plays,
    int filter_row, int filter_col, const MachineLetter *prefix_mls,
    int prefix_len, bool exclude_tile_placement_moves, bool use_ucgi_format,
    const char *game_board_string);

char *move_list_get_string(const MoveList *move_list, const Board *board,
                           const LetterDistribution *ld,
                           int max_num_display_plays, int filter_row,
                           int filter_col, const MachineLetter *prefix_mls,
                           int prefix_len, bool exclude_tile_placement_moves,
                           bool use_ucgi_format,
                           const char *game_board_string);

#endif
