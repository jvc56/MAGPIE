#include "infer_bench_test.h"

#include "../src/compat/ctime.h"
#include "../src/def/board_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/inference_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/pat_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/alias_method.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/encoded_rack.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/inference_args.h"
#include "../src/ent/inference_results.h"
#include "../src/ent/klv.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/pat.h"
#include "../src/ent/pat_eval.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/static_eval.h"
#include "../src/ent/stats.h"
#include "../src/ent/thread_control.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/inference.h"
#include "../src/impl/move_gen.h"
#include "../src/str/move_string.h"
#include "../src/str/rack_string.h"
#include "../src/util/io_util.h"
#include "../src/util/math_util.h"
#include "../src/util/string_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  INFBENCH_POLICIES = 3,
  INFBENCH_MODES = 11,
  INFBENCH_METRICS = 5,
  INFBENCH_DEFAULT_PAIRS = 4,
  INFBENCH_DEFAULT_POSITIONS = 2,
  INFBENCH_DEFAULT_SEED = 20261102,
  INFBENCH_THREADS = 4,
  INFBENCH_MAX_TURNS = 200,
  INFBENCH_SIM_ITERS = 1000,
};

// All finite comparisons use the same declared mixture. Raw log loss is
// also reported: a single unsupported true leave makes its mean infinite.
static const double INFBENCH_PRIOR_MIX = 0.01;
static const char *const infbench_policies[] = {"static", "pat", "sim2"};
static const char *const infbench_metric_names[] = {
    "mixed_nll", "klv_mse", "vowel_mse", "score_mse", "tile_mse"};

typedef struct InferBenchMode {
  const char *name;
  double margin;
  double temperature;
  int control;
} InferBenchMode;

static const InferBenchMode infbench_modes[INFBENCH_MODES] = {
    {"prior", 0, 0, 0},     {"hard0", 0, 0, 0},     {"hard5", 5, 0, 0},
    {"hard10", 10, 0, 0},   {"hard20", 20, 0, 0},   {"soft5t2", 5, 2, 2},
    {"soft5t5", 5, 5, 2},   {"soft10t2", 10, 2, 3}, {"soft10t5", 10, 5, 3},
    {"soft20t2", 20, 2, 4}, {"soft20t5", 20, 5, 4},
};

typedef struct InferBenchPrediction {
  double true_probability;
  double klv;
  double counts[MAX_ALPHABET_SIZE];
  double mass;
} InferBenchPrediction;

typedef struct InferBenchScore {
  double metrics[INFBENCH_METRICS];
  double tile_mse[MAX_ALPHABET_SIZE];
  double raw_nll;
  double prior_gain;
  double seconds;
  int observations;
  int excluded;
  int empty;
} InferBenchScore;

typedef struct InferBenchMoments {
  double sum;
  double squares;
  int count;
} InferBenchMoments;

static void infbench_add_score(InferBenchScore *total,
                               const InferBenchScore *score) {
  for (int metric_idx = 0; metric_idx < INFBENCH_METRICS; metric_idx++) {
    total->metrics[metric_idx] += score->metrics[metric_idx];
  }
  for (int ml = 0; ml < MAX_ALPHABET_SIZE; ml++) {
    total->tile_mse[ml] += score->tile_mse[ml];
  }
  total->raw_nll += score->raw_nll;
  total->prior_gain += score->prior_gain;
  total->seconds += score->seconds;
  total->observations += score->observations;
  total->excluded += score->excluded;
  total->empty += score->empty;
}

// Exact draw-count prior. Unlike inference this needs no move generation.
// The recursion is solely for E[KLV]; tile-count means are analytic.
static double infbench_prior_klv(const Rack *unseen, Rack *leave,
                                 const KLV *klv, int ml, int remaining,
                                 double ways) {
  if (remaining == 0) {
    return ways * equity_to_double(klv_get_leave_value(klv, leave));
  }
  if (ml == rack_get_dist_size(unseen)) {
    return 0;
  }
  const int available = rack_get_letter(unseen, ml);
  const int max_count = available < remaining ? available : remaining;
  double sum = 0;
  for (int count = 0; count <= max_count; count++) {
    rack_add_letters(leave, ml, count);
    sum += infbench_prior_klv(unseen, leave, klv, ml + 1, remaining - count,
                              ways * (double)choose(available, count));
    rack_take_letters(leave, ml, count);
  }
  return sum;
}

static InferBenchPrediction infbench_prior(const Rack *unseen,
                                           const Rack *truth, const KLV *klv) {
  InferBenchPrediction prediction = {.mass = 1};
  const int size = rack_get_total_letters(truth);
  const int total = rack_get_total_letters(unseen);
  const double combinations = (double)choose(total, size);
  assert(combinations > 0);
  double true_ways = 1;
  for (int ml = 0; ml < rack_get_dist_size(unseen); ml++) {
    true_ways *=
        (double)choose(rack_get_letter(unseen, ml), rack_get_letter(truth, ml));
    prediction.counts[ml] = (double)size * rack_get_letter(unseen, ml) / total;
  }
  prediction.true_probability = true_ways / combinations;
  Rack leave;
  rack_set_dist_size_and_reset(&leave, rack_get_dist_size(unseen));
  prediction.klv =
      infbench_prior_klv(unseen, &leave, klv, 0, size, 1) / combinations;
  return prediction;
}

static void infbench_add_mass(InferBenchPrediction *prediction,
                              const EncodedRack *encoded, double probability,
                              const Rack *truth, const KLV *klv, Rack *rack) {
  if (probability == 0) {
    return;
  }
  rack_decode(encoded, rack);
  prediction->mass += probability;
  if (racks_are_equal(rack, truth)) {
    prediction->true_probability += probability;
  }
  prediction->klv +=
      probability * equity_to_double(klv_get_leave_value(klv, rack));
  for (int ml = 0; ml < rack_get_dist_size(rack); ml++) {
    prediction->counts[ml] += probability * rack_get_letter(rack, ml);
  }
}

// Recover exact probabilities from the finalized alias table, without Monte
// Carlo sampling or a truncated common-leave list. Multiple exchange racks
// can map to the same kept leave; their probability is summed here.
static InferBenchPrediction infbench_posterior(const InferenceResults *results,
                                               const Rack *truth,
                                               const KLV *klv) {
  InferBenchPrediction prediction = {0};
  const AliasMethod *am = inference_results_get_alias_method(results);
  Rack rack;
  rack_set_dist_size_and_reset(&rack, rack_get_dist_size(truth));
  for (uint32_t item_idx = 0; item_idx < am->num_items; item_idx++) {
    const AliasMethodItem *item = &am->items[item_idx];
    assert(item->alias < am->num_items);
    infbench_add_mass(&prediction, &item->rack,
                      item->probability / am->num_items, truth, klv, &rack);
    infbench_add_mass(&prediction, &am->items[item->alias].rack,
                      (1 - item->probability) / am->num_items, truth, klv,
                      &rack);
  }
  assert(am->num_items == 0 || fabs(prediction.mass - 1) < 1e-7);
  return prediction;
}

