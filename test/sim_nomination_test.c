#include "sim_nomination_test.h"

#include "../src/def/equity_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/move_defs.h"
#include "../src/ent/blocking_setup_params.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/impl/config.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/play_chooser.h"
#include "../src/impl/sim_nomination.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

enum {
  SNT_MOVE_LIST_CAPACITY = 200000,
};

// A late CSW24 position (16 in the bag) from the candidate-diversity study.
static const char *snt_late_cgp =
    "cgp 3JILEBI3I2/3U4TYNING1/3D6O1D2/3OS5V1E2/4T5A1V2/2CROW4E1o2/4R7U2/"
    "4MOLD2OUTGO/4I10/3ZEK6B2/3ERE1W4A2/3N1T1HEXADS2/4lACINIA1H2/7F4O2/7T7 "
    "AENRRUY/AELLMPS 249/413 0";

// An early position where exchanges, with and without the blank, are close
// to the best play.
static const char *snt_exchange_cgp =
    "cgp 15/15/15/15/15/15/15/7QI6/15/15/15/15/15/15/15 ?AEUUVW/ 0/22 0";

static const char *snt_params_text = "magpie_bsp_v1\n"
                                     "lexicon,CSW24\n"
                                     "model_version,test\n"
                                     "objective,sim_admission\n"
                                     "teacher_racks,16\n"
                                     "blocking_weight,1.4\n"
                                     "setup_weight,0.75\n";

