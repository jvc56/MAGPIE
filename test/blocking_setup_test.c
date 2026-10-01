#include "blocking_setup_test.h"

#include "../src/compat/ctime.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/blocking_setup_params.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/impl/blocking_setup.h"
#include "../src/impl/config.h"
#include "../src/impl/move_gen.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  BST_POOL_CAPACITY = 256,
  BST_NUM_RACKS = 16,
  BST_LINE_CAPACITY = 65536,
  BST_MOVE_TEXT_CAPACITY = 128,
  BST_NUM_CHECKED = 12,
  BST_MOVE_LIST_CAPACITY = 200000,
  BST_MAX_UNIVERSE = 512,
  BST_BENCH_EXCHANGES = 5,
  BST_BENCH_EXCHANGE_MARGIN = 35,
  BST_MAX_RACE_SETTINGS = 8,
};

// A late CSW24 position (16 in the bag) from the candidate-diversity study.
static const char *bst_cgp =
    "cgp 3JILEBI3I2/3U4TYNING1/3D6O1D2/3OS5V1E2/4T5A1V2/2CROW4E1o2/4R7U2/"
    "4MOLD2OUTGO/4I10/3ZEK6B2/3ERE1W4A2/3N1T1HEXADS2/4lACINIA1H2/7F4O2/7T7 "
    "AENRRUY/AELLMPS 249/413 0";