static InferBenchScore
infbench_feature_score(const InferBenchPrediction *prediction,
                       const InferBenchPrediction *prior, const Rack *truth,
                       const KLV *klv, const LetterDistribution *ld) {
  InferBenchScore score = {
      .observations = 1, .raw_nll = NAN, .prior_gain = NAN};
  score.metrics[0] = NAN;
  score.empty = prediction->mass == 0;
  const double feature_prior_fraction = score.empty ? 1 : 0;
  const double expected_klv = ((1 - feature_prior_fraction) * prediction->klv) +
                              (feature_prior_fraction * prior->klv);
  const double klv_error =
      expected_klv - equity_to_double(klv_get_leave_value(klv, truth));
  score.metrics[1] = klv_error * klv_error;
  double vowel_error = 0;
  double tile_score_error = 0;
  for (int ml = 0; ml < ld_get_size(ld); ml++) {
    const double expected_count =
        ((1 - feature_prior_fraction) * prediction->counts[ml]) +
        (feature_prior_fraction * prior->counts[ml]);
    const double error = expected_count - rack_get_letter(truth, ml);
    score.tile_mse[ml] = error * error;
    score.metrics[4] += score.tile_mse[ml];
    if (ld_get_is_vowel(ld, ml)) {
      vowel_error += error;
    }
    tile_score_error += error * equity_to_double(ld_get_score(ld, ml));
  }
  score.metrics[2] = vowel_error * vowel_error;
  score.metrics[3] = tile_score_error * tile_score_error;
  return score;
}

static InferBenchScore infbench_score(const InferBenchPrediction *prediction,
                                      const InferBenchPrediction *prior,
                                      const Rack *truth, const KLV *klv,
                                      const LetterDistribution *ld) {
  InferBenchScore score =
      infbench_feature_score(prediction, prior, truth, klv, ld);
  score.excluded = prediction->true_probability == 0;
  score.raw_nll =
      score.excluded ? INFINITY : -log(prediction->true_probability);
  // An empty posterior falls back explicitly to the prior. Log loss uses
  // the declared mixture; feature MSE uses the actual posterior mean.
  const double prior_fraction = score.empty ? 1 : INFBENCH_PRIOR_MIX;
  const double probability =
      ((1 - prior_fraction) * prediction->true_probability) +
      (prior_fraction * prior->true_probability);
  assert(probability > 0);
  score.metrics[0] = -log(probability);
  score.prior_gain = log(probability / prior->true_probability);
  return score;
}

// Exchange feature marginals already cover every accepted candidate rack.
// Do not infer joint exchanged-set probability from these marginals.
static InferBenchPrediction
infbench_exchanged_posterior(InferenceResults *results, int ld_size) {
  InferBenchPrediction prediction = {.true_probability = NAN};
  const Stat *equities =
      inference_results_get_equity_values(results, INFERENCE_TYPE_EXCHANGED);
  const double total_weight = stat_get_total_weight(equities);
  if (total_weight == 0) {
    return prediction;
  }
  prediction.mass = 1;
  prediction.klv = stat_get_mean(equities);
  for (int ml = 0; ml < ld_size; ml++) {
    for (int count = 1; count <= RACK_SIZE; count++) {
      prediction.counts[ml] +=
          count *
          inference_results_get_weight_subtotal(
              results, INFERENCE_TYPE_EXCHANGED, ml, count) /
          total_weight;
    }
  }
  return prediction;
}

static void infbench_played_tiles(const Move *move, Rack *played) {
  rack_reset(played);
  const bool exchange = move_get_type(move) == GAME_EVENT_EXCHANGE;
  for (int tile_idx = 0; tile_idx < move_get_tiles_length(move); tile_idx++) {
    const MachineLetter ml = move_get_tile(move, tile_idx);
    if (exchange || ml != PLAYED_THROUGH_MARKER) {
      rack_add_letter(played, get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml);
    }
  }
  assert(rack_get_total_letters(played) == move_get_tiles_played(move));
}

static void infbench_observe(const Config *config, const Move *move, int policy,
                             int pair_idx, int game_idx, int turn,
                             InferBenchScore totals[INFBENCH_MODES],
                             InferBenchScore exchanged_totals[INFBENCH_MODES],
                             bool use_census) {
  const Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  const KLV *klv = player_get_klv(game_get_player(game, 0));
  const bool exchange = move_get_type(move) == GAME_EVENT_EXCHANGE;
  Rack played;
  rack_set_dist_size_and_reset(&played, ld_get_size(ld));
  infbench_played_tiles(move, &played);
  const Rack exchanged_truth = played;
  Rack truth = *player_get_rack(game_get_player(game, 0));
  rack_subtract_using_floor_zero(&truth, &played);
  assert(rack_get_total_letters(&truth) ==
         rack_get_total_letters(player_get_rack(game_get_player(game, 0))) -
             move_get_tiles_played(move));
  Rack unseen = *player_get_rack(game_get_player(game, 0));
  int bag_counts[MAX_ALPHABET_SIZE] = {0};
  bag_increment_unseen_count(game_get_bag(game), bag_counts);
  for (int ml = 0; ml < ld_get_size(ld); ml++) {
    rack_add_letters(&unseen, ml, bag_counts[ml]);
  }
  // Exchange identities are hidden from inference and from the prior.
  if (!exchange) {
    rack_subtract_using_floor_zero(&unseen, &played);
  }
  Timer prior_timer;
  ctimer_start(&prior_timer);
  const InferBenchPrediction prior = infbench_prior(&unseen, &truth, klv);
  InferBenchPrediction exchanged_prior = {0};
  if (exchange) {
    exchanged_prior = infbench_prior(&unseen, &exchanged_truth, klv);
  }
  const double prior_seconds = ctimer_elapsed_seconds(&prior_timer);
  assert(prior.true_probability > 0);
  Rack known;
  rack_set_dist_size_and_reset(&known, ld_get_size(ld));
  Rack observer = *player_get_rack(game_get_player(game, 1));
  if (exchange) {
    rack_reset(&played);
  }
  InferenceResults *results = inference_results_create(NULL);
  InferenceCtx *ctx = NULL;
  ErrorStack *errors = error_stack_create();
  InferenceArgs args;
  infer_args_fill(
      &args, 0, 0, NULL, game, INFBENCH_THREADS, 0, 0,
      config_get_thread_control(config), false, true, 0, move_get_score(move),
      exchange ? move_get_tiles_played(move) : 0, &played, &known, &observer);
  args.target_move = move;
  args.use_census_play_scores = use_census;
  StringBuilder *description = string_builder_create();
  string_builder_add_move_description(description, move, ld);
  printf("INFBENCH observation policy=%s pair=%d game=%d turn=%d bag=%d "
         "leave=%d exchange=%d move=%s\n",
         infbench_policies[policy], pair_idx, game_idx, turn,
         bag_get_letters(game_get_bag(game)), rack_get_total_letters(&truth),
         exchange, string_builder_peek(description));
  string_builder_clear(description);
  string_builder_add_rack(description, &truth, ld, true);
  char *cgp = game_get_cgp(game, false);
  printf("INFBENCH truth policy=%s pair=%d game=%d turn=%d rack=%s "
         "prior_probability=%.12g prior_seconds=%.6f cgp=%s\n",
         infbench_policies[policy], pair_idx, game_idx, turn,
         string_builder_peek(description), prior.true_probability,
         prior_seconds, cgp);
  free(cgp);
  if (exchange) {
    string_builder_clear(description);
    string_builder_add_rack(description, &exchanged_truth, ld, true);
    printf(
        "INFBENCH exchanged_truth policy=%s pair=%d game=%d turn=%d rack=%s\n",
        infbench_policies[policy], pair_idx, game_idx, turn,
        string_builder_peek(description));
  }
  string_builder_destroy(description);
  for (int mode_idx = 0; mode_idx < INFBENCH_MODES; mode_idx++) {
    Timer timer;
    ctimer_start(&timer);
    InferBenchPrediction prediction = prior;
    if (mode_idx > 0) {
      args.equity_margin = double_to_equity(infbench_modes[mode_idx].margin);
      args.temperature = double_to_equity(infbench_modes[mode_idx].temperature);
      thread_control_set_status(args.thread_control,
                                THREAD_CONTROL_STATUS_STARTED);
      infer(&args, &ctx, results, errors);
      if (!error_stack_is_empty(errors)) {
        error_stack_print_and_reset(errors);
        log_fatal("infbench inference failed: policy=%s pair=%d game=%d "
                  "turn=%d mode=%s",
                  infbench_policies[policy], pair_idx, game_idx, turn,
                  infbench_modes[mode_idx].name);
      }
      prediction = infbench_posterior(results, &truth, klv);
    }
    InferBenchScore score =
        infbench_score(&prediction, &prior, &truth, klv, ld);
    score.seconds =
        mode_idx == 0 ? prior_seconds : ctimer_elapsed_seconds(&timer);
    infbench_add_score(&totals[mode_idx], &score);
    printf("INFBENCH decision policy=%s pair=%d game=%d turn=%d mode=%s "
           "raw_nll=%.9g mixed_nll=%.9f gain_nats=%.9f klv_mse=%.9f "
           "vowel_mse=%.9f score_mse=%.9f tile_mse=%.9f excluded=%d empty=%d "
           "seconds=%.6f\n",
           infbench_policies[policy], pair_idx, game_idx, turn,
           infbench_modes[mode_idx].name, score.raw_nll, score.metrics[0],
           score.prior_gain, score.metrics[1], score.metrics[2],
           score.metrics[3], score.metrics[4], score.excluded, score.empty,
           score.seconds);
    if (exchange) {
      const InferBenchPrediction exchanged_prediction =
          mode_idx == 0
              ? exchanged_prior
              : infbench_exchanged_posterior(results, ld_get_size(ld));
      InferBenchScore exchanged_score = infbench_feature_score(
          &exchanged_prediction, &exchanged_prior, &exchanged_truth, klv, ld);
      exchanged_score.seconds = score.seconds;
      infbench_add_score(&exchanged_totals[mode_idx], &exchanged_score);
      printf("INFBENCH exchanged_decision policy=%s pair=%d game=%d turn=%d "
             "mode=%s klv_mse=%.9f vowel_mse=%.9f score_mse=%.9f tile_mse=%.9f "
             "empty=%d\n",
             infbench_policies[policy], pair_idx, game_idx, turn,
             infbench_modes[mode_idx].name, exchanged_score.metrics[1],
             exchanged_score.metrics[2], exchanged_score.metrics[3],
             exchanged_score.metrics[4], exchanged_score.empty);
    }
    (void)fflush(stdout);
  }
  error_stack_destroy(errors);
  inference_ctx_destroy(ctx);
  inference_results_destroy(results);
}

