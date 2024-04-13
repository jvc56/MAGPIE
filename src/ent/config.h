#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#include "../def/board_defs.h"
#include "../def/config_defs.h"
#include "../def/game_defs.h"
#include "../def/kwg_defs.h"
#include "../def/simmer_defs.h"

#include "board_layout.h"
#include "letter_distribution.h"
#include "players_data.h"
#include "rack.h"
#include "thread_control.h"
#include "win_pct.h"

typedef struct Config Config;

Config *config_create_default();
void config_destroy(Config *config);

command_t config_get_command_type(const Config *config);
LetterDistribution *config_get_ld(const Config *config);
char *config_get_ld_name(const Config *config);
bool config_get_ld_name_changed(const Config *config);
const char *config_get_cgp(const Config *config);
const char *config_get_moves(const Config *config);
int config_get_bingo_bonus(const Config *config);
const BoardLayout *config_get_board_layout(const Config *config);
game_variant_t config_get_game_variant(const Config *config);

PlayersData *config_get_players_data(const Config *config);
Rack *config_get_rack(const Config *config);
int config_get_target_index(const Config *config);
int config_get_target_score(const Config *config);
int config_get_target_number_of_tiles_exchanged(const Config *config);
double config_get_equity_margin(const Config *config);
WinPct *config_get_win_pcts(const Config *config);
int config_get_num_plays(const Config *config);
int config_get_plies(const Config *config);
int config_get_max_iterations(const Config *config);
sim_stopping_condition_t config_get_stopping_condition(const Config *config);
bool config_get_static_search_only(const Config *config);
bool config_get_use_game_pairs(const Config *config);
uint64_t config_get_seed(const Config *config);
ThreadControl *config_get_thread_control(const Config *config);
exec_mode_t config_get_exec_mode(const Config *config);
conversion_type_t config_get_conversion_type(const Config *config);
char *config_get_input_filename(const Config *config);
char *config_get_output_filename(const Config *config);
bool config_get_command_set_infile(const Config *config);
bool config_get_command_set_exec_mode(const Config *config);

config_load_status_t config_load(Config *config, const char *cmd);
bool config_continue_on_coldstart(const Config *config);
game_variant_t get_game_variant_type_from_name(const char *variant_name);

#endif
