#include "policy_fidelity_test.h"

#include "../src/def/equity_defs.h"
#include "../src/def/move_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Research harness: does a static evaluator pick what a simming player picks?
//
// A corpus row is one position with a simmer's label. For each static model
// the scorer generates every move with its equity and reports, per position:
// whether the model's best move is the label (top-1 agreement), the label's
// rank, the equity gap from the model's best to the label, and the label's
// log-likelihood under a softmax over equities, exp(equity / tau). tau is
// fit per model by maximum likelihood on the even positions and the
// likelihood is scored on the odd ones. Every metric is paired over
// positions against the first model (the baseline, KLV2 without PAT).
//
// Corpus format (tab-separated, one position per line, '#' comments):
//   pos <TAB> cgp <TAB> label <TAB> ranked
// label is the simmer's chosen move as a UCGI string (as
// string_builder_add_ucgi_move writes it, e.g. "d3 TOG"); ranked is
// optional: "move=win_pct=equity" entries joined by ';', best first.
//
// PF_MODE=export writes such a corpus from the PlayChooser candidate-count
// oracle (test/pc_cands_oracle_test.c on claude/pc-candidates-oracle): its
// CSVs record, per position index, the move each candidate count chose and
// the oracle's value of each, and the position itself is regenerated from
// the index exactly as that harness made it (static PAT self-play from a
// seeded deal, stopped at a bag threshold). The label is the oracle's best
// move among those choices (PF_LABEL=oracle, the default) or the move
// PlayChooser chose with 15 candidates (PF_LABEL=k15).
//
// PF_MODE=score reads the corpus and scores the models.
//
// Environment:
//   PF_MODE      export | score
//   PF_CORPUS    corpus path (written by export, read by score)
//   PF_ORACLE    export: comma-separated oracle CSV paths
//   PF_LABEL     export: oracle (default) | k15
//   PF_SEED      export: the oracle's base seed (default 20260926)
//   PF_OUT       score: per-position CSV path (optional)
//   PF_MAX       score: stop after this many positions (default all)
//   PF_MODELS    score: models as name=set-args entries joined by ';', the
//                first being the baseline. Default:
//                  klv2=-pat none;pat=-pat CSW24 -patclasses all;
//                  pat_tw=-pat CSW24 -patclasses tws,windows
//                A KLV3 model is one more entry once its leaves load, e.g.
//                  klv3=-pat none -leaves CSW24_klv3

enum {
  PF_MAX_MODELS = 8,
  PF_LINE_CAP = 1 << 16,
  PF_FIELD_CAP = 1 << 12,
  PF_MOVE_CAP = 100000,
  PF_MIN_BAG = 5,
  PF_MAX_BAG = 80,
  PF_MAX_GEN_TURNS = 60,
  PF_TAU_STEPS = 60,
};

static const char *const pf_default_models =
    "klv2=-pat none;pat=-pat CSW24 -patclasses all;"
    "pat_tw=-pat CSW24 -patclasses tws,windows";
static const char *const pf_base_set =
    "set -lex CSW24 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
    "-numplays 1 -threads 1 ";

// The oracle harness's seed mixer and position generator, reproduced so
// its position indices map back to positions.
static uint64_t pf_mix(uint64_t x) {
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31);
}

static bool pf_make_position(Game *game, MoveList *move_list, uint64_t seed) {
  game_reset(game);
  game_seed(game, seed);
  game_set_starting_player_index(game, (int)(seed & 1));
  draw_starting_racks(game);
  const int threshold =
      PF_MIN_BAG + (int)(pf_mix(seed) % (PF_MAX_BAG - PF_MIN_BAG + 1));
  for (int turn = 0; turn < PF_MAX_GEN_TURNS; turn++) {
    if (game_over(game)) {
      return false;
    }
    const int bag = bag_get_letters(game_get_bag(game));
    if (turn >= 2 && bag <= threshold && bag >= PF_MIN_BAG) {
      return true;
    }
    if (bag < PF_MIN_BAG) {
      return false;
    }
    const Move *move = get_top_equity_move(game, move_list);
    play_move(move, game, NULL);
  }
  return false;
}