static Config *infbench_config(int policy, unsigned int seed) {
  char *settings = get_formatted_string(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-pat CSW21 -patcand1 %s -patcand2 false -patrollout false -numplays 15 "
      "-plies 2 -iterations %d -minplayiterations 10 -scond none "
      "-sr rr -tlim 0 -threads 1 -sinfer false -sbagcycle false "
      "-uwin 1 -uspread 0.5 -uspreadscale 100 -seed %u "
      "-savesettings false -autosavegcg false",
      policy == 1 ? "true" : "false", INFBENCH_SIM_ITERS, seed);
  Config *config = config_create_or_die(settings);
  free(settings);
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  if (policy == 1) {
    assert(player_get_pat(game_get_player(config_get_game(config), 0)) != NULL);
    assert(
        !player_get_pat_disabled(game_get_player(config_get_game(config), 0)));
    printf("INFBENCH pat name=%s\n",
           pat_get_name(
               player_get_pat(game_get_player(config_get_game(config), 0))));
  }
  return config;
}

void test_infer_bench_metrics(void) {
  Config *config = infbench_config(0, INFBENCH_DEFAULT_SEED);
  const LetterDistribution *ld = config_get_ld(config);
  const KLV *klv = player_get_klv(game_get_player(config_get_game(config), 0));
  Rack unseen;
  Rack truth;
  Rack other;
  rack_set_dist_size_and_reset(&unseen, ld_get_size(ld));
  rack_set_dist_size_and_reset(&truth, ld_get_size(ld));
  rack_set_dist_size_and_reset(&other, ld_get_size(ld));
  rack_set_to_string(ld, &unseen, "AAB");
  rack_set_to_string(ld, &truth, "AB");
  rack_set_to_string(ld, &other, "AA");
  // Blank placements are rack blanks; played-through squares consume none.
  Move blank_play = {0};
  move_set_type(&blank_play, GAME_EVENT_TILE_PLACEMENT_MOVE);
  move_set_tiles_length(&blank_play, 3);
  move_set_tiles_played(&blank_play, 2);
  const MachineLetter letter_a = ld_hl_to_ml(ld, "A");
  move_set_tile(&blank_play, get_blanked_machine_letter(letter_a), 0);
  move_set_tile(&blank_play, PLAYED_THROUGH_MARKER, 1);
  move_set_tile(&blank_play, letter_a, 2);
  Rack consumed;
  rack_set_dist_size_and_reset(&consumed, ld_get_size(ld));
  infbench_played_tiles(&blank_play, &consumed);
  assert(rack_get_letter(&consumed, BLANK_MACHINE_LETTER) == 1);
  assert(rack_get_letter(&consumed, letter_a) == 1);
  move_set_type(&blank_play, GAME_EVENT_EXCHANGE);
  move_set_tiles_length(&blank_play, 2);
  move_set_tile(&blank_play, BLANK_MACHINE_LETTER, 0);
  move_set_tile(&blank_play, letter_a, 1);
  infbench_played_tiles(&blank_play, &consumed);
  assert(rack_get_letter(&consumed, BLANK_MACHINE_LETTER) == 1);
  assert(rack_get_letter(&consumed, letter_a) == 1);
  const InferBenchPrediction prior = infbench_prior(&unseen, &truth, klv);
  assert(fabs(prior.true_probability - (2.0 / 3.0)) < 1e-12);
  const double truth_klv = equity_to_double(klv_get_leave_value(klv, &truth));
  const double other_klv = equity_to_double(klv_get_leave_value(klv, &other));
  assert(fabs(prior.klv - (((2 * truth_klv) + other_klv) / 3)) < 1e-12);
  InferenceResults *results = inference_results_create(NULL);
  AliasMethod *am = inference_results_get_alias_method(results);
  alias_method_add_weighted_rack(am, &truth, 1, 0.5);
  alias_method_add_weighted_rack(am, &other, 1, 0.25);
  alias_method_add_weighted_rack(am, &truth, 1, 0.25);
  const bool generated = alias_method_generate_tables(am);
  assert(generated);
  const InferBenchPrediction prediction =
      infbench_posterior(results, &truth, klv);
  assert(fabs(prediction.true_probability - 0.75) < 1e-12);
  assert(fabs(prediction.klv - (0.75 * truth_klv + 0.25 * other_klv)) < 1e-12);
  const InferBenchScore score =
      infbench_score(&prediction, &prior, &truth, klv, ld);
  const double expected_nll = -log((0.99 * 0.75) + (0.01 * (2.0 / 3.0)));
  const double expected_raw_nll = -log(0.75);
  assert(fabs(score.metrics[0] - expected_nll) < 1e-12);
  assert(fabs(score.raw_nll - expected_raw_nll) < 1e-12);
  assert(fabs(score.metrics[2] - 0.0625) < 1e-12);
  assert(fabs(score.metrics[3] - 0.25) < 1e-12);
  assert(fabs(score.metrics[4] - 0.125) < 1e-12);
  assert(!score.empty && !score.excluded);
  // Exchange marginals must match the exact joint example above, including
  // duplicate A's, in both weighted and draw-count modes.
  for (int weighted_idx = 0; weighted_idx < 2; weighted_idx++) {
    inference_results_reset(results, 0, ld_get_size(ld));
    Stat *equities =
        inference_results_get_equity_values(results, INFERENCE_TYPE_EXCHANGED);
    if (weighted_idx == 1) {
      stat_push_weighted(equities, truth_klv, 3, 0.75);
      stat_push_weighted(equities, other_klv, 1, 0.25);
    } else {
      stat_push(equities, truth_klv, 3);
      stat_push(equities, other_klv, 1);
    }
    for (int ml = 0; ml < ld_get_size(ld); ml++) {
      const int truth_count = rack_get_letter(&truth, ml);
      const int other_count = rack_get_letter(&other, ml);
      if (truth_count > 0) {
        if (weighted_idx == 1) {
          inference_results_add_to_letter_weight_subtotal(
              results, INFERENCE_TYPE_EXCHANGED, ml, truth_count, 0.75);
        } else {
          inference_results_add_to_letter_subtotal(
              results, INFERENCE_TYPE_EXCHANGED, ml, truth_count,
              INFERENCE_SUBTOTAL_DRAW, 3);
        }
      }
      if (other_count > 0) {
        if (weighted_idx == 1) {
          inference_results_add_to_letter_weight_subtotal(
              results, INFERENCE_TYPE_EXCHANGED, ml, other_count, 0.25);
        } else {
          inference_results_add_to_letter_subtotal(
              results, INFERENCE_TYPE_EXCHANGED, ml, other_count,
              INFERENCE_SUBTOTAL_DRAW, 1);
        }
      }
    }
    const InferBenchPrediction exchanged =
        infbench_exchanged_posterior(results, ld_get_size(ld));
    const InferBenchScore exchanged_score =
        infbench_feature_score(&exchanged, &prior, &truth, klv, ld);
    assert(isnan(exchanged.true_probability));
    assert(isnan(exchanged_score.raw_nll) && isnan(exchanged_score.metrics[0]));
    for (int metric_idx = 1; metric_idx < INFBENCH_METRICS; metric_idx++) {
      assert(fabs(exchanged_score.metrics[metric_idx] -
                  score.metrics[metric_idx]) < 1e-12);
    }
  }
  inference_results_reset(results, 0, ld_get_size(ld));
  const InferBenchPrediction empty_exchange =
      infbench_exchanged_posterior(results, ld_get_size(ld));
  assert(empty_exchange.mass == 0);
  alias_method_reset(am);
  const InferBenchPrediction empty = infbench_posterior(results, &truth, klv);
  const InferBenchScore fallback =
      infbench_score(&empty, &prior, &truth, klv, ld);
  const InferBenchScore prior_score =
      infbench_score(&prior, &prior, &truth, klv, ld);
  assert(fallback.empty && fallback.excluded && isinf(fallback.raw_nll));
  for (int metric_idx = 0; metric_idx < INFBENCH_METRICS; metric_idx++) {
    assert(fabs(fallback.metrics[metric_idx] -
                prior_score.metrics[metric_idx]) < 1e-12);
  }
  inference_results_destroy(results);
  config_destroy(config);
}

