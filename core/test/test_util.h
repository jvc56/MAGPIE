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

#define TEST_MOVE_LIST_CAPACITY 1000000

typedef struct SortedMoveList {
  int count;
  Move **moves;
} SortedMoveList;

void draw_rack_to_string(Bag *bag, Rack *rack, char *letters,
                         LetterDistribution *letter_distribution);
void generate_moves_for_game(Game *game);
double get_leave_value_for_rack(KLV *klv, Rack *rack);
void play_top_n_equity_move(Game *game, int n);
SortedMoveList *create_sorted_move_list(MoveList *ml);
void destroy_sorted_move_list(SortedMoveList *sorted_move_list);
void print_anchor_list(Generator *gen);
void print_move_list(Board *board, LetterDistribution *letter_distribution,
                     SortedMoveList *sml, int move_list_length);
void sort_and_print_move_list(Board *board,
                              LetterDistribution *letter_distribution,
                              MoveList *ml);
int within_epsilon(double a, double b);
int count_newlines(const char *str);
void assert_strings_equal(const char *str1, const char *str2);
void assert_move(Game *game, SortedMoveList *sml, int move_index,
                 char *expected_move_string);
void print_game(Game *game);
void print_inference(Inference *inference, Rack *rack);

#endif