static void bst_generate_all(const Game *game, MoveList *list) {
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

static void bst_measure_all(const Game *game,
                            const BlockingSetupSamples *samples,
                            const Move *const *moves, int num_moves,
                            BlockingSetupResult *results) {
  BlockingSetupChecker *checker = blocking_setup_checker_create();
  blocking_setup_checker_load(checker, game, samples, 1);
  for (int move_idx = 0; move_idx < num_moves; move_idx++) {
    blocking_setup_checker_measure(checker, moves[move_idx],
                                   &results[move_idx]);
  }
  blocking_setup_checker_destroy(checker);
}

static bool bst_results_equal(const BlockingSetupResult *a,
                              const BlockingSetupResult *b) {
  return a->pass_reply_mean == b->pass_reply_mean &&
         a->candidate_reply_mean == b->candidate_reply_mean &&
         a->blocking_delta == b->blocking_delta &&
         a->pass_followup_mean == b->pass_followup_mean &&
         a->candidate_followup_mean == b->candidate_followup_mean &&
         a->setup_delta == b->setup_delta &&
         a->terminal_replies == b->terminal_replies;
}

static void test_blocking_setup_leave(void) {
  Config *config = config_create_or_die(
      "set -lex CSW24 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1");
  load_and_exec_config_or_die(config, "cgp 15/15/15/15/15/15/15/15/15/15/15/"
                                      "15/15/15/15 ?AEINST/ 0/0 0");
  const Game *game = config_get_game(config);
  MoveList *list = move_list_create(100000);
  bst_generate_all(game, list);
  bool found_blank_exchange = false;
  bool found_blank_placement = false;
  for (int move_idx = 0; move_idx < move_list_get_count(list); move_idx++) {
    const Move *move = move_list_get_move(list, move_idx);
    Rack leave;
    blocking_setup_candidate_leave(game, move, &leave);
    const int expected_leave = RACK_SIZE - move_get_tiles_played(move);
    assert(rack_get_total_letters(&leave) == expected_leave);
    if (move_get_type(move) == GAME_EVENT_EXCHANGE &&
        rack_get_letter(&leave, BLANK_MACHINE_LETTER) == 0) {
      found_blank_exchange = true;
    }
    if (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE &&
        rack_get_letter(&leave, BLANK_MACHINE_LETTER) == 0) {
      found_blank_placement = true;
    }
  }
  assert(found_blank_exchange);
  assert(found_blank_placement);
  move_list_destroy(list);
  config_destroy(config);
}

// Opponent's actual rack and bag order must not matter: only the unseen
// multiset does.
static void bst_swap_opponent_tile_with_bag(const Game *game) {
  const int opponent_idx = 1 - game_get_player_on_turn_index(game);
  Rack *opponent_rack = player_get_rack(game_get_player(game, opponent_idx));
  Bag *bag = game_get_bag(game);
  const LetterDistribution *ld = game_get_ld(game);
  for (int ml = 0; ml < ld_get_size(ld); ml++) {
    if (rack_get_letter(opponent_rack, (MachineLetter)ml) == 0) {
      continue;
    }
    for (int other = 0; other < ld_get_size(ld); other++) {
      if (other != ml && bag_get_letter(bag, (MachineLetter)other) > 0) {
        const int draw_index = game_get_player_draw_index(game, opponent_idx);
        bag_draw_letter(bag, (MachineLetter)other, draw_index);
        bag_add_letter(bag, (MachineLetter)ml, draw_index);
        rack_take_letter(opponent_rack, (MachineLetter)ml);
        rack_add_letter(opponent_rack, (MachineLetter)other);
        return;
      }
    }
  }
  assert(false);
}

static void test_blocking_setup_checks(void) {
  Config *config = config_create_or_die(
      "set -lex CSW24 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1");
  load_and_exec_config_or_die(config, bst_cgp);
  const Game *game = config_get_game(config);
  MoveList *list = move_list_create(100000);
  bst_generate_all(game, list);
  const Move *moves[BST_NUM_CHECKED];
  int num_moves = 0;
  for (int move_idx = 0;
       move_idx < move_list_get_count(list) && num_moves < BST_NUM_CHECKED;
       move_idx++) {
    const Move *move = move_list_get_move(list, move_idx);
    if (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      moves[num_moves++] = move;
    }
  }
  assert(num_moves == BST_NUM_CHECKED);

  BlockingSetupSamples *samples =
      blocking_setup_samples_create(BST_NUM_RACKS, BST_POOL_CAPACITY);
  blocking_setup_samples_deal(samples, game, BST_NUM_RACKS, true, true, 42);
  // Pool: 100 tiles less 69 on the board less our 7.
  MachineLetter pool[BST_POOL_CAPACITY];
  const int pool_size = blocking_setup_unseen_pool(game, pool);
  assert(samples->pool_size == pool_size);
  assert(pool_size == bag_get_letters(game_get_bag(game)) + RACK_SIZE);
  for (int rack_idx = 0; rack_idx < BST_NUM_RACKS; rack_idx++) {
    assert(rack_get_total_letters(&samples->opponent_racks[rack_idx]) ==
           RACK_SIZE);
    // Conditioned draws exclude exactly the rack's tiles.
    assert(samples->draw_sizes[rack_idx] == pool_size - RACK_SIZE);
  }
  // Partitioned deals: the first pool_size / RACK_SIZE racks are disjoint.
  int dealt[MAX_ALPHABET_SIZE] = {0};
  for (int rack_idx = 0; rack_idx < pool_size / RACK_SIZE; rack_idx++) {
    for (int ml = 0; ml < MAX_ALPHABET_SIZE; ml++) {
      dealt[ml] += rack_get_letter(&samples->opponent_racks[rack_idx],
                                   (MachineLetter)ml);
    }
  }
  int unseen[MAX_ALPHABET_SIZE] = {0};
  for (int tile_idx = 0; tile_idx < pool_size; tile_idx++) {
    unseen[pool[tile_idx]]++;
  }
  for (int ml = 0; ml < MAX_ALPHABET_SIZE; ml++) {
    assert(dealt[ml] <= unseen[ml]);
  }

  BlockingSetupResult results[BST_NUM_CHECKED];
  bst_measure_all(game, samples, moves, num_moves, results);
  bool any_blocking = false;
  bool any_setup = false;
  for (int move_idx = 0; move_idx < num_moves; move_idx++) {
    const BlockingSetupResult *result = &results[move_idx];
    assert(fabs(result->blocking_delta -
                (result->pass_reply_mean - result->candidate_reply_mean)) <
           1e-9);
    assert(fabs(result->setup_delta - (result->candidate_followup_mean -
                                       result->pass_followup_mean)) < 1e-9);
    assert(result->terminal_replies == 0);
    any_blocking = any_blocking || result->blocking_delta != 0.0;
    any_setup = any_setup || result->setup_delta != 0.0;
    // Every candidate shares the pass baseline.
    assert(result->pass_reply_mean == results[0].pass_reply_mean);
  }
  assert(any_blocking);
  assert(any_setup);

  // Same seed: identical. The opponent's actual rack is not information the
  // check may use, so moving one of its tiles to the bag changes nothing.
  BlockingSetupResult repeat[BST_NUM_CHECKED];
  bst_swap_opponent_tile_with_bag(game);
  blocking_setup_samples_deal(samples, game, BST_NUM_RACKS, true, true, 42);
  bst_measure_all(game, samples, moves, num_moves, repeat);
  for (int move_idx = 0; move_idx < num_moves; move_idx++) {
    assert(bst_results_equal(&results[move_idx], &repeat[move_idx]));
  }
  blocking_setup_samples_destroy(samples);
  move_list_destroy(list);
  config_destroy(config);
}

// An exchange leaves the board alone: both deltas are exactly zero.
static void test_blocking_setup_exchange(void) {
  Config *config = config_create_or_die(
      "set -lex CSW24 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/7QI6/15/15/15/15/15/15/15 "
              "?AEUUVW/ 0/22 0");
  const Game *game = config_get_game(config);
  MoveList *list = move_list_create(100000);
  bst_generate_all(game, list);
  const Move *exchanges[2];
  int num_exchanges = 0;
  for (int move_idx = 0;
       move_idx < move_list_get_count(list) && num_exchanges < 2; move_idx++) {
    const Move *move = move_list_get_move(list, move_idx);
    if (move_get_type(move) != GAME_EVENT_EXCHANGE) {
      continue;
    }
    Rack leave;
    blocking_setup_candidate_leave(game, move, &leave);
    // One exchange that keeps the blank and one that throws it back.
    const bool keeps_blank = rack_get_letter(&leave, BLANK_MACHINE_LETTER) > 0;
    if ((num_exchanges == 0 && keeps_blank) ||
        (num_exchanges == 1 && !keeps_blank)) {
      exchanges[num_exchanges++] = move;
    }
  }
  assert(num_exchanges == 2);
  BlockingSetupSamples *samples =
      blocking_setup_samples_create(BST_NUM_RACKS, BST_POOL_CAPACITY);
  blocking_setup_samples_deal(samples, game, BST_NUM_RACKS, false, true, 7);
  BlockingSetupResult results[2];
  bst_measure_all(game, samples, exchanges, num_exchanges, results);
  for (int exchange_idx = 0; exchange_idx < num_exchanges; exchange_idx++) {
    assert(results[exchange_idx].blocking_delta == 0.0);
    assert(results[exchange_idx].setup_delta == 0.0);
    assert(results[exchange_idx].pass_reply_mean > 0.0);
  }
  blocking_setup_samples_destroy(samples);
  move_list_destroy(list);
  config_destroy(config);
}

static const char *bst_params_text =
    "# research coefficients\n"
    "magpie_bsp_v1\n"
    "lexicon,CSW24\n"
    "model_version,research-v0\n"
    "objective,sim_admission\n"
    "teacher_racks,64\n"
    "teacher_partition,1\n"
    "teacher_condition_draws,1\n"
    "teacher_followup_draws,1\n"
    "blocking_weight,1.4\n"
    "setup_weight,0.75\n"
    "bin,5,29,40,80,2.0,0.5\n"
    "bin,0,6,-1000,1000,0,1.5\n"
    "provenance,seed=1,commit=abc, with commas\n";

static void bst_expect_params_error(const char *contents, error_code_t code) {
  ErrorStack *error_stack = error_stack_create();
  const BlockingSetupParams *params =
      blocking_setup_params_create_from_string("bad", contents, error_stack);
  assert(params == NULL);
  assert(error_stack_top(error_stack) == code);
  error_stack_destroy(error_stack);
}

static void test_blocking_setup_params(void) {
  ErrorStack *error_stack = error_stack_create();
  BlockingSetupParams *params = blocking_setup_params_create_from_string(
      "fixture", bst_params_text, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(strings_equal(blocking_setup_params_get_lexicon(params), "CSW24"));
  assert(strings_equal(blocking_setup_params_get_model_version(params),
                       "research-v0"));
  assert(strings_equal(blocking_setup_params_get_objective(params),
                       "sim_admission"));
  assert(blocking_setup_params_get_teacher_racks(params) == 64);
  assert(blocking_setup_params_get_teacher_partition(params));
  assert(blocking_setup_params_get_teacher_condition_draws(params));
  assert(blocking_setup_params_get_teacher_followup_draws(params) == 1);
  assert(blocking_setup_params_get_num_bins(params) == 2);
  double blocking_weight = 0.0;
  double setup_weight = 0.0;
  blocking_setup_params_get_weights(params, 40, 0, &blocking_weight,
                                    &setup_weight);
  assert(blocking_weight == 1.4 && setup_weight == 0.75);
  blocking_setup_params_get_weights(params, 5, 80, &blocking_weight,
                                    &setup_weight);
  assert(blocking_weight == 2.0 && setup_weight == 0.5);
  blocking_setup_params_get_weights(params, 5, 81, &blocking_weight,
                                    &setup_weight);
  assert(blocking_weight == 0.0 && setup_weight == 1.5);
  blocking_setup_params_destroy(params);

  bst_expect_params_error("lexicon,CSW24\n", ERROR_STATUS_BSP_INVALID_HEADER);
  bst_expect_params_error("magpie_bsp_v2\n",
                          ERROR_STATUS_BSP_UNSUPPORTED_VERSION);
  bst_expect_params_error("magpie_bsp_v1\nlexicon,CSW24\nmodel_version,x\n"
                          "objective,y\nblocking_weight,1\n",
                          ERROR_STATUS_BSP_MISSING_ROW);
  bst_expect_params_error("magpie_bsp_v1\nblocking_weight,one\n",
                          ERROR_STATUS_BSP_INVALID_ROW);
  bst_expect_params_error("magpie_bsp_v1\nweight,1\n",
                          ERROR_STATUS_BSP_INVALID_ROW);
  bst_expect_params_error("magpie_bsp_v1\nbin,9,5,0,0,1,1\n",
                          ERROR_STATUS_BSP_INVALID_ROW);
  bst_expect_params_error("magpie_bsp_v1\nteacher_partition,2\n",
                          ERROR_STATUS_BSP_INVALID_ROW);

  // Missing files are an error, never a silent fallback.
  const BlockingSetupParams *missing = blocking_setup_params_create(
      DEFAULT_TEST_DATA_PATH, "no_such_blocking_setup_params", error_stack);
  assert(missing == NULL);
  assert(!error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
}

void test_blocking_setup(void) {
  test_blocking_setup_params();
  test_blocking_setup_leave();
  test_blocking_setup_checks();
  test_blocking_setup_exchange();
}

// Replays the candidate-diversity study's teacher (the research harness's
// pass-relative checks) with its exact sampling and compares every archived
// check row. Spec: "<positions.cgp>:<checks.csv>:<pcd_seed>[:<max_positions>
// [:<racks>]]" where positions.cgp holds "pos,cgp" lines and checks.csv is a
// harness PCD_CHECK_OUT file. Prints matched and mismatched rows.
static uint64_t bst_harness_next(uint64_t *state) {
  *state += UINT64_C(0x9e3779b97f4a7c15);
  uint64_t value = *state;
  value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
  return value ^ (value >> 31);
}

static void bst_harness_shuffle(MachineLetter *tiles, int count,
                                uint64_t *state) {
  for (int tile_idx = 0; tile_idx < count - 1; tile_idx++) {
    const int pick = tile_idx + (int)(bst_harness_next(state) %
                                      (uint64_t)(count - tile_idx));
    const MachineLetter tile = tiles[pick];
    tiles[pick] = tiles[tile_idx];
    tiles[tile_idx] = tile;
  }
}

static void bst_harness_samples(const Game *game, uint64_t seed, int num_racks,
                                BlockingSetupSamples *samples) {
  const int ld_size = ld_get_size(game_get_ld(game));
  MachineLetter pool[BST_POOL_CAPACITY];
  const int pool_size = blocking_setup_unseen_pool(game, pool);
  const int rack_size = pool_size < RACK_SIZE ? pool_size : RACK_SIZE;
  uint64_t rng = seed;
  int rack_idx = 0;
  while (rack_idx < num_racks) {
    MachineLetter shuffled[BST_POOL_CAPACITY];
    memcpy(shuffled, pool, sizeof(MachineLetter) * (size_t)pool_size);
    bst_harness_shuffle(shuffled, pool_size, &rng);
    for (int start = 0; start + rack_size <= pool_size && rack_idx < num_racks;
         start += rack_size) {
      rack_set_dist_size_and_reset(&samples->opponent_racks[rack_idx], ld_size);
      for (int tile_idx = 0; tile_idx < rack_size; tile_idx++) {
        rack_add_letter(&samples->opponent_racks[rack_idx],
                        shuffled[start + tile_idx]);
      }
      rack_idx++;
    }
  }
  uint64_t draw_rng = (seed * UINT64_C(2654435761)) + UINT64_C(12345);
  for (rack_idx = 0; rack_idx < num_racks; rack_idx++) {
    Rack excluded;
    rack_copy(&excluded, &samples->opponent_racks[rack_idx]);
    MachineLetter *order = samples->draw_orders +
                           ((size_t)rack_idx * (size_t)samples->pool_capacity);
    int draw_size = 0;
    for (int tile_idx = 0; tile_idx < pool_size; tile_idx++) {
      if (rack_get_letter(&excluded, pool[tile_idx]) > 0) {
        rack_take_letter(&excluded, pool[tile_idx]);
      } else {
        order[draw_size++] = pool[tile_idx];
      }
    }
    bst_harness_shuffle(order, draw_size, &draw_rng);
    samples->draw_sizes[rack_idx] = draw_size;
  }
  samples->num_racks = num_racks;
  samples->pool_size = pool_size;
}

static const Move *bst_find_move(const Game *game, const MoveList *list,
                                 const char *text) {
  for (int move_idx = 0; move_idx < move_list_get_count(list); move_idx++) {
    const Move *move = move_list_get_move(list, move_idx);
    StringBuilder *sb = string_builder_create();
    string_builder_add_ucgi_move(sb, move, game_get_board(game),
                                 game_get_ld(game));
    const bool match = strings_equal(string_builder_peek(sb), text);
    string_builder_destroy(sb);
    if (match) {
      return move;
    }
  }
  return NULL;
}

static char *bst_find_cgp(const char *path, long pos) {
  FILE *file = fopen_or_die(path, "r");
  char *line = malloc_or_die(BST_LINE_CAPACITY);
  char *found = NULL;
  while (found == NULL && fgets(line, BST_LINE_CAPACITY, file) != NULL) {
    char *end = NULL;
    if (strtol(line, &end, 10) == pos && end != line && *end == ',') {
      end[strcspn(end, "\r\n")] = '\0';
      found = get_formatted_string("cgp %s", end + 1);
    }
  }
  free(line);
  (void)fclose(file);
  return found;
}

void blocking_setup_replay_run_spec(const char *spec) {
  StringSplitter *fields = split_string(spec, ':', true);
  const int num_fields = string_splitter_get_number_of_items(fields);
  assert(num_fields >= 3);
  const char *cgp_path = string_splitter_get_item(fields, 0);
  const char *checks_path = string_splitter_get_item(fields, 1);
  const uint64_t pcd_seed =
      strtoull(string_splitter_get_item(fields, 2), NULL, 10);
  const long max_positions =
      num_fields >= 4 ? strtol(string_splitter_get_item(fields, 3), NULL, 10)
                      : 1000000;
  const int num_racks =
      num_fields >= 5
          ? (int)strtol(string_splitter_get_item(fields, 4), NULL, 10)
          : 64;
  Config *config = config_create_or_die(
      "set -lex CSW24 -leaves CSW24 -wmp true -s1 equity -s2 equity "
      "-r1 all -r2 all -numplays 1 -threads 1");
  MoveList *list = move_list_create(100000);
  BlockingSetupSamples *samples =
      blocking_setup_samples_create(num_racks, BST_POOL_CAPACITY);
  BlockingSetupChecker *checker = blocking_setup_checker_create();
  FILE *checks = fopen_or_die(checks_path, "r");
  char *line = malloc_or_die(BST_LINE_CAPACITY);
  long current_pos = -1;
  long positions = 0;
  long rows = 0;
  long mismatches = 0;
  double max_error = 0.0;
  while (fgets(line, BST_LINE_CAPACITY, checks) != NULL) {
    long pos = 0;
    int pool_index = 0;
    char move_text[BST_MOVE_TEXT_CAPACITY];
    double static_eq = 0.0;
    double expected[4];
    double unused_delta = 0.0;
    double unused_adjustment = 0.0;
    // The archived rows are test data; the field count is checked here.
    // NOLINTNEXTLINE(cert-err34-c)
    if (sscanf(line, "%ld,%d,%127[^,],%lf,%lf,%lf,%lf,%lf,%lf,%lf", &pos,
               &pool_index, move_text, &static_eq, &expected[0], &expected[1],
               &unused_delta, &unused_adjustment, &expected[2],
               &expected[3]) != 10) {
      continue;
    }
    if (pos != current_pos) {
      if (positions >= max_positions) {
        break;
      }
      char *cgp = bst_find_cgp(cgp_path, pos);
      assert(cgp != NULL);
      load_and_exec_config_or_die(config, cgp);
      free(cgp);
      const Game *game = config_get_game(config);
      bst_generate_all(game, list);
      uint64_t seed_state = pcd_seed + (uint64_t)pos;
      bst_harness_samples(game, bst_harness_next(&seed_state), num_racks,
                          samples);
      blocking_setup_checker_load(checker, game, samples, 1);
      current_pos = pos;
      positions++;
    }
    const Game *game = config_get_game(config);
    const Move *move = bst_find_move(game, list, move_text);
    assert(move != NULL);
    BlockingSetupResult result;
    blocking_setup_checker_measure(checker, move, &result);
    const double actual[4] = {
        result.pass_reply_mean, result.candidate_reply_mean,
        result.pass_followup_mean, result.candidate_followup_mean};
    bool row_matches = true;
    for (int field_idx = 0; field_idx < 4; field_idx++) {
      const double error = fabs(actual[field_idx] - expected[field_idx]);
      if (error > max_error) {
        max_error = error;
      }
      row_matches = row_matches && error < 1e-6;
    }
    if (!row_matches) {
      mismatches++;
      printf("mismatch pos=%ld move=%s expected=%.6f,%.6f,%.6f,%.6f "
             "actual=%.6f,%.6f,%.6f,%.6f\n",
             pos, move_text, expected[0], expected[1], expected[2], expected[3],
             actual[0], actual[1], actual[2], actual[3]);
    }
    rows++;
  }
  printf("blocking_setup_replay positions=%ld rows=%ld mismatches=%ld "
         "max_abs_error=%.9f\n",
         positions, rows, mismatches, max_error);
  free(line);
  (void)fclose(checks);
  blocking_setup_checker_destroy(checker);
  blocking_setup_samples_destroy(samples);
  move_list_destroy(list);
  config_destroy(config);
  string_splitter_destroy(fields);
}

// Times the teacher on a fixed set of positions and writes every result at
// full precision, so speed changes can be checked to leave results
// unchanged. Spec: "<positions.csv>:<out.csv>[:<racks>[:<universe>
// [:<max_positions>]]]" with a bsgen positions file. Each position's
// universe is its top <universe> static non-pass moves plus up to five
// exchanges within 35 points of the top move; the samples are dealt from a
// seed fixed by the game index.
void blocking_setup_bench_run_spec(const char *spec) {
  StringSplitter *fields = split_string(spec, ':', true);
  const int num_fields = string_splitter_get_number_of_items(fields);
  assert(num_fields >= 2);
  const int num_racks =
      num_fields >= 3
          ? (int)strtol(string_splitter_get_item(fields, 2), NULL, 10)
          : 64;
  const int universe =
      num_fields >= 4
          ? (int)strtol(string_splitter_get_item(fields, 3), NULL, 10)
          : 60;
  const long max_positions =
      num_fields >= 5 ? strtol(string_splitter_get_item(fields, 4), NULL, 10)
                      : 1000000;
  Config *config = config_create_or_die(
      "set -lex CSW24 -leaves CSW24 -wmp true -s1 equity -s2 equity "
      "-r1 all -r2 all -numplays 1 -threads 1");
  MoveList *list = move_list_create(BST_MOVE_LIST_CAPACITY);
  BlockingSetupSamples *samples =
      blocking_setup_samples_create(num_racks, BST_POOL_CAPACITY);
  BlockingSetupChecker *checker = blocking_setup_checker_create();
  FILE *in = fopen_or_die(string_splitter_get_item(fields, 0), "r");
  FILE *out = fopen_or_die(string_splitter_get_item(fields, 1), "w");
  (void)fprintf(out, "game,cand,move,pass_reply,cand_reply,blocking,"
                     "pass_followup,cand_followup,setup,terminal\n");
  char *line = malloc_or_die(BST_LINE_CAPACITY);
  long positions = 0;
  long candidates = 0;
  int64_t total_ns = 0;
  while (positions < max_positions &&
         fgets(line, BST_LINE_CAPACITY, in) != NULL) {
    char *end = NULL;
    const long game_idx = strtol(line, &end, 10);
    if (end == line) {
      continue;
    }
    // The CGP is the sixth field.
    char *cgp = line;
    for (int field_idx = 0; field_idx < 5 && cgp != NULL; field_idx++) {
      cgp = strchr(cgp, ',');
      if (cgp != NULL) {
        cgp++;
      }
    }
    assert(cgp != NULL);
    cgp[strcspn(cgp, "\r\n")] = '\0';
    char *command = get_formatted_string("cgp %s", cgp);
    load_and_exec_config_or_die(config, command);
    free(command);
    const Game *game = config_get_game(config);
    bst_generate_all(game, list);
    const Move *universe_moves[BST_MAX_UNIVERSE];
    int count = 0;
    int exchanges = 0;
    const Equity top = move_get_equity(move_list_get_move(list, 0));
    int placements = 0;
    for (int move_idx = 0; move_idx < move_list_get_count(list); move_idx++) {
      const Move *move = move_list_get_move(list, move_idx);
      if (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE &&
          placements < universe) {
        universe_moves[count++] = move;
        placements++;
      } else if (move_get_type(move) == GAME_EVENT_EXCHANGE &&
                 exchanges < BST_BENCH_EXCHANGES &&
                 move_get_equity(move) >=
                     top - int_to_equity(BST_BENCH_EXCHANGE_MARGIN)) {
        universe_moves[count++] = move;
        exchanges++;
      }
    }
    if (bag_get_letters(game_get_bag(game)) == 0 || count == 0) {
      continue;
    }
    blocking_setup_samples_deal(samples, game, num_racks, true, true,
                                ((uint64_t)game_idx * UINT64_C(7919)) + 1);
    const int64_t start = ctimer_monotonic_ns();
    blocking_setup_checker_load(checker, game, samples, 1);
    BlockingSetupResult results[BST_MAX_UNIVERSE];
    for (int cand_idx = 0; cand_idx < count; cand_idx++) {
      blocking_setup_checker_measure(checker, universe_moves[cand_idx],
                                     &results[cand_idx]);
    }
    total_ns += ctimer_monotonic_ns() - start;
    for (int cand_idx = 0; cand_idx < count; cand_idx++) {
      StringBuilder *sb = string_builder_create();
      string_builder_add_ucgi_move(sb, universe_moves[cand_idx],
                                   game_get_board(game), game_get_ld(game));
      const BlockingSetupResult *result = &results[cand_idx];
      (void)fprintf(out, "%ld,%d,%s,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%d\n",
                    game_idx, cand_idx, string_builder_peek(sb),
                    result->pass_reply_mean, result->candidate_reply_mean,
                    result->blocking_delta, result->pass_followup_mean,
                    result->candidate_followup_mean, result->setup_delta,
                    result->terminal_replies);
      string_builder_destroy(sb);
    }
    positions++;
    candidates += count;
  }
  printf("blocking_setup_bench positions=%ld candidates=%ld racks=%d "
         "total_ms=%.1f ms_per_position=%.2f us_per_candidate=%.1f\n",
         positions, candidates, num_racks, (double)total_ns / 1e6,
         positions > 0 ? (double)total_ns / 1e6 / (double)positions : 0.0,
         candidates > 0 ? (double)total_ns / 1e3 / (double)candidates : 0.0);
  free(line);
  (void)fclose(in);
  (void)fclose(out);
  blocking_setup_checker_destroy(checker);
  blocking_setup_samples_destroy(samples);
  move_list_destroy(list);
  config_destroy(config);
  string_splitter_destroy(fields);
}

// Races against full measurement on a fixed position set. Spec:
// "<positions.csv>:<racks>:<universe>:<max_positions>:<blocking_weight>:
// <setup_weight>:<z>[,<z>...]". For each position, every universe candidate
// is measured in full, and the argmax of static equity plus the weighted
// deltas is the reference choice; the race without elimination must pick
// exactly that. Each z then reports how often its pick agrees, the mean and
// worst adjusted-value regret of its pick (points, by the full values) and
// its time and work relative to full measurement.
void blocking_setup_race_run_spec(const char *spec) {
  StringSplitter *fields = split_string(spec, ':', true);
  assert(string_splitter_get_number_of_items(fields) == 7);
  const char *positions_path = string_splitter_get_item(fields, 0);
  const int num_racks =
      (int)strtol(string_splitter_get_item(fields, 1), NULL, 10);
  const int universe =
      (int)strtol(string_splitter_get_item(fields, 2), NULL, 10);
  const long max_positions =
      strtol(string_splitter_get_item(fields, 3), NULL, 10);
  const double blocking_weight =
      strtod(string_splitter_get_item(fields, 4), NULL);
  const double setup_weight = strtod(string_splitter_get_item(fields, 5), NULL);
  StringSplitter *z_values =
      split_string(string_splitter_get_item(fields, 6), ',', true);
  const int num_z = string_splitter_get_number_of_items(z_values);
  assert(num_z >= 1 && num_z <= BST_MAX_RACE_SETTINGS);
  double z_list[BST_MAX_RACE_SETTINGS];
  long agree[BST_MAX_RACE_SETTINGS] = {0};
  double regret_sum[BST_MAX_RACE_SETTINGS] = {0};
  double regret_max[BST_MAX_RACE_SETTINGS] = {0};
  int64_t race_ns[BST_MAX_RACE_SETTINGS] = {0};
  long race_work[BST_MAX_RACE_SETTINGS] = {0};
  for (int z_idx = 0; z_idx < num_z; z_idx++) {
    z_list[z_idx] = strtod(string_splitter_get_item(z_values, z_idx), NULL);
  }
  Config *config = config_create_or_die(
      "set -lex CSW24 -leaves CSW24 -wmp true -s1 equity -s2 equity "
      "-r1 all -r2 all -numplays 1 -threads 1");
  MoveList *list = move_list_create(BST_MOVE_LIST_CAPACITY);
  BlockingSetupSamples *samples =
      blocking_setup_samples_create(num_racks, BST_POOL_CAPACITY);
  BlockingSetupChecker *checker = blocking_setup_checker_create();
  FILE *in = fopen_or_die(positions_path, "r");
  char *line = malloc_or_die(BST_LINE_CAPACITY);
  long positions = 0;
  long full_work = 0;
  int64_t full_ns = 0;
  while (positions < max_positions &&
         fgets(line, BST_LINE_CAPACITY, in) != NULL) {
    char *end = NULL;
    const long game_idx = strtol(line, &end, 10);
    if (end == line) {
      continue;
    }
    char *cgp = line;
    for (int field_idx = 0; field_idx < 5 && cgp != NULL; field_idx++) {
      cgp = strchr(cgp, ',');
      if (cgp != NULL) {
        cgp++;
      }
    }
    assert(cgp != NULL);
    cgp[strcspn(cgp, "\r\n")] = '\0';
    char *command = get_formatted_string("cgp %s", cgp);
    load_and_exec_config_or_die(config, command);
    free(command);
    const Game *game = config_get_game(config);
    if (bag_get_letters(game_get_bag(game)) == 0) {
      continue;
    }
    bst_generate_all(game, list);
    const Move *moves[BST_MAX_UNIVERSE];
    Equity base[BST_MAX_UNIVERSE];
    int count = 0;
    for (int move_idx = 0;
         move_idx < move_list_get_count(list) && count < universe; move_idx++) {
      const Move *move = move_list_get_move(list, move_idx);
      if (move_get_type(move) != GAME_EVENT_PASS) {
        base[count] = move_get_equity(move);
        moves[count++] = move;
      }
    }
    if (count < 2) {
      continue;
    }
    blocking_setup_samples_deal(samples, game, num_racks, true, true,
                                ((uint64_t)game_idx * UINT64_C(7919)) + 1);
    // Full measurement and its argmax.
    int64_t start = ctimer_monotonic_ns();
    blocking_setup_checker_load(checker, game, samples, 1);
    Equity full_values[BST_MAX_UNIVERSE];
    int full_best = 0;
    for (int cand_idx = 0; cand_idx < count; cand_idx++) {
      BlockingSetupResult result;
      blocking_setup_checker_measure(checker, moves[cand_idx], &result);
      full_values[cand_idx] =
          base[cand_idx] +
          double_to_equity((blocking_weight * result.blocking_delta) +
                           (setup_weight * result.setup_delta));
      if (full_values[cand_idx] > full_values[full_best]) {
        full_best = cand_idx;
      }
    }
    full_ns += ctimer_monotonic_ns() - start;
    full_work += (long)count * num_racks;
    // Without elimination the race is the full argmax.
    blocking_setup_checker_load(checker, game, samples, 1);
    const BlockingSetupRaceSettings exact = {.z = 0.0};
    assert(blocking_setup_checker_choose(checker, moves, base, count,
                                         blocking_weight, setup_weight, &exact,
                                         NULL) == full_best);
    for (int z_idx = 0; z_idx < num_z; z_idx++) {
      const BlockingSetupRaceSettings race = {
          .batch_racks = BLOCKING_SETUP_RACE_BATCH,
          .min_racks = BLOCKING_SETUP_RACE_BATCH,
          .z = z_list[z_idx]};
      BlockingSetupRaceStats stats;
      start = ctimer_monotonic_ns();
      blocking_setup_checker_load(checker, game, samples, 1);
      const int pick = blocking_setup_checker_choose(
          checker, moves, base, count, blocking_weight, setup_weight, &race,
          &stats);
      race_ns[z_idx] += ctimer_monotonic_ns() - start;
      race_work[z_idx] += stats.candidate_racks;
      agree[z_idx] += pick == full_best;
      const double regret =
          equity_to_double(full_values[full_best] - full_values[pick]);
      regret_sum[z_idx] += regret;
      if (regret > regret_max[z_idx]) {
        regret_max[z_idx] = regret;
      }
    }
    positions++;
  }
  printf("blocking_setup_race positions=%ld racks=%d weights=%.3f,%.3f "
         "full_ms_per_position=%.2f\n",
         positions, num_racks, blocking_weight, setup_weight,
         (double)full_ns / 1e6 / (double)positions);
  for (int z_idx = 0; z_idx < num_z; z_idx++) {
    printf("  z=%.2f agree=%.4f mean_regret=%.4f max_regret=%.3f "
           "ms_per_position=%.2f speedup=%.2f work=%.3f\n",
           z_list[z_idx], (double)agree[z_idx] / (double)positions,
           regret_sum[z_idx] / (double)positions, regret_max[z_idx],
           (double)race_ns[z_idx] / 1e6 / (double)positions,
           (double)full_ns / (double)race_ns[z_idx],
           (double)race_work[z_idx] / (double)full_work);
  }
  free(line);
  (void)fclose(in);
  blocking_setup_checker_destroy(checker);
  blocking_setup_samples_destroy(samples);
  move_list_destroy(list);
  config_destroy(config);
  string_splitter_destroy(z_values);
  string_splitter_destroy(fields);
}
