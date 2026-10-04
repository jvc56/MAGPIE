// Oracle-eval pilot for simmed inference. See siminf_oracle_test.h.

#include "siminf_oracle_test.h"

#include "../src/def/game_defs.h"
#include "../src/def/move_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/inference_results.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/win_pct.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/simmer.h"
#include "../src/util/io_util.h"
#include "simmedinf_benchmark.h"
#include "test_util.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ── Configuration ────────────────────────────────────────────────────────

#define ORACLE_LEX "CSW21"

// Arm move-selection: shorter than the 15s live-match budget so the pilot's
// position-generation phase (which runs both arms at every candidate
// position) stays fast to iterate on. Scale this up to match the live-match
// budget once the harness itself is validated.
#define ARM_NUM_PLAYS 15
#define ARM_NUM_PLIES 4
#define ARM_SIM_BUDGET_S 10.0

// Position-generation window: capture the first turn in [MIN,MAX] unseen
// tiles with a full bag (>= RACK_SIZE), the same "can_infer" floor
// simmedinf_benchmark.c's play_game uses for simmed inference.
#define GEN_MIN_UNSEEN 8
#define GEN_MAX_UNSEEN 30
#define GEN_TARGET_POSITIONS 5
#define GEN_MAX_ATTEMPTS 100000
#define POSITIONS_FILE "notes/siminf_positions/oracle_pilot.txt"

// Oracle continuation and paired resampling.
#define ORACLE_RESAMPLES 3
#define ORACLE_ENDGAME_PLIES 20
#define ORACLE_ENDGAME_SOFT_S 1.0
#define ORACLE_ENDGAME_HARD_S 2.0
#define ORACLE_SEED_STRIDE 1000000ULL
#define RESULTS_FILE "notes/siminf_positions/oracle_pilot_results.csv"

// ── Move <-> compact field serialization ──────────────────────────────────
//
// The corpus only needs to replay two already-chosen candidate moves later,
// not re-derive them, so this round-trips a Move's raw fields directly
// (type,row,col,dir,tiles_played,tiles_length,score,tile0,tile1,...) rather
// than a human-readable move string -- simpler and avoids depending on
// move-string parsing matching move-string formatting exactly.

static void move_to_field(const Move *m, char *buf, size_t buf_size) {
  int n = snprintf(buf, buf_size, "%d,%d,%d,%d,%d,%d,%d",
                   (int)move_get_type(m), move_get_row_start(m),
                   move_get_col_start(m), move_get_dir(m),
                   move_get_tiles_played(m), move_get_tiles_length(m),
                   (int)move_get_score(m));
  const int len = move_get_tiles_length(m);
  for (int i = 0; i < len && n > 0 && (size_t)n < buf_size; i++) {
    n += snprintf(buf + n, buf_size - (size_t)n, ",%d",
                 (int)move_get_tile(m, i));
  }
}

static void move_from_field(const char *field, Move *m) {
  char buf[512];
  snprintf(buf, sizeof(buf), "%s", field);
  char *save = NULL;
  char *tok = strtok_r(buf, ",", &save);
  const int type = atoi(tok);
  tok = strtok_r(NULL, ",", &save);
  const int row = atoi(tok);
  tok = strtok_r(NULL, ",", &save);
  const int col = atoi(tok);
  tok = strtok_r(NULL, ",", &save);
  const int dir = atoi(tok);
  tok = strtok_r(NULL, ",", &save);
  const int played = atoi(tok);
  tok = strtok_r(NULL, ",", &save);
  const int length = atoi(tok);
  tok = strtok_r(NULL, ",", &save);
  const int score_raw = atoi(tok);
  MachineLetter tiles[MOVE_MAX_TILES];
  for (int i = 0; i < length; i++) {
    tok = strtok_r(NULL, ",", &save);
    tiles[i] = (MachineLetter)atoi(tok);
  }
  move_set_all_except_equity(m, tiles, 0, length - 1, (Equity)score_raw, row,
                             col, played, dir, (game_event_t)type);
  move_set_equity(m, (Equity)score_raw);
}

// True move identity (not compare_moves_without_equity, which is a sort
// comparator, not an equality test): same type, and for tile placements or
// exchanges, the same squares/tiles.
static bool moves_are_equal(const Move *a, const Move *b) {
  if (move_get_type(a) != move_get_type(b)) {
    return false;
  }
  if (move_get_type(a) == GAME_EVENT_PASS) {
    return true;
  }
  if (move_get_row_start(a) != move_get_row_start(b) ||
      move_get_col_start(a) != move_get_col_start(b) ||
      move_get_dir(a) != move_get_dir(b) ||
      move_get_tiles_length(a) != move_get_tiles_length(b)) {
    return false;
  }
  const int n = move_get_tiles_length(a);
  for (int i = 0; i < n; i++) {
    if (move_get_tile(a, i) != move_get_tile(b, i)) {
      return false;
    }
  }
  return true;
}

