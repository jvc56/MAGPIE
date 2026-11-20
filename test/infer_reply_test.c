#include "infer_reply_test.h"

#include "../src/compat/ctime.h"
#include "../src/def/bai_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/inference_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/inference_args.h"
#include "../src/ent/inference_results.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_args.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/stats.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/validated_move.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/inference.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/rack_draw.h"
#include "../src/impl/simmer.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  REPLY_CANDIDATES = 100,
  REPLY_LINE_SIZE = 4096,
  MODEL_CANDIDATES = 15,
  MODEL_DRAWS = 8,
  MODEL_RIVALS = 4
};

// Score the observed move given each possible leave and the configured opponent.
// Its rack is hypothetical; its opponent rack is unknown inside every rollout.
// The two-ply model normalizes over the entire 15-move shortlist. The
// four-ply model compares the observed move with the top four static rivals.
static void predict_search(const Config *config, const Game *base,
                           const Move *observed, const Rack *played,
                           char *leaves, bool sim2, ErrorStack *errors) {
  Timer timer;
  ctimer_start(&timer);
  Game *work = game_duplicate(base);
  const LetterDistribution *ld = game_get_ld(work);
  Rack empty;
  rack_set_dist_size_and_reset(&empty, ld_get_size(ld));
  MoveList *moves =
      move_list_create(sim2 ? MODEL_CANDIDATES : REPLY_CANDIDATES);
  MoveList *subset = move_list_create(MODEL_RIVALS + 1);
  SimResults *values = sim_results_create(0);
  SimCtx *sim = NULL;
  double search_seconds = 0;
  const int bag_size = bag_get_letters(game_get_bag(base));
  while (leaves) {
    char *next = strchr(leaves, ',');
    if (next) {
      *next++ = '\0';
    }
    Rack hypothetical;
    rack_set_dist_size_and_reset(&hypothetical, ld_get_size(ld));
    rack_set_to_string(ld, &hypothetical, leaves);
    rack_add(&hypothetical, played);
    assert(rack_get_total_letters(&hypothetical) == RACK_SIZE);
    return_rack_to_bag(work, 0);
    const bool drawable = draw_rack_from_bag(work, 0, &hypothetical);
    assert(drawable && bag_get_letters(game_get_bag(work)) == bag_size);
    const MoveGenArgs gen = {.game = work,
                             .move_list = moves,
                             .move_record_type = MOVE_RECORD_ALL,
                             .move_sort_type = MOVE_SORT_EQUITY,
                             .target_equity = EQUITY_MAX_VALUE,
                             .disable_pat = sim2};
    generate_moves(&gen);
    move_list_sort_moves(moves);
    const int count = move_list_get_count(moves);
    int rank = -1;
    for (int idx = 0; idx < count; idx++) {
      if (compare_moves_without_equity(move_list_get_move(moves, idx), observed,
                                       true) == -1) {
        rank = idx;
        break;
      }
    }
    double probability = 0;
    if (rank >= 0) {
      Timer search_timer;
      ctimer_start(&search_timer);
      SimArgs args;
      config_fill_sim_args(config, &empty, NULL, NULL, NULL, &args);
      args.game = work;
      if (!sim2) {
        move_list_reset(subset);
        for (int idx = 0; idx < count && idx < MODEL_RIVALS; idx++) {
          move_list_add_move(subset, move_list_get_move(moves, idx));
        }
        if (rank >= MODEL_RIVALS) {
          move_list_add_move(subset, move_list_get_move(moves, rank));
        }
      }
      args.move_list = sim2 ? moves : subset;
      args.num_plays = move_list_get_count(args.move_list);
      args.num_plies = sim2 ? 2 : 4;
      args.seed = 2120000001;
      args.use_inference = false;
      args.bag_cycle = false;
      args.pat_rollout_disabled = sim2;
      args.census_rollout_auto = false;
      args.use_census_rollout = false;
      args.bai_options.sampling_rule = BAI_SAMPLING_RULE_ROUND_ROBIN;
      args.bai_options.threshold = BAI_THRESHOLD_NONE;
      args.bai_options.sample_minimum = MODEL_DRAWS;
      args.bai_options.sample_limit = (uint64_t)args.num_plays * MODEL_DRAWS;
      args.bai_options.time_limit_seconds = 0;
      thread_control_set_status(args.thread_control,
                                THREAD_CONTROL_STATUS_STARTED);
      simulate(&args, &sim, values, errors);
      assert(error_stack_is_empty(errors));
      double total = 0;
      double best = 0;
      for (int idx = 0; idx < args.num_plays; idx++) {
        const SimmedPlay *sp = sim_results_get_simmed_play(values, idx);
        assert(simmed_play_get_num_rollouts(sp) == MODEL_DRAWS);
        const double utility = stat_get_mean(simmed_play_get_utility_stat(sp));
        const double weight = exp(utility / 0.04);
        total += weight;
        best = fmax(best, weight);
        if (compare_moves_without_equity(simmed_play_get_move(sp), observed,
                                         true) == -1) {
          probability = weight;
        }
      }
      probability /= sim2 ? total : best;
      search_seconds += ctimer_elapsed_seconds(&search_timer);
    }
    printf("REPLY_LIKELIHOOD %s %d %.12g\n", leaves, rank, probability);
    leaves = next;
  }
  move_list_destroy(subset);
  sim_ctx_destroy(sim);
  sim_results_destroy(values);
  move_list_destroy(moves);
  game_destroy(work);
  const double elapsed = ctimer_elapsed_seconds(&timer);
  printf("REPLY_MODEL_SECONDS %.6f %.6f\n", elapsed - search_seconds, elapsed);
}

