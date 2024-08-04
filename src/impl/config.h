#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>

#include "../def/autoplay_defs.h"
#include "../def/config_defs.h"
#include "../def/game_defs.h"
#include "../def/simmer_defs.h"

#include "../ent/autoplay_results.h"
#include "../ent/board_layout.h"
#include "../ent/error_status.h"
#include "../ent/game.h"
#include "../ent/inference_results.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/players_data.h"
#include "../ent/sim_results.h"
#include "../ent/thread_control.h"
#include "../ent/win_pct.h"

typedef struct Config Config;

// Constructors and Destructors
Config *config_create_default(void);
void config_destroy(Config *config);

// Loading commands and execution
config_load_status_t config_load_command(Config *config, const char *cmd);
void config_execute_command(Config *config);
char *config_get_execute_status(Config *config);
bool config_continue_on_coldstart(const Config *config);

// Getters
const char *config_get_data_paths(const Config *config);
int config_get_bingo_bonus(const Config *config);
BoardLayout *config_get_board_layout(const Config *config);
game_variant_t config_get_game_variant(const Config *config);
double config_get_equity_margin(const Config *config);
WinPct *config_get_win_pcts(const Config *config);
int config_get_num_plays(const Config *config);
int config_get_plies(const Config *config);
int config_get_max_iterations(const Config *config);
double config_get_stop_cond_pct(const Config *config);
bool config_get_use_game_pairs(const Config *config);
uint64_t config_get_seed(const Config *config);
PlayersData *config_get_players_data(const Config *config);
LetterDistribution *config_get_ld(const Config *config);
ThreadControl *config_get_thread_control(const Config *config);
ErrorStatus *config_get_error_status(const Config *config);
exec_mode_t config_get_exec_mode(const Config *config);
Game *config_get_game(const Config *config);
MoveList *config_get_move_list(const Config *config);
SimResults *config_get_sim_results(const Config *config);
AutoplayResults *config_get_autoplay_results(const Config *config);

// Entity creators

Game *config_game_create(const Config *config);

// Impl
inference_status_t config_infer(const Config *config, int target_index,
                                int target_score, int target_num_exch,
                                Rack *target_played_tiles,
                                InferenceResults *results);
autoplay_status_t config_autoplay(const Config *config,
                                  AutoplayResults *autoplay_results, int gens,
                                  int max_force_draw_turn,
                                  autoplay_t autoplay_type);
sim_status_t config_simulate(const Config *config, Rack *known_opp_rack,
                             SimResults *sim_results);
#endif