static double infbench_pat_best(Game *game, MoveList *moves, double *seconds) {
  Timer timer;
  ctimer_start(&timer);
  Player *player = game_get_player(game, 0);
  player_set_pat_usage(player, false, 0);
  const double best =
      equity_to_double(move_get_equity(get_top_equity_move(game, moves)));
  player_set_pat_usage(player, true, 0);
  *seconds += ctimer_elapsed_seconds(&timer);
  return best;
}

// Test-only one-tile posterior: enumerate every available letter, rescore
// the top 64 static moves with PAT, and always include the observed move.
static void infbench_pat64_observe(const Config *config, const Move *observed,
                                   int pair_idx, int game_idx, int turn) {
  Game *game = game_duplicate(config_get_game(config));
  const LetterDistribution *ld = game_get_ld(game);
  const Player *player = game_get_player(game, 0);
  const KLV *klv = player_get_klv(player);
  const PATWeights *pat = player_get_pat(player);
  assert(pat != NULL && player_get_pat_disabled(player));
  Rack played;
  rack_set_dist_size_and_reset(&played, ld_get_size(ld));
  infbench_played_tiles(observed, &played);
  Rack truth = *player_get_rack(player);
  rack_subtract_using_floor_zero(&truth, &played);
  assert(rack_get_total_letters(&truth) == 1);
  Rack unseen = truth;
  int bag_counts[MAX_ALPHABET_SIZE] = {0};
  bag_increment_unseen_count(game_get_bag(game), bag_counts);
  for (int ml = 0; ml < ld_get_size(ld); ml++) {
    rack_add_letters(&unseen, ml, bag_counts[ml]);
  }
  const InferBenchPrediction prior = infbench_prior(&unseen, &truth, klv);
  InferBenchPrediction predictions[3] = {0};
  double shortlist_seconds = 0;
  double direct_seconds = 0;
  int differing_best = 0;
  int candidate_racks = 0;
  MoveList *direct_moves = move_list_create(1);
  Equity word_penalties[2 * BOARD_DIM];
  Equity letter_penalties[2 * BOARD_DIM];
  board_copy_opening_penalties(game_get_board(game), word_penalties,
                               letter_penalties);
  MoveList *moves = move_list_create(64);
  PATEvalContext *pat_context = malloc_or_die(sizeof(PATEvalContext));
  Timer timer;
  ctimer_start(&timer);
  for (int ml = 0; ml < ld_get_size(ld); ml++) {
    const int count = rack_get_letter(&unseen, ml);
    if (count == 0) {
      continue;
    }
    Rack leave;
    rack_set_dist_size_and_reset(&leave, ld_get_size(ld));
    rack_add_letter(&leave, ml);
    rack_copy(player_get_rack(player), &played);
    rack_add_letter(player_get_rack(player), ml);
    double direct_best = 0;
    if (ml % 2 == 0) {
      direct_best = infbench_pat_best(game, direct_moves, &direct_seconds);
    }
    Timer path_timer;
    ctimer_start(&path_timer);
    pat_eval_context_load(
        pat_context, pat, board_get_readonly_lanes(game_get_board(game), 0), ld,
        player_get_rack(player), PAT_CLASS_MASK_ALL,
        rack_get_total_letters(player_get_rack(game_get_player(game, 1))));
    pat_eval_context_set_kwg(pat_context, player_get_kwg(player),
                             game_get_variant(game));
    pat_eval_context_set_utility(
        pat_context,
        equity_to_int(player_get_score(player) -
                      player_get_score(game_get_player(game, 1))),
        bag_get_letters(game_get_bag(game)));
    const MoveGenArgs gen_args = {.game = game,
                                  .move_list = moves,
                                  .move_record_type = MOVE_RECORD_ALL,
                                  .move_sort_type = MOVE_SORT_EQUITY,
                                  .target_equity = EQUITY_MAX_VALUE};
    generate_moves(&gen_args);
    double observed_values[3];
    double best[3];
    for (int mode_idx = 0; mode_idx < 3; mode_idx++) {
      observed_values[mode_idx] = equity_to_double(static_eval_get_move_equity(
          ld, klv, observed, &leave, player_get_rack(game_get_player(game, 1)),
          word_penalties, letter_penalties,
          mode_idx ? pat_eval_context_get_active_classes(pat_context) : 0,
          mode_idx ? pat_context : NULL,
          board_get_tiles_played(game_get_board(game)),
          bag_get_letters(game_get_bag(game))));
      best[mode_idx] = observed_values[mode_idx];
    }
    for (int move_idx = 0; move_idx < move_list_get_count(moves); move_idx++) {
      const Move *candidate = move_list_get_move(moves, move_idx);
      Rack used;
      rack_set_dist_size_and_reset(&used, ld_get_size(ld));
      infbench_played_tiles(candidate, &used);
      Rack candidate_leave = *player_get_rack(player);
      rack_subtract_using_floor_zero(&candidate_leave, &used);
      best[0] = fmax(best[0], equity_to_double(move_get_equity(candidate)));
      best[1] =
          fmax(best[1],
               equity_to_double(static_eval_get_move_equity(
                   ld, klv, candidate, &candidate_leave,
                   player_get_rack(game_get_player(game, 1)), word_penalties,
                   letter_penalties,
                   pat_eval_context_get_active_classes(pat_context),
                   pat_context, board_get_tiles_played(game_get_board(game)),
                   bag_get_letters(game_get_bag(game)))));
    }
    shortlist_seconds += ctimer_elapsed_seconds(&path_timer);
    if (ml % 2 != 0) {
      direct_best = infbench_pat_best(game, direct_moves, &direct_seconds);
    }
    best[2] = fmax(observed_values[2], direct_best);
    assert(best[2] + 0.0011 >= best[1]);
    differing_best += fabs(best[2] - best[1]) > 0.0011;
    candidate_racks++;
    for (int mode_idx = 0; mode_idx < 3; mode_idx++) {
      const double regret = fmax(0, best[mode_idx] - observed_values[mode_idx]);
      if (regret > 20) {
        continue;
      }
      const double weight = count * exp(-regret / 5);
      InferBenchPrediction *prediction = &predictions[mode_idx];
      prediction->mass += weight;
      prediction->counts[ml] += weight;
      prediction->klv +=
          weight * equity_to_double(klv_get_leave_value(klv, &leave));
      if (racks_are_equal(&leave, &truth)) {
        prediction->true_probability += weight;
      }
    }
  }
  // Independently verify the enumerated static control against production.
  Rack known;
  rack_set_dist_size_and_reset(&known, ld_get_size(ld));
  Rack observer = *player_get_rack(game_get_player(config_get_game(config), 1));
  InferenceArgs reference_args;
  infer_args_fill(&reference_args, 0, int_to_equity(20), NULL,
                  config_get_game(config), 1, 0, 0,
                  config_get_thread_control(config), false, true, 0,
                  move_get_score(observed), 0, &played, &known, &observer);
  reference_args.temperature = int_to_equity(5);
  reference_args.target_move = observed;
  reference_args.use_census_play_scores = false;
  InferenceResults *reference_results = inference_results_create(NULL);
  ErrorStack *reference_errors = error_stack_create();
  thread_control_set_status(reference_args.thread_control,
                            THREAD_CONTROL_STATUS_STARTED);
  infer_without_ctx(&reference_args, reference_results, reference_errors);
  assert(error_stack_is_empty(reference_errors));
  const InferBenchPrediction reference =
      infbench_posterior(reference_results, &truth, klv);
  const InferBenchScore reference_score =
      infbench_score(&reference, &prior, &truth, klv, ld);
  error_stack_destroy(reference_errors);
  inference_results_destroy(reference_results);
  for (int mode_idx = 0; mode_idx < 4; mode_idx++) {
    InferBenchPrediction prediction =
        mode_idx == 0 ? prior : predictions[mode_idx - 1];
    if (mode_idx > 0 && prediction.mass > 0) {
      prediction.klv /= prediction.mass;
      prediction.true_probability /= prediction.mass;
      for (int ml = 0; ml < ld_get_size(ld); ml++) {
        prediction.counts[ml] /= prediction.mass;
      }
      prediction.mass = 1;
    }
    const InferBenchScore score =
        infbench_score(&prediction, &prior, &truth, klv, ld);
    if (mode_idx == 1) {
      for (int metric_idx = 0; metric_idx < INFBENCH_METRICS; metric_idx++) {
        assert(fabs(score.metrics[metric_idx] -
                    reference_score.metrics[metric_idx]) < 1e-7);
      }
    }
    printf("PATBENCH pair=%d game=%d turn=%d mode=%s nll=%.9f klv_mse=%.9f "
           "vowel_mse=%.9f score_mse=%.9f tile_mse=%.9f excluded=%d empty=%d "
           "seconds=%.6f\n",
           pair_idx, game_idx, turn,
           mode_idx == 0   ? "prior"
           : mode_idx == 1 ? "static20t5"
           : mode_idx == 2 ? "pat64_20t5"
                           : "patbest_20t5",
           score.metrics[0], score.metrics[1], score.metrics[2],
           score.metrics[3], score.metrics[4], score.excluded, score.empty,
           ctimer_elapsed_seconds(&timer));
  }
  printf("PATTIMING pair=%d game=%d turn=%d racks=%d differing=%d "
         "shortlist_seconds=%.9f direct_seconds=%.9f\n",
         pair_idx, game_idx, turn, candidate_racks, differing_best,
         shortlist_seconds, direct_seconds);
  move_list_destroy(direct_moves);
  free(pat_context);
  move_list_destroy(moves);
  game_destroy(game);
}

