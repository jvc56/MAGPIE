#ifndef RACK_TEST_H
#define RACK_TEST_H

void test_rack();
void set_rack_to_string(Rack * expected_rack, const char* rack_string, Alphabet * alphabet);
int equal_rack(Rack * expected_rack, Rack * actual_rack);

#endif