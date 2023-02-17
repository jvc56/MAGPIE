#include <stdlib.h>

#include "../src/config.h"

#include "superconfig.h"

Config * get_csw_config(SuperConfig * superconfig) {
    return superconfig->csw_config;
}

Config * get_america_config(SuperConfig * superconfig) {
    return superconfig->america_config;
}

SuperConfig * create_superconfig(Config * csw_config, Config * america_config) {
    SuperConfig * superconfig = malloc(sizeof(SuperConfig));
    superconfig->csw_config = csw_config;
    superconfig->america_config = america_config;
    return superconfig;
}

void destroy_superconfig(SuperConfig * superconfig) {
    destroy_config(superconfig->csw_config);
    destroy_config(superconfig->america_config);
    free(superconfig);
}