// Splits line in place on sep into at most cap fields. Returns the count.
static int pf_split(char *line, char sep, char **fields, int cap) {
  int count = 0;
  char *cursor = line;
  while (count < cap) {
    fields[count++] = cursor;
    char *next = strchr(cursor, sep);
    if (!next) {
      break;
    }
    *next = '\0';
    cursor = next + 1;
  }
  return count;
}

static void pf_chomp(char *line) {
  const size_t length = strlen(line);
  if (length > 0 && line[length - 1] == '\n') {
    line[length - 1] = '\0';
  }
}

static int pf_column(char **header, int num_columns, const char *name) {
  for (int column = 0; column < num_columns; column++) {
    if (strings_equal(header[column], name)) {
      return column;
    }
  }
  return -1;
}

// Writes the corpus rows of one oracle CSV.
static void pf_export_file(const char *oracle_path, FILE *out,
                           const char *label_kind, uint64_t base_seed,
                           Game *game, MoveList *gen_list, int *exported) {
  FILE *in = fopen(oracle_path, "r");
  if (!in) {
    log_fatal("cannot open %s", oracle_path);
  }
  char *line = malloc_or_die(PF_LINE_CAP);
  char *header_line = malloc_or_die(PF_LINE_CAP);
  char *header[PF_FIELD_CAP];
  char *fields[PF_FIELD_CAP];
  if (!fgets(header_line, PF_LINE_CAP, in)) {
    free(line);
    free(header_line);
    (void)fclose(in);
    return;
  }
  pf_chomp(header_line);
  const int num_columns = pf_split(header_line, ',', header, PF_FIELD_CAP);
  const int best_wp_col = pf_column(header, num_columns, "oracle_best_wp");
  const int k15_col = pf_column(header, num_columns, "k15_move");
  if (best_wp_col < 0 || k15_col < 0) {
    log_fatal("%s is not a candidate-count oracle CSV", oracle_path);
  }
  while (fgets(line, PF_LINE_CAP, in)) {
    pf_chomp(line);
    const int count = pf_split(line, ',', fields, PF_FIELD_CAP);
    if (count != num_columns) {
      continue; // "pos,skip" rows and truncated lines
    }
    const long pos = strtol(fields[0], NULL, 10);
    const double best_wp = strtod(fields[best_wp_col], NULL);
    // Every recorded choice with its oracle value: the static move and each
    // setting's move, from the columns named *_move with *_wp and *_eq
    // beside them.
    StringBuilder *ranked = string_builder_create();
    const char *label = NULL;
    const char *seen[PF_FIELD_CAP];
    int num_seen = 0;
    for (int column = 0; column + 2 < num_columns; column++) {
      const size_t name_length = strlen(header[column]);
      if (name_length < 5 ||
          !strings_equal(header[column] + name_length - 5, "_move")) {
        continue;
      }
      // static_move is followed by static_wp, static_eq; a setting's move
      // by its iterations, then wp and eq.
      const bool is_static = strings_equal(header[column], "static_move");
      const int wp_col = is_static ? column + 1 : column + 2;
      const int eq_col = wp_col + 1;
      const char *move = fields[column];
      const double wp = strtod(fields[wp_col], NULL);
      if (label == NULL && fabs(wp - best_wp) < 1e-9) {
        label = move;
      }
      bool duplicate = false;
      for (int seen_idx = 0; seen_idx < num_seen; seen_idx++) {
        duplicate = duplicate || strings_equal(seen[seen_idx], move);
      }
      if (!duplicate) {
        seen[num_seen++] = move;
        string_builder_add_formatted_string(ranked, "%s%s=%s=%s",
                                            num_seen > 1 ? ";" : "", move,
                                            fields[wp_col], fields[eq_col]);
      }
    }
    if (strings_equal(label_kind, "k15")) {
      label = fields[k15_col];
    }
    if (label == NULL ||
        !pf_make_position(game, gen_list, pf_mix(base_seed ^ (uint64_t)pos))) {
      string_builder_destroy(ranked);
      continue;
    }
    char *cgp = game_get_cgp(game, true);
    fprintf(out, "%ld\t%s\t%s\t%s\n", pos, cgp, label,
            string_builder_peek(ranked));
    free(cgp);
    string_builder_destroy(ranked);
    (*exported)++;
  }
  free(line);
  free(header_line);
  (void)fclose(in);
}

