#ifndef CONFIG_H
#define CONFIG_H

#include "../def/autoplay_defs.h"
#include "../def/config_defs.h"
#include "../def/convert_defs.h"
#include "../def/game_defs.h"
#include "../ent/autoplay_results.h"
#include "../ent/board_layout.h"
#include "../ent/conversion_results.h"
#include "../ent/endgame_results.h"
#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../ent/inference_results.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/players_data.h"
#include "../ent/sim_results.h"
#include "../ent/thread_control.h"
#include "../ent/win_pct.h"
#include "../impl/simmer.h"
#include "../util/io_util.h"
#include <stdbool.h>

typedef struct Config Config;

typedef struct ConfigArgs {
  const char *data_paths;
  const char *settings_filename;
  bool use_wmp;
} ConfigArgs;

// Constructors and Destructors
Config *config_create(const ConfigArgs *args, ErrorStack *error_stack);
void config_destroy(Config *config);

// Loading commands and execution
void config_load_command(Config *config, const char *cmd,
                         ErrorStack *error_stack);
void config_execute_command(Config *config, ErrorStack *error_stack);
char *config_get_execute_status(Config *config);
bool config_continue_on_coldstart(const Config *config);
bool config_run_str_api_command(Config *config, ErrorStack *error_stack,
                                char **output);

// Getters
const char *config_get_data_paths(const Config *config);
int config_get_bingo_bonus(const Config *config);
BoardLayout *config_get_board_layout(const Config *config);
game_variant_t config_get_game_variant(const Config *config);
WinPct *config_get_win_pcts(const Config *config);
int config_get_num_plays(const Config *config);
int config_get_num_small_plays(const Config *config);
int config_get_plies(const Config *config);
int config_get_shplies(const Config *config);
int config_get_endgame_plies(const Config *config);
uint64_t config_get_max_iterations(const Config *config);
uint64_t config_get_seed(const Config *config);
double config_get_stop_cond_pct(const Config *config);
bool config_get_use_game_pairs(const Config *config);
bool config_get_use_small_plays(const Config *config);
bool config_get_human_readable(const Config *config);
bool config_get_show_prompt(const Config *config);
bool config_get_save_settings(const Config *config);
bool config_get_loaded_settings(const Config *config);
void config_set_loaded_settings(Config *config, const bool value);
double config_get_tt_fraction_of_mem(const Config *config);
PlayersData *config_get_players_data(const Config *config);
LetterDistribution *config_get_ld(const Config *config);
ThreadControl *config_get_thread_control(const Config *config);
exec_mode_t config_get_exec_mode(const Config *config);
Game *config_get_game(const Config *config);
GameHistory *config_get_game_history(const Config *config);
MoveList *config_get_move_list(const Config *config);
SimResults *config_get_sim_results(const Config *config);
EndgameResults *config_get_endgame_results(const Config *config);
AutoplayResults *config_get_autoplay_results(const Config *config);
const char *config_get_settings_filename(const Config *config);
const char *config_get_current_exec_name(const Config *config);
int config_get_num_threads(const Config *config);
int config_get_print_interval(const Config *config);
Equity config_get_eq_margin_inference(const Config *config);

// Entity creators
Game *config_game_create(const Config *config);

// Impl
void config_infer(const Config *config, bool use_game_history, int target_index,
                  Equity target_score, int target_num_exch,
                  Rack *target_played_tiles, Rack *target_known_rack,
                  Rack *nontarget_known_rack,
                  bool use_inference_cutoff_optimization,
                  InferenceResults *results, ErrorStack *error_stack);
void config_endgame(Config *config, EndgameResults *endgame_results,
                    ErrorStack *error_stack);
void config_autoplay(const Config *config, AutoplayResults *autoplay_results,
                     autoplay_t autoplay_type,
                     const char *num_games_or_min_rack_targets,
                     int games_before_force_draw_start,
                     ErrorStack *error_stack);
void config_simulate(Config *config, SimCtx **sim_ctx, Rack *known_opp_rack,
                     SimResults *sim_results, ErrorStack *error_stack);
void config_convert(const Config *config, ConversionResults *results,
                    ErrorStack *error_stack);
void config_parse_gcg(Config *config, const char *gcg_filename,
                      GameHistory *game_history, ErrorStack *error_stack);
void config_parse_gcg_string(Config *config, const char *gcg_string,
                             GameHistory *game_history,
                             ErrorStack *error_stack);
// Settings
void config_add_settings_to_string_builder(const Config *config,
                                           StringBuilder *sb);

#endif