// Read tab-separated CGP, observed move and optional list of feasible leaves.
// The synthetic player-0 rack supplies a legal way to replay the move. Only
// its union with the bag is used after that. Enumerate every remaining draw
// and report the engine's four-ply value of every common reply candidate.
void test_infer_reply(void) {
  Config *config = config_create_or_die(
      "set -lex CSW24 -leaves CSW24 -wmp true -rit true -ritmmap true "
      "-wit true -s1 equity -s2 equity -r1 all -r2 all -pat auto -threads 1 "
      "-numplays 100 -plies 4 -savesettings false -autosavegcg false");
  load_and_exec_config_or_die(
      config,
      "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0 -lex CSW24;");
  ErrorStack *errors = error_stack_create();
  config_load_win_pcts(config, errors);
  Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  ThreadControl *control = config_get_thread_control(config);
  MoveList *moves = move_list_create(REPLY_CANDIDATES);
  SimResults *values = sim_results_create(0);
  SimCtx *sim = NULL;
  InferenceResults *inferences = inference_results_create(NULL);
  InferenceCtx *inference = NULL;
  char line[REPLY_LINE_SIZE];
  while (fgets(line, sizeof(line), stdin)) {
    char *move_text = strchr(line, '\t');
    assert(move_text != NULL && strchr(move_text, '\n') != NULL);
    *move_text++ = '\0';
    move_text[strcspn(move_text, "\r\n")] = '\0';
    char *leaves = strchr(move_text, '\t');
    if (leaves) {
      *leaves++ = '\0';
    }
    game_load_cgp(game, line, errors);
    ValidatedMoves *observed =
        validated_moves_create(game, 0, move_text, false, true, errors);
    if (!error_stack_is_empty(errors)) {
      error_stack_print_and_reset(errors);
      log_fatal("bad pre-endgame replay: %s %s", line, move_text);
    }
    const Move *move = validated_moves_get_move(observed, 0);
    const int pre_bag = bag_get_letters(game_get_bag(game));
    assert(pre_bag >= 2 &&
           move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE &&
           move_get_tiles_played(move) == pre_bag - 1);
    Rack played, empty;
    rack_set_dist_size_and_reset(&played, ld_get_size(ld));
    rack_set_dist_size_and_reset(&empty, ld_get_size(ld));
    for (int idx = 0; idx < move_get_tiles_length(move); idx++) {
      const MachineLetter ml = move_get_tile(move, idx);
      if (ml != PLAYED_THROUGH_MARKER) {
        rack_add_letter(&played,
                        get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml);
      }
    }
    if (leaves) {
      predict_search(config, game, move, &played, leaves,
                     pre_bag == 2 && getenv("INFREPLY_SIM2") != NULL, errors);
    }
    Rack observer = *player_get_rack(game_get_player(game, 1));
    InferenceArgs inference_args;
    infer_args_fill(&inference_args, 0, int_to_equity(10000), NULL, game, 1, 0,
                    0, control, false, true, 0, move_get_score(move), 0,
                    &played, &empty, &observer);
    inference_args.temperature = int_to_equity(8);
    inference_args.target_move = move;
    thread_control_set_status(control, THREAD_CONTROL_STATUS_STARTED);
    infer(&inference_args, &inference, inferences, errors);
    assert(error_stack_is_empty(errors));
    const double mass = stat_get_total_weight(
        inference_results_get_equity_values(inferences, INFERENCE_TYPE_LEAVE));
    assert(mass > 0);
    play_move(move, game, NULL);
    validated_moves_destroy(observed);
    assert(bag_get_letters(game_get_bag(game)) == 1);
    Rack unseen = *player_get_rack(game_get_player(game, 0));
    MachineLetter last_tile;
    const int remaining = bag_peek_tiles(game_get_bag(game), &last_tile);
    assert(remaining == 1);
    rack_add_letter(&unseen, last_tile);
    assert(rack_get_total_letters(&unseen) == RACK_SIZE + 1);
    const MoveGenArgs gen = {.game = game,
                             .move_list = moves,
                             .move_record_type = MOVE_RECORD_ALL,
                             .move_sort_type = MOVE_SORT_EQUITY,
                             .target_equity = EQUITY_MAX_VALUE};
    generate_moves(&gen);
    move_list_sort_moves(moves);
    const int candidates = move_list_get_count(moves);
    for (int ml = 0; ml < ld_get_size(ld); ml++) {
      const int copies = rack_get_letter(&unseen, ml);
      if (copies == 0) {
        continue;
      }
      double expected_kept = 0.0;
      for (int count = 1; count <= copies && count <= RACK_SIZE; count++) {
        expected_kept += count *
                         inference_results_get_weight_subtotal(
                             inferences, INFERENCE_TYPE_LEAVE, ml, count) /
                         mass;
      }
      const double t8_bag_prob = (copies - expected_kept) / pre_bag;
      Rack full = unseen;
      rack_take_letter(&full, ml);
      SimArgs args;
      config_fill_sim_args(config, &full, &played, &observer, &empty, &args);
      args.move_list = moves;
      args.num_plays = candidates;
      args.use_inference = false;
      args.bag_cycle = false;
      args.seed = 2120000000;
      args.bai_options.threshold = BAI_THRESHOLD_NONE;
      args.bai_options.sampling_rule = BAI_SAMPLING_RULE_ROUND_ROBIN;
      args.bai_options.sample_minimum = 1;
      args.bai_options.sample_limit = (uint64_t)candidates;
      args.bai_options.time_limit_seconds = 0;
      thread_control_set_status(control, THREAD_CONTROL_STATUS_STARTED);
      simulate(&args, &sim, values, errors);
      assert(error_stack_is_empty(errors));
      char *tile = ld_ml_to_hl(ld, ml);
      printf("REPLY_TILE %s %d %.12g", tile, copies, t8_bag_prob);
      free(tile);
      for (int idx = 0; idx < candidates; idx++) {
        const SimmedPlay *reply = sim_results_get_simmed_play(values, idx);
        assert(simmed_play_get_num_rollouts(reply) == 1);
        assert(simmed_play_get_play_index_by_sort_type(reply) == idx);
        printf(" %.12g,%.12g,%.6f",
               stat_get_mean(simmed_play_get_utility_stat(reply)),
               stat_get_mean(simmed_play_get_win_pct_stat(reply)),
               stat_get_mean(simmed_play_get_equity_stat(reply)));
      }
      printf("\n");
    }
    printf("REPLY_END\n");
    fflush(stdout);
  }
  assert(!ferror(stdin));
  sim_ctx_destroy(sim);
  inference_ctx_destroy(inference);
  inference_results_destroy(inferences);
  sim_results_destroy(values);
  move_list_destroy(moves);
  error_stack_destroy(errors);
  config_destroy(config);
}

