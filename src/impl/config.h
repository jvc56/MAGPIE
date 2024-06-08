#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>

#include "../def/config_defs.h"
#include "../def/game_defs.h"

#include "../ent/board_layout.h"
#include "../ent/error_status.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"
#include "../ent/players_data.h"
#include "../ent/thread_control.h"
#include "../ent/win_pct.h"

typedef struct Config Config;

Config *config_create_default();
void config_destroy(Config *config);
config_load_status_t config_load_command(Config *config, const char *cmd);
void config_execute_command(Config *config);
char *config_get_execute_status(Config *config);
bool config_continue_on_coldstart(const Config *config);

int config_get_bingo_bonus(const Config *config);
BoardLayout *config_get_board_layout(const Config *config);
game_variant_t config_get_game_variant(const Config *config);
float config_get_equity_margin(const Config *config);
WinPct *config_get_win_pct(const Config *config);
int config_get_num_plays(const Config *config);
int config_get_plies(const Config *config);
int config_get_max_iterations(const Config *config);
float config_get_stopping_condition(const Config *config);
bool config_get_use_game_pairs(const Config *config);
uint64_t config_get_seed(const Config *config);
PlayersData *config_get_players_data(const Config *config);
LetterDistribution *config_get_letter_distribution(const Config *config);
ThreadControl *config_get_thread_control(const Config *config);
ErrorStatus *config_get_error_status(const Config *config);
exec_mode_t config_get_exec_mode(const Config *config);
Game *config_get_game(const Config *config);

// FIXME: see where this goes
void string_builder_add_game_variant(StringBuilder *sb,
                                     game_variant_t game_variant_type);

#endif
