#ifndef superconfig_H
#define superconfig_H

#include "../src/config.h"

typedef struct SuperConfig {
    Config * csw_config;
    Config * nwl_config;
} SuperConfig;

SuperConfig * create_superconfig(Config * csw_config, Config * nwl_config);
Config * get_nwl_config(SuperConfig * superconfig);
Config * get_csw_config(SuperConfig * superconfig);
void destroy_superconfig(SuperConfig * superconfig);

#endif