static void pf_export(void) {
  const char *corpus_path = getenv("PF_CORPUS");
  const char *oracle_paths = getenv("PF_ORACLE");
  if (!corpus_path || !oracle_paths) {
    log_fatal("set PF_CORPUS and PF_ORACLE");
  }
  const char *label_kind = getenv("PF_LABEL") ? getenv("PF_LABEL") : "oracle";
  const char *seed_env = getenv("PF_SEED");
  const uint64_t base_seed =
      seed_env ? (uint64_t)strtoull(seed_env, NULL, 10) : 20260926;
  // The oracle's own configuration, so its positions replay exactly.
  Config *config = config_create_or_die(
      "set -lex CSW24 -wmp true -pat CSW24 -s1 equity -s2 equity -r1 best "
      "-r2 best -numplays 1 -threads 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *game = config_get_game(config);
  MoveList *gen_list = move_list_create(1);
  FILE *out = fopen(corpus_path, "w");
  assert(out);
  fprintf(out, "# pos\tcgp\tlabel (%s)\tranked (move=oracle_wp=oracle_eq)\n",
          label_kind);
  char *paths = string_duplicate(oracle_paths);
  char *path_fields[PF_MAX_MODELS * 8];
  const int num_paths = pf_split(paths, ',', path_fields, PF_MAX_MODELS * 8);
  int exported = 0;
  for (int path_idx = 0; path_idx < num_paths; path_idx++) {
    pf_export_file(path_fields[path_idx], out, label_kind, base_seed, game,
                   gen_list, &exported);
  }
  (void)fclose(out);
  printf("policy fidelity export: %d positions to %s\n", exported, corpus_path);
  free(paths);
  move_list_destroy(gen_list);
  config_destroy(config);
}

typedef struct PFModel {
  char name[64];
  Config *config;
  // Per position: the label's rank (0 = top), the gap from the best move to
  // the label in points, and the equities (points) of every move, for the
  // softmax likelihood.
  int *rank;
  double *gap;
  double **equities;
  int *num_moves;
  double *label_equity;
  double tau;
} PFModel;

// Log-likelihood of the label under exp(equity / tau) over the position's
// moves.
static double pf_log_likelihood(const double *equities, int num_moves,
                                double label_equity, double tau) {
  double best = equities[0];
  for (int move_idx = 1; move_idx < num_moves; move_idx++) {
    best = equities[move_idx] > best ? equities[move_idx] : best;
  }
  double sum = 0.0;
  for (int move_idx = 0; move_idx < num_moves; move_idx++) {
    sum += exp((equities[move_idx] - best) / tau);
  }
  return (label_equity - best) / tau - log(sum);
}

static double pf_total_log_likelihood(const PFModel *model, int num_positions,
                                      int parity, double tau) {
  double total = 0.0;
  for (int pos_idx = parity; pos_idx < num_positions; pos_idx += 2) {
    if (model->rank[pos_idx] < 0) {
      continue;
    }
    total +=
        pf_log_likelihood(model->equities[pos_idx], model->num_moves[pos_idx],
                          model->label_equity[pos_idx], tau);
  }
  return total;
}

// Maximum-likelihood tau on the even positions: golden-section search on
// log tau over [0.1, 200] points (the likelihood is unimodal in tau).
static double pf_fit_tau(const PFModel *model, int num_positions) {
  const double golden = 0.5 * (sqrt(5.0) - 1.0);
  double low = log(0.1);
  double high = log(200.0);
  double left = high - golden * (high - low);
  double right = low + golden * (high - low);
  double f_left = pf_total_log_likelihood(model, num_positions, 0, exp(left));
  double f_right = pf_total_log_likelihood(model, num_positions, 0, exp(right));
  for (int step = 0; step < PF_TAU_STEPS; step++) {
    if (f_left < f_right) {
      low = left;
      left = right;
      f_left = f_right;
      right = low + golden * (high - low);
      f_right = pf_total_log_likelihood(model, num_positions, 0, exp(right));
    } else {
      high = right;
      right = left;
      f_right = f_left;
      left = high - golden * (high - low);
      f_left = pf_total_log_likelihood(model, num_positions, 0, exp(left));
    }
  }
  return exp(0.5 * (low + high));
}

// Mean and standard error of values[0..count).
static void pf_mean_se(const double *values, int count, double *mean,
                       double *se) {
  double sum = 0.0;
  for (int idx = 0; idx < count; idx++) {
    sum += values[idx];
  }
  *mean = count > 0 ? sum / count : 0.0;
  double squares = 0.0;
  for (int idx = 0; idx < count; idx++) {
    squares += (values[idx] - *mean) * (values[idx] - *mean);
  }
  *se = count > 1 ? sqrt(squares / (count - 1) / count) : 0.0;
}

// Scores one position under one model: generates every move, finds the
// label by its UCGI string, and records rank, gap and equities.
static void pf_score_position(PFModel *model, int pos_idx, const char *cgp,
                              const char *label, MoveList *move_list,
                              StringBuilder *sb) {
  char *cgp_cmd = get_formatted_string("cgp %s", cgp);
  load_and_exec_config_or_die(model->config, cgp_cmd);
  free(cgp_cmd);
  Game *game = config_get_game(model->config);
  move_list_reset(move_list);
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
  if (num_moves >= PF_MOVE_CAP) {
    log_fatal("position %d has at least %d moves; raise PF_MOVE_CAP", pos_idx,
              PF_MOVE_CAP);
  }
  // The pass is recorded with a sentinel equity below every real one; it is
  // left out, so a position whose label is the pass is unusable.
  double *equities = malloc_or_die(sizeof(double) * (size_t)num_moves);
  int num_scored = 0;
  double best = -1e300;
  int label_idx = -1;
  for (int move_idx = 0; move_idx < num_moves; move_idx++) {
    const Move *move = move_list_get_move(move_list, move_idx);
    if (move_get_equity(move) == EQUITY_PASS_VALUE) {
      continue;
    }
    const double equity = equity_to_double(move_get_equity(move));
    best = equity > best ? equity : best;
    if (label_idx < 0) {
      string_builder_clear(sb);
      string_builder_add_ucgi_move(sb, move, game_get_board(game),
                                   game_get_ld(game));
      if (strings_equal(string_builder_peek(sb), label)) {
        label_idx = num_scored;
      }
    }
    equities[num_scored++] = equity;
  }
  model->equities[pos_idx] = equities;
  model->num_moves[pos_idx] = num_scored;
  if (label_idx < 0) {
    model->rank[pos_idx] = -1;
    return;
  }
  const double label_equity = equities[label_idx];
  int better = 0;
  for (int move_idx = 0; move_idx < num_scored; move_idx++) {
    better += equities[move_idx] > label_equity;
  }
  model->rank[pos_idx] = better;
  model->gap[pos_idx] = best - label_equity;
  model->label_equity[pos_idx] = label_equity;
}

static void pf_score(void) {
  const char *corpus_path = getenv("PF_CORPUS");
  if (!corpus_path) {
    log_fatal("set PF_CORPUS");
  }
  const char *max_env = getenv("PF_MAX");
  const long max_positions = max_env ? strtol(max_env, NULL, 10) : 1000000;
  const char *models_env =
      getenv("PF_MODELS") ? getenv("PF_MODELS") : pf_default_models;

  // Read the corpus.
  FILE *in = fopen(corpus_path, "r");
  if (!in) {
    log_fatal("cannot open %s", corpus_path);
  }
  int capacity = 1024;
  int num_positions = 0;
  char **cgps = malloc_or_die(sizeof(char *) * (size_t)capacity);
  char **labels = malloc_or_die(sizeof(char *) * (size_t)capacity);
  long *pos_ids = malloc_or_die(sizeof(long) * (size_t)capacity);
  char *line = malloc_or_die(PF_LINE_CAP);
  while (num_positions < max_positions && fgets(line, PF_LINE_CAP, in)) {
    pf_chomp(line);
    if (line[0] == '#' || line[0] == '\0') {
      continue;
    }
    char *fields[4];
    if (pf_split(line, '\t', fields, 4) < 3) {
      continue;
    }
    if (num_positions == capacity) {
      capacity *= 2;
      cgps = realloc_or_die(cgps, sizeof(char *) * (size_t)capacity);
      labels = realloc_or_die(labels, sizeof(char *) * (size_t)capacity);
      pos_ids = realloc_or_die(pos_ids, sizeof(long) * (size_t)capacity);
    }
    pos_ids[num_positions] = strtol(fields[0], NULL, 10);
    cgps[num_positions] = string_duplicate(fields[1]);
    labels[num_positions] = string_duplicate(fields[2]);
    num_positions++;
  }
  free(line);
  (void)fclose(in);

  // Build the models.
  char *models_copy = string_duplicate(models_env);
  char *model_specs[PF_MAX_MODELS];
  const int num_models = pf_split(models_copy, ';', model_specs, PF_MAX_MODELS);
  PFModel models[PF_MAX_MODELS];
  for (int model_idx = 0; model_idx < num_models; model_idx++) {
    PFModel *model = &models[model_idx];
    char *spec = model_specs[model_idx];
    char *equals = strchr(spec, '=');
    if (!equals) {
      log_fatal("model '%s' is not name=set-args", spec);
    }
    *equals = '\0';
    (void)snprintf(model->name, sizeof(model->name), "%s", spec);
    char *set_cmd = get_formatted_string("%s%s", pf_base_set, equals + 1);
    model->config = config_create_or_die(set_cmd);
    free(set_cmd);
    model->rank = malloc_or_die(sizeof(int) * (size_t)num_positions);
    model->gap = malloc_or_die(sizeof(double) * (size_t)num_positions);
    model->equities = calloc_or_die((size_t)num_positions, sizeof(double *));
    model->num_moves = calloc_or_die((size_t)num_positions, sizeof(int));
    model->label_equity = malloc_or_die(sizeof(double) * (size_t)num_positions);
  }

  MoveList *move_list = move_list_create(PF_MOVE_CAP);
  StringBuilder *sb = string_builder_create();
  for (int pos_idx = 0; pos_idx < num_positions; pos_idx++) {
    for (int model_idx = 0; model_idx < num_models; model_idx++) {
      pf_score_position(&models[model_idx], pos_idx, cgps[pos_idx],
                        labels[pos_idx], move_list, sb);
    }
  }
  for (int model_idx = 0; model_idx < num_models; model_idx++) {
    models[model_idx].tau = pf_fit_tau(&models[model_idx], num_positions);
  }

  // A position counts when every model found the label.
  bool *usable = malloc_or_die(sizeof(bool) * (size_t)num_positions);
  int num_usable = 0;
  for (int pos_idx = 0; pos_idx < num_positions; pos_idx++) {
    usable[pos_idx] = true;
    for (int model_idx = 0; model_idx < num_models; model_idx++) {
      usable[pos_idx] = usable[pos_idx] && models[model_idx].rank[pos_idx] >= 0;
    }
    num_usable += usable[pos_idx];
  }

  const char *out_path = getenv("PF_OUT");
  FILE *out = out_path ? fopen(out_path, "w") : NULL;
  if (out) {
    fprintf(out, "pos,model,rank,top1,gap,log_likelihood,num_moves,test\n");
  }
  double *values = malloc_or_die(sizeof(double) * (size_t)num_positions);
  double *diffs = malloc_or_die(sizeof(double) * (size_t)num_positions);
  printf("policy fidelity: %d positions, %d usable (label found by every "
         "model); likelihood on the %d odd positions\n",
         num_positions, num_usable, num_usable / 2);
  printf("%-10s %8s %18s %18s %18s %8s %22s\n", "model", "tau", "top-1",
         "mean rank", "gap (pts)", "", "test log-lik/pos");
  const PFModel *base = &models[0];
  for (int model_idx = 0; model_idx < num_models; model_idx++) {
    const PFModel *model = &models[model_idx];
    double top1_mean = 0.0;
    double top1_se = 0.0;
    double rank_mean = 0.0;
    double rank_se = 0.0;
    double gap_mean = 0.0;
    double gap_se = 0.0;
    double ll_mean = 0.0;
    double ll_se = 0.0;
    int count = 0;
    for (int pos_idx = 0; pos_idx < num_positions; pos_idx++) {
      if (usable[pos_idx]) {
        values[count++] = model->rank[pos_idx] == 0;
      }
    }
    pf_mean_se(values, count, &top1_mean, &top1_se);
    count = 0;
    for (int pos_idx = 0; pos_idx < num_positions; pos_idx++) {
      if (usable[pos_idx]) {
        values[count++] = model->rank[pos_idx] + 1;
      }
    }
    pf_mean_se(values, count, &rank_mean, &rank_se);
    count = 0;
    for (int pos_idx = 0; pos_idx < num_positions; pos_idx++) {
      if (usable[pos_idx]) {
        values[count++] = model->gap[pos_idx];
      }
    }
    pf_mean_se(values, count, &gap_mean, &gap_se);
    count = 0;
    int diff_count = 0;
    for (int pos_idx = 1; pos_idx < num_positions; pos_idx += 2) {
      if (!usable[pos_idx]) {
        continue;
      }
      const double ll =
          pf_log_likelihood(model->equities[pos_idx], model->num_moves[pos_idx],
                            model->label_equity[pos_idx], model->tau);
      const double base_ll =
          pf_log_likelihood(base->equities[pos_idx], base->num_moves[pos_idx],
                            base->label_equity[pos_idx], base->tau);
      values[count++] = ll;
      diffs[diff_count++] = ll - base_ll;
    }
    pf_mean_se(values, count, &ll_mean, &ll_se);
    printf("%-10s %8.2f %7.3f +/- %6.3f %7.2f +/- %6.2f %7.2f +/- %6.2f "
           "%8s %9.4f +/- %7.4f\n",
           model->name, model->tau, top1_mean, top1_se, rank_mean, rank_se,
           gap_mean, gap_se, "", ll_mean, ll_se);
    if (model_idx > 0) {
      // Paired differences against the baseline.
      double diff_mean = 0.0;
      double diff_se = 0.0;
      count = 0;
      for (int pos_idx = 0; pos_idx < num_positions; pos_idx++) {
        if (usable[pos_idx]) {
          values[count++] = (double)(model->rank[pos_idx] == 0) -
                            (double)(base->rank[pos_idx] == 0);
        }
      }
      pf_mean_se(values, count, &diff_mean, &diff_se);
      printf("  vs %-6s top-1 %+7.3f +/- %6.3f", base->name, diff_mean,
             diff_se);
      count = 0;
      for (int pos_idx = 0; pos_idx < num_positions; pos_idx++) {
        if (usable[pos_idx]) {
          values[count++] = model->gap[pos_idx] - base->gap[pos_idx];
        }
      }
      pf_mean_se(values, count, &diff_mean, &diff_se);
      printf("   gap %+7.2f +/- %6.2f", diff_mean, diff_se);
      pf_mean_se(diffs, diff_count, &diff_mean, &diff_se);
      printf("   test log-lik %+8.4f +/- %7.4f\n", diff_mean, diff_se);
    }
    if (out) {
      for (int pos_idx = 0; pos_idx < num_positions; pos_idx++) {
        if (!usable[pos_idx]) {
          continue;
        }
        fprintf(out, "%ld,%s,%d,%d,%.3f,%.6f,%d,%d\n", pos_ids[pos_idx],
                model->name, model->rank[pos_idx] + 1,
                model->rank[pos_idx] == 0, model->gap[pos_idx],
                pf_log_likelihood(model->equities[pos_idx],
                                  model->num_moves[pos_idx],
                                  model->label_equity[pos_idx], model->tau),
                model->num_moves[pos_idx], pos_idx % 2);
      }
    }
  }
  if (out) {
    (void)fclose(out);
  }

  free(values);
  free(diffs);
  free(usable);
  string_builder_destroy(sb);
  move_list_destroy(move_list);
  for (int model_idx = 0; model_idx < num_models; model_idx++) {
    PFModel *model = &models[model_idx];
    for (int pos_idx = 0; pos_idx < num_positions; pos_idx++) {
      free(model->equities[pos_idx]);
    }
    free(model->equities);
    free(model->num_moves);
    free(model->label_equity);
    free(model->rank);
    free(model->gap);
    config_destroy(model->config);
  }
  free(models_copy);
  for (int pos_idx = 0; pos_idx < num_positions; pos_idx++) {
    free(cgps[pos_idx]);
    free(labels[pos_idx]);
  }
  free(cgps);
  free(labels);
  free(pos_ids);
}

void test_policy_fidelity(void) {
  const char *mode = getenv("PF_MODE");
  if (mode && strings_equal(mode, "export")) {
    pf_export();
  } else if (mode && strings_equal(mode, "score")) {
    pf_score();
  } else {
    log_fatal("set PF_MODE to export or score");
  }
}
