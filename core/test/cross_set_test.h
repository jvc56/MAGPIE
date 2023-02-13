#ifndef CROSS_SET_TEST_H
#define CROSS_SET_TEST_H

#include "../src/game.h"

#include "test_config.h"

void set_row(Game * game, int row, const char* row_content);
void test_cross_set(TestConfig * test_config);

#endif