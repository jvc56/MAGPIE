#include <stdlib.h>

#include "../src/config.h"

#include "alphabet_test.h"
#include "bag_test.h"
#include "board_test.h"
#include "cross_set_test.h"
#include "equity_adjustment_test.h"
#include "game_test.h"
#include "gameplay_test.h"
#include "leaves_test.h"
#include "letter_distribution_test.h"
#include "movegen_test.h"
#include "rack_test.h"

int main() {
    // Test the readonly data first
    test_alphabet();
    test_leaves();
    test_letter_distribution();
    test_bag();
    test_rack();
    test_board();
    test_cross_set();
    test_game();
    test_movegen();
    test_equity_adjustments();
    test_gameplay();
}