enum {
  CAL_MARGIN_COUNT = 4,
  CAL_TEMP_COUNT = 5,
  CAL_MODE_COUNT = CAL_MARGIN_COUNT * CAL_TEMP_COUNT
};
static const int calibration_margins[CAL_MARGIN_COUNT] = {10, 20, 40, 80};
static const int calibration_temperatures[CAL_TEMP_COUNT] = {0, 2, 5, 10, 20};

typedef struct CalibrationContext {
  Game *game;
  const Move *observed;
  const LetterDistribution *ld;
  const KLV *klv;
  Rack truth;
  Rack unseen;
  Rack played;
  Rack leave;
  MoveList *moves;
  PATEvalContext *pat;
  Equity word_penalties[2 * BOARD_DIM];
  Equity letter_penalties[2 * BOARD_DIM];
  InferBenchPrediction predictions[2][CAL_MODE_COUNT];
  double seconds[2];
  int racks;
} CalibrationContext;

static void calibration_evaluate_rack(CalibrationContext *ctx, double ways) {
  Player *player = game_get_player(ctx->game, 0);
  rack_copy(player_get_rack(player), &ctx->played);
  for (int ml = 0; ml < ld_get_size(ctx->ld); ml++) {
    rack_add_letters(player_get_rack(player), ml,
                     rack_get_letter(&ctx->leave, ml));
  }
  for (int model = 0; model < 2; model++) {
    Timer timer;
    ctimer_start(&timer);
    player_set_pat_usage(player, model == 0, 0);
    const double best = equity_to_double(
        move_get_equity(get_top_equity_move(ctx->game, ctx->moves)));
    PATEvalContext *pat = NULL;
    uint32_t classes = 0;
    if (model == 1) {
      pat = ctx->pat;
      pat_eval_context_load(
          pat, player_get_pat(player),
          board_get_readonly_lanes(game_get_board(ctx->game), 0), ctx->ld,
          player_get_rack(player), PAT_CLASS_MASK_ALL,
          rack_get_total_letters(
              player_get_rack(game_get_player(ctx->game, 1))));
      pat_eval_context_set_kwg(pat, player_get_kwg(player),
                               game_get_variant(ctx->game));
      pat_eval_context_set_utility(
          pat,
          equity_to_int(player_get_score(player) -
                        player_get_score(game_get_player(ctx->game, 1))),
          bag_get_letters(game_get_bag(ctx->game)));
      classes = pat_eval_context_get_active_classes(pat);
    }
    const double observed = equity_to_double(static_eval_get_move_equity(
        ctx->ld, ctx->klv, ctx->observed, &ctx->leave,
        player_get_rack(game_get_player(ctx->game, 1)), ctx->word_penalties,
        ctx->letter_penalties, classes, pat,
        board_get_tiles_played(game_get_board(ctx->game)),
        bag_get_letters(game_get_bag(ctx->game))));
    const double regret = fmax(0, best - observed);
    ctx->seconds[model] += ctimer_elapsed_seconds(&timer);
    for (int margin_idx = 0; margin_idx < CAL_MARGIN_COUNT; margin_idx++) {
      if (regret > calibration_margins[margin_idx] + 0.000001) {
        continue;
      }
      for (int temp_idx = 0; temp_idx < CAL_TEMP_COUNT; temp_idx++) {
        const int temperature = calibration_temperatures[temp_idx];
        const double weight =
            ways * (temperature == 0 ? 1 : exp(-regret / temperature));
        InferBenchPrediction *prediction =
            &ctx->predictions[model][margin_idx * CAL_TEMP_COUNT + temp_idx];
        prediction->mass += weight;
        prediction->klv +=
            weight *
            equity_to_double(klv_get_leave_value(ctx->klv, &ctx->leave));
        if (racks_are_equal(&ctx->leave, &ctx->truth)) {
          prediction->true_probability += weight;
        }
        for (int ml = 0; ml < ld_get_size(ctx->ld); ml++) {
          prediction->counts[ml] += weight * rack_get_letter(&ctx->leave, ml);
        }
      }
    }
  }
  ctx->racks++;
}