static BlockingSetupParams *snt_params_create(void) {
  ErrorStack *error_stack = error_stack_create();
  BlockingSetupParams *params = blocking_setup_params_create_from_string(
      "test", snt_params_text, error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  return params;
}

static void snt_static_list(const Game *game, MoveList *list) {
  move_list_reset(list);
  const MoveGenArgs args = {
      .game = game,
      .move_list = list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .disable_pat = true,
  };
  generate_moves(&args);
  move_list_sort_moves(list);
}

static const MoveList *snt_nominate(SimNominator *nominator, const Game *game,
                                    const SimNominationSettings *settings) {
  ErrorStack *error_stack = error_stack_create();
  const MoveList *list =
      sim_nominator_nominate(nominator, game, settings, error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  assert(list != NULL);
  return list;
}

static void snt_expect_error(SimNominator *nominator, const Game *game,
                             const SimNominationSettings *settings) {
  ErrorStack *error_stack = error_stack_create();
  const MoveList *list =
      sim_nominator_nominate(nominator, game, settings, error_stack);
  assert(list == NULL);
  assert(error_stack_top(error_stack) == ERROR_STATUS_SIM_NOMINATION_INVALID);
  error_stack_destroy(error_stack);
}

static void test_sim_nomination_static(void) {
  Config *config = config_create_or_die(
      "set -lex CSW24 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1");
  load_and_exec_config_or_die(config, snt_late_cgp);
  const Game *game = config_get_game(config);
  MoveList *expected = move_list_create(SNT_MOVE_LIST_CAPACITY);
  snt_static_list(game, expected);
  SimNominator *nominator = sim_nominator_create();
  const SimNominationSettings settings = {.static_count = 10};
  const MoveList *list = snt_nominate(nominator, game, &settings);
  assert(move_list_get_count(list) == 10);
  for (int idx = 0; idx < 10; idx++) {
    assert(compare_moves_without_equity(move_list_get_move(list, idx),
                                        move_list_get_move(expected, idx),
                                        true) == -1);
    const SimNominee *nominee = sim_nominator_get_nominee(nominator, idx);
    assert(nominee->sources == SIM_NOMINATION_SOURCE_STATIC);
    assert(nominee->static_rank == idx + 1);
    assert(!nominee->checked);
    assert(move_get_equity(move_list_get_move(list, idx)) ==
           nominee->static_equity);
  }
  // Checks without parameters, PAT nominees without PAT weights, PAT-based
  // check ranking without PAT nominees and negative counts are errors.
  const SimNominationSettings no_params = {
      .static_count = 5, .blocking_count = 5, .universe_count = 20};
  snt_expect_error(nominator, game, &no_params);
  const SimNominationSettings no_pat = {.static_count = 5, .pat_count = 5};
  snt_expect_error(nominator, game, &no_pat);
  const SimNominationSettings bad_base = {.static_count = 5,
                                          .check_base_pat = true};
  snt_expect_error(nominator, game, &bad_base);
  const SimNominationSettings negative = {.static_count = -1};
  snt_expect_error(nominator, game, &negative);
  sim_nominator_destroy(nominator);
  move_list_destroy(expected);
  config_destroy(config);
}

static int snt_best_rank(const SimNominee *nominee) {
  int best = 1000;
  if ((nominee->sources & SIM_NOMINATION_SOURCE_STATIC) &&
      nominee->static_rank < best) {
    best = nominee->static_rank;
  }
  if ((nominee->sources & SIM_NOMINATION_SOURCE_BLOCKING) &&
      nominee->blocking_rank < best) {
    best = nominee->blocking_rank;
  }
  if ((nominee->sources & SIM_NOMINATION_SOURCE_SETUP) &&
      nominee->setup_rank < best) {
    best = nominee->setup_rank;
  }
  return best;
}

static void test_sim_nomination_checks(void) {
  Config *config = config_create_or_die(
      "set -lex CSW24 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1");
  load_and_exec_config_or_die(config, snt_late_cgp);
  const Game *game = config_get_game(config);
  BlockingSetupParams *params = snt_params_create();
  SimNominator *nominator = sim_nominator_create();
  const SimNominationSettings settings = {.static_count = 5,
                                          .blocking_count = 5,
                                          .setup_count = 5,
                                          .universe_count = 30,
                                          .params = params,
                                          .seed = 99};
  const MoveList *list = snt_nominate(nominator, game, &settings);
  const int count = move_list_get_count(list);
  assert(count >= 5 && count <= 15);
  int blocking_nominees = 0;
  int setup_nominees = 0;
  int static_nominees = 0;
  int previous_rank = 0;
  Move *first_moves[15];
  for (int idx = 0; idx < count; idx++) {
    const SimNominee *nominee = sim_nominator_get_nominee(nominator, idx);
    assert(nominee->checked);
    assert(nominee->static_rank > previous_rank);
    previous_rank = nominee->static_rank;
    assert(nominee->blocking_adjustment ==
           double_to_equity(1.4 * nominee->check.blocking_delta));
    assert(nominee->setup_adjustment ==
           double_to_equity(0.75 * nominee->check.setup_delta));
    if (nominee->sources & SIM_NOMINATION_SOURCE_BLOCKING) {
      assert(nominee->blocking_rank >= 1 && nominee->blocking_rank <= 5);
      blocking_nominees++;
    }
    if (nominee->sources & SIM_NOMINATION_SOURCE_SETUP) {
      assert(nominee->setup_rank >= 1 && nominee->setup_rank <= 5);
      setup_nominees++;
    }
    if (nominee->sources & SIM_NOMINATION_SOURCE_STATIC) {
      assert(nominee->static_rank <= 5);
      static_nominees++;
    }
    first_moves[idx] = move_create();
    move_copy(first_moves[idx], move_list_get_move(list, idx));
  }
  assert(blocking_nominees == 5 && setup_nominees == 5 && static_nominees == 5);
  // Same seed, same nominees in the same order.
  const MoveList *again = snt_nominate(nominator, game, &settings);
  assert(move_list_get_count(again) == count);
  for (int idx = 0; idx < count; idx++) {
    assert(compare_moves_without_equity(move_list_get_move(again, idx),
                                        first_moves[idx], true) == -1);
    move_destroy(first_moves[idx]);
  }
  // The cap keeps the best-ranked nominees.
  SimNominationSettings capped = settings;
  capped.max_candidates = 3;
  const MoveList *capped_list = snt_nominate(nominator, game, &capped);
  assert(move_list_get_count(capped_list) == 3);
  for (int idx = 0; idx < 3; idx++) {
    // Three sources have three rank-1 entries; overlaps leave room for
    // rank-2 entries but nothing worse.
    assert(snt_best_rank(sim_nominator_get_nominee(nominator, idx)) <= 2);
  }
  // Below min_check_bag the checks are skipped.
  SimNominationSettings late = settings;
  late.min_check_bag = 50;
  const MoveList *unchecked = snt_nominate(nominator, game, &late);
  assert(move_list_get_count(unchecked) == 5);
  assert(!sim_nominator_get_nominee(nominator, 0)->checked);
  sim_nominator_destroy(nominator);
  blocking_setup_params_destroy(params);
  config_destroy(config);
}

static void test_sim_nomination_exchanges(void) {
  Config *config = config_create_or_die(
      "set -lex CSW24 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1");
  load_and_exec_config_or_die(config, snt_exchange_cgp);
  const Game *game = config_get_game(config);
  SimNominator *nominator = sim_nominator_create();
  const SimNominationSettings settings = {.static_count = 3,
                                          .exchange_quota = 4,
                                          .exchange_margin = int_to_equity(35)};
  const MoveList *list = snt_nominate(nominator, game, &settings);
  int exchanges = 0;
  const Equity top = sim_nominator_get_nominee(nominator, 0)->static_equity;
  for (int idx = 0; idx < move_list_get_count(list); idx++) {
    const Move *move = move_list_get_move(list, idx);
    const SimNominee *nominee = sim_nominator_get_nominee(nominator, idx);
    if (nominee->sources & SIM_NOMINATION_SOURCE_EXCHANGE) {
      assert(move_get_type(move) == GAME_EVENT_EXCHANGE);
      assert(nominee->static_equity >= top - int_to_equity(35));
      exchanges++;
    }
  }
  assert(exchanges == 4);
  sim_nominator_destroy(nominator);
  config_destroy(config);
}

static void test_sim_nomination_pat(void) {
  Config *config = config_create_or_die(
      "set -lex CSW24 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -pat CSW24 -patcand false");
  load_and_exec_config_or_die(config, snt_late_cgp);
  const Game *game = config_get_game(config);
  const Player *player =
      game_get_player(game, game_get_player_on_turn_index(game));
  const bool was_disabled = player_get_pat_disabled(player);
  BlockingSetupParams *params = snt_params_create();
  SimNominator *nominator = sim_nominator_create();
  const SimNominationSettings settings = {.static_count = 5,
                                          .pat_count = 5,
                                          .blocking_count = 5,
                                          .universe_count = 20,
                                          .check_base_pat = true,
                                          .params = params,
                                          .seed = 3};
  const MoveList *list = snt_nominate(nominator, game, &settings);
  int pat_nominees = 0;
  bool any_pat_difference = false;
  for (int idx = 0; idx < move_list_get_count(list); idx++) {
    const SimNominee *nominee = sim_nominator_get_nominee(nominator, idx);
    assert(nominee->pat_rank >= 1);
    if (nominee->sources & SIM_NOMINATION_SOURCE_PAT) {
      assert(nominee->pat_rank <= 5);
      pat_nominees++;
    }
    any_pat_difference =
        any_pat_difference || nominee->pat_equity != nominee->static_equity;
  }
  assert(pat_nominees == 5);
  assert(any_pat_difference);
  // Nomination works on a scratch copy: the player's own PAT settings, which
  // the rollouts read, are untouched.
  assert(player_get_pat_disabled(player) == was_disabled);
  sim_nominator_destroy(nominator);
  blocking_setup_params_destroy(params);
  config_destroy(config);
}

static void test_sim_nomination_play_chooser(void) {
  Config *config = config_create_or_die(
      "set -lex CSW24 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1");
  load_and_exec_config_or_die(config, snt_late_cgp);
  Game *game = config_get_game(config);
  ErrorStack *error_stack = error_stack_create();
  config_load_win_pcts(config, error_stack);
  assert(error_stack_is_empty(error_stack));
  BlockingSetupParams *params = snt_params_create();
  const SimNominationSettings settings = {.static_count = 3,
                                          .blocking_count = 3,
                                          .setup_count = 3,
                                          .universe_count = 20,
                                          .params = params,
                                          .seed = 5};
  SimNominationCandidateSource source;
  sim_nomination_candidate_source_init(&source, &settings);
  const PlayChooserStrategy strategy = {
      .pre_endgame_eval = PLAY_CHOOSER_EVAL_SIM,
      .endgame_eval = PLAY_CHOOSER_EVAL_STATIC,
      .fixed_seconds_per_move = 0.2,
      .win_pcts = config_get_win_pcts(config),
      .num_threads = 1,
      .sim_candidates_fn = sim_nomination_candidates,
      .sim_candidates_context = &source,
      .pat_rollout_disabled = true,
      .seed = 7,
  };
  PlayChooser *play_chooser = play_chooser_create(&strategy);
  Move *chosen = move_create();
  play_chooser_choose_move(play_chooser, game, chosen, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(source.calls == 1);
  // The chosen move is one of the nominees of that call.
  bool found = false;
  const MoveList *nominees = snt_nominate(source.nominator, game, &settings);
  for (int idx = 0; idx < move_list_get_count(nominees) && !found; idx++) {
    found = compare_moves_without_equity(
                chosen, move_list_get_move(nominees, idx), true) == -1;
  }
  assert(found);
  move_destroy(chosen);
  play_chooser_destroy(play_chooser);
  sim_nomination_candidate_source_cleanup(&source);
  blocking_setup_params_destroy(params);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

void test_sim_nomination(void) {
  test_sim_nomination_static();
  test_sim_nomination_checks();
  test_sim_nomination_exchanges();
  test_sim_nomination_pat();
  test_sim_nomination_play_chooser();
}
