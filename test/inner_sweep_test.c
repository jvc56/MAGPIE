// Inner-sim configuration sweep.
//
// For a set of test positions (CGPs), runs a "gold" inner sim once per
// position to get a high-confidence ranking of candidates, then sweeps a
// grid of cheap test configs (varying K, N, plies, weights) and records
// each test config's pick + wall time + utility loss vs gold.
//
// "Inner sim" here is the per-ply candidate evaluator used inside a nested
// outer sim. It generates K candidates by equity, runs N rollouts of
// `plies` static-eval moves each, and picks the candidate with the highest
// sim_utility_blend (using the picker's own weights).
//
// Goal: find (K, N, plies, weights) combinations that:
//   - Run in ~3 ms wall time on average (matches the per-inner-sample
//     budget if the outer turn budget is 12 s split across ~4000 outer
//     samples)
//   - Have low mean utility-loss vs gold
//
// Input  (env var INNERSWEEP_POSITIONS, default
//        /tmp/gamepairbai/run5_disagree.csv): CSV with a `cgp` column, OR
//        a plain text file with one CGP per line.
// Output (env var INNERSWEEP_OUT, default
//        /tmp/gamepairbai/innersweep.csv): per-(config, position) rows.

#include "inner_sweep_test.h"

#include "../src/compat/ctime.h"
#include "../src/def/bai_defs.h"
#include "../src/def/board_defs.h"
#include "../src/def/config_defs.h"
#include "../src/def/move_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_args.h"
#include "../src/ent/win_pct.h"
#include "../src/ent/xoshiro.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/str/move_string.h"
#include "../src/str/rack_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_constants.h"
#include "test_util.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_GOLD_K 50
#define MAX_POSITIONS 2000

typedef enum {
  INNER_ALG_ROUND_ROBIN,
  INNER_ALG_TOP_TWO,
} inner_algorithm_t;

typedef struct InnerSimConfig {
  int K;
  // For ROUND_ROBIN: N = rollouts per candidate (total samples = K*N).
  // For TOP_TWO:     N = total-samples-divided-by-K (total samples = K*N
  //                  for matched-budget comparison with ROUND_ROBIN).
  int N;
  int plies;
  double w_winpct;
  double w_spread;
  double spread_scale;
  inner_algorithm_t algorithm;
} InnerSimConfig;

