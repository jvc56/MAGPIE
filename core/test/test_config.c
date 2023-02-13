#include <stdlib.h>

#include "../src/config.h"

#include "test_config.h"

Config * get_csw_config(TestConfig * test_config) {
    return test_config->csw_config;
}

Config * get_america_config(TestConfig * test_config) {
    return test_config->america_config;
}

TestConfig * create_test_config(Config * csw_config, Config * america_config) {
    TestConfig * test_config = malloc(sizeof(TestConfig));
    test_config->csw_config = csw_config;
    test_config->america_config = america_config;
    return test_config;
}

void destroy_test_config(TestConfig * test_config) {
    destroy_config(test_config->csw_config);
    destroy_config(test_config->america_config);
    free(test_config);
}
