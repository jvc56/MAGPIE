#include <stdlib.h>

#include "../src/config.h"

#include "superconfig.h"

Config * get_csw_config(SuperConfig * superconfig) {
    return superconfig->csw_config;
}

Config * get_nwl_config(SuperConfig * superconfig) {
    return superconfig->nwl_config;
}

SuperConfig * create_superconfig(Config * csw_config, Config * nwl_config) {
    SuperConfig * superconfig = malloc(sizeof(SuperConfig));
    superconfig->csw_config = csw_config;
    superconfig->nwl_config = nwl_config;
    return superconfig;
}

void destroy_superconfig(SuperConfig * superconfig) {
    destroy_config(superconfig->csw_config);
    destroy_config(superconfig->nwl_config);
    free(superconfig);
}