static void calibration_enumerate(CalibrationContext *ctx, int ml,
                                  int remaining, double ways) {
  if (remaining == 0) {
    calibration_evaluate_rack(ctx, ways);
    return;
  }
  if (ml == ld_get_size(ctx->ld)) {
    return;
  }
  const int available = rack_get_letter(&ctx->unseen, ml);
  const int maximum = available < remaining ? available : remaining;
  for (int count = 0; count <= maximum; count++) {
    rack_add_letters(&ctx->leave, ml, count);
    calibration_enumerate(ctx, ml + 1, remaining - count,
                          ways * (double)choose(available, count));
    rack_take_letters(&ctx->leave, ml, count);
  }
}

static void calibration_print(const InferBenchScore *score, int pair_idx,
                              int game_idx, int turn, int leave, int bag,
                              bool exchange, const char *model, int margin,
                              int temperature) {
  printf("CALIBRATION pair=%d game=%d turn=%d leave=%d bag=%d exchange=%d "
         "model=%s margin=%d temperature=%d nll=%.9f klv_mse=%.9f "
         "vowel_mse=%.9f score_mse=%.9f tile_mse=%.9f excluded=%d empty=%d\n",
         pair_idx, game_idx, turn, leave, bag, exchange, model, margin,
         temperature, score->metrics[0], score->metrics[1], score->metrics[2],
         score->metrics[3], score->metrics[4], score->excluded, score->empty);
}

static void calibration_observe(const Config *config, const Move *observed,
                                int pair_idx, int game_idx, int turn,
                                bool timing_only) {
  CalibrationContext *ctx = calloc_or_die(1, sizeof(CalibrationContext));
  ctx->game = game_duplicate(config_get_game(config));
  ctx->observed = observed;
  ctx->ld = game_get_ld(ctx->game);
  ctx->klv = player_get_klv(game_get_player(ctx->game, 0));
  rack_set_dist_size_and_reset(&ctx->played, ld_get_size(ctx->ld));
  infbench_played_tiles(observed, &ctx->played);
  ctx->truth = *player_get_rack(game_get_player(ctx->game, 0));
  rack_subtract_using_floor_zero(&ctx->truth, &ctx->played);
  const bool exchange = move_get_type(observed) == GAME_EVENT_EXCHANGE;
  ctx->unseen =
      exchange ? *player_get_rack(game_get_player(ctx->game, 0)) : ctx->truth;
  int bag_counts[MAX_ALPHABET_SIZE] = {0};
  bag_increment_unseen_count(game_get_bag(ctx->game), bag_counts);
  for (int ml = 0; ml < ld_get_size(ctx->ld); ml++) {
    rack_add_letters(&ctx->unseen, ml, bag_counts[ml]);
  }
  const int leave_size = rack_get_total_letters(&ctx->truth);
  const int bag = bag_get_letters(game_get_bag(ctx->game));
  const InferBenchPrediction prior =
      infbench_prior(&ctx->unseen, &ctx->truth, ctx->klv);
  const InferBenchScore prior_score =
      infbench_score(&prior, &prior, &ctx->truth, ctx->klv, ctx->ld);
  calibration_print(&prior_score, pair_idx, game_idx, turn, leave_size, bag,
                    exchange, "prior", 0, 0);
  const bool enumerate = !timing_only && !exchange && leave_size <= 4;
  if (enumerate) {
    ctx->moves = move_list_create(1);
    ctx->pat = malloc_or_die(sizeof(PATEvalContext));
    rack_set_dist_size_and_reset(&ctx->leave, ld_get_size(ctx->ld));
    board_copy_opening_penalties(game_get_board(ctx->game), ctx->word_penalties,
                                 ctx->letter_penalties);
    calibration_enumerate(ctx, 0, leave_size, 1);
  }
  Rack known;
  rack_set_dist_size_and_reset(&known, ld_get_size(ctx->ld));
  Rack observer = *player_get_rack(game_get_player(config_get_game(config), 1));
  if (exchange) {
    rack_reset(&ctx->played);
  }
  InferenceArgs args;
  infer_args_fill(&args, 0, 0, NULL, config_get_game(config), 1, 0, 0,
                  config_get_thread_control(config), false, true, 0,
                  move_get_score(observed),
                  exchange ? move_get_tiles_played(observed) : 0, &ctx->played,
                  &known, &observer);
  args.target_move = observed;
  InferenceResults *results = inference_results_create(NULL);
  InferenceCtx *inference_ctx = NULL;
  ErrorStack *errors = error_stack_create();
  for (int iteration = 0; iteration < (timing_only ? 2 : CAL_MODE_COUNT);
       iteration++) {
    const int mode =
        timing_only ? ((iteration + turn) % 2 == 0 ? 7 : 17) : iteration;
    args.equity_margin =
        int_to_equity(calibration_margins[mode / CAL_TEMP_COUNT]);
    args.temperature =
        int_to_equity(calibration_temperatures[mode % CAL_TEMP_COUNT]);
    InferBenchScore reference = {0};
    // One production parity check per enumerated observation; all modes for
    // larger leaves and exchanges, where census avoids rack-by-rack movegen.
    if (!enumerate || mode == 7) {
      thread_control_set_status(args.thread_control,
                                THREAD_CONTROL_STATUS_STARTED);
      Timer inference_timer;
      ctimer_start(&inference_timer);
      infer(&args, &inference_ctx, results, errors);
      if (timing_only) {
        printf("CALINFER_TIME pair=%d game=%d turn=%d leave=%d exchange=%d "
               "margin=%d seconds=%.9f\n",
               pair_idx, game_idx, turn, leave_size, exchange,
               calibration_margins[mode / CAL_TEMP_COUNT],
               ctimer_elapsed_seconds(&inference_timer));
      }
      assert(error_stack_is_empty(errors));
      const InferBenchPrediction prediction =
          infbench_posterior(results, &ctx->truth, ctx->klv);
      reference =
          infbench_score(&prediction, &prior, &ctx->truth, ctx->klv, ctx->ld);
    }
    for (int model = 0; model < (enumerate ? 2 : 1); model++) {
      InferBenchScore score = reference;
      if (enumerate) {
        InferBenchPrediction prediction = ctx->predictions[model][mode];
        if (prediction.mass > 0) {
          prediction.klv /= prediction.mass;
          prediction.true_probability /= prediction.mass;
          for (int ml = 0; ml < ld_get_size(ctx->ld); ml++) {
            prediction.counts[ml] /= prediction.mass;
          }
          prediction.mass = 1;
        }
        score =
            infbench_score(&prediction, &prior, &ctx->truth, ctx->klv, ctx->ld);
        if (model == 0 && mode == 7) {
          for (int metric_idx = 0; metric_idx < INFBENCH_METRICS;
               metric_idx++) {
            assert(fabs(score.metrics[metric_idx] -
                        reference.metrics[metric_idx]) < 1e-6);
          }
        }
      }
      calibration_print(&score, pair_idx, game_idx, turn, leave_size, bag,
                        exchange, model == 0 ? "static" : "pat",
                        calibration_margins[mode / CAL_TEMP_COUNT],
                        calibration_temperatures[mode % CAL_TEMP_COUNT]);
    }
  }
  printf("CALTIMING pair=%d game=%d turn=%d leave=%d racks=%d "
         "static_seconds=%.6f pat_seconds=%.6f\n",
         pair_idx, game_idx, turn, leave_size, ctx->racks, ctx->seconds[0],
         ctx->seconds[1]);
  inference_ctx_destroy(inference_ctx);
  inference_results_destroy(results);
  error_stack_destroy(errors);
  if (ctx->moves != NULL) {
    move_list_destroy(ctx->moves);
  }
  free(ctx->pat);
  game_destroy(ctx->game);
  free(ctx);
}