// Replenishing a sampled leave must draw uniformly from the remaining pool.
// There are eight distinct unseen letters. EGLNRS leaves I/Y for the draw,
// and GLNRSY leaves E/I. Compare the ordinary sim against its exact mixture.
void test_infer_reply_sampler(void) {
  Config *config = config_create_or_die(
      "set -lex CSW24 -leaves CSW24 -wmp true -rit true -ritmmap true "
      "-wit true -pat auto -threads 1 -plies 4 -numplays 100");
  load_and_exec_config_or_die(
      config, "cgp 7W6W/7U5LI/7D4KIF/MAISTERS4ONE/3HUE6BO1/1PrEPLAN1VOG1C1/"
              "6HEJAB2U1/3LOUIE1GAROTE/2QI10R/3pARROTY4I/2FAX2T6N/5ANTECEDE1I/"
              "7A6T/7V6E/7A6S EILRSYZ/DDIMNOO 375/417 0 -lex CSW24;");
  Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  ErrorStack *errors = error_stack_create();
  config_load_win_pcts(config, errors);
  ValidatedMoves *previous =
      validated_moves_create(game, 0, "14N Z.", false, true, errors);
  assert(error_stack_is_empty(errors));
  play_move(validated_moves_get_move(previous, 0), game, NULL);
  validated_moves_destroy(previous);
  load_and_exec_config_or_die(config, "gen");
  MoveList *only = move_list_create(1);
  move_list_add_move(only, move_list_get_move(config_get_move_list(config), 0));
  InferenceResults *inferences = inference_results_create(NULL);
  AliasMethod *alias = inference_results_get_alias_method(inferences);
  const char *leaves[] = {"EGLNRS", "GLNRSY"};
  Rack rack;
  rack_set_dist_size_and_reset(&rack, ld_get_size(ld));
  for (int idx = 0; idx < 2; idx++) {
    rack_set_to_string(ld, &rack, leaves[idx]);
    alias_method_add_weighted_rack(alias, &rack, 1, 1.0);
  }
  const bool usable = alias_method_generate_tables(alias);
  assert(usable);
  SimCtx *ctx = NULL;
  SimResults *results = sim_results_create(0);
  const char *full[] = {"GILNRSY", "EGLNRSY", "EGILNRS"};
  const double probs[] = {0.25, 0.5, 0.25};
  double expected = 0.0;
  for (int idx = 0; idx < 4; idx++) {
    const bool sample_leave = idx == 3;
    rack_reset(&rack);
    if (!sample_leave) {
      rack_set_to_string(ld, &rack, full[idx]);
    }
    SimArgs args;
    config_fill_sim_args(config, &rack, NULL, NULL, NULL, &args);
    args.use_inference = sample_leave;
    args.use_precomputed_inference = sample_leave;
    args.inference_results = inferences;
    args.move_list = only;
    args.num_plays = 1;
    args.seed = 2120000002;
    args.bag_cycle = false;
    args.bai_options.threshold = BAI_THRESHOLD_NONE;
    args.bai_options.sampling_rule = BAI_SAMPLING_RULE_ROUND_ROBIN;
    args.bai_options.sample_minimum = 1;
    args.bai_options.sample_limit = sample_leave ? 8192 : 1;
    args.bai_options.time_limit_seconds = 0;
    thread_control_set_status(args.thread_control,
                              THREAD_CONTROL_STATUS_STARTED);
    simulate(&args, &ctx, results, errors);
    assert(error_stack_is_empty(errors));
    const SimmedPlay *sp = sim_results_get_simmed_play(results, 0);
    assert(simmed_play_get_num_rollouts(sp) == args.bai_options.sample_limit);
    const double win = stat_get_mean(simmed_play_get_win_pct_stat(sp));
    if (sample_leave) {
      printf("posterior refill: expected %.5f observed %.5f\n", expected, win);
      fflush(stdout);
      assert(fabs(win - expected) < 0.025);
    } else {
      expected += probs[idx] * win;
    }
  }
  sim_ctx_destroy(ctx);
  sim_results_destroy(results);
  inference_results_destroy(inferences);
  move_list_destroy(only);
  error_stack_destroy(errors);
  config_destroy(config);
}
