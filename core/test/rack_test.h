#ifndef RACK_TEST_H
#define RACK_TEST_H

#include "../src/letter_distribution.h"
#include "testconfig.h"

void test_rack(TestConfig *testconfig);
int equal_rack(Rack *expected_rack, Rack *actual_rack);

#endif