// Deterministic per-index seed (splitmix64). Used to give the n-th sample
// of any candidate a fixed seed so candidate comparison is paired (CRN).
static inline uint64_t seed_from_index(uint64_t idx) {
  uint64_t z = idx + 0x9E3779B97F4A7C15ULL;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

typedef struct CandidateResult {
  Move move;
  double avg_wpct;
  double avg_spread;
  double blended_utility;
} CandidateResult;

typedef struct InnerSimResult {
  int pick_idx;
  int num_candidates;
  CandidateResult candidates[MAX_GOLD_K];
  double wall_ms;
} InnerSimResult;

// Local copy of moves_equivalent (kept self-contained to this test file).
static bool moves_equiv(const Move *a, const Move *b) {
  if (a->move_type != b->move_type) {
    return false;
  }
  if (a->move_type == GAME_EVENT_PASS) {
    return true;
  }
  if (a->row_start != b->row_start || a->col_start != b->col_start) {
    return false;
  }
  if (a->dir != b->dir) {
    return false;
  }
  if (a->tiles_played != b->tiles_played) {
    return false;
  }
  if (a->tiles_length != b->tiles_length) {
    return false;
  }
  for (int i = 0; i < a->tiles_length; i++) {
    if (a->tiles[i] != b->tiles[i]) {
      return false;
    }
  }
  return true;
}

// Run one rollout: from `game` after playing `candidate`, do `plies`
// static-eval moves. Returns (wpct, spread) from player_idx's perspective.
static void run_one_rollout(Game *game, const Move *candidate, uint64_t seed,
                            int plies, int player_idx, bool plies_are_odd,
                            const WinPct *win_pcts, Game *scratch,
                            MoveList *scratch_ml, double *out_wpct,
                            double *out_spread) {
  game_copy(scratch, game);
  game_seed(scratch, seed);
  game_set_backup_mode(scratch, BACKUP_MODE_OFF);
  play_move(candidate, scratch, NULL);
  for (int ply = 0; ply < plies; ply++) {
    if (game_over(scratch)) {
      break;
    }
    const Move *best = get_top_equity_move(scratch, 0, scratch_ml);
    play_move(best, scratch, NULL);
  }
  const int spread = equity_to_int(player_get_score(
                         game_get_player(scratch, player_idx))) -
                     equity_to_int(player_get_score(
                         game_get_player(scratch, 1 - player_idx)));
  *out_spread = (double)spread;
  double wpct;
  if (game_over(scratch)) {
    wpct = (spread > 0) ? 1.0 : (spread == 0) ? 0.5 : 0.0;
  } else {
    int lookup_spread = spread;
    if (!plies_are_odd) {
      lookup_spread = -lookup_spread;
    }
    unsigned int unseen =
        bag_get_letters(game_get_bag(scratch)) +
        rack_get_total_letters(
            player_get_rack(game_get_player(scratch, 1 - player_idx)));
    if (unseen == 0) {
      wpct = (spread > 0) ? 1.0 : (spread == 0) ? 0.5 : 0.0;
    } else {
      wpct = (double)win_pct_get(win_pcts, lookup_spread, unseen);
      if (!plies_are_odd) {
        wpct = 1.0 - wpct;
      }
    }
  }
  *out_wpct = wpct;
}

// Run round-robin inner sim: K * N rollouts, N rollouts per candidate,
// candidates share rollout seeds (CRN via seed_from_index).
static void run_inner_round_robin(Game *game, const InnerSimConfig *cfg,
                                  int num_cand, const WinPct *win_pcts,
                                  Game *scratch_game, MoveList *scratch_ml,
                                  MoveList *cand_list, int player_idx,
                                  bool plies_are_odd,
                                  InnerSimResult *result) {
  int best = 0;
  double best_util = -1.0;
  for (int k = 0; k < num_cand; k++) {
    const Move *cand = move_list_get_move(cand_list, k);
    result->candidates[k].move = *cand;
    double total_wpct = 0.0;
    double total_spread = 0.0;
    for (int n = 0; n < cfg->N; n++) {
      double wpct, spread;
      run_one_rollout(game, cand, seed_from_index((uint64_t)n), cfg->plies,
                      player_idx, plies_are_odd, win_pcts, scratch_game,
                      scratch_ml, &wpct, &spread);
      total_wpct += wpct;
      total_spread += spread;
    }
    const double avg_wpct = total_wpct / cfg->N;
    const double avg_spread = total_spread / cfg->N;
    const double utility =
        sim_utility_blend(avg_wpct, double_to_equity(avg_spread),
                          cfg->w_winpct, cfg->w_spread, cfg->spread_scale);
    result->candidates[k].avg_wpct = avg_wpct;
    result->candidates[k].avg_spread = avg_spread;
    result->candidates[k].blended_utility = utility;
    if (utility > best_util) {
      best_util = utility;
      best = k;
    }
  }
  result->pick_idx = best;
}

// Run top-two-IDS inner sim. To avoid degenerate behavior at low budgets
// (where ranks after 1 sample are pure noise), each candidate first gets
// `min_samples` rollouts. Remaining budget (= K * (N - min_samples)) is
// distributed adaptively: each step picks top1 (best mean) with prob 0.5,
// else top2 (second-best). Candidate-comparison is paired via CRN: the
// m-th sample of any candidate uses seed_from_index(m).
//
// Total samples = K * N (matches round-robin budget exactly).
// When min_samples >= N, the algorithm degenerates to plain round-robin.
static void run_inner_top_two(Game *game, const InnerSimConfig *cfg,
                              int num_cand, int min_samples,
                              const WinPct *win_pcts, XoshiroPRNG *prng,
                              Game *scratch_game, MoveList *scratch_ml,
                              MoveList *cand_list, int player_idx,
                              bool plies_are_odd, InnerSimResult *result) {
  const int total_budget = cfg->K * cfg->N;
  int floor_per_arm = min_samples;
  if (floor_per_arm > cfg->N) {
    floor_per_arm = cfg->N;
  }
  double sum_wpct[MAX_GOLD_K] = {0.0};
  double sum_spread[MAX_GOLD_K] = {0.0};
  int n_samples[MAX_GOLD_K] = {0};

  // Initial round: floor_per_arm samples per candidate, paired across
  // candidates via seed_from_index(m).
  for (int k = 0; k < num_cand; k++) {
    const Move *cand = move_list_get_move(cand_list, k);
    result->candidates[k].move = *cand;
    double total_w = 0.0, total_s = 0.0;
    for (int m = 0; m < floor_per_arm; m++) {
      double w, s;
      run_one_rollout(game, cand, seed_from_index((uint64_t)m), cfg->plies,
                      player_idx, plies_are_odd, win_pcts, scratch_game,
                      scratch_ml, &w, &s);
      total_w += w;
      total_s += s;
    }
    sum_wpct[k] = total_w;
    sum_spread[k] = total_s;
    n_samples[k] = floor_per_arm;
  }

  // Adaptive: remaining samples.
  const int adaptive = total_budget - num_cand * floor_per_arm;
  for (int step = 0; step < adaptive; step++) {
    // Compute current utility for each candidate.
    double best_u = -1e9, second_u = -1e9;
    int top1 = -1, top2 = -1;
    for (int k = 0; k < num_cand; k++) {
      const double aw = sum_wpct[k] / n_samples[k];
      const double as = sum_spread[k] / n_samples[k];
      const double u =
          sim_utility_blend(aw, double_to_equity(as), cfg->w_winpct,
                            cfg->w_spread, cfg->spread_scale);
      if (u > best_u) {
        second_u = best_u;
        top2 = top1;
        best_u = u;
        top1 = k;
      } else if (u > second_u) {
        second_u = u;
        top2 = k;
      }
    }
    int chosen = top1;
    if (top2 >= 0 && (prng_next(prng) & 1ULL)) {
      chosen = top2;
    }
    const Move *cand = move_list_get_move(cand_list, chosen);
    double wpct, spread;
    run_one_rollout(game, cand, seed_from_index((uint64_t)n_samples[chosen]),
                    cfg->plies, player_idx, plies_are_odd, win_pcts,
                    scratch_game, scratch_ml, &wpct, &spread);
    sum_wpct[chosen] += wpct;
    sum_spread[chosen] += spread;
    n_samples[chosen]++;
  }

  // Final picks + utility.
  int best = 0;
  double best_util = -1.0;
  for (int k = 0; k < num_cand; k++) {
    const double avg_wpct = sum_wpct[k] / n_samples[k];
    const double avg_spread = sum_spread[k] / n_samples[k];
    const double utility =
        sim_utility_blend(avg_wpct, double_to_equity(avg_spread),
                          cfg->w_winpct, cfg->w_spread, cfg->spread_scale);
    result->candidates[k].avg_wpct = avg_wpct;
    result->candidates[k].avg_spread = avg_spread;
    result->candidates[k].blended_utility = utility;
    if (utility > best_util) {
      best_util = utility;
      best = k;
    }
  }
  result->pick_idx = best;
}

// Run one inner sim and record per-candidate stats + chosen pick.
// `candidate_list` must have capacity >= cfg->K (it'll get re-filled).
// `scratch_game` and `scratch_move_list` are workspaces.
static void run_inner_sim(Game *game, const InnerSimConfig *cfg,
                          const WinPct *win_pcts, XoshiroPRNG *prng,
                          MoveList *candidate_list, Game *scratch_game,
                          MoveList *scratch_move_list,
                          InnerSimResult *result) {
  struct timespec ts_start, ts_end;
  clock_gettime(CLOCK_MONOTONIC, &ts_start);

  const MoveGenArgs gen_args = {
      .game = game,
      .move_list = candidate_list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&gen_args);
  // generate_moves with MOVE_RECORD_ALL keeps the top-K by equity but
  // doesn't sort within. Sort so candidate index reflects equity rank
  // (matches production get_top_nested_sim_move).
  move_list_sort_moves(candidate_list);

  int num_cand = move_list_get_count(candidate_list);
  if (num_cand > cfg->K) {
    num_cand = cfg->K;
  }
  if (num_cand > MAX_GOLD_K) {
    num_cand = MAX_GOLD_K;
  }
  result->num_candidates = num_cand;

  const int player_idx = game_get_player_on_turn_index(game);
  const bool plies_are_odd = (cfg->plies % 2) != 0;

  if (cfg->algorithm == INNER_ALG_TOP_TWO) {
    static int min_samples = -1;
    if (min_samples < 0) {
      const char *e = getenv("INNERSWEEP_MIN_SAMPLES");
      min_samples = (e && *e) ? atoi(e) : 3;
      if (min_samples < 1) {
        min_samples = 1;
      }
    }
    run_inner_top_two(game, cfg, num_cand, min_samples, win_pcts, prng,
                      scratch_game, scratch_move_list, candidate_list,
                      player_idx, plies_are_odd, result);
  } else {
    run_inner_round_robin(game, cfg, num_cand, win_pcts, scratch_game,
                          scratch_move_list, candidate_list, player_idx,
                          plies_are_odd, result);
  }

  clock_gettime(CLOCK_MONOTONIC, &ts_end);
  result->wall_ms = (double)(ts_end.tv_sec - ts_start.tv_sec) * 1000.0 +
                    (double)(ts_end.tv_nsec - ts_start.tv_nsec) / 1e6;
}

// Look up `target` in gold's candidate list, return its index (or -1).
static int find_in_gold(const InnerSimResult *gold, const Move *target) {
  for (int i = 0; i < gold->num_candidates; i++) {
    if (moves_equiv(&gold->candidates[i].move, target)) {
      return i;
    }
  }
  return -1;
}

// Read CGPs from a file: tries CSV (looks for `cgp` column in header) and
// falls back to one CGP per line. Returns count, fills cgps with strdup'd
// strings (caller frees).
static int load_positions(const char *path, char **cgps, int max_pos) {
  FILE *f = fopen(path, "r");
  if (!f) {
    log_fatal("could not open positions file: %s", path);
    return 0;
  }
  char *line = NULL;
  size_t cap = 0;
  ssize_t len;
  int count = 0;
  int cgp_col = -1;
  int line_num = 0;

  while ((len = getline(&line, &cap, f)) != -1) {
    line_num++;
    if (len > 0 && line[len - 1] == '\n') {
      line[--len] = '\0';
    }
    if (len == 0) {
      continue;
    }

    // First non-empty line: try to parse as CSV header.
    if (line_num == 1 && strchr(line, ',')) {
      // CSV header — find cgp column index
      int col = 0;
      char *tok = line;
      while (tok) {
        char *next = strchr(tok, ',');
        if (next) {
          *next = '\0';
        }
        if (strcmp(tok, "cgp") == 0) {
          cgp_col = col;
          break;
        }
        col++;
        tok = next ? next + 1 : NULL;
      }
      if (cgp_col < 0) {
        log_fatal("CSV header has no `cgp` column");
        break;
      }
      continue;
    }

    if (cgp_col >= 0) {
      // CSV row: split by commas (CGPs don't contain commas), pick cgp_col,
      // strip surrounding quotes.
      int col = 0;
      char *tok = line;
      char *field = NULL;
      while (tok) {
        char *next = strchr(tok, ',');
        if (next) {
          *next = '\0';
        }
        if (col == cgp_col) {
          field = tok;
          break;
        }
        col++;
        tok = next ? next + 1 : NULL;
      }
      if (!field) {
        continue;
      }
      if (field[0] == '"') {
        field++;
      }
      size_t fl = strlen(field);
      if (fl > 0 && field[fl - 1] == '"') {
        field[fl - 1] = '\0';
      }
      if (count < max_pos && field[0]) {
        cgps[count++] = string_duplicate(field);
      }
    } else {
      // Plain text: each line is a CGP
      if (count < max_pos) {
        cgps[count++] = string_duplicate(line);
      }
    }
  }

  free(line);
  fclose(f);
  return count;
}

void test_inner_sweep_run(void) {
  setbuf(stdout, NULL);

  const char *positions_path = getenv("INNERSWEEP_POSITIONS");
  if (!positions_path || !*positions_path) {
    positions_path = "/tmp/gamepairbai/run5_disagree.csv";
  }
  const char *out_path = getenv("INNERSWEEP_OUT");
  if (!out_path || !*out_path) {
    out_path = "/tmp/gamepairbai/innersweep.csv";
  }

  // Gold sweep config (fixed; user-specified).
  int gold_K = 50;
  int gold_N = 10000;
  int gold_plies = 6;
  const char *e;
  if ((e = getenv("INNERSWEEP_GOLD_K"))) {
    gold_K = atoi(e);
  }
  if ((e = getenv("INNERSWEEP_GOLD_N"))) {
    gold_N = atoi(e);
  }
  if ((e = getenv("INNERSWEEP_GOLD_PLIES"))) {
    gold_plies = atoi(e);
  }
  const InnerSimConfig gold_cfg = {
      .K = gold_K,
      .N = gold_N,
      .plies = gold_plies,
      .w_winpct = 1.0,
      .w_spread = 0.2,
      .spread_scale = 100.0,
      .algorithm = INNER_ALG_ROUND_ROBIN,
  };

  // Round-robin vs top-two comparison grid. Same K, N grid, plies=2, two
  // weight settings. Each (K,N) is run with both algorithms at matched total
  // budget K*N. ws=0 (pure win%) and ws=0.2 (current production setting).
  static const int test_Ks[] = {10, 15, 20, 25, 50};
  static const int test_Ns[] = {4, 8, 12, 16, 20, 32};
  static const int test_plies[] = {2};
  static const double test_w_spreads[] = {0.0, 0.2, 0.5, 1.0};
  static const inner_algorithm_t test_algs[] = {INNER_ALG_ROUND_ROBIN,
                                                INNER_ALG_TOP_TWO};
  const int nK = sizeof(test_Ks) / sizeof(test_Ks[0]);
  const int nN = sizeof(test_Ns) / sizeof(test_Ns[0]);
  const int nP = sizeof(test_plies) / sizeof(test_plies[0]);
  const int nW = sizeof(test_w_spreads) / sizeof(test_w_spreads[0]);
  const int nA = sizeof(test_algs) / sizeof(test_algs[0]);
  const int num_test_configs = nK * nN * nP * nW * nA;

  // Limit positions via env var (for fast iteration).
  int max_pos = MAX_POSITIONS;
  if ((e = getenv("INNERSWEEP_MAX_POSITIONS"))) {
    max_pos = atoi(e);
  }

  // Load positions
  char *cgps[MAX_POSITIONS];
  int num_positions = load_positions(positions_path, cgps, max_pos);
  printf("Loaded %d positions from %s\n", num_positions, positions_path);
  printf("Gold config: K=%d N=%d plies=%d weights=(%.2f,%.2f,%.1f)\n",
         gold_cfg.K, gold_cfg.N, gold_cfg.plies, gold_cfg.w_winpct,
         gold_cfg.w_spread, gold_cfg.spread_scale);
  printf("Test configs: %d (K=%d N=%d plies=%d w_spread=%d)\n",
         num_test_configs, nK, nN, nP, nW);

  // Open output CSV
  FILE *out = fopen(out_path, "w");
  if (!out) {
    log_fatal("cannot open output %s", out_path);
    return;
  }
  fprintf(out,
          "pos_idx,alg,K,N,plies,w_winpct,w_spread,spread_scale,wall_ms,"
          "pick_in_gold,gold_pick,utility_loss\n");
  fflush(out);

  // Set up runtime
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -rit true -s1 equity -s2 equity "
      "-r1 all -r2 all -numplays 25 -plies 2 -threads 1");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);

  ErrorStack *err = error_stack_create();
  WinPct *win_pcts =
      win_pct_create(config_get_data_paths(config), DEFAULT_WIN_PCT, err);
  if (!error_stack_is_empty(err)) {
    error_stack_print_and_reset(err);
    return;
  }

  Game *game = game_duplicate(config_get_game(config));
  Game *scratch = game_duplicate(config_get_game(config));
  MoveList *cand_list = move_list_create(MAX_GOLD_K);
  MoveList *scratch_ml = move_list_create(1);
  XoshiroPRNG *prng = prng_create(0xC0FFEEULL);

  InnerSimResult gold_result;
  InnerSimResult test_result;

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  for (int pos_idx = 0; pos_idx < num_positions; pos_idx++) {
    game_load_cgp(game, cgps[pos_idx], err);
    if (!error_stack_is_empty(err)) {
      printf("  pos %d: cgp load failed\n", pos_idx);
      error_stack_print_and_reset(err);
      continue;
    }

    // Run gold once per position.
    run_inner_sim(game, &gold_cfg, win_pcts, prng, cand_list, scratch,
                  scratch_ml, &gold_result);
    if (gold_result.num_candidates < 2) {
      printf("  pos %d: only %d candidates, skipping\n", pos_idx,
             gold_result.num_candidates);
      continue;
    }
    const double gold_pick_utility =
        gold_result.candidates[gold_result.pick_idx].blended_utility;

    // Sweep test configs (both algorithms × K × N × plies × ws).
    for (int ia = 0; ia < nA; ia++) {
      for (int ik = 0; ik < nK; ik++) {
        for (int iN = 0; iN < nN; iN++) {
          for (int ip = 0; ip < nP; ip++) {
            for (int iw = 0; iw < nW; iw++) {
              InnerSimConfig cfg = {
                  .K = test_Ks[ik],
                  .N = test_Ns[iN],
                  .plies = test_plies[ip],
                  .w_winpct = 1.0,
                  .w_spread = test_w_spreads[iw],
                  .spread_scale = 100.0,
                  .algorithm = test_algs[ia],
              };
              run_inner_sim(game, &cfg, win_pcts, prng, cand_list, scratch,
                            scratch_ml, &test_result);

              const Move *test_pick_move =
                  &test_result.candidates[test_result.pick_idx].move;
              int gold_idx = find_in_gold(&gold_result, test_pick_move);
              double test_utility_in_gold;
              if (gold_idx >= 0) {
                test_utility_in_gold =
                    gold_result.candidates[gold_idx].blended_utility;
              } else {
                test_utility_in_gold = 0.0;
              }
              const double utility_loss =
                  gold_pick_utility - test_utility_in_gold;
              const char *alg_str =
                  (cfg.algorithm == INNER_ALG_TOP_TWO) ? "TOP_TWO" : "RR";

              fprintf(out,
                      "%d,%s,%d,%d,%d,%.2f,%.2f,%.1f,%.3f,%d,%d,%.6f\n",
                      pos_idx, alg_str, cfg.K, cfg.N, cfg.plies, cfg.w_winpct,
                      cfg.w_spread, cfg.spread_scale, test_result.wall_ms,
                      gold_idx, gold_result.pick_idx, utility_loss);
            }
          }
        }
      }
    }
    fflush(out);

    if ((pos_idx + 1) % 5 == 0 || pos_idx == num_positions - 1) {
      clock_gettime(CLOCK_MONOTONIC, &t1);
      double elapsed = (double)(t1.tv_sec - t0.tv_sec) +
                       (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
      double per_pos = elapsed / (pos_idx + 1);
      double remaining = per_pos * (num_positions - pos_idx - 1);
      printf("  pos %d/%d done (gold wall %.1fs)  elapsed %.0fs  "
             "ETA %.0fs\n",
             pos_idx + 1, num_positions, gold_result.wall_ms / 1000.0,
             elapsed, remaining);
    }
  }

  fclose(out);

  // Cleanup
  prng_destroy(prng);
  move_list_destroy(scratch_ml);
  move_list_destroy(cand_list);
  game_destroy(scratch);
  game_destroy(game);
  win_pct_destroy(win_pcts);
  error_stack_destroy(err);
  config_destroy(config);
  for (int i = 0; i < num_positions; i++) {
    free(cgps[i]);
  }

  printf("Wrote sweep CSV to %s\n", out_path);
}

