#include "infer_score_test.h"

#include "../src/def/equity_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/inference_results.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/pat.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/stats.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/validated_move.h"
#include "../src/ent/win_pct.h"
#include "../src/ent/xoshiro.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/play_chooser.h"
#include "../src/str/move_string.h"
#include "../src/str/rack_string.h"
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
#include <time.h>

// Research harness: how well does an opponent model infer the rack a mover
// kept? Two modes, chosen by INFS_MODE.
//
// gen: plays seeded games and writes a corpus of observed tile placements,
// one CSV row each: the position before the move (CGP, the mover on turn
// and listed first), the move, and the mover's true rack. Only positions
// with at least RACK_SIZE tiles in the bag and not the first move are kept,
// and a game stops once the bag drops below that. The mover is
//   static   both players static equity, no PAT (both players' moves kept)
//   pat      both players static equity with PAT (both kept)
//   pc       player one is PlayChooser (PAT candidates, 4-ply sims, a fixed
//            per-move budget, tws,windows rollouts), player two static PAT;
//            only PlayChooser's moves are kept.
//
// score: for every corpus row, the observer (the other player) knows its own
// rack and sees the move, so the candidate leaves are every multiset of the
// kept size drawn from the tiles unseen to it (bag plus the mover's rack)
// less the played tiles. The prior weight of a leave is its number of draws.
// For each evaluator (the opponent model) and each candidate leave, the
// mover's rack is set to played tiles plus that leave and every move is
// generated with that evaluator's equity; from the list come the played
// move's equity E_p, the best equity E_top and, per softmax temperature tau,
// log sum exp(E / tau). Likelihoods:
//   hard margin m   1 if E_p >= E_top - m, else 0 (current static inference
//                   uses m = 5 with the KLV and no PAT)
//   softmax tau     exp(E_p / tau) / sum over moves exp(E / tau)
// The posterior over leaves is prior times likelihood. Scored: log P(true
// leave) (the same as log P(true rack), since the played tiles are known),
// its rank, and the posterior entropy. A hard margin can give the true leave
// probability 0, so every reported log score is of the posterior mixed with
// the prior, (1 - INFS_EPS) * P + INFS_EPS * prior, with raw zeros counted.
//
// When the kept size gives more distinct leaves than INFS_MAX_LEAVES, the
// normalizer is estimated from INFS_SAMPLES leaves drawn from the prior
// (with the same leaves for every evaluator and method): P(true) = prior *
// lik(true) / mean(lik over samples). Rank and entropy are not reported then,
// and a sample mean of 0 is replaced by 1 / (2 * samples).
//
// Evaluators (INFS_EVALUATORS, comma separated): klv (static, no PAT), pat
// (PAT, every class), pattw (PAT, tws,windows).
//
// Environment:
//   INFS_MODE         gen | score
//   INFS_CORPUS       corpus CSV (gen writes, score reads)
//   INFS_OUT          score output CSV (appended; observations in it skipped)
//   INFS_GAMES        gen: games to play (default 20)
//   INFS_FIRST_GAME   gen: first game index (default 0); observation ids
//                     are game * 100 + turn
//   INFS_APPEND       gen: 1 appends to the corpus instead of replacing it
//   INFS_SEED         gen: base seed (default 20260927)
//   INFS_MOVER        gen: static | pat | pc (default pat)
//   INFS_BUDGET       gen: PlayChooser seconds per move (default 13)
//   INFS_WORKER, INFS_NUM_WORKERS  score: handle observations w, w + n, ...
//   INFS_MAX_OBS      score: stop after this observation index
//   INFS_DEADLINE     score: Unix time; no observation starts at or after it
//   INFS_EVALUATORS   score: default klv,pat,pattw
//   INFS_MARGINS      score: hard margins in points, default 0,2,5,10
//   INFS_TAUS         score: softmax temperatures in points,
//                     default 0.5,1,2,4,6,10,15,25,40
//   INFS_MAX_LEAVES   score: exact enumeration cap (default 4000)
//   INFS_SAMPLES      score: prior samples above the cap (default 400)
//   INFS_EPS          score: prior mixing weight (default 0.01)
//   INFS_VALIDATE     score: 1 also runs infer() for klv at margin 5 on
//                     exactly enumerated rows and compares accepted draws
//   INFS_DEBUG        score: prints every leave's played and best equity

enum {
  INFS_MAX_EVALUATORS = 3,
  INFS_MAX_METHODS = 16,
  INFS_MOVE_LIST_CAP = 20000,
  INFS_LINE_CAP = 1 << 14,
  INFS_FIELD_CAP = 8,
  INFS_NAME_CAP = 16,
  INFS_MAX_GEN_TURNS = 60,
  INFS_PC_CANDIDATES = 15,
  INFS_PC_PLIES = 4,
  INFS_VALIDATE_MARGIN = 5,
};