static void infbench_game(Config *config, int policy, int pair_idx,
                          int game_idx, int positions, unsigned int seed,
                          InferBenchScore scores[INFBENCH_MODES],
                          InferBenchScore exchanged_scores[INFBENCH_MODES],
                          bool exchanges_only, bool use_census, bool one_pat,
                          bool calibrate, bool timing_only) {
  Game *game = config_get_game(config);
  game_reset(game);
  game_seed(game, seed);
  game_set_starting_player_index(game, game_idx);
  draw_starting_racks(game);
  MoveList *moves = move_list_create(1);
  bool sampled[RACK_SIZE + 1][3] = {0};
  int decisions = 0;
  int observations = 0;
  int turn = 0;
  // Sample at target decisions 4, 10, 16, ...; defer passes and bingos to
  // the next nontrivial leave. No filtering on inferred probability or regret.
  while (!game_over(game) && observations < positions &&
         (!exchanges_only || bag_get_letters(game_get_bag(game)) > 0) &&
         turn < INFBENCH_MAX_TURNS) {
    Move chosen;
    if (policy == 2 && game_get_player_on_turn_index(game) == 0 &&
        bag_get_letters(game_get_bag(game)) > 0) {
      load_and_exec_config_or_die(config, "gen");
      char *sim_command =
          get_formatted_string("sim -seed %u", seed + (unsigned int)turn);
      load_and_exec_config_or_die(config, sim_command);
      free(sim_command);
      move_copy(&chosen,
                sim_results_get_best_move(config_get_sim_results(config)));
    } else {
      move_copy(&chosen, get_top_equity_move(game, moves));
    }
    if (game_get_player_on_turn_index(game) == 0) {
      decisions++;
      const game_event_t type = move_get_type(&chosen);
      const int leave_size =
          rack_get_total_letters(player_get_rack(game_get_player(game, 0))) -
          move_get_tiles_played(&chosen);
      const int bag_count = bag_get_letters(game_get_bag(game));
      const int stage = bag_count <= 14 ? 0 : bag_count <= 42 ? 1 : 2;
      const bool sample =
          calibrate
              ? (type == GAME_EVENT_EXCHANGE ||
                 (!exchanges_only && type == GAME_EVENT_TILE_PLACEMENT_MOVE &&
                  leave_size > 0 && !sampled[leave_size][stage]))
          : one_pat
              ? (leave_size == 1 && type == GAME_EVENT_TILE_PLACEMENT_MOVE)
          : exchanges_only
              ? type == GAME_EVENT_EXCHANGE
              : (decisions >= 4 + 6 * observations && leave_size > 0 &&
                 (type == GAME_EVENT_TILE_PLACEMENT_MOVE ||
                  type == GAME_EVENT_EXCHANGE));
      if (sample && bag_get_letters(game_get_bag(game)) > 0) {
        if (calibrate) {
          calibration_observe(config, &chosen, pair_idx, game_idx, turn,
                              timing_only);
          sampled[leave_size][stage] = true;
        } else if (one_pat) {
          infbench_pat64_observe(config, &chosen, pair_idx, game_idx, turn);
        } else {
          infbench_observe(config, &chosen, policy, pair_idx, game_idx, turn,
                           scores, exchanged_scores, use_census);
        }
        observations++;
      }
    }
    play_move(&chosen, game, NULL);
    turn++;
  }
  printf("INFBENCH game policy=%s pair=%d game=%d observations=%d requested=%d "
         "turns=%d\n",
         infbench_policies[policy], pair_idx, game_idx, observations, positions,
         turn);
  move_list_destroy(moves);
}

