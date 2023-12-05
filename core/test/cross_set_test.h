#ifndef CROSS_SET_TEST_H
#define CROSS_SET_TEST_H

#include "../src/ent/game.h"

#include "testconfig.h"

void set_row(Game *game, int row, const char *row_content);
void test_cross_set(TestConfig *testconfig);

#endif