static const char *const infs_evaluator_set_cmds[INFS_MAX_EVALUATORS] = {
    "set -lex CSW24 -wmp true -pat none -s1 equity -s2 equity -r1 all -r2 all "
    "-numplays 1 -threads 1 -ima 5",
    "set -lex CSW24 -wmp true -pat CSW24 -patclasses all -s1 equity -s2 "
    "equity -r1 all -r2 all -numplays 1 -threads 1 -ima 5",
    "set -lex CSW24 -wmp true -pat CSW24 -patclasses tws,windows -s1 equity "
    "-s2 equity -r1 all -r2 all -numplays 1 -threads 1 -ima 5",
};
static const char *const infs_evaluator_names[INFS_MAX_EVALUATORS] = {
    "klv", "pat", "pattw"};

static long infs_env_long(const char *name, long default_value) {
  const char *value = getenv(name);
  return value ? strtol(value, NULL, 10) : default_value;
}

static double infs_env_double(const char *name, double default_value) {
  const char *value = getenv(name);
  return value ? strtod(value, NULL) : default_value;
}

// Parses a comma-separated list of numbers; defaults when unset.
static int infs_env_doubles(const char *name, const char *default_value,
                            double *values, int cap) {
  const char *value = getenv(name);
  if (!value) {
    value = default_value;
  }
  int count = 0;
  const char *cursor = value;
  while (*cursor != '\0' && count < cap) {
    char *end = NULL;
    const double parsed = strtod(cursor, &end);
    if (end == cursor) {
      log_fatal("invalid %s: %s", name, value);
    }
    values[count++] = parsed;
    cursor = (*end == ',') ? end + 1 : end;
  }
  return count;
}

static uint64_t infs_mix(uint64_t x) {
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31);
}

// ---------------------------------------------------------------- gen mode

static void infs_write_row(FILE *out, long obs_index, uint64_t game_seed,
                           int turn, const Game *game, const Move *move,
                           const char *mover_type) {
  const Player *mover =
      game_get_player(game, game_get_player_on_turn_index(game));
  char *cgp = game_get_cgp(game, true);
  StringBuilder *sb = string_builder_create();
  string_builder_add_ucgi_move(sb, move, game_get_board(game),
                               game_get_ld(game));
  string_builder_add_string(sb, ",");
  string_builder_add_rack(sb, player_get_rack(mover), game_get_ld(game), false);
  fprintf(out, "%ld,%llu,%d,%d,%s,%s,%s\n", obs_index,
          (unsigned long long)game_seed, turn,
          bag_get_letters(game_get_bag(game)), mover_type, cgp,
          string_builder_peek(sb));
  (void)fflush(out);
  string_builder_destroy(sb);
  free(cgp);
}

