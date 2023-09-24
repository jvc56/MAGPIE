#ifndef superconfig_H
#define superconfig_H

#include "../src/config.h"

typedef struct SuperConfig {
  Config *csw_config;
  Config *nwl_config;
  Config *osps_config; // osps - polish - many unicode chars
  Config *disc_config; // disc - catalan - includes multi-char tiles
  Config *distinct_lexica_config;
} SuperConfig;

SuperConfig *create_superconfig(Config *csw_config, Config *nwl_config,
                                Config *osps_config, Config *disc_config,
                                Config *distinct_lexica_config);
Config *get_nwl_config(SuperConfig *superconfig);
Config *get_csw_config(SuperConfig *superconfig);
Config *get_osps_config(SuperConfig *superconfig);
Config *get_disc_config(SuperConfig *superconfig);
Config *get_distinct_lexica_config(SuperConfig *superconfig);
void destroy_superconfig(SuperConfig *superconfig);

#endif
