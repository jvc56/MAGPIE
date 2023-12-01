#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>

#include "../def/config_defs.h"

struct Config;
typedef struct Config Config;

config_load_status_t load_config(Config *config, const char *cmd);
bool continue_on_coldstart(const Config *config);
Config *create_default_config();
void destroy_config(Config *config);

#endif
