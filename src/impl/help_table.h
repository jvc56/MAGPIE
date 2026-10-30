#ifndef HELP_TABLE_H
#define HELP_TABLE_H

#include "../def/config_defs.h"
#include <stdbool.h>

// The option groups, in the order help shows them.
typedef enum {
  HELP_OPTIONS_SETUP,
  HELP_OPTIONS_TABLES,
  HELP_OPTIONS_MOVEGEN,
  HELP_OPTIONS_SIM,
  HELP_OPTIONS_INFERENCE,
  HELP_OPTIONS_ENDGAME,
  HELP_OPTIONS_SOLVER,
  HELP_OPTIONS_PEG,
  HELP_OPTIONS_PAT,
  HELP_OPTIONS_ANALYZE,
  HELP_OPTIONS_AUTOPLAY,
  HELP_OPTIONS_DISPLAY,
  HELP_OPTIONS_SESSION,
  NUMBER_OF_HELP_OPTION_GROUPS,
} help_option_group_t;

// One row of an option list: an option and its per-player forms. A form the
// option does not have is NUMBER_OF_ARG_TOKENS.
typedef struct HelpOptionRow {
  arg_token_t both_players;
  arg_token_t player_one;
  arg_token_t player_two;
} HelpOptionRow;

typedef struct HelpOptionGroup {
  const char *title;
  // What to give help to see the group: a command that lists it, or for a
  // shared group a topic of its own.
  const char *help_subject;
  // A shared group applies too widely for its options to name their commands.
  bool is_shared;
  const HelpOptionRow *rows;
  int num_rows;
} HelpOptionGroup;

// A section of the command index.
typedef struct HelpCommandSection {
  const char *title;
  const arg_token_t *commands;
  int num_commands;
} HelpCommandSection;

const HelpOptionGroup *help_table_get_option_group(help_option_group_t group);

int help_table_get_num_command_sections(void);
const HelpCommandSection *help_table_get_command_section(int section_idx);

// The group holding the option, or NUMBER_OF_HELP_OPTION_GROUPS for a command.
help_option_group_t help_table_find_option_group(arg_token_t option);

// Whether the help for the command lists the group's options.
bool help_table_command_lists_group(arg_token_t command,
                                    help_option_group_t group);

// Whether the help for the command points to the group, because the command
// uses it but it belongs to another command.
bool help_table_command_points_to_group(arg_token_t command,
                                        help_option_group_t group);

#endif