static void
infbench_summary(int policy, const InferBenchScore *scores,
                 InferBenchMoments deltas[INFBENCH_MODES][INFBENCH_METRICS],
                 const LetterDistribution *ld, bool exchanged) {
  const char *summary_record = exchanged ? "exchanged_summary" : "summary";
  const char *tile_record = exchanged ? "exchanged_tile" : "tile";
  const char *delta_record = exchanged ? "exchanged_delta" : "delta";
  if (scores[0].observations == 0) {
    printf("INFBENCH %s policy=%s n=0\n", summary_record,
           infbench_policies[policy]);
    return;
  }
  for (int mode_idx = 0; mode_idx < INFBENCH_MODES; mode_idx++) {
    const InferBenchScore *score = &scores[mode_idx];
    const double count = score->observations;
    if (count == 0) {
      log_fatal("infbench collected no observations");
    }
    printf("INFBENCH %s policy=%s mode=%s n=%d raw_nll=%.9g mixed_nll=%.9f "
           "gain_nats=%.9f klv_mse=%.9f vowel_mse=%.9f score_mse=%.9f "
           "tile_mse=%.9f excluded_rate=%.6f empty_rate=%.6f seconds=%.6f\n",
           summary_record, infbench_policies[policy],
           infbench_modes[mode_idx].name, score->observations,
           score->raw_nll / count, score->metrics[0] / count,
           score->prior_gain / count, score->metrics[1] / count,
           score->metrics[2] / count, score->metrics[3] / count,
           score->metrics[4] / count, exchanged ? NAN : score->excluded / count,
           score->empty / count, score->seconds / count);
    for (int ml = 0; ml < ld_get_size(ld); ml++) {
      printf("INFBENCH %s policy=%s mode=%s tile=%s mse=%.9f\n", tile_record,
             infbench_policies[policy], infbench_modes[mode_idx].name,
             ld_ml_to_hl(ld, ml), score->tile_mse[ml] / count);
    }
    if (mode_idx == 0) {
      continue;
    }
    for (int metric_idx = exchanged ? 1 : 0; metric_idx < INFBENCH_METRICS;
         metric_idx++) {
      const InferBenchMoments *moment = &deltas[mode_idx][metric_idx];
      const double mean = moment->sum / moment->count;
      double half_width = NAN;
      if (moment->count > 1) {
        const double variance =
            fmax(0, (moment->squares - moment->count * mean * mean) /
                        (moment->count - 1));
        half_width = 1.96 * sqrt(variance / moment->count);
      }
      printf("INFBENCH %s policy=%s mode=%s control=%s metric=%s pairs=%d "
             "mean=%.9f normal95_half_width=%.9f\n",
             delta_record, infbench_policies[policy],
             infbench_modes[mode_idx].name,
             infbench_modes[infbench_modes[mode_idx].control].name,
             infbench_metric_names[metric_idx], moment->count, mean,
             half_width);
    }
  }
}

static void infbench_accumulate_pair(
    const InferBenchScore scores[INFBENCH_MODES],
    InferBenchScore totals[INFBENCH_MODES],
    InferBenchMoments deltas[INFBENCH_MODES][INFBENCH_METRICS]) {
  for (int mode_idx = 0; mode_idx < INFBENCH_MODES; mode_idx++) {
    infbench_add_score(&totals[mode_idx], &scores[mode_idx]);
    if (scores[mode_idx].observations == 0) {
      continue;
    }
    const InferBenchScore *control = &scores[infbench_modes[mode_idx].control];
    for (int metric_idx = 0; metric_idx < INFBENCH_METRICS; metric_idx++) {
      const double difference = (scores[mode_idx].metrics[metric_idx] -
                                 control->metrics[metric_idx]) /
                                scores[mode_idx].observations;
      InferBenchMoments *moment = &deltas[mode_idx][metric_idx];
      moment->sum += difference;
      moment->squares += difference * difference;
      moment->count++;
    }
  }
}

void infer_bench_run_spec(const char *spec) {
  int pairs = INFBENCH_DEFAULT_PAIRS;
  int positions = INFBENCH_DEFAULT_POSITIONS;
  unsigned int seed = INFBENCH_DEFAULT_SEED;
  bool exchanges_only = false;
  bool one_pat = false;
  bool calibrate = false;
  bool timing_only = false;
  bool use_census = true;
  if (spec != NULL) {
    StringSplitter *fields = split_string(spec, ':', true);
    const int num_fields = string_splitter_get_number_of_items(fields);
    if (num_fields < 3 || num_fields > 5) {
      string_splitter_destroy(fields);
      log_fatal("usage: "
                "infbench:PAIRS:POSITIONS_PER_GAME:SEED[:exchanges|onepat[:"
                "movegen]]");
    }
    ErrorStack *errors = error_stack_create();
    pairs = string_to_int(string_splitter_get_item(fields, 0), errors);
    positions = string_to_int(string_splitter_get_item(fields, 1), errors);
    const uint64_t parsed_seed =
        string_to_uint64(string_splitter_get_item(fields, 2), errors);
    if (num_fields >= 4) {
      timing_only =
          strings_equal(string_splitter_get_item(fields, 3), "margintiming");
      calibrate =
          timing_only ||
          (strings_equal(string_splitter_get_item(fields, 3), "calibrate") ||
           strings_equal(string_splitter_get_item(fields, 3), "calexchanges"));
      one_pat = strings_equal(string_splitter_get_item(fields, 3), "onepat");
      exchanges_only =
          (strings_equal(string_splitter_get_item(fields, 3), "exchanges") ||
           strings_equal(string_splitter_get_item(fields, 3), "calexchanges"));
    }
    if (num_fields == 5) {
      use_census =
          !strings_equal(string_splitter_get_item(fields, 4), "movegen");
    }
    const bool invalid =
        (num_fields >= 4 && !exchanges_only && !one_pat && !calibrate) ||
        (num_fields == 5 && (use_census || one_pat || calibrate)) ||
        !error_stack_is_empty(errors) || pairs < 1 || pairs > 10000 ||
        positions < 1 || positions > 20 || parsed_seed > UINT_MAX;
    error_stack_destroy(errors);
    string_splitter_destroy(fields);
    if (invalid) {
      log_fatal("infbench needs pairs 1..10000, positions 1..20, and an "
                "unsigned int seed");
    }
    seed = (unsigned int)parsed_seed;
  }
  printf("INFBENCH settings pairs=%d positions_per_game=%d seed=%u lex=CSW21 "
         "inference_threads=%d sim_threads=1 sim_plies=2 sim_candidates=15 "
         "sim_iterations=%d sim_minplay=10 sim_sampling=rr sim_pat=false "
         "prior_mix=%.6f feature_mse=raw_posterior empty_fallback=prior "
         "sampling=%s census=%d\n",
         pairs, positions, seed, INFBENCH_THREADS, INFBENCH_SIM_ITERS,
         INFBENCH_PRIOR_MIX,
         calibrate        ? "calibrate"
         : one_pat        ? "onepat"
         : exchanges_only ? "exchanges"
                          : "scheduled",
         use_census);
  for (int policy = (one_pat || calibrate) ? 2 : 0; policy < INFBENCH_POLICIES;
       policy++) {
    Config *config = infbench_config(policy, seed);
    InferBenchScore totals[INFBENCH_MODES] = {0};
    InferBenchScore exchanged_totals[INFBENCH_MODES] = {0};
    InferBenchMoments exchanged_deltas[INFBENCH_MODES][INFBENCH_METRICS] = {0};
    InferBenchMoments deltas[INFBENCH_MODES][INFBENCH_METRICS] = {0};
    for (int pair_idx = 0; pair_idx < pairs; pair_idx++) {
      InferBenchScore pair_scores[INFBENCH_MODES] = {0};
      InferBenchScore exchanged_pair_scores[INFBENCH_MODES] = {0};
      for (int game_idx = 0; game_idx < 2; game_idx++) {
        infbench_game(config, policy, pair_idx, game_idx, positions,
                      seed + (unsigned int)pair_idx, pair_scores,
                      exchanged_pair_scores, exchanges_only, use_census,
                      one_pat, calibrate, timing_only);
      }
      infbench_accumulate_pair(pair_scores, totals, deltas);
      infbench_accumulate_pair(exchanged_pair_scores, exchanged_totals,
                               exchanged_deltas);
    }
    infbench_summary(policy, totals, deltas, config_get_ld(config), false);
    infbench_summary(policy, exchanged_totals, exchanged_deltas,
                     config_get_ld(config), true);
    config_destroy(config);
  }
}

void test_infer_bench(void) { infer_bench_run_spec(NULL); }