// Dump markdown showing, for each of the first N positions, the top-25
// candidates by equity. Marks STATIC's pick (rank 0) and gold's pick (rank
// from gold_picks file). Helpful for sanity-checking that STATIC's low
// agreement with gold isn't an artifact.
void test_dump_position_rankings(void) {
  setbuf(stdout, NULL);
  const char *positions_path = getenv("INNERSWEEP_POSITIONS");
  if (!positions_path || !*positions_path) {
    positions_path = "/tmp/gamepairbai/run5_disagree.csv";
  }
  const char *gold_picks_path = getenv("INNERSWEEP_GOLD_PICKS");
  if (!gold_picks_path || !*gold_picks_path) {
    gold_picks_path = "/tmp/gamepairbai/gold_picks.csv";
  }
  const char *out_path = getenv("INNERSWEEP_DUMP_OUT");
  if (!out_path || !*out_path) {
    out_path = "/tmp/gamepairbai/position_rankings.md";
  }
  int max_pos = 100;
  const char *e = getenv("INNERSWEEP_MAX_POSITIONS");
  if (e && *e) {
    max_pos = atoi(e);
  }

  char *cgps[MAX_POSITIONS];
  int num_positions = load_positions(positions_path, cgps, max_pos);

  // Load gold picks (CSV with pos_idx,gold_pick).
  int gold_picks[MAX_POSITIONS];
  for (int i = 0; i < MAX_POSITIONS; i++) {
    gold_picks[i] = -1;
  }
  FILE *gp = fopen(gold_picks_path, "r");
  if (!gp) {
    log_fatal("could not open gold picks file: %s", gold_picks_path);
    return;
  }
  char gpline[256];
  fgets(gpline, sizeof(gpline), gp); // header
  while (fgets(gpline, sizeof(gpline), gp)) {
    int p, gpk;
    if (sscanf(gpline, "%d,%d", &p, &gpk) == 2 && p < MAX_POSITIONS) {
      gold_picks[p] = gpk;
    }
  }
  fclose(gp);

  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -rit true -s1 equity -s2 equity "
      "-r1 all -r2 all -numplays 25 -plies 2 -threads 1");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  ErrorStack *err = error_stack_create();
  Game *game = game_duplicate(config_get_game(config));
  MoveList *cand_list = move_list_create(MAX_GOLD_K);

  FILE *out = fopen(out_path, "w");
  if (!out) {
    log_fatal("cannot open %s", out_path);
    return;
  }
  fprintf(out, "# Position rankings (top-25 by equity)\n\n");
  fprintf(out, "STATIC eval picks rank 0 in every position. Gold's pick is "
               "marked. %d positions.\n\n",
          num_positions);

  StringBuilder *sb = string_builder_create();
  StringBuilder *rsb = string_builder_create();

  for (int pos_idx = 0; pos_idx < num_positions; pos_idx++) {
    game_load_cgp(game, cgps[pos_idx], err);
    if (!error_stack_is_empty(err)) {
      error_stack_print_and_reset(err);
      continue;
    }

    const MoveGenArgs gen_args = {
        .game = game,
        .move_list = cand_list,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_EQUITY,
        .override_kwg = NULL,
        .thread_index = 0,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&gen_args);
    move_list_sort_moves(cand_list);
    int num = move_list_get_count(cand_list);

    int player_on_turn = game_get_player_on_turn_index(game);
    const Rack *rack = player_get_rack(game_get_player(game, player_on_turn));
    string_builder_clear(rsb);
    string_builder_add_rack(rsb, rack, game_get_ld(game), false);

    fprintf(out, "## Position %d (rack `%s`, P%d to move)\n\n",
            pos_idx, string_builder_peek(rsb), player_on_turn + 1);
    fprintf(out, "Gold's pick: rank **%d** of %d candidates. STATIC's pick: rank 0.\n\n",
            gold_picks[pos_idx], num);
    fprintf(out, "| Rank | Move | Marker |\n");
    fprintf(out, "|---|---|---|\n");
    for (int i = 0; i < num && i < MAX_GOLD_K; i++) {
      const Move *m = move_list_get_move(cand_list, i);
      string_builder_clear(sb);
      string_builder_add_move(sb, game_get_board(game), m, game_get_ld(game),
                              true);
      const char *marker = "";
      if (i == 0 && i == gold_picks[pos_idx]) {
        marker = "**STATIC + GOLD**";
      } else if (i == 0) {
        marker = "**STATIC**";
      } else if (i == gold_picks[pos_idx]) {
        marker = "**GOLD**";
      }
      fprintf(out, "| %d | `%s` | %s |\n", i, string_builder_peek(sb),
              marker);
    }
    fprintf(out, "\n<details><summary>CGP</summary>\n\n```\n%s\n```\n</details>\n\n",
            cgps[pos_idx]);
  }

  string_builder_destroy(sb);
  string_builder_destroy(rsb);
  fclose(out);
  move_list_destroy(cand_list);
  game_destroy(game);
  error_stack_destroy(err);
  config_destroy(config);
  for (int i = 0; i < num_positions; i++) {
    free(cgps[i]);
  }
  printf("Wrote position rankings to %s (%d positions)\n", out_path,
         num_positions);
}