// ── Position generation ─────────────────────────────────────────────────

void test_generate_siminf_oracle_positions(void) {
  setbuf(stdout, NULL);
  Config *config = config_create_or_die("set -lex " ORACLE_LEX
                                        " -threads 1 -s1 score -s2 score");
  load_and_exec_config_or_die(config, "new");
  Game *game = config_get_game(config);
  MoveList *gen_move_list = move_list_create(1);
  MoveList *arm_move_list = move_list_create(ARM_NUM_PLAYS);
  ErrorStack *error_stack = error_stack_create();
  ErrorStack *wp_err = error_stack_create();
  WinPct *win_pcts =
      win_pct_create(config_get_data_paths(config), DEFAULT_WIN_PCT, wp_err);
  if (!error_stack_is_empty(wp_err)) {
    error_stack_print_and_reset(wp_err);
    log_fatal("failed to load win_pcts");
  }
  error_stack_destroy(wp_err);
  ThreadControl *tc = config_get_thread_control(config);
  InferenceResults *inference_results = inference_results_create(NULL);
  SimResults *sim_results = sim_results_create(0.0);
  SimCtx *sim_ctx = NULL;

  FILE *fp = fopen_or_die(POSITIONS_FILE, "we");
  int found = 0;
  int examined = 0;
  const uint64_t base_seed = 1;
  for (uint64_t attempt = 0;
       found < GEN_TARGET_POSITIONS && attempt < GEN_MAX_ATTEMPTS;
       attempt++) {
    game_reset(game);
    game_seed(game, base_seed + attempt);
    draw_starting_racks(game);

    Game *game_before_prev = game_duplicate(game);
    Move saved_prev_move;
    memset(&saved_prev_move, 0, sizeof(saved_prev_move));
    int prev_player_index = -1;
    bool have_capture = false;

    while (!game_over(game)) {
      game_destroy(game_before_prev);
      game_before_prev = game_duplicate(game);
      prev_player_index = game_get_player_on_turn_index(game);

      const Move *move = get_top_equity_move(game, gen_move_list);
      move_copy(&saved_prev_move, move);
      play_move(move, game, NULL);

      if (game_over(game)) {
        break;
      }
      const int bag_tiles = bag_get_letters(game_get_bag(game));
      const int unseen = tiles_unseen(game);
      if (bag_tiles >= RACK_SIZE && unseen >= GEN_MIN_UNSEEN &&
          unseen <= GEN_MAX_UNSEEN) {
        have_capture = true;
        break;
      }
    }

    if (!have_capture) {
      game_destroy(game_before_prev);
      continue;
    }
    examined++;

    // NOINF arm: plain sim, no inference.
    Game *g_noinf = game_duplicate(game);
    const Move *m_noinf =
        play_sim_turn(g_noinf, arm_move_list, sim_results, &sim_ctx, win_pcts,
                      tc, ARM_NUM_PLIES, ARM_SIM_BUDGET_S, NULL, false, NULL,
                      error_stack);
    Move move_noinf;
    move_copy(&move_noinf, m_noinf);
    game_destroy(g_noinf);

    // SIMINF arm: simmed inference on the opponent's previous move, then sim
    // with that precomputed distribution.
    double infer_elapsed = 0.0;
    const bool infer_ok = run_simmed_inference(
        game_before_prev, win_pcts, tc, &saved_prev_move, prev_player_index,
        inference_results, error_stack, &infer_elapsed);
    Game *g_siminf = game_duplicate(game);
    const Move *m_siminf = play_sim_turn(
        g_siminf, arm_move_list, sim_results, &sim_ctx, win_pcts, tc,
        ARM_NUM_PLIES, ARM_SIM_BUDGET_S, infer_ok ? inference_results : NULL,
        infer_ok, NULL, error_stack);
    Move move_siminf;
    move_copy(&move_siminf, m_siminf);
    game_destroy(g_siminf);

    game_destroy(game_before_prev);

    if (moves_are_equal(&move_noinf, &move_siminf)) {
      printf("[siminfgenoracle] agree (examined %d, %" PRIu64
            " attempts, found %d/%d)\n",
            examined, attempt + 1, found, GEN_TARGET_POSITIONS);
      continue; // agreement -- no comparative information
    }

    char *cgp = game_get_cgp(game, true);
    char field_a[256];
    char field_b[256];
    move_to_field(&move_noinf, field_a, sizeof(field_a));
    move_to_field(&move_siminf, field_b, sizeof(field_b));
    fprintf(fp, "%s -lex %s|%s|%s\n", cgp, ORACLE_LEX, field_a, field_b);
    fflush(fp);
    free(cgp);
    found++;
    printf("[siminfgenoracle] found %d/%d (examined %d disagreement checks, "
          "%" PRIu64 " self-play attempts)\n",
          found, GEN_TARGET_POSITIONS, examined, attempt + 1);
  }
  fclose(fp);
  printf("[siminfgenoracle] done: %d/%d positions -> %s (%d positions "
        "examined for disagreement)\n",
        found, GEN_TARGET_POSITIONS, POSITIONS_FILE, examined);

  sim_ctx_destroy(sim_ctx);
  sim_results_destroy(sim_results);
  inference_results_destroy(inference_results);
  move_list_destroy(arm_move_list);
  move_list_destroy(gen_move_list);
  win_pct_destroy(win_pcts);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// ── Oracle continuation ──────────────────────────────────────────────────
//
// Best-equity playout to bag-empty, then a real endgame solve (mirrors the
// production convention in src/impl/peg.c's peg_nested_endgame_value: lead +
// eg_val is the on-turn player's final spread). Returns the final spread
// from mover_idx's perspective.

static int oracle_continue(Game *g, int mover_idx, MoveList *playout_ml,
                          EndgameCtx **eg_ctx, EndgameResults *eg_results,
                          ThreadControl *tc) {
  while (!game_over(g) && !bag_is_empty(game_get_bag(g))) {
    const Move *move = get_top_equity_move(g, playout_ml);
    play_move(move, g, NULL);
  }
  if (game_over(g)) {
    return equity_to_int(player_get_score(game_get_player(g, mover_idx))) -
           equity_to_int(
               player_get_score(game_get_player(g, 1 - mover_idx)));
  }

  const int on_turn = game_get_player_on_turn_index(g);
  const int32_t lead =
      equity_to_int(player_get_score(game_get_player(g, on_turn))) -
      equity_to_int(player_get_score(game_get_player(g, 1 - on_turn)));

  const EndgameArgs ea = {
      .thread_control = tc,
      .game = g,
      .plies = ORACLE_ENDGAME_PLIES,
      .initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
      .num_threads = 1,
      .use_heuristics = true,
      .num_top_moves = 1,
      .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
      .soft_time_limit = ORACLE_ENDGAME_SOFT_S,
      .hard_time_limit = ORACLE_ENDGAME_HARD_S,
  };
  endgame_results_reset(eg_results);
  endgame_solve_inline(eg_ctx, &ea, eg_results);
  const int32_t eg_val =
      endgame_results_get_value(eg_results, ENDGAME_RESULT_BEST);
  const int32_t on_turn_final = lead + eg_val;
  return (on_turn == mover_idx) ? on_turn_final : -on_turn_final;
}

// Scores one candidate move at `pos` via `resamples` paired draws: reseed,
// draw a fresh uniformly random (unconditioned -- no known leave to respect)
// opponent rack, force-play the candidate, then oracle_continue. Writes each
// resample's spread to spreads_out (size >= resamples) so the caller can pair
// them against another move's spreads drawn from the same seed_base.
static void oracle_score_move(const Game *pos, const Move *candidate,
                              int mover_idx, int resamples,
                              uint64_t seed_base, MoveList *playout_ml,
                              EndgameCtx **eg_ctx, EndgameResults *eg_results,
                              ThreadControl *tc, int *spreads_out) {
  const int opp_idx = 1 - mover_idx;
  for (int r = 0; r < resamples; r++) {
    Game *g = game_duplicate(pos);
    game_seed(g, seed_base + (uint64_t)r);
    set_random_rack(g, opp_idx, NULL);
    Move cand;
    move_copy(&cand, candidate);
    play_move(&cand, g, NULL);
    spreads_out[r] =
        oracle_continue(g, mover_idx, playout_ml, eg_ctx, eg_results, tc);
    game_destroy(g);
  }
}

static bool parse_position_line(char *line, char **cgp_out, char **move_a_out,
                                char **move_b_out) {
  size_t len = strlen(line);
  while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
    line[--len] = '\0';
  }
  char *bar1 = strchr(line, '|');
  if (!bar1) {
    return false;
  }
  *bar1 = '\0';
  char *bar2 = strchr(bar1 + 1, '|');
  if (!bar2) {
    return false;
  }
  *bar2 = '\0';
  *cgp_out = line;
  *move_a_out = bar1 + 1;
  *move_b_out = bar2 + 1;
  return true;
}