static void infs_gen(const char *corpus_path) {
  const long num_games = infs_env_long("INFS_GAMES", 20);
  // Games first_game .. first_game + num_games - 1; observation ids are
  // game * 100 + turn, so corpora from different game ranges can be joined.
  const long first_game = infs_env_long("INFS_FIRST_GAME", 0);
  const uint64_t base_seed = (uint64_t)infs_env_long("INFS_SEED", 20260927);
  const char *mover_type = getenv("INFS_MOVER");
  if (!mover_type) {
    mover_type = "pat";
  }
  const bool use_pc = strings_equal(mover_type, "pc");
  const bool use_pat = !strings_equal(mover_type, "static");
  const bool append = infs_env_long("INFS_APPEND", 0) != 0;
  const double budget = infs_env_double("INFS_BUDGET", 13.0);
  Config *config = config_create_or_die(
      use_pat ? "set -lex CSW24 -wmp true -pat CSW24 -s1 equity -s2 equity "
                "-r1 best -r2 best -numplays 1 -threads 1"
              : "set -lex CSW24 -wmp true -pat none -s1 equity -s2 equity "
                "-r1 best -r2 best -numplays 1 -threads 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  ErrorStack *error_stack = error_stack_create();
  config_load_win_pcts(config, error_stack);
  assert(error_stack_is_empty(error_stack));
  Game *game = config_get_game(config);
  MoveList *move_list = move_list_create(1);
  Move *pc_move = move_create();
  FILE *out = fopen(corpus_path, append ? "a" : "w");
  assert(out);
  if (ftell(out) == 0) {
    fprintf(out, "obs,game_seed,turn,bag,mover,cgp,move,true_rack\n");
  }
  long num_written = 0;
  for (long game_idx = first_game; game_idx < first_game + num_games;
       game_idx++) {
    const uint64_t seed = infs_mix(base_seed + (uint64_t)game_idx);
    game_reset(game);
    game_seed(game, seed);
    game_set_starting_player_index(game, (int)(game_idx & 1));
    draw_starting_racks(game);
    for (int turn = 0; turn < INFS_MAX_GEN_TURNS && !game_over(game); turn++) {
      if (bag_get_letters(game_get_bag(game)) < RACK_SIZE) {
        break;
      }
      const int on_turn = game_get_player_on_turn_index(game);
      const Move *move = NULL;
      const bool record = !use_pc || on_turn == 0;
      if (use_pc && on_turn == 0) {
        PlayChooserStrategy strategy = {
            .pre_endgame_eval = PLAY_CHOOSER_EVAL_SIM,
            .endgame_eval = PLAY_CHOOSER_EVAL_ENDGAME,
            .sim_plies = INFS_PC_PLIES,
            .sim_max_candidates = INFS_PC_CANDIDATES,
            .fixed_seconds_per_move = budget,
            .win_pcts = config_get_win_pcts(config),
            .num_threads = 1,
            .utility_w_winpct = 1.0,
            .utility_w_spread = 0.5,
            .utility_spread_scale = 100.0,
            .pat_rollout_disabled = false,
            .pat_rollout_disabled_classes_mask =
                PAT_CLASS_MASK_ALL & ~PAT_CLASS_MASK_ROLLOUT_DEFAULT,
            .seed = infs_mix(seed + (uint64_t)turn),
        };
        PlayChooser *play_chooser = play_chooser_create(&strategy);
        Game *game_copy = game_duplicate(game);
        play_chooser_choose_move(play_chooser, game_copy, pc_move, error_stack);
        assert(error_stack_is_empty(error_stack));
        game_destroy(game_copy);
        play_chooser_destroy(play_chooser);
        move = pc_move;
      } else {
        move = get_top_equity_move(game, move_list);
      }
      if (record && turn > 0 &&
          move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
        infs_write_row(out, game_idx * 100 + turn, seed, turn, game, move,
                       mover_type);
        num_written++;
      }
      play_move(move, game, NULL);
    }
  }
  (void)fclose(out);
  printf("wrote %ld observations to %s\n", num_written, corpus_path);
  move_destroy(pc_move);
  move_list_destroy(move_list);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// -------------------------------------------------------------- score mode

typedef struct InfsEvaluator {
  Config *config;
  Game *game;
  const char *name;
} InfsEvaluator;

typedef struct InfsSetup {
  int num_margins;
  double margins[INFS_MAX_METHODS];
  int num_taus;
  double taus[INFS_MAX_METHODS];
  int num_methods;
} InfsSetup;

// Splits a CSV line into at most INFS_FIELD_CAP fields in place.
static int infs_split(char *line, char **fields) {
  int count = 0;
  char *cursor = line;
  while (count < INFS_FIELD_CAP) {
    fields[count++] = cursor;
    char *comma = strchr(cursor, ',');
    if (!comma) {
      break;
    }
    *comma = '\0';
    cursor = comma + 1;
  }
  char *last = fields[count - 1];
  last[strcspn(last, "\r\n")] = '\0';
  return count;
}

static double infs_log_choose(int n, int k) {
  return lgamma(n + 1.0) - lgamma(k + 1.0) - lgamma(n - k + 1.0);
}

// log of the number of draws of leave from pool.
static double infs_log_draws(const int *pool, const int *leave, int ld_size) {
  double total = 0.0;
  for (int ml = 0; ml < ld_size; ml++) {
    if (leave[ml] > 0) {
      total += infs_log_choose(pool[ml], leave[ml]);
    }
  }
  return total;
}

// Number of distinct multisets of size k from pool (capped at cap + 1).
static long infs_count_leaves(const int *pool, int ld_size, int ml, int k,
                              long cap) {
  if (k == 0) {
    return 1;
  }
  if (ml >= ld_size) {
    return 0;
  }
  long total = 0;
  for (int take = 0; take <= pool[ml] && take <= k; take++) {
    total += infs_count_leaves(pool, ld_size, ml + 1, k - take, cap);
    if (total > cap) {
      return cap + 1;
    }
  }
  return total;
}

typedef struct InfsLeaves {
  int count;
  int capacity;
  int ld_size;
  int *counts; // [count][ld_size]
  double *log_prior;
} InfsLeaves;

static void infs_leaves_push(InfsLeaves *leaves, const int *leave,
                             double log_prior) {
  if (leaves->count == leaves->capacity) {
    leaves->capacity = leaves->capacity ? 2 * leaves->capacity : 256;
    leaves->counts =
        realloc_or_die(leaves->counts, sizeof(int) * (size_t)leaves->capacity *
                                           (size_t)leaves->ld_size);
    leaves->log_prior = realloc_or_die(
        leaves->log_prior, sizeof(double) * (size_t)leaves->capacity);
  }
  memcpy(leaves->counts + (size_t)leaves->count * leaves->ld_size, leave,
         sizeof(int) * (size_t)leaves->ld_size);
  leaves->log_prior[leaves->count] = log_prior;
  leaves->count++;
}

static void infs_enumerate(const int *pool, int ld_size, int ml, int k,
                           int *leave, InfsLeaves *leaves) {
  if (k == 0) {
    infs_leaves_push(leaves, leave, infs_log_draws(pool, leave, ld_size));
    return;
  }
  if (ml >= ld_size) {
    return;
  }
  for (int take = 0; take <= pool[ml] && take <= k; take++) {
    leave[ml] = take;
    infs_enumerate(pool, ld_size, ml + 1, k - take, leave, leaves);
  }
  leave[ml] = 0;
}

// Evaluates one candidate rack with one evaluator: whether the played move
// is within each margin of the best, and its softmax log likelihood per tau.
// Returns false if the played move is missing from the list.
static bool infs_evaluate(InfsEvaluator *evaluator, MoveList *move_list,
                          const Move *played, const int *played_tiles,
                          const int *leave, int ld_size, const InfsSetup *setup,
                          double *log_lik) {
  Game *game = evaluator->game;
  Rack *rack = player_get_rack(game_get_player(game, 0));
  rack_reset(rack);
  for (int ml = 0; ml < ld_size; ml++) {
    for (int count = 0; count < played_tiles[ml] + leave[ml]; count++) {
      rack_add_letter(rack, (MachineLetter)ml);
    }
  }
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
  const int num_moves = move_list_get_count(move_list);
  double top = -INFINITY;
  double played_equity = -INFINITY;
  for (int move_idx = 0; move_idx < num_moves; move_idx++) {
    const Move *move = move_list_get_move(move_list, move_idx);
    // Passes carry a sentinel equity; they are never the observed move here
    // (only tile placements are scored) and are left out of the softmax.
    if (move_get_equity(move) == EQUITY_PASS_VALUE) {
      continue;
    }
    const double equity = equity_to_double(move_get_equity(move));
    if (equity > top) {
      top = equity;
    }
    if (played_equity == -INFINITY &&
        compare_moves_without_equity(move, played, true) == -1) {
      played_equity = equity;
    }
  }
  const bool found = played_equity != -INFINITY;
  if (getenv("INFS_DEBUG")) {
    const Move *best = get_top_equity_move_for_inferences(
        game, move_list, EQUITY_MAX_VALUE, UNSET_LEAVE_SIZE, 0);
    printf("  leave:");
    for (int ml = 0; ml < ld_size; ml++) {
      for (int count = 0; count < leave[ml]; count++) {
        printf("%d.", ml);
      }
    }
    printf(" E_p %.3f top %.3f | infer-style top %.3f score %.3f\n",
           played_equity, top, equity_to_double(move_get_equity(best)),
           equity_to_double(move_get_score(played)));
    generate_moves(&args);
  }
  int method = 0;
  for (int m_idx = 0; m_idx < setup->num_margins; m_idx++) {
    log_lik[method++] = (found && played_equity >= top - setup->margins[m_idx])
                            ? 0.0
                            : -INFINITY;
  }
  for (int t_idx = 0; t_idx < setup->num_taus; t_idx++) {
    const double tau = setup->taus[t_idx];
    double sum = 0.0;
    for (int move_idx = 0; move_idx < num_moves; move_idx++) {
      const Equity raw_equity =
          move_get_equity(move_list_get_move(move_list, move_idx));
      if (raw_equity == EQUITY_PASS_VALUE) {
        continue;
      }
      sum += exp((equity_to_double(raw_equity) - top) / tau);
    }
    log_lik[method++] =
        found ? (played_equity - top) / tau - log(sum) : -INFINITY;
  }
  return found;
}

static double infs_logsumexp_add(double a, double b) {
  if (a == -INFINITY) {
    return b;
  }
  if (b == -INFINITY) {
    return a;
  }
  const double hi = a > b ? a : b;
  return hi + log(exp(a - hi) + exp(b - hi));
}

static void infs_method_name(const InfsSetup *setup, int method, char *name) {
  if (method < setup->num_margins) {
    (void)snprintf(name, INFS_NAME_CAP, "m%g", setup->margins[method]);
  } else {
    (void)snprintf(name, INFS_NAME_CAP, "t%g",
                   setup->taus[method - setup->num_margins]);
  }
}

static bool *infs_load_done(const char *path, long max_obs) {
  bool *done = calloc_or_die((size_t)max_obs, sizeof(bool));
  FILE *file = fopen(path, "r");
  if (!file) {
    return done;
  }
  char *line = malloc_or_die(INFS_LINE_CAP);
  while (fgets(line, INFS_LINE_CAP, file)) {
    char *end = NULL;
    const long index = strtol(line, &end, 10);
    if (end != line && index >= 0 && index < max_obs) {
      done[index] = true;
    }
  }
  free(line);
  (void)fclose(file);
  return done;
}

// Runs the KLV2 static inference on the evaluator's game (the position before
// the move, the mover on turn as player 0) and returns its accepted draws.
static double infs_infer_accepted_draws(InfsEvaluator *evaluator,
                                        const Move *played,
                                        const int *played_tiles, int ld_size,
                                        const Rack *observer_rack) {
  Rack target_played_tiles;
  rack_set_dist_size_and_reset(&target_played_tiles, ld_size);
  for (int ml = 0; ml < ld_size; ml++) {
    for (int count = 0; count < played_tiles[ml]; count++) {
      rack_add_letter(&target_played_tiles, (MachineLetter)ml);
    }
  }
  Rack target_known_rack;
  rack_set_dist_size_and_reset(&target_known_rack, ld_size);
  Rack nontarget_known_rack;
  rack_copy(&nontarget_known_rack, observer_rack);
  InferenceResults *results = inference_results_create(NULL);
  ErrorStack *error_stack = error_stack_create();
  thread_control_set_status(config_get_thread_control(evaluator->config),
                            THREAD_CONTROL_STATUS_STARTED);
  config_infer(evaluator->config, false, 0, move_get_score(played), 0,
               &target_played_tiles, &target_known_rack, &nontarget_known_rack,
               true, results, error_stack);
  double accepted = -1.0;
  if (getenv("INFS_DEBUG") && error_stack_is_empty(error_stack)) {
    for (int ml = 0; ml < ld_size; ml++) {
      const uint64_t draws = inference_results_get_subtotal(
          results, INFERENCE_TYPE_LEAVE, (MachineLetter)ml, 1,
          INFERENCE_SUBTOTAL_DRAW);
      if (draws > 0) {
        printf("  infer accepted letter %d draws %llu\n", ml,
               (unsigned long long)draws);
      }
    }
    printf("  infer margin %.3f\n",
           equity_to_double(inference_results_get_equity_margin(results)));
  }
  if (error_stack_is_empty(error_stack)) {
    accepted = (double)stat_get_num_samples(
        inference_results_get_equity_values(results, INFERENCE_TYPE_LEAVE));
  } else {
    error_stack_print_and_reset(error_stack);
  }
  error_stack_destroy(error_stack);
  inference_results_destroy(results);
  return accepted;
}

static void infs_score(const char *corpus_path) {
  const char *out_path = getenv("INFS_OUT");
  if (!out_path) {
    log_fatal("set INFS_OUT");
  }
  const long worker = infs_env_long("INFS_WORKER", 0);
  const long num_workers = infs_env_long("INFS_NUM_WORKERS", 1);
  const long max_obs = infs_env_long("INFS_MAX_OBS", 1000000);
  const long deadline = infs_env_long("INFS_DEADLINE", 0);
  const long max_leaves = infs_env_long("INFS_MAX_LEAVES", 4000);
  const long num_samples = infs_env_long("INFS_SAMPLES", 400);
  const double eps = infs_env_double("INFS_EPS", 0.01);
  const bool validate = infs_env_long("INFS_VALIDATE", 0) != 0;
  InfsSetup setup;
  setup.num_margins =
      infs_env_doubles("INFS_MARGINS", "0,2,5,10", setup.margins, 8);
  setup.num_taus =
      infs_env_doubles("INFS_TAUS", "0.5,1,2,4,6,10,15,25,40", setup.taus,
                       INFS_MAX_METHODS - setup.num_margins);
  setup.num_methods = setup.num_margins + setup.num_taus;

  const char *evaluator_list = getenv("INFS_EVALUATORS");
  if (!evaluator_list) {
    evaluator_list = "klv,pat,pattw";
  }
  InfsEvaluator evaluators[INFS_MAX_EVALUATORS];
  int num_evaluators = 0;
  for (int e_idx = 0; e_idx < INFS_MAX_EVALUATORS; e_idx++) {
    if (!strstr(evaluator_list, infs_evaluator_names[e_idx])) {
      continue;
    }
    // "pat" is a prefix of "pattw": require a whole-word match.
    const char *hit = evaluator_list;
    bool matched = false;
    const size_t name_length = strlen(infs_evaluator_names[e_idx]);
    while ((hit = strstr(hit, infs_evaluator_names[e_idx])) != NULL) {
      const bool starts = hit == evaluator_list || hit[-1] == ',';
      const bool ends = hit[name_length] == '\0' || hit[name_length] == ',';
      if (starts && ends) {
        matched = true;
        break;
      }
      hit += name_length;
    }
    if (!matched) {
      continue;
    }
    evaluators[num_evaluators].config =
        config_create_or_die(infs_evaluator_set_cmds[e_idx]);
    evaluators[num_evaluators].name = infs_evaluator_names[e_idx];
    num_evaluators++;
  }
  assert(num_evaluators > 0);

  bool *done = infs_load_done(out_path, max_obs);
  FILE *out = fopen(out_path, "a");
  assert(out);
  if (ftell(out) == 0) {
    fprintf(out, "obs,turn,bag,leave_size,n_leaves,mode,evaluator,method,"
                 "log_prior,logp_raw,logp,zero,rank,entropy,missing,"
                 "seconds\n");
    (void)fflush(out);
  }
  FILE *corpus = fopen(corpus_path, "r");
  assert(corpus);
  char *line = malloc_or_die(INFS_LINE_CAP);
  MoveList *move_list = move_list_create(INFS_MOVE_LIST_CAP);
  Move *played = move_create();
  ErrorStack *error_stack = error_stack_create();
  const LetterDistribution *ld = config_get_ld(evaluators[0].config);
  const int ld_size = ld_get_size(ld);
  int *pool = malloc_or_die(sizeof(int) * (size_t)ld_size);
  int *played_tiles = malloc_or_die(sizeof(int) * (size_t)ld_size);
  int *true_leave = malloc_or_die(sizeof(int) * (size_t)ld_size);
  int *scratch = malloc_or_die(sizeof(int) * (size_t)ld_size);
  XoshiroPRNG *prng = prng_create(1);
  long validate_rows = 0;
  long validate_mismatches = 0;

  while (fgets(line, INFS_LINE_CAP, corpus)) {
    char *fields[INFS_FIELD_CAP];
    if (infs_split(line, fields) < 8) {
      continue;
    }
    char *end = NULL;
    const long obs = strtol(fields[0], &end, 10);
    if (end == fields[0] || obs < 0 || obs >= max_obs ||
        obs % num_workers != worker || done[obs]) {
      continue;
    }
    if (deadline > 0 && (long)time(NULL) >= deadline) {
      break;
    }
    const time_t start = time(NULL);
    const int turn = (int)strtol(fields[2], NULL, 10);
    const int bag = (int)strtol(fields[3], NULL, 10);
    char *cgp_cmd = get_formatted_string("cgp %s", fields[5]);
    for (int e_idx = 0; e_idx < num_evaluators; e_idx++) {
      load_and_exec_config_or_die(evaluators[e_idx].config, cgp_cmd);
      evaluators[e_idx].game = config_get_game(evaluators[e_idx].config);
    }
    free(cgp_cmd);
    Game *game = evaluators[0].game;
    ValidatedMoves *vms =
        validated_moves_create(game, 0, fields[6], true, true, error_stack);
    if (!error_stack_is_empty(error_stack) ||
        validated_moves_get_number_of_moves(vms) != 1) {
      error_stack_print_and_reset(error_stack);
      validated_moves_destroy(vms);
      fprintf(out, "%ld,skip_move\n", obs);
      (void)fflush(out);
      continue;
    }
    move_copy(played, validated_moves_get_move(vms, 0));
    validated_moves_destroy(vms);

    const Rack *mover_rack = player_get_rack(game_get_player(game, 0));
    const Rack *observer_rack = player_get_rack(game_get_player(game, 1));
    memset(played_tiles, 0, sizeof(int) * (size_t)ld_size);
    for (int tile_idx = 0; tile_idx < move_get_tiles_length(played);
         tile_idx++) {
      const MachineLetter ml = move_get_tile(played, tile_idx);
      if (ml == PLAYED_THROUGH_MARKER) {
        continue;
      }
      played_tiles[get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml]++;
    }
    int bag_counts[MAX_ALPHABET_SIZE];
    memset(bag_counts, 0, sizeof(bag_counts));
    bag_increment_unseen_count(game_get_bag(game), bag_counts);
    int leave_size = 0;
    bool consistent = true;
    for (int ml = 0; ml < ld_size; ml++) {
      true_leave[ml] = rack_get_letter(mover_rack, ml) - played_tiles[ml];
      pool[ml] =
          bag_counts[ml] + rack_get_letter(mover_rack, ml) - played_tiles[ml];
      if (true_leave[ml] < 0) {
        consistent = false;
      }
      leave_size += true_leave[ml] > 0 ? true_leave[ml] : 0;
    }
    if (!consistent) {
      fprintf(out, "%ld,skip_rack\n", obs);
      (void)fflush(out);
      continue;
    }
    int pool_total = 0;
    for (int ml = 0; ml < ld_size; ml++) {
      pool_total += pool[ml];
    }
    const double log_total_draws = infs_log_choose(pool_total, leave_size);
    const double true_log_prior =
        infs_log_draws(pool, true_leave, ld_size) - log_total_draws;

    // The candidate leaves: exact, or prior samples (plus the true leave).
    const long distinct =
        infs_count_leaves(pool, ld_size, 0, leave_size, max_leaves);
    const bool exact = distinct <= max_leaves;
    InfsLeaves leaves = {.ld_size = ld_size};
    if (exact) {
      memset(scratch, 0, sizeof(int) * (size_t)ld_size);
      infs_enumerate(pool, ld_size, 0, leave_size, scratch, &leaves);
    } else {
      prng_seed(prng, infs_mix((uint64_t)obs));
      MachineLetter *tiles =
          malloc_or_die(sizeof(MachineLetter) * (size_t)pool_total);
      int tile_count = 0;
      for (int ml = 0; ml < ld_size; ml++) {
        for (int count = 0; count < pool[ml]; count++) {
          tiles[tile_count++] = (MachineLetter)ml;
        }
      }
      for (long sample = 0; sample < num_samples; sample++) {
        memset(scratch, 0, sizeof(int) * (size_t)ld_size);
        for (int draw = 0; draw < leave_size; draw++) {
          const int pick = draw + (int)prng_get_random_number(
                                      prng, (uint64_t)(tile_count - draw));
          const MachineLetter swap = tiles[draw];
          tiles[draw] = tiles[pick];
          tiles[pick] = swap;
          scratch[tiles[draw]]++;
        }
        infs_leaves_push(&leaves, scratch, 0.0);
      }
      free(tiles);
    }
    // Index of the true leave among exact leaves.
    int true_index = -1;
    if (exact) {
      for (int leave_idx = 0; leave_idx < leaves.count; leave_idx++) {
        if (memcmp(leaves.counts + (size_t)leave_idx * ld_size, true_leave,
                   sizeof(int) * (size_t)ld_size) == 0) {
          true_index = leave_idx;
          break;
        }
      }
      assert(true_index >= 0);
    }

    const int num_methods = setup.num_methods;
    double *log_lik = malloc_or_die(sizeof(double) * (size_t)leaves.count *
                                    (size_t)num_methods);
    double true_log_lik[INFS_MAX_METHODS];
    double klv_accepted_draws = 0.0;
    for (int e_idx = 0; e_idx < num_evaluators; e_idx++) {
      InfsEvaluator *evaluator = &evaluators[e_idx];
      int missing = 0;
      if (!infs_evaluate(evaluator, move_list, played, played_tiles, true_leave,
                         ld_size, &setup, true_log_lik)) {
        missing++;
      }
      for (int leave_idx = 0; leave_idx < leaves.count; leave_idx++) {
        if (!infs_evaluate(evaluator, move_list, played, played_tiles,
                           leaves.counts + (size_t)leave_idx * ld_size, ld_size,
                           &setup, log_lik + (size_t)leave_idx * num_methods)) {
          missing++;
        }
      }
      for (int method = 0; method < num_methods; method++) {
        double log_norm = -INFINITY;
        for (int leave_idx = 0; leave_idx < leaves.count; leave_idx++) {
          const double weight =
              (exact ? leaves.log_prior[leave_idx] - log_total_draws : 0.0) +
              log_lik[(size_t)leave_idx * num_methods + method];
          log_norm = infs_logsumexp_add(log_norm, weight);
        }
        double logp_raw = 0.0;
        double rank = -1.0;
        double entropy = -1.0;
        if (exact) {
          logp_raw = true_log_prior + true_log_lik[method] - log_norm;
          // Rank: leaves with strictly larger posterior, plus one; entropy.
          const double true_weight = true_log_prior + true_log_lik[method];
          long above = 0;
          entropy = 0.0;
          for (int leave_idx = 0; leave_idx < leaves.count; leave_idx++) {
            const double weight =
                leaves.log_prior[leave_idx] - log_total_draws +
                log_lik[(size_t)leave_idx * num_methods + method];
            if (weight > true_weight + 1e-12) {
              above++;
            }
            if (weight != -INFINITY && log_norm != -INFINITY) {
              const double p = exp(weight - log_norm);
              entropy -= p * log(p);
            }
          }
          rank = (double)(above + 1);
        } else {
          // mean likelihood over prior samples
          double log_mean = log_norm - log((double)leaves.count);
          if (log_mean == -INFINITY) {
            log_mean = -log(2.0 * (double)leaves.count);
          }
          logp_raw = true_log_prior + true_log_lik[method] - log_mean;
        }
        // No candidate leave explains the move under this model: the
        // posterior is undefined, and counts as giving the true leave zero.
        if (log_norm == -INFINITY || isnan(logp_raw)) {
          logp_raw = -INFINITY;
        }
        if (logp_raw > 0.0) {
          logp_raw = 0.0;
        }
        const bool zero = logp_raw == -INFINITY;
        const double p = zero ? 0.0 : exp(logp_raw);
        const double logp = log((1.0 - eps) * p + eps * exp(true_log_prior));
        char method_name[INFS_NAME_CAP];
        infs_method_name(&setup, method, method_name);
        fprintf(out,
                "%ld,%d,%d,%d,%d,%s,%s,%s,%.6f,%.6f,%.6f,%d,%.0f,%.6f,%d,"
                "%ld\n",
                obs, turn, bag, leave_size, leaves.count,
                exact ? "exact" : "mc", evaluator->name, method_name,
                true_log_prior, zero ? -999.0 : logp_raw, logp, zero ? 1 : 0,
                rank, entropy, missing, (long)(time(NULL) - start));
        if (exact && e_idx == 0 && strings_equal(evaluator->name, "klv") &&
            method < setup.num_margins &&
            setup.margins[method] == INFS_VALIDATE_MARGIN) {
          klv_accepted_draws = 0.0;
          for (int leave_idx = 0; leave_idx < leaves.count; leave_idx++) {
            if (log_lik[(size_t)leave_idx * num_methods + method] == 0.0) {
              klv_accepted_draws += exp(leaves.log_prior[leave_idx]);
            }
          }
        }
      }
    }
    (void)fflush(out);
    if (validate && exact && strings_equal(evaluators[0].name, "klv")) {
      // infer() on a fresh copy of the position (evaluation changed the rack).
      char *revalidate_cmd = get_formatted_string("cgp %s", fields[5]);
      load_and_exec_config_or_die(evaluators[0].config, revalidate_cmd);
      free(revalidate_cmd);
      evaluators[0].game = config_get_game(evaluators[0].config);
      const double infer_draws = infs_infer_accepted_draws(
          &evaluators[0], played, played_tiles, ld_size,
          player_get_rack(game_get_player(evaluators[0].game, 1)));
      validate_rows++;
      if (fabs(infer_draws - klv_accepted_draws) > 0.5) {
        validate_mismatches++;
        printf("validate obs %ld: infer %.0f vs scorer %.0f accepted draws\n",
               obs, infer_draws, klv_accepted_draws);
      }
    }
    (void)observer_rack;
    free(log_lik);
    free(leaves.counts);
    free(leaves.log_prior);
  }
  if (validate) {
    printf("validation: %ld rows, %ld mismatches\n", validate_rows,
           validate_mismatches);
  }
  prng_destroy(prng);
  free(scratch);
  free(true_leave);
  free(played_tiles);
  free(pool);
  error_stack_destroy(error_stack);
  move_destroy(played);
  move_list_destroy(move_list);
  free(line);
  free(done);
  (void)fclose(corpus);
  (void)fclose(out);
  for (int e_idx = 0; e_idx < num_evaluators; e_idx++) {
    config_destroy(evaluators[e_idx].config);
  }
}

void test_infer_score(void) {
  const char *mode = getenv("INFS_MODE");
  const char *corpus_path = getenv("INFS_CORPUS");
  if (!mode || !corpus_path) {
    log_fatal("set INFS_MODE (gen or score) and INFS_CORPUS");
  }
  if (strings_equal(mode, "gen")) {
    infs_gen(corpus_path);
  } else if (strings_equal(mode, "score")) {
    infs_score(corpus_path);
  } else {
    log_fatal("unknown INFS_MODE: %s", mode);
  }
}