// Debug the STARTER opening position: load CGP, run gold sim, and for two
// candidates of interest log a few rollouts' move sequences. Output to a
// markdown file so we can see how the opp plays after our STARTER placement.
void test_debug_starter(void) {
  setbuf(stdout, NULL);
  const char *cgp = getenv("DEBUG_STARTER_CGP");
  if (!cgp || !*cgp) {
    cgp = "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 AERRSTT/BEMOOU? 0/0 0";
  }
  const char *out_path = getenv("DEBUG_STARTER_OUT");
  if (!out_path || !*out_path) {
    out_path = "/tmp/gamepairbai/debug_starter.md";
  }
  int gold_N = 1000;
  const char *e = getenv("DEBUG_STARTER_N");
  if (e && *e) {
    gold_N = atoi(e);
  }
  int log_rollouts = 6;
  e = getenv("DEBUG_STARTER_LOG_ROLLOUTS");
  if (e && *e) {
    log_rollouts = atoi(e);
  }
  int plies = 6;
  e = getenv("DEBUG_STARTER_PLIES");
  if (e && *e) {
    plies = atoi(e);
  }

  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -rit true -s1 equity -s2 equity "
      "-r1 all -r2 all -numplays 25 -plies 2 -threads 1");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  ErrorStack *err = error_stack_create();
  WinPct *win_pcts =
      win_pct_create(config_get_data_paths(config), DEFAULT_WIN_PCT, err);
  if (!error_stack_is_empty(err)) {
    error_stack_print_and_reset(err);
    return;
  }
  Game *game = game_duplicate(config_get_game(config));
  Game *scratch = game_duplicate(config_get_game(config));
  MoveList *cand_list = move_list_create(MAX_GOLD_K);
  MoveList *scratch_ml = move_list_create(1);
  XoshiroPRNG *prng = prng_create(0xDEB06);

  game_load_cgp(game, cgp, err);
  if (!error_stack_is_empty(err)) {
    error_stack_print_and_reset(err);
    return;
  }

  // Generate + sort candidates.
  const MoveGenArgs gen_args = {
      .game = game,
      .move_list = cand_list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&gen_args);
  move_list_sort_moves(cand_list);
  const int num_cand = move_list_get_count(cand_list);

  const int player_idx = game_get_player_on_turn_index(game);
  const bool plies_are_odd = (plies % 2) != 0;

  FILE *out = fopen(out_path, "w");
  if (!out) {
    log_fatal("cannot open %s", out_path);
    return;
  }
  fprintf(out, "# Debug: STARTER opening rollouts\n\n");
  fprintf(out, "CGP: `%s`\n\nN=%d rollouts per candidate, plies=%d. "
               "First %d rollouts logged per candidate.\n\n",
          cgp, gold_N, plies, log_rollouts);

  StringBuilder *sb = string_builder_create();

  // For each candidate, run N rollouts. Aggregate. Log first M.
  typedef struct {
    Move move;
    double avg_wpct;
    double avg_spread;
    int n;
  } CandStat;
  CandStat *stats = malloc_or_die(sizeof(CandStat) * num_cand);

  fprintf(out, "## Per-candidate summary\n\n");
  fprintf(out, "| Rank | Candidate | avg_wpct | avg_spread |\n");
  fprintf(out, "|---|---|---|---|\n");

  // Collect detailed rollout traces (only first log_rollouts each).
  // We'll write them after the summary.
  StringBuilder *traces = string_builder_create();
  string_builder_add_string(traces, "## Sample rollouts (first ");
  char num_buf[32];
  snprintf(num_buf, sizeof(num_buf), "%d", log_rollouts);
  string_builder_add_string(traces, num_buf);
  string_builder_add_string(traces, " per candidate)\n\n");

  for (int k = 0; k < num_cand; k++) {
    const Move *cand = move_list_get_move(cand_list, k);
    stats[k].move = *cand;
    double total_w = 0.0, total_s = 0.0;

    string_builder_clear(sb);
    string_builder_add_move(sb, game_get_board(game), cand, game_get_ld(game),
                            true);
    string_builder_add_string(traces, "### Rank ");
    snprintf(num_buf, sizeof(num_buf), "%d", k);
    string_builder_add_string(traces, num_buf);
    string_builder_add_string(traces, ": `");
    string_builder_add_string(traces, string_builder_peek(sb));
    string_builder_add_string(traces, "`\n\n");
    string_builder_add_string(traces, "| Roll | Plies | Spread (us-opp) | wpct |\n");
    string_builder_add_string(traces, "|---|---|---|---|\n");

    for (int n = 0; n < gold_N; n++) {
      game_copy(scratch, game);
      game_seed(scratch, prng_next(prng));
      game_set_backup_mode(scratch, BACKUP_MODE_OFF);

      // For logging: capture move strings.
      StringBuilder *ply_log = NULL;
      if (n < log_rollouts) {
        ply_log = string_builder_create();
      }

      play_move(cand, scratch, NULL);
      for (int ply = 0; ply < plies; ply++) {
        if (game_over(scratch)) {
          break;
        }
        const Move *best = get_top_equity_move(scratch, 0, scratch_ml);
        if (ply_log) {
          if (ply > 0) {
            string_builder_add_string(ply_log, " → ");
          }
          StringBuilder *msb = string_builder_create();
          string_builder_add_move(msb, game_get_board(scratch), best,
                                  game_get_ld(scratch), true);
          string_builder_add_string(ply_log, string_builder_peek(msb));
          string_builder_destroy(msb);
        }
        play_move(best, scratch, NULL);
      }

      const int spread = equity_to_int(player_get_score(
                             game_get_player(scratch, player_idx))) -
                         equity_to_int(player_get_score(
                             game_get_player(scratch, 1 - player_idx)));
      total_s += (double)spread;
      double wpct;
      if (game_over(scratch)) {
        wpct = (spread > 0) ? 1.0 : (spread == 0) ? 0.5 : 0.0;
      } else {
        int lookup_spread = spread;
        if (!plies_are_odd) {
          lookup_spread = -lookup_spread;
        }
        unsigned int unseen =
            bag_get_letters(game_get_bag(scratch)) +
            rack_get_total_letters(player_get_rack(
                game_get_player(scratch, 1 - player_idx)));
        if (unseen == 0) {
          wpct = (spread > 0) ? 1.0 : (spread == 0) ? 0.5 : 0.0;
        } else {
          wpct = (double)win_pct_get(win_pcts, lookup_spread, unseen);
          if (!plies_are_odd) {
            wpct = 1.0 - wpct;
          }
        }
      }
      total_w += wpct;

      if (ply_log) {
        snprintf(num_buf, sizeof(num_buf), "| %d | ", n);
        string_builder_add_string(traces, num_buf);
        string_builder_add_string(traces, string_builder_peek(ply_log));
        snprintf(num_buf, sizeof(num_buf), " | %+d | %.3f |\n", spread, wpct);
        string_builder_add_string(traces, num_buf);
        string_builder_destroy(ply_log);
      }
    }

    stats[k].avg_wpct = total_w / gold_N;
    stats[k].avg_spread = total_s / gold_N;
    stats[k].n = gold_N;

    fprintf(out, "| %d | `%s` | %.4f | %+.1f |\n", k,
            string_builder_peek(sb), stats[k].avg_wpct, stats[k].avg_spread);
    string_builder_add_string(traces, "\n");
    fflush(out);
  }

  fprintf(out, "\n%s", string_builder_peek(traces));
  fclose(out);
  string_builder_destroy(traces);
  string_builder_destroy(sb);
  free(stats);
  prng_destroy(prng);
  move_list_destroy(scratch_ml);
  move_list_destroy(cand_list);
  game_destroy(scratch);
  game_destroy(game);
  win_pct_destroy(win_pcts);
  error_stack_destroy(err);
  config_destroy(config);
  printf("Wrote debug to %s\n", out_path);
}

