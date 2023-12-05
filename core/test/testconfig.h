#ifndef testconfig_H
#define testconfig_H

#include "../src/ent/config.h"

typedef struct TestConfig {
  Config *csw_config;
  Config *nwl_config;
  Config *osps_config; // osps - polish - many unicode chars
  Config *disc_config; // disc - catalan - includes multi-char tiles
  Config *distinct_lexica_config;
} TestConfig;

TestConfig *create_testconfig(const char *csw_config, const char *nwl_config,
                              const char *osps_config, const char *disc_config,
                              const char *distinct_lexica_config);
Config *get_nwl_config(TestConfig *testconfig);
Config *get_csw_config(TestConfig *testconfig);
Config *get_osps_config(TestConfig *testconfig);
Config *get_disc_config(TestConfig *testconfig);
Config *get_distinct_lexica_config(TestConfig *testconfig);
void destroy_testconfig(TestConfig *testconfig);

#endif
