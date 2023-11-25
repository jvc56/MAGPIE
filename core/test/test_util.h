#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include "../src/board.h"
#include "../src/config.h"
#include "../src/game.h"
#include "../src/infer.h"
#include "../src/klv.h"
#include "../src/letter_distribution.h"
#include "../src/move.h"
#include "../src/movegen.h"
#include "../src/rack.h"

typedef struct SortedMoveList {
  int count;
  Move **moves;
} SortedMoveList;

void draw_rack_to_string(const LetterDistribution *letter_distribution,
                         Bag *bag, Rack *rack, char *letters, int player_index);
void generate_moves_for_game(Game *game);
double get_leave_value_for_rack(const KLV *klv, const Rack *rack);
void generate_leaves_for_game(Game *game, bool add_exchanges);
void play_top_n_equity_move(Game *game, int n);
SortedMoveList *create_sorted_move_list(MoveList *ml);
void destroy_sorted_move_list(SortedMoveList *sorted_move_list);
void print_anchor_list(const Generator *gen);
void print_move_list(const Board *board,
                     const LetterDistribution *letter_distribution,
                     const SortedMoveList *sml, int move_list_length);
void sort_and_print_move_list(const Board *board,
                              const LetterDistribution *letter_distribution,
                              MoveList *ml);
bool within_epsilon(double a, double b);
int count_newlines(const char *str);
bool equal_rack(const Rack *expected_rack, const Rack *actual_rack);
void assert_strings_equal(const char *str1, const char *str2);
void assert_move(const Game *game, const SortedMoveList *sml, int move_index,
                 const char *expected_move_string);
void assert_bags_are_equal(const Bag *b1, const Bag *b2, int rack_array_size);
void print_game(const Game *game);
void print_inference(const Inference *inference, const Rack *rack);
void load_config_or_die(Config *config, const char *cmd);
char *get_test_filename(const char *filename);
void delete_file(const char *filename);
void reset_file(const char *filename);
void create_fifo(const char *fifo_name);
void delete_fifo(const char *fifo_name);

#endif
