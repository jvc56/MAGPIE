#include <stdlib.h>

#include "../src/ent/config.h"

#include "../src/util/log.h"
#include "../src/util/util.h"

#include "testconfig.h"

Config *get_csw_config(TestConfig *testconfig) {
  return testconfig->csw_config;
}

Config *get_nwl_config(TestConfig *testconfig) {
  return testconfig->nwl_config;
}

Config *get_osps_config(TestConfig *testconfig) {
  return testconfig->osps_config;
}

Config *get_disc_config(TestConfig *testconfig) {
  return testconfig->disc_config;
}

Config *get_distinct_lexica_config(TestConfig *testconfig) {
  return testconfig->distinct_lexica_config;
}

Config *create_and_load_config(const char *cmd) {
  Config *config = create_default_config();
  config_load_status_t config_load_status = load_config(config, cmd);
  if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
    log_fatal("failed to create config with\ncommand: %s\nerror:   %d\n", cmd,
              config_load_status);
  }
  return config;
}

TestConfig *create_testconfig(const char *csw_config_string,
                              const char *nwl_config_string,
                              const char *osps_config_string,
                              const char *disc_config_string,
                              const char *distinct_lexica_config_string) {
  TestConfig *testconfig = malloc_or_die(sizeof(TestConfig));
  testconfig->csw_config = create_and_load_config(csw_config_string);
  testconfig->nwl_config = create_and_load_config(nwl_config_string);
  testconfig->osps_config = create_and_load_config(osps_config_string);
  testconfig->disc_config = create_and_load_config(disc_config_string);
  testconfig->distinct_lexica_config =
      create_and_load_config(distinct_lexica_config_string);
  return testconfig;
}

void destroy_testconfig(TestConfig *testconfig) {
  destroy_config(testconfig->csw_config);
  destroy_config(testconfig->nwl_config);
  destroy_config(testconfig->osps_config);
  destroy_config(testconfig->disc_config);
  destroy_config(testconfig->distinct_lexica_config);
  free(testconfig);
}
