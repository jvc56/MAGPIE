#include "pat_position_check_test.h"

#include "../src/def/equity_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/pat_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/pat.h"
#include "../src/ent/pat_features.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/validated_move.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Opening positions where a simulation has priced a placement's board
// exposure, printed next to what PAT charges, for checking a set of weights
// against them. Run as patpositions:<pat name> (default CSW24).

enum {
  PAT_POSITION_CHECK_MOVE_CAPACITY = 5000,
  PAT_POSITION_CHECK_MAX_MOVES = 4,
  PAT_POSITION_CHECK_NAME_SIZE = 64,
};

typedef struct PATPositionCheckCase {
  const char *rack;
  // The first move is the exposed placement, compared with each other one.
  const char *moves[PAT_POSITION_CHECK_MAX_MOVES];
  int num_moves;
  const char *reference;
} PATPositionCheckCase;

static const PATPositionCheckCase pat_position_check_cases[] = {
    {"EELPRTZ",
     {"8H PRETZEL", "8D PRETZEL"},
     2,
     "sim, static rollouts without PAT, 8H - 8D equity: +8.0 at 2 plies, "
     "+6.1 at 4 plies (8D ahead on win% by 1.1)"},
    {"ACEHMRS",
     {"8H MARCHES", "8H MACHERS", "8D MACHERS"},
     3,
     "8H MARCHES opens O8 to MARCHESA/E/I; 8H MACHERS opens nothing; 8D "
     "MACHERS opens A8 only to STOMACHERS"},
};

static bool pat_position_check_same_move(const Move *first,
                                         const Move *second) {
  if (move_get_type(first) != move_get_type(second) ||
      move_get_row_start(first) != move_get_row_start(second) ||
      move_get_col_start(first) != move_get_col_start(second) ||
      move_get_dir(first) != move_get_dir(second) ||
      move_get_tiles_length(first) != move_get_tiles_length(second)) {
    return false;
  }
  for (int tile_idx = 0; tile_idx < move_get_tiles_length(first); tile_idx++) {
    if (move_get_tile(first, tile_idx) != move_get_tile(second, tile_idx)) {
      return false;
    }
  }
  return true;
}

// The move's static equity as move generation ranks it.
static Equity pat_position_check_equity(const Game *game, const Move *move) {
  MoveList *move_list = move_list_create(PAT_POSITION_CHECK_MOVE_CAPACITY);
  const MoveGenArgs args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&args);
  Equity equity = EQUITY_MIN_VALUE;
  for (int move_idx = 0; move_idx < move_list_get_count(move_list);
       move_idx++) {
    const Move *candidate = move_list_get_move(move_list, move_idx);
    if (pat_position_check_same_move(candidate, move)) {
      equity = move_get_equity(candidate);
      break;
    }
  }
  move_list_destroy(move_list);
  return equity;
}

// The defense term's feature row for the board the move leaves, in the
// combination the weights apply (see pat_extract_features_combined).
static void pat_position_check_features(const Game *game, const Move *move,
                                        const PATWeights *pat,
                                        double *features) {
  Rack leave;
  get_leave_for_move(move, game, &leave);
  Game *after = game_duplicate(game);
  play_move(move, after, NULL);
  pat_extract_features_combined(
      board_get_readonly_lanes(game_get_board(after), 0), game_get_ld(after),
      &leave, pat, RACK_SIZE, features);
  game_destroy(after);
}

static void pat_position_check_case(Config *with_pat, Config *without_pat,
                                    const PATPositionCheckCase *check) {
  Game *game = config_get_game(with_pat);
  Game *plain_game = config_get_game(without_pat);
  const LetterDistribution *ld = game_get_ld(game);
  rack_set_to_string(ld, player_get_rack(game_get_player(game, 0)),
                     check->rack);
  rack_set_to_string(ld, player_get_rack(game_get_player(plain_game, 0)),
                     check->rack);
  const PATWeights *pat = player_get_pat(game_get_player(game, 0));
  printf("\n%s: %s\n", check->rack, check->reference);
  double features[PAT_POSITION_CHECK_MAX_MOVES][PAT_NUM_FEATURES];
  double pat_total[PAT_POSITION_CHECK_MAX_MOVES];
  for (int move_idx = 0; move_idx < check->num_moves; move_idx++) {
    ValidatedMoves *vms = validated_moves_create_and_assert_status(
        game, 0, check->moves[move_idx], false, false, ERROR_STATUS_SUCCESS);
    const Move *move = validated_moves_get_move(vms, 0);
    const Equity equity = pat_position_check_equity(game, move);
    const Equity plain_equity = pat_position_check_equity(plain_game, move);
    pat_position_check_features(game, move, pat, features[move_idx]);
    double defense = 0.0;
    for (int feature_idx = 0; feature_idx < PAT_NUM_FEATURES; feature_idx++) {
      defense += features[move_idx][feature_idx] *
                 (double)pat_get_weight(pat, feature_idx);
    }
    pat_total[move_idx] = equity_to_double(equity - plain_equity);
    printf("  %-12s score %3d  equity %7.2f  without PAT %7.2f  PAT %6.2f  "
           "defense %6.2f\n",
           check->moves[move_idx], equity_to_int(move_get_score(move)),
           equity_to_double(equity), equity_to_double(plain_equity),
           pat_total[move_idx], defense / 1000.0);
    validated_moves_destroy(vms);
  }
  for (int move_idx = 1; move_idx < check->num_moves; move_idx++) {
    printf("  %s vs %s: PAT %+.2f; defense by channel:", check->moves[0],
           check->moves[move_idx], pat_total[0] - pat_total[move_idx]);
    for (int feature_idx = 0; feature_idx < PAT_NUM_FEATURES; feature_idx++) {
      const double contribution =
          (features[0][feature_idx] - features[move_idx][feature_idx]) *
          (double)pat_get_weight(pat, feature_idx) / 1000.0;
      if (contribution > 0.005 || contribution < -0.005) {
        char name[PAT_POSITION_CHECK_NAME_SIZE];
        pat_feature_name(feature_idx, name, sizeof(name));
        printf(" %s %+.2f", name, contribution);
      }
    }
    printf("\n");
  }
}

void pat_position_check_run(const char *pat_name) {
  if (pat_name == NULL || pat_name[0] == '\0') {
    pat_name = "CSW24";
  }
  const char *settings = "set -lex CSW24 -leaves CSW24 -wmp true -s1 equity "
                         "-s2 equity -r1 all -r2 all -numplays 1";
  char *with_pat_command =
      get_formatted_string("%s -pat %s", settings, pat_name);
  Config *with_pat = config_create_or_die(with_pat_command);
  free(with_pat_command);
  Config *without_pat = config_create_or_die(settings);
  load_and_exec_config_or_die(with_pat, "new");
  load_and_exec_config_or_die(without_pat, "new");
  printf("PAT position check, weights %s", pat_name);
  for (size_t case_idx = 0; case_idx < sizeof(pat_position_check_cases) /
                                           sizeof(pat_position_check_cases[0]);
       case_idx++) {
    pat_position_check_case(with_pat, without_pat,
                            &pat_position_check_cases[case_idx]);
  }
  config_destroy(without_pat);
  config_destroy(with_pat);
}
