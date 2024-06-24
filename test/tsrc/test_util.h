#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stdbool.h>

#include "../../src/ent/bag.h"
#include "../../src/ent/board.h"
#include "../../src/ent/game.h"
#include "../../src/ent/inference_results.h"
#include "../../src/ent/letter_distribution.h"
#include "../../src/ent/move.h"
#include "../../src/ent/rack.h"
#include "../../src/ent/validated_move.h"

#include "../../src/impl/config.h"

#define TRIVIAL_CROSS_SET_STRING "*"
#define DEFAULT_TEST_DATA_PATH "./data:./testdata"

typedef struct SortedMoveList {
  int count;
  Move **moves;
} SortedMoveList;

uint64_t string_to_cross_set(const LetterDistribution *ld, const char *letters);
char *cross_set_to_string(const LetterDistribution *ld, uint64_t input);
void draw_rack_to_string(const LetterDistribution *ld, Bag *bag, Rack *rack,
                         char *letters, int player_index);
void play_top_n_equity_move(Game *game, int n);
SortedMoveList *sorted_move_list_create(MoveList *ml);
void sorted_move_list_destroy(SortedMoveList *sorted_move_list);
void print_move_list(const Board *board, const LetterDistribution *ld,
                     const SortedMoveList *sml, int move_list_length);
void sort_and_print_move_list(const Board *board, const LetterDistribution *ld,
                              MoveList *ml);
void resort_sorted_move_list_by_score(SortedMoveList *sml);
bool within_epsilon(double a, double b);
int count_newlines(const char *str);
bool equal_rack(const Rack *expected_rack, const Rack *actual_rack);
void assert_strings_equal(const char *str1, const char *str2);
void assert_move(Game *game, MoveList *move_list, const SortedMoveList *sml,
                 int move_index, const char *expected_move_string);
void assert_bags_are_equal(const Bag *b1, const Bag *b2, int rack_array_size);
void assert_boards_are_equal(Board *b1, Board *b2);
void assert_games_are_equal(Game *g1, Game *g2, bool check_scores);
void print_game(Game *game, MoveList *move_list);
void print_cgp(const Game *game);
void print_rack(const Rack *rack, const LetterDistribution *ld);
void print_inference(const LetterDistribution *ld,
                     const Rack *target_played_tiles,
                     InferenceResults *inference_results);
void load_cgp_or_die(Game *game, const char *cgp);
void load_and_exec_config_or_die(Config *config, const char *cmd);
char *get_test_filename(const char *filename);
void delete_file(const char *filename);
void reset_file(const char *filename);
void fifo_create(const char *fifo_name);
void delete_fifo(const char *fifo_name);
Config *config_create_or_die(const char *cmd);
Config *config_create_default_test(void);
void set_row(Game *game, int row, const char *row_content);
void assert_board_layout_error(const char *data_path,
                               const char *board_layout_filename,
                               board_layout_load_status_t expected_status);
void load_game_with_test_board(Game *game, const char *data_path,
                               const char *board_layout_name);
void assert_validated_and_generated_moves(Game *game, const char *rack_string,
                                          const char *move_position,
                                          const char *move_tiles,
                                          int move_score,
                                          bool play_move_on_board);
ValidatedMoves *assert_validated_move_success(Game *game, const char *cgp_str,
                                              const char *move_str,
                                              int player_index,
                                              bool allow_phonies,
                                              bool allow_playthrough);
void assert_game_matches_cgp(const Game *game, const char *expected_cgp,
                             bool write_player_on_turn_first);
#endif
