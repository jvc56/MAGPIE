#ifndef PLAYERS_STRATEGIES_H
#define PLAYERS_STRATEGIES_H

#include "klv.h"
#include "kwg.h"
#include "letter_distribution.h"
#include "rack.h"
#include "winpct.h"

typedef enum {
  PLAYERS_STRATEGY_TYPE_KWG,
  PLAYERS_STRATEGY_TYPE_KLV,
  NUMBER_OF_STRATEGIES
} player_data_t;

typedef struct PlayersStrategy {
  bool data_is_shared;
  char *p1_data_name;
  char *p2_data_name;
  void *p1_data[NUMBER_OF_STRATEGIES];
  void *p2_data[NUMBER_OF_STRATEGIES];
} PlayersStrategy;

typedef struct PlayersStrategies {
  PlayersStrategy *strategies[NUMBER_OF_STRATEGIES];
  move_sort_t move_sort_type;
  move_record_t move_record_type;
} PlayersStrategies;

#endif
