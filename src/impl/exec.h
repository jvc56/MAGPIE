
#ifndef EXEC_H
#define EXEC_H

#include "../def/config_defs.h"
#include "../util/io_util.h"
#include "config.h"

void execute_command_sync(Config *config, ErrorStack *error_stack,
                          const char *command);
void execute_command_async(Config *config, ErrorStack *error_stack,
                           const char *command);
bool run_str_api_command(Config *config, ErrorStack *error_stack,
                         const char *command, char **output);
bool load_command_sync(Config *config, ErrorStack *error_stack,
                       const char *command);
char *command_search_status(Config *config, bool should_exit);
// Writes config's current settings to config_get_settings_filename(config)
// (settings.txt by default), the same file the REPL loop keeps in sync after
// every command. No-op if config_get_save_settings(config) is false.
void save_config_settings(const Config *config, ErrorStack *error_stack);
void caches_destroy(void);
void process_command_default(int argc, char *argv[]);
void process_command_with_config_args(int argc, char *argv[],
                                      const ConfigArgs *config_args);

#endif
