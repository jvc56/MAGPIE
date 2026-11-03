#include "infer_bench_test.h"

#include "../src/compat/ctime.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/alias_method.h"
#include "../src/ent/bag.h"
#include "../src/ent/encoded_rack.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/inference_args.h"
#include "../src/ent/inference_results.h"
#include "../src/ent/klv.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/pat.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/thread_control.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/inference.h"
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

static InferBenchScore infbench_score(const InferBenchPrediction *prediction,
                                      const InferBenchPrediction *prior,
                                      const Rack *truth, const KLV *klv,
                                      const LetterDistribution *ld) {
  InferBenchScore score = {.observations = 1};
  score.empty = prediction->mass == 0;
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
                             InferBenchScore totals[INFBENCH_MODES]) {
  const Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  const KLV *klv = player_get_klv(game_get_player(game, 0));
  const bool exchange = move_get_type(move) == GAME_EVENT_EXCHANGE;
  Rack played;
  rack_set_dist_size_and_reset(&played, ld_get_size(ld));
  infbench_played_tiles(move, &played);
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

static void infbench_game(Config *config, int policy, int pair_idx,
                          int game_idx, int positions, unsigned int seed,
                          InferBenchScore scores[INFBENCH_MODES]) {
  Game *game = config_get_game(config);
  game_reset(game);
  game_seed(game, seed);
  game_set_starting_player_index(game, game_idx);
  draw_starting_racks(game);
  MoveList *moves = move_list_create(1);
  int decisions = 0;
  int observations = 0;
  int turn = 0;
  // Sample at target decisions 4, 10, 16, ...; defer passes and bingos to
  // the next nontrivial leave. No filtering on inferred probability or regret.
  while (!game_over(game) && observations < positions &&
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
      if (decisions >= 4 + 6 * observations && leave_size > 0 &&
          bag_get_letters(game_get_bag(game)) > 0 &&
          (type == GAME_EVENT_TILE_PLACEMENT_MOVE ||
           type == GAME_EVENT_EXCHANGE)) {
        infbench_observe(config, &chosen, policy, pair_idx, game_idx, turn,
                         scores);
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
                 const LetterDistribution *ld) {
  for (int mode_idx = 0; mode_idx < INFBENCH_MODES; mode_idx++) {
    const InferBenchScore *score = &scores[mode_idx];
    const double count = score->observations;
    if (count == 0) {
      log_fatal("infbench collected no observations");
    }
    printf(
        "INFBENCH summary policy=%s mode=%s n=%d raw_nll=%.9g mixed_nll=%.9f "
        "gain_nats=%.9f klv_mse=%.9f vowel_mse=%.9f score_mse=%.9f "
        "tile_mse=%.9f excluded_rate=%.6f empty_rate=%.6f seconds=%.6f\n",
        infbench_policies[policy], infbench_modes[mode_idx].name,
        score->observations, score->raw_nll / count, score->metrics[0] / count,
        score->prior_gain / count, score->metrics[1] / count,
        score->metrics[2] / count, score->metrics[3] / count,
        score->metrics[4] / count, score->excluded / count,
        score->empty / count, score->seconds / count);
    for (int ml = 0; ml < ld_get_size(ld); ml++) {
      printf("INFBENCH tile policy=%s mode=%s tile=%s mse=%.9f\n",
             infbench_policies[policy], infbench_modes[mode_idx].name,
             ld_ml_to_hl(ld, ml), score->tile_mse[ml] / count);
    }
    if (mode_idx == 0) {
      continue;
    }
    for (int metric_idx = 0; metric_idx < INFBENCH_METRICS; metric_idx++) {
      const InferBenchMoments *moment = &deltas[mode_idx][metric_idx];
      const double mean = moment->sum / moment->count;
      double half_width = NAN;
      if (moment->count > 1) {
        const double variance =
            fmax(0, (moment->squares - moment->count * mean * mean) /
                        (moment->count - 1));
        half_width = 1.96 * sqrt(variance / moment->count);
      }
      printf("INFBENCH delta policy=%s mode=%s control=%s metric=%s pairs=%d "
             "mean=%.9f normal95_half_width=%.9f\n",
             infbench_policies[policy], infbench_modes[mode_idx].name,
             infbench_modes[infbench_modes[mode_idx].control].name,
             infbench_metric_names[metric_idx], moment->count, mean,
             half_width);
    }
  }
}

void infer_bench_run_spec(const char *spec) {
  int pairs = INFBENCH_DEFAULT_PAIRS;
  int positions = INFBENCH_DEFAULT_POSITIONS;
  unsigned int seed = INFBENCH_DEFAULT_SEED;
  if (spec != NULL) {
    StringSplitter *fields = split_string(spec, ':', true);
    if (string_splitter_get_number_of_items(fields) != 3) {
      string_splitter_destroy(fields);
      log_fatal("usage: infbench:PAIRS:POSITIONS_PER_GAME:SEED");
    }
    ErrorStack *errors = error_stack_create();
    pairs = string_to_int(string_splitter_get_item(fields, 0), errors);
    positions = string_to_int(string_splitter_get_item(fields, 1), errors);
    const uint64_t parsed_seed =
        string_to_uint64(string_splitter_get_item(fields, 2), errors);
    const bool invalid = !error_stack_is_empty(errors) || pairs < 1 ||
                         pairs > 10000 || positions < 1 || positions > 20 ||
                         parsed_seed > UINT_MAX;
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
         "prior_mix=%.6f feature_mse=raw_posterior empty_fallback=prior\n",
         pairs, positions, seed, INFBENCH_THREADS, INFBENCH_SIM_ITERS,
         INFBENCH_PRIOR_MIX);
  for (int policy = 0; policy < INFBENCH_POLICIES; policy++) {
    Config *config = infbench_config(policy, seed);
    InferBenchScore totals[INFBENCH_MODES] = {0};
    InferBenchMoments deltas[INFBENCH_MODES][INFBENCH_METRICS] = {0};
    for (int pair_idx = 0; pair_idx < pairs; pair_idx++) {
      InferBenchScore pair_scores[INFBENCH_MODES] = {0};
      for (int game_idx = 0; game_idx < 2; game_idx++) {
        infbench_game(config, policy, pair_idx, game_idx, positions,
                      seed + (unsigned int)pair_idx, pair_scores);
      }
      for (int mode_idx = 0; mode_idx < INFBENCH_MODES; mode_idx++) {
        infbench_add_score(&totals[mode_idx], &pair_scores[mode_idx]);
        if (pair_scores[mode_idx].observations == 0) {
          continue;
        }
        const InferBenchScore *control =
            &pair_scores[infbench_modes[mode_idx].control];
        for (int metric_idx = 0; metric_idx < INFBENCH_METRICS; metric_idx++) {
          const double difference = (pair_scores[mode_idx].metrics[metric_idx] -
                                     control->metrics[metric_idx]) /
                                    pair_scores[mode_idx].observations;
          InferBenchMoments *moment = &deltas[mode_idx][metric_idx];
          moment->sum += difference;
          moment->squares += difference * difference;
          moment->count++;
        }
      }
    }
    infbench_summary(policy, totals, deltas, config_get_ld(config));
    config_destroy(config);
  }
}

void test_infer_bench(void) { infer_bench_run_spec(NULL); }
