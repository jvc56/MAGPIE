
#include "players_strategies.h"
#include "constants.h"
#include "util.h"

#define DEFAULT_MOVE_SORT_TYPE MOVE_SORT_EQUITY
#define DEFAULT_MOVE_RECORD_TYPE MOVE_RECORD_BEST

PlayersStrategy *create_players_strategy(player_data_t PLAYERS_STRATEGY_TYPE) {
  PlayersStrategy *players_strategy = malloc_or_die(sizeof(PlayersStrategy));
  players_strategy->data_is_shared = false;
  players_strategy->p1_data_name = NULL;
  players_strategy->p2_data_name = NULL;
  for (int i = 0; i < NUMBER_OF_STRATEGIES; i++) {
    players_strategy->p1_data[i] = NULL;
    players_strategy->p2_data[i] = NULL;
  }
  return players_strategy;
}

void destroy_players_strategy(PlayersStrategy *players_strategy) {
  for (int i = 0; i < 2; i++) {
    destroy_player_data(players_strategy->players_data[i], false);
  }
  free(players_strategy);
}

PlayersStrategies *create_players_strategies() {
  PlayersStrategies *players_stratgies =
      malloc_or_die(sizeof(PlayersStrategies));
  players_stratgies->move_record_type = DEFAULT_MOVE_RECORD_TYPE;
  players_stratgies->move_sort_type = DEFAULT_MOVE_SORT_TYPE;
  for (int i = 0; i < NUMBER_OF_STRATEGIES; i++) {
    players_stratgies->strategies[i] =
        create_players_strategy((player_data_t)i);
  }
}

void destroy_players_strategies(PlayersStrategies *players_strategies) {
  for (int i = 0; i < NUMBER_OF_STRATEGIES; i++) {
    destroy_players_strategy(players_strategies->strategies[i]);
  }
  free(players_strategies);
}

const char *get_player_data_name(PlayerData *player_data,
                                 player_data_t PLAYERS_STRATEGY_TYPE) {}

int get_index_of_existing_data(PlayersStrategies *players_strategies,
                               player_data_t PLAYERS_STRATEGY_TYPE,
                               const char *data_name) {
  int index = -1;
  for (int i = 0;
       i < players_strategies->strategies[(int)PLAYERS_STRATEGY_TYPE]; i++) {
    if (strings_equal(players_strategies->strategies[(int)PLAYERS_STRATEGY_TYPE]
                          ->players_data[i]
                          ->name,
                      data_name)) {
      index = i;
      break;
    }
  }
  return index;
}

void set_players_strategy(PlayersStrategies *players_strategies,
                          player_data_t PLAYERS_STRATEGY_TYPE,
                          const char *p1_data_name, const char *p2_data_name) {

  int p1_data_index = get_index_of_existing_data(
      players_strategies, PLAYERS_STRATEGY_TYPE, p1_data_name);
  int p2_data_index = get_index_of_existing_data(
      players_strategies, PLAYERS_STRATEGY_TYPE, p2_data_name);

  PlayerData *p1_player_data = NULL;

  if (p1_data_index < 0) {
    p1_player_data = create_player_data(PLAYERS_STRATEGY_TYPE, p1_data_name);

  } else {
    p1_kwg = config->player_strategy_params[p1_kwg_to_use_index]->kwg;
    p1_kwg_name = config->player_strategy_params[p1_kwg_to_use_index]->kwg_name;
  }

  KWG *p2_kwg;
  char *p2_kwg_name;

  if (p2_kwg_to_use_index < 0) {
    if (strings_equal(p1_lexicon_name, p2_lexicon_name)) {
      p2_kwg = p1_kwg;
      p2_kwg_name = p1_lexicon_name;
    } else {
      p2_kwg = create_kwg(p2_lexicon_name);
      p2_kwg_name = p2_lexicon_name;
    }
  } else {
    p2_kwg = config->player_strategy_params[p2_kwg_to_use_index]->kwg;
    p2_kwg_name = config->player_strategy_params[p2_kwg_to_use_index]->kwg_name;
  }

  for (int i = 0; i < 2; i++) {
    KWG *existing_kwg = config->player_strategy_params[i]->kwg;
    if (existing_kwg != p1_kwg && existing_kwg != p2_kwg) {
      destroy_kwg(existing_kwg);
    }
  }

  if (!p1_kwg || !p2_kwg) {
    return CONFIG_LOAD_STATUS_MISSING_LEXICON;
  }

  config->player_strategy_params[0]->kwg = p1_kwg;
  config->player_strategy_params[1]->kwg = p2_kwg;

  if (!strings_equal(config->player_strategy_params[0]->kwg_name,
                     p1_lexicon_name)) {
    free(config->player_strategy_params[0]->kwg_name);
    config->player_strategy_params[0]->kwg_name =
        get_formatted_string("%s", p1_lexicon_name);
  }
  if (!strings_equal(config->player_strategy_params[1]->kwg_name,
                     p2_lexicon_name)) {
    free(config->player_strategy_params[1]->kwg_name);
    config->player_strategy_params[1]->kwg_name =
        get_formatted_string("%s", p2_lexicon_name);
  }
  config->kwg_is_shared =
      strings_equal(config->player_strategy_params[0]->kwg_name,
                    config->player_strategy_params[1]->kwg_name);
}