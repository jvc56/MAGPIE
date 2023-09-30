#include <stdlib.h>

#include "../src/config.h"
#include "../src/util.h"

#include "superconfig.h"

Config *get_csw_config(SuperConfig *superconfig) {
  return superconfig->csw_config;
}

Config *get_nwl_config(SuperConfig *superconfig) {
  return superconfig->nwl_config;
}

Config *get_osps_config(SuperConfig *superconfig) {
  return superconfig->osps_config;
}

Config *get_disc_config(SuperConfig *superconfig) {
  return superconfig->disc_config;
}

Config *get_distinct_lexica_config(SuperConfig *superconfig) {
  return superconfig->distinct_lexica_config;
}

SuperConfig *create_superconfig(Config *csw_config, Config *nwl_config,
                                Config *osps_config, Config *disc_config,
                                Config *distinct_lexica_config) {
  SuperConfig *superconfig = malloc_or_die(sizeof(SuperConfig));
  superconfig->csw_config = csw_config;
  superconfig->nwl_config = nwl_config;
  superconfig->osps_config = osps_config;
  superconfig->disc_config = disc_config;
  superconfig->distinct_lexica_config = distinct_lexica_config;
  return superconfig;
}

void destroy_superconfig(SuperConfig *superconfig) {
  destroy_config(superconfig->csw_config);
  destroy_config(superconfig->nwl_config);
  destroy_config(superconfig->osps_config);
  destroy_config(superconfig->disc_config);
  destroy_config(superconfig->distinct_lexica_config);
  free(superconfig);
}