// For each disagreement position, run gold once (sorted candidate list) and
// extract (a) STATIC eval's loss = gold's pick utility minus top-equity move
// (= candidate 0 after sort) utility, and (b) whether gold's top pick equals
// STATIC's pick. Writes a per-position CSV plus prints aggregate stats.
void test_static_vs_gold(void) {
  setbuf(stdout, NULL);
  const char *positions_path = getenv("INNERSWEEP_POSITIONS");
  if (!positions_path || !*positions_path) {
    positions_path = "/tmp/gamepairbai/run5_disagree.csv";
  }
  const char *out_path = getenv("STATIC_VS_GOLD_OUT");
  if (!out_path || !*out_path) {
    out_path = "/tmp/gamepairbai/static_vs_gold.csv";
  }
  int gold_K = 25, gold_N = 10000, gold_plies = 6;
  const char *e;
  if ((e = getenv("INNERSWEEP_GOLD_K"))) gold_K = atoi(e);
  if ((e = getenv("INNERSWEEP_GOLD_N"))) gold_N = atoi(e);
  if ((e = getenv("INNERSWEEP_GOLD_PLIES"))) gold_plies = atoi(e);
  int max_pos = MAX_POSITIONS;
  if ((e = getenv("INNERSWEEP_MAX_POSITIONS"))) max_pos = atoi(e);

  const InnerSimConfig gold_cfg = {
      .K = gold_K, .N = gold_N, .plies = gold_plies,
      .w_winpct = 1.0, .w_spread = 0.2, .spread_scale = 100.0,
      .algorithm = INNER_ALG_ROUND_ROBIN,
  };

  char *cgps[MAX_POSITIONS];
  int num_positions = load_positions(positions_path, cgps, max_pos);
  printf("Loaded %d positions. Gold: K=%d N=%d plies=%d.\n", num_positions,
         gold_K, gold_N, gold_plies);

  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -rit true -s1 equity -s2 equity "
      "-r1 all -r2 all -numplays 25 -plies 2 -threads 1");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  ErrorStack *err = error_stack_create();
  WinPct *win_pcts =
      win_pct_create(config_get_data_paths(config), DEFAULT_WIN_PCT, err);
  if (!error_stack_is_empty(err)) {
    error_stack_print_and_reset(err);
    return;
  }
  Game *game = game_duplicate(config_get_game(config));
  Game *scratch = game_duplicate(config_get_game(config));
  MoveList *cand_list = move_list_create(MAX_GOLD_K);
  MoveList *scratch_ml = move_list_create(1);
  XoshiroPRNG *prng = prng_create(0xC0FFEEULL);

  FILE *out = fopen(out_path, "w");
  if (!out) { log_fatal("cannot open %s", out_path); return; }
  fprintf(out, "pos_idx,gold_pick_idx,num_cand,gold_pick_util,"
               "static_pick_util,static_loss,agree\n");
  fflush(out);

  InnerSimResult gold_result;
  double total_loss = 0.0;
  int n_agree = 0, n_used = 0;
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  for (int pos_idx = 0; pos_idx < num_positions; pos_idx++) {
    game_load_cgp(game, cgps[pos_idx], err);
    if (!error_stack_is_empty(err)) {
      error_stack_print_and_reset(err);
      continue;
    }
    run_inner_sim(game, &gold_cfg, win_pcts, prng, cand_list, scratch,
                  scratch_ml, &gold_result);
    if (gold_result.num_candidates < 2) continue;
    const double gp_util =
        gold_result.candidates[gold_result.pick_idx].blended_utility;
    // STATIC eval's pick = top-equity move = candidate 0 (since cand_list
    // is sorted by equity descending after run_inner_sim's sort call).
    const double sp_util = gold_result.candidates[0].blended_utility;
    const double loss = gp_util - sp_util;
    const int agree = (gold_result.pick_idx == 0) ? 1 : 0;
    fprintf(out, "%d,%d,%d,%.6f,%.6f,%.6f,%d\n", pos_idx,
            gold_result.pick_idx, gold_result.num_candidates, gp_util, sp_util,
            loss, agree);
    fflush(out);
    total_loss += loss;
    n_agree += agree;
    n_used++;
    if ((pos_idx + 1) % 5 == 0 || pos_idx == num_positions - 1) {
      clock_gettime(CLOCK_MONOTONIC, &t1);
      double elapsed = (double)(t1.tv_sec - t0.tv_sec) +
                       (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
      double eta = elapsed / (pos_idx + 1) * (num_positions - pos_idx - 1);
      printf("  pos %d/%d done (%.0fs elapsed, ETA %.0fs)\n", pos_idx + 1,
             num_positions, elapsed, eta);
    }
  }
  fclose(out);

  printf("\n=== STATIC eval vs gold (%d positions) ===\n", n_used);
  printf("  Agreement: %d/%d (%.1f%%)\n", n_agree, n_used,
         100.0 * n_agree / n_used);
  printf("  Mean STATIC loss: %.5f\n", total_loss / n_used);
  printf("Wrote per-position data to %s\n", out_path);

  prng_destroy(prng);
  move_list_destroy(scratch_ml);
  move_list_destroy(cand_list);
  game_destroy(scratch);
  game_destroy(game);
  win_pct_destroy(win_pcts);
  error_stack_destroy(err);
  config_destroy(config);
  for (int i = 0; i < num_positions; i++) free(cgps[i]);
}
