#ifndef TEST_CONFIG_H
#define TEST_CONFIG_H

#include "../src/config.h"

typedef struct TestConfig {
    Config * csw_config;
    Config * america_config;
} TestConfig;

TestConfig * create_test_config(Config * csw_config, Config * america_config);
Config * get_america_config(TestConfig * test_config);
Config * get_csw_config(TestConfig * test_config);
void destroy_test_config(TestConfig * test_config);

#endif