// ── Oracle eval over the corpus ──────────────────────────────────────────

void test_siminf_oracle_eval(void) {
  setbuf(stdout, NULL);
  Config *config = config_create_or_die("set -lex " ORACLE_LEX
                                        " -threads 1 -s1 score -s2 score");
  ThreadControl *tc = config_get_thread_control(config);
  MoveList *playout_ml = move_list_create(1);
  EndgameCtx *eg_ctx = NULL;
  EndgameResults *eg_results = endgame_results_create();

  FILE *in = fopen_or_die(POSITIONS_FILE, "re");
  FILE *out = fopen_or_die(RESULTS_FILE, "we");
  fprintf(out, "position_idx,mean_D,mean_spread_noinf,mean_spread_siminf\n");

  enum { MAX_POSITIONS = 10000 };
  static double diffs[MAX_POSITIONS];
  int n_positions = 0;

  char line[4096];
  while (n_positions < MAX_POSITIONS && fgets(line, sizeof(line), in)) {
    char *cgp_field;
    char *move_a_field;
    char *move_b_field;
    if (!parse_position_line(line, &cgp_field, &move_a_field,
                             &move_b_field)) {
      continue;
    }

    char load_cmd[4096];
    snprintf(load_cmd, sizeof(load_cmd), "cgp %s", cgp_field);
    load_and_exec_config_or_die(config, load_cmd);
    const Game *pos = config_get_game(config);
    const int mover_idx = game_get_player_on_turn_index(pos);

    Move move_noinf;
    Move move_siminf;
    move_from_field(move_a_field, &move_noinf);
    move_from_field(move_b_field, &move_siminf);

    int spreads_noinf[ORACLE_RESAMPLES];
    int spreads_siminf[ORACLE_RESAMPLES];
    const uint64_t seed_base =
        ORACLE_SEED_STRIDE * (uint64_t)(n_positions + 1);
    oracle_score_move(pos, &move_noinf, mover_idx, ORACLE_RESAMPLES,
                      seed_base, playout_ml, &eg_ctx, eg_results, tc,
                      spreads_noinf);
    oracle_score_move(pos, &move_siminf, mover_idx, ORACLE_RESAMPLES,
                      seed_base, playout_ml, &eg_ctx, eg_results, tc,
                      spreads_siminf);

    double sum_noinf = 0.0;
    double sum_siminf = 0.0;
    double sum_d = 0.0;
    for (int r = 0; r < ORACLE_RESAMPLES; r++) {
      sum_noinf += spreads_noinf[r];
      sum_siminf += spreads_siminf[r];
      sum_d += (spreads_siminf[r] - spreads_noinf[r]);
    }
    const double mean_noinf = sum_noinf / ORACLE_RESAMPLES;
    const double mean_siminf = sum_siminf / ORACLE_RESAMPLES;
    const double d_i = sum_d / ORACLE_RESAMPLES;
    diffs[n_positions] = d_i;

    fprintf(out, "%d,%.3f,%.3f,%.3f\n", n_positions, d_i, mean_noinf,
           mean_siminf);
    fflush(out);
    printf("[siminforacle] position %d: D=%+.2f (noinf=%+.2f siminf=%+.2f)\n",
          n_positions, d_i, mean_noinf, mean_siminf);
    n_positions++;
  }
  fclose(in);
  fclose(out);

  double mean_d = 0.0;
  for (int i = 0; i < n_positions; i++) {
    mean_d += diffs[i];
  }
  mean_d /= n_positions;
  double sum_sq = 0.0;
  for (int i = 0; i < n_positions; i++) {
    const double dev = diffs[i] - mean_d;
    sum_sq += dev * dev;
  }
  const double sd_d = n_positions > 1 ? sqrt(sum_sq / (n_positions - 1)) : 0.0;
  const double se_d = n_positions > 0 ? sd_d / sqrt(n_positions) : 0.0;
  const double t_stat = se_d > 0.0 ? mean_d / se_d : 0.0;

  printf("\n=== Oracle eval summary (SIMINF - NOINF, per-position paired "
        "spread diff) ===\n");
  printf("positions: %d, resamples/position: %d\n", n_positions,
        ORACLE_RESAMPLES);
  printf("mean D: %+.3f, sd: %.3f, se: %.3f, t: %.3f\n", mean_d, sd_d, se_d,
        t_stat);
  printf("(two-sided 5%%/80%% power sizing: n ~= 7.85 * sd^2 / delta^2)\n");
  const double deltas[] = {1.0, 2.0, 5.0, 10.0};
  for (size_t i = 0; i < sizeof(deltas) / sizeof(deltas[0]); i++) {
    const double delta = deltas[i];
    const double n_needed = 7.85 * sd_d * sd_d / (delta * delta);
    printf("  detect true effect >= %.1f spread/position: n ~= %.0f "
          "positions\n",
          delta, n_needed);
  }

  endgame_ctx_destroy(eg_ctx);
  endgame_results_destroy(eg_results);
  move_list_destroy(playout_ml);
  config_destroy(config);
}
