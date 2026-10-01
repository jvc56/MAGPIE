#include "blocking_setup_study_test.h"

#include "../src/compat/ctime.h"
#include "../src/def/bai_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/blocking_setup_params.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_args.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/stats.h"
#include "../src/ent/thread_control.h"
#include "../src/impl/blocking_setup.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/play_chooser.h"
#include "../src/impl/sim_nomination.h"
#include "../src/impl/simmer.h"
#include "../src/str/move_string.h"
#include "../src/str/rack_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "blocking_setup_options.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Studies for blocking/setup candidate nomination and adjusted static play.
// On-demand tests "bsstudy:<stage>[:key=value...]", sharded with
// worker=<i>:workers=<n>:
//
//   pools  For each position of in= (a bsgen positions file), the root
//          candidate pools of the arms in arms= and, for each, the
//          static pool of exactly the same size; one top-two selection sim
//          per distinct pool (select_ms=, plies=, no-PAT static rollouts,
//          one seed shared by every pool), and one independent round-robin
//          reference sim (ref_ms=) over the union of every pool. Arms:
//            A   static + blocking + setup + exchanges
//            B   PAT + blocking + setup + exchanges (checks ranked by static
//                equity: PAT only replaces the base list)
//            Bp  as B, but the checks rank by PAT equity (PAT and blocking
//                both act on one score)
//            C   static + PAT + blocking + setup + exchanges
//            S   static + exchanges (no checks)
//          Counts: static=, patn=, block=, setup=, exch=, exchmargin=,
//          universe=. Needs pat= and params=<path to a .bsp file>.
//          Writes <out>.pools.csv, <out>.nominees.csv, <out>.select.csv
//          and <out>.refs.csv; sims=0 writes only the nominees (an untimed
//          nomination audit).
//   games  Game pairs between two players (a= and b=, each "static", "pat",
//          "adjusted" or "sim": a PlayChooser simulating sim_cands=
//          candidates sim_plies= plies deep for sim_ms= per move, no-PAT
//          static rollouts): both games of a pair use one seed, so each seat
//          draws the same tiles, and the players swap seats. "adjusted"
//          plays argmax(static equity + blocking + setup adjustments) over
//          the top universe= static moves and exch= exchanges, with weights
//          from params= (its bins give conditional policies) and racks=
//          sampled racks (default: the file's teacher_racks); player b
//          uses params_b=, universe_b= and z_b= when given; z=<z> races
//          the candidates (blocking_setup_checker_choose; 0, the default,
//          measures all) in batches of batch= racks. Writes
//          <out>.games.csv and per-decision timing <out>.moves.csv.
enum {
  BSS_MAX_ARMS = 8,
  BSS_MAX_POOL = 512,
  BSS_LINE_CAPACITY = 65536,
  BSS_MOVE_LIST_CAPACITY = 200000,
  BSS_POOL_CAPACITY = 1024,
  BSS_MAX_TURNS = 256,
  BSS_DEFAULT_SELECT_MS = 15000,
  BSS_DEFAULT_REF_MS = 60000,
  BSS_DEFAULT_PLIES = 4,
  BSS_DEFAULT_MIN_PLAY_ITERATIONS = 30,
  BSS_DEFAULT_COUNT = 25,
  BSS_DEFAULT_EXCHANGES = 5,
  BSS_DEFAULT_EXCHANGE_MARGIN = 35,
  BSS_DEFAULT_UNIVERSE = 60,
  BSS_DEFAULT_PAIRS = 10,
};

static Config *bss_config_create(const BSOptions *options) {
  const char *lexicon = bs_options_require(options, "lex");
  const char *pat = bs_options_get(options, "pat", NULL);
  char *settings = get_formatted_string(
      "set -lex %s -leaves %s -wmp %s -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -threads 1%s%s",
      lexicon, bs_options_get(options, "leaves", lexicon),
      bs_options_get(options, "wmp", "true"), pat != NULL ? " -pat " : "",
      pat != NULL ? pat : "");
  Config *config = config_create_or_die(settings);
  free(settings);
  return config;
}

static BlockingSetupParams *bss_params_create_from(const char *path);

static BlockingSetupParams *bss_params_create(const BSOptions *options) {
  return bss_params_create_from(bs_options_get(options, "params", NULL));
}

static BlockingSetupParams *bss_params_create_from(const char *path) {
  if (path == NULL) {
    return NULL;
  }
  ErrorStack *error_stack = error_stack_create();
  char *contents = get_string_from_file(path, error_stack);
  BlockingSetupParams *params = NULL;
  if (error_stack_is_empty(error_stack)) {
    params =
        blocking_setup_params_create_from_string(path, contents, error_stack);
  }
  free(contents);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("cannot load blocking/setup parameters %s", path);
  }
  error_stack_destroy(error_stack);
  return params;
}

static double bss_now_ms(void) {
  return (double)ctimer_monotonic_ns() / 1000000.0;
}

// Splits a bsgen positions row; returns false for the header.
static bool bss_parse_position(char *line, long *game_idx, char **cgp) {
  line[strcspn(line, "\r\n")] = '\0';
  char *end = NULL;
  *game_idx = strtol(line, &end, 10);
  if (end == line) {
    return false;
  }
  char *cursor = line;
  for (int field_idx = 0; field_idx < 5; field_idx++) {
    cursor = strchr(cursor, ',');
    if (cursor == NULL) {
      return false;
    }
    cursor++;
  }
  *cgp = cursor;
  return true;
}

static char *bss_move_text(const Game *game, const Move *move) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_ucgi_move(sb, move, game_get_board(game),
                               game_get_ld(game));
  char *text = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  return text;
}

typedef struct BSSPool {
  char name[32];
  MoveList *moves;
  // Index of the pool whose identical selection sim this one reuses, or -1.
  int same_as;
  char *chosen;
  uint64_t iterations;
  double wall_ms;
} BSSPool;

static int bss_find(const MoveList *list, const Move *move) {
  for (int move_idx = 0; move_idx < move_list_get_count(list); move_idx++) {
    if (compare_moves_without_equity(move, move_list_get_move(list, move_idx),
                                     true) == -1) {
      return move_idx;
    }
  }
  return -1;
}

static bool bss_same_moves(const MoveList *a, const MoveList *b) {
  if (move_list_get_count(a) != move_list_get_count(b)) {
    return false;
  }
  for (int move_idx = 0; move_idx < move_list_get_count(a); move_idx++) {
    if (bss_find(b, move_list_get_move(a, move_idx)) < 0) {
      return false;
    }
  }
  return true;
}

static void bss_simulate(const Config *config, const Game *game,
                         const MoveList *moves, int plies, double seconds,
                         bool reference, uint64_t seed, SimCtx **sim_ctx,
                         SimResults *results) {
  ThreadControl *control = thread_control_create();
  thread_control_set_status(control, THREAD_CONTROL_STATUS_STARTED);
  const int count = move_list_get_count(moves);
  SimArgs args;
  sim_args_fill(
      plies, moves, count, NULL, config_get_win_pcts(config), NULL, control,
      game, false, false, 1, 0, count, plies, seed, UINT64_C(1000000000000000),
      BSS_DEFAULT_MIN_PLAY_ITERATIONS, 0.0, BAI_THRESHOLD_NONE, seconds,
      reference ? BAI_SAMPLING_RULE_ROUND_ROBIN : BAI_SAMPLING_RULE_TOP_TWO_IDS,
      -1.0, 1.0, 0.0, 100.0, false, NULL, &args);
  // The comparison holds rollouts fixed: no PAT, whatever nominated the
  // roots.
  args.pat_rollout_disabled = true;
  ErrorStack *errors = error_stack_create();
  simulate(&args, sim_ctx, results, errors);
  assert(error_stack_is_empty(errors));
  error_stack_destroy(errors);
  thread_control_destroy(control);
}

static void bss_arm_settings(const BSOptions *options, const char *arm,
                             const BlockingSetupParams *params, uint64_t seed,
                             SimNominationSettings *settings) {
  const int count =
      (int)bs_options_get_long(options, "static", BSS_DEFAULT_COUNT);
  const int pat_count =
      (int)bs_options_get_long(options, "patn", BSS_DEFAULT_COUNT);
  const int blocking =
      (int)bs_options_get_long(options, "block", BSS_DEFAULT_COUNT);
  const int setup =
      (int)bs_options_get_long(options, "setup", BSS_DEFAULT_COUNT);
  *settings = (SimNominationSettings){
      .exchange_quota =
          (int)bs_options_get_long(options, "exch", BSS_DEFAULT_EXCHANGES),
      .exchange_margin = int_to_equity((int)bs_options_get_long(
          options, "exchmargin", BSS_DEFAULT_EXCHANGE_MARGIN)),
      .universe_count =
          (int)bs_options_get_long(options, "universe", BSS_DEFAULT_UNIVERSE),
      .params = params,
      .seed = seed,
  };
  if (strings_equal(arm, "A")) {
    settings->static_count = count;
    settings->blocking_count = blocking;
    settings->setup_count = setup;
  } else if (strings_equal(arm, "B") || strings_equal(arm, "Bp")) {
    settings->pat_count = pat_count;
    settings->blocking_count = blocking;
    settings->setup_count = setup;
    settings->check_base_pat = strings_equal(arm, "Bp");
  } else if (strings_equal(arm, "C")) {
    settings->static_count = count;
    settings->pat_count = pat_count;
    settings->blocking_count = blocking;
    settings->setup_count = setup;
  } else if (strings_equal(arm, "S")) {
    settings->static_count = count;
  } else {
    log_fatal("unknown arm %s", arm);
  }
}

static void bss_write_nominees(FILE *out, long game_idx, const char *arm,
                               const Game *game, const SimNominator *nominator,
                               const MoveList *list) {
  for (int idx = 0; idx < move_list_get_count(list); idx++) {
    const SimNominee *nominee = sim_nominator_get_nominee(nominator, idx);
    const Move *move = move_list_get_move(list, idx);
    char *text = bss_move_text(game, move);
    Rack leave;
    blocking_setup_candidate_leave(game, move, &leave);
    StringBuilder *leave_text = string_builder_create();
    string_builder_add_rack(leave_text, &leave, game_get_ld(game), false);
    const BlockingSetupResult *check = &nominee->check;
    (void)fprintf(
        out,
        "%ld,%s,%s,%s,%d,%d,%s,%u,%d,%d,%d,%d,%.3f,%.3f,%d,%.6f,"
        "%.6f,%.3f,%.3f,%.6f,%.6f,%.6f,%.6f\n",
        game_idx, arm, text,
        move_get_type(move) == GAME_EVENT_EXCHANGE ? "exchange" : "place",
        move_get_tiles_played(move), equity_to_int(move_get_score(move)),
        string_builder_peek(leave_text), nominee->sources, nominee->static_rank,
        nominee->pat_rank, nominee->blocking_rank, nominee->setup_rank,
        equity_to_double(nominee->static_equity),
        equity_to_double(nominee->pat_equity), nominee->checked,
        check->blocking_delta, check->setup_delta,
        equity_to_double(nominee->blocking_adjustment),
        equity_to_double(nominee->setup_adjustment), check->pass_reply_mean,
        check->candidate_reply_mean, check->pass_followup_mean,
        check->candidate_followup_mean);
    string_builder_destroy(leave_text);
    free(text);
  }
}

// Every play of the last sim of a pool: its selection statistics.
static void bss_write_select(FILE *out, long game_idx, const char *pool,
                             const Game *game, const SimResults *results) {
  const Move *best = sim_results_get_best_move(results);
  for (int play_idx = 0; play_idx < sim_results_get_number_of_plays(results);
       play_idx++) {
    const SimmedPlay *play = sim_results_get_simmed_play(results, play_idx);
    const Stat *wp = simmed_play_get_win_pct_stat(play);
    const Stat *eq = simmed_play_get_equity_stat(play);
    const Move *move = simmed_play_get_move(play);
    char *text = bss_move_text(game, move);
    (void)fprintf(out, "%ld,%s,%s,%.9f,%.9f,%.6f,%.6f,%d\n", game_idx, pool,
                  text, stat_get_mean(wp), stat_get_sem(wp), stat_get_mean(eq),
                  stat_get_sem(eq),
                  compare_moves_without_equity(move, best, true) == -1);
    free(text);
  }
}

static void bss_pools(const BSOptions *options) {
  Config *config = bss_config_create(options);
  BlockingSetupParams *params = bss_params_create(options);
  ErrorStack *errors = error_stack_create();
  config_load_win_pcts(config, errors);
  assert(error_stack_is_empty(errors));
  error_stack_destroy(errors);
  const long worker = bs_options_get_long(options, "worker", 0);
  const long workers = bs_options_get_long(options, "workers", 1);
  const uint64_t seed = bs_options_get_u64(options, "seed", 0);
  const int plies =
      (int)bs_options_get_long(options, "plies", BSS_DEFAULT_PLIES);
  const double select_seconds =
      (double)bs_options_get_long(options, "select_ms", BSS_DEFAULT_SELECT_MS) /
      1000.0;
  const double ref_seconds =
      (double)bs_options_get_long(options, "ref_ms", BSS_DEFAULT_REF_MS) /
      1000.0;
  // sims=0 writes the pools and their provenance without simulating.
  const bool run_sims = bs_options_get_long(options, "sims", 1) != 0;
  StringSplitter *arms =
      split_string(bs_options_get(options, "arms", "A,B,C"), ',', true);
  const int num_arms = string_splitter_get_number_of_items(arms);
  assert(num_arms >= 1 && num_arms <= BSS_MAX_ARMS);
  const char *out_path = bs_options_require(options, "out");
  char *path = get_formatted_string("%s.pools.csv", out_path);
  FILE *pools_out = fopen_or_die(path, "w");
  free(path);
  path = get_formatted_string("%s.nominees.csv", out_path);
  FILE *nominees_out = fopen_or_die(path, "w");
  free(path);
  path = get_formatted_string("%s.refs.csv", out_path);
  FILE *refs_out = fopen_or_die(path, "w");
  free(path);
  path = get_formatted_string("%s.select.csv", out_path);
  FILE *select_out = fopen_or_die(path, "w");
  free(path);
  (void)fprintf(select_out, "game,pool,move,sim_wp,sim_wp_sem,sim_eq,"
                            "sim_eq_sem,chosen\n");
  (void)fprintf(pools_out, "game,pool,count,same_as,chosen,iterations,"
                           "wall_ms,nominate_ms,check_ms\n");
  (void)fprintf(nominees_out,
                "game,pool,move,type,tiles_played,score,leave,sources,"
                "static_rank,pat_rank,blocking_rank,setup_rank,static_eq,"
                "pat_eq,checked,blocking_delta,setup_delta,blocking_adj,"
                "setup_adj,pass_reply,cand_reply,pass_followup,"
                "cand_followup\n");
  (void)fprintf(refs_out, "game,move,ref_wp,ref_wp_sem,ref_eq,ref_eq_sem,"
                          "iterations\n");
  SimNominator *nominator = sim_nominator_create();
  SimResults *results = sim_results_create(0.0);
  SimCtx *sim_ctx = NULL;
  BSSPool pools[2 * BSS_MAX_ARMS];
  for (int pool_idx = 0; pool_idx < 2 * BSS_MAX_ARMS; pool_idx++) {
    pools[pool_idx].moves = move_list_create(BSS_MAX_POOL);
    pools[pool_idx].chosen = NULL;
  }
  MoveList *reference = move_list_create(BSS_MAX_POOL);
  FILE *in = fopen_or_die(bs_options_require(options, "in"), "r");
  char *line = malloc_or_die(BSS_LINE_CAPACITY);
  long input_idx = 0;
  while (fgets(line, BSS_LINE_CAPACITY, in) != NULL) {
    long game_idx = 0;
    char *cgp = NULL;
    if (!bss_parse_position(line, &game_idx, &cgp)) {
      continue;
    }
    if (input_idx++ % workers != worker) {
      continue;
    }
    char *command = get_formatted_string("cgp %s", cgp);
    load_and_exec_config_or_die(config, command);
    free(command);
    const Game *game = config_get_game(config);
    const uint64_t position_seed = bs_mix(seed ^ bs_mix((uint64_t)game_idx));
    // Pools: each arm, then each arm's equal-size static control.
    int num_pools = 0;
    double nominate_ms[2 * BSS_MAX_ARMS];
    double check_ms[2 * BSS_MAX_ARMS];
    for (int arm_idx = 0; arm_idx < num_arms; arm_idx++) {
      const char *arm = string_splitter_get_item(arms, arm_idx);
      SimNominationSettings settings;
      bss_arm_settings(options, arm, params, position_seed, &settings);
      ErrorStack *error_stack = error_stack_create();
      const double start = bss_now_ms();
      const MoveList *list =
          sim_nominator_nominate(nominator, game, &settings, error_stack);
      if (!error_stack_is_empty(error_stack)) {
        error_stack_print_and_reset(error_stack);
        log_fatal("nomination failed for arm %s", arm);
      }
      error_stack_destroy(error_stack);
      BSSPool *pool = &pools[num_pools];
      nominate_ms[num_pools] = bss_now_ms() - start;
      check_ms[num_pools] =
          (double)sim_nominator_get_timing(nominator)->check_ns / 1000000.0;
      (void)snprintf(pool->name, sizeof(pool->name), "%s", arm);
      move_list_reset(pool->moves);
      for (int idx = 0; idx < move_list_get_count(list); idx++) {
        move_list_add_move_to_sorted_list(pool->moves,
                                          move_list_get_move(list, idx));
      }
      bss_write_nominees(nominees_out, game_idx, arm, game, nominator, list);
      num_pools++;
    }
    for (int arm_idx = 0; arm_idx < num_arms; arm_idx++) {
      const int count = move_list_get_count(pools[arm_idx].moves);
      SimNominationSettings settings = {.static_count = count};
      ErrorStack *error_stack = error_stack_create();
      const MoveList *list =
          sim_nominator_nominate(nominator, game, &settings, error_stack);
      assert(error_stack_is_empty(error_stack));
      error_stack_destroy(error_stack);
      BSSPool *pool = &pools[num_pools];
      nominate_ms[num_pools] = 0.0;
      check_ms[num_pools] = 0.0;
      (void)snprintf(pool->name, sizeof(pool->name), "static_%s",
                     string_splitter_get_item(arms, arm_idx));
      bss_write_nominees(nominees_out, game_idx, pool->name, game, nominator,
                         list);
      move_list_reset(pool->moves);
      for (int idx = 0; idx < move_list_get_count(list); idx++) {
        move_list_add_move_to_sorted_list(pool->moves,
                                          move_list_get_move(list, idx));
      }
      num_pools++;
    }
    // Selection: one sim per distinct pool.
    for (int pool_idx = 0; pool_idx < num_pools && run_sims; pool_idx++) {
      BSSPool *pool = &pools[pool_idx];
      pool->same_as = -1;
      for (int previous = 0; previous < pool_idx; previous++) {
        if (pools[previous].same_as < 0 &&
            bss_same_moves(pool->moves, pools[previous].moves)) {
          pool->same_as = previous;
          break;
        }
      }
      free(pool->chosen);
      if (pool->same_as >= 0) {
        const BSSPool *original = &pools[pool->same_as];
        pool->chosen = string_duplicate(original->chosen);
        pool->iterations = original->iterations;
        pool->wall_ms = original->wall_ms;
      } else if (move_list_get_count(pool->moves) == 1) {
        pool->chosen = bss_move_text(game, move_list_get_move(pool->moves, 0));
        pool->iterations = 0;
        pool->wall_ms = 0.0;
      } else {
        const double start = bss_now_ms();
        bss_simulate(config, game, pool->moves, plies, select_seconds, false,
                     position_seed ^ UINT64_C(0x5e1ec7), &sim_ctx, results);
        pool->wall_ms = bss_now_ms() - start;
        pool->iterations = sim_results_get_iteration_count(results);
        pool->chosen = bss_move_text(game, sim_results_get_best_move(results));
        bss_write_select(select_out, game_idx, pool->name, game, results);
      }
      (void)fprintf(pools_out, "%ld,%s,%d,%s,%s,%llu,%.1f,%.3f,%.3f\n",
                    game_idx, pool->name, move_list_get_count(pool->moves),
                    pool->same_as >= 0 ? pools[pool->same_as].name : "",
                    pool->chosen, (unsigned long long)pool->iterations,
                    pool->wall_ms, nominate_ms[pool_idx], check_ms[pool_idx]);
    }
    // Reference over the union, with independent randomness.
    move_list_reset(reference);
    for (int pool_idx = 0; pool_idx < num_pools; pool_idx++) {
      for (int idx = 0; idx < move_list_get_count(pools[pool_idx].moves);
           idx++) {
        const Move *move = move_list_get_move(pools[pool_idx].moves, idx);
        if (bss_find(reference, move) < 0) {
          move_list_add_move_to_sorted_list(reference, move);
        }
      }
    }
    if (run_sims && move_list_get_count(reference) > 1) {
      bss_simulate(config, game, reference, plies, ref_seconds, true,
                   position_seed ^ UINT64_C(0x7ef0e7ce), &sim_ctx, results);
      for (int play_idx = 0;
           play_idx < sim_results_get_number_of_plays(results); play_idx++) {
        const SimmedPlay *play = sim_results_get_simmed_play(results, play_idx);
        const Stat *wp = simmed_play_get_win_pct_stat(play);
        const Stat *eq = simmed_play_get_equity_stat(play);
        char *text = bss_move_text(game, simmed_play_get_move(play));
        (void)fprintf(
            refs_out, "%ld,%s,%.9f,%.9f,%.6f,%.6f,%llu\n", game_idx, text,
            stat_get_mean(wp), stat_get_sem(wp), stat_get_mean(eq),
            stat_get_sem(eq),
            (unsigned long long)sim_results_get_iteration_count(results));
        free(text);
      }
    }
    (void)fflush(pools_out);
    (void)fflush(nominees_out);
    (void)fflush(refs_out);
    (void)fflush(select_out);
  }
  free(line);
  (void)fclose(in);
  for (int pool_idx = 0; pool_idx < 2 * BSS_MAX_ARMS; pool_idx++) {
    move_list_destroy(pools[pool_idx].moves);
    free(pools[pool_idx].chosen);
  }
  move_list_destroy(reference);
  sim_ctx_destroy(sim_ctx);
  sim_results_destroy(results);
  sim_nominator_destroy(nominator);
  string_splitter_destroy(arms);
  (void)fclose(pools_out);
  (void)fclose(nominees_out);
  (void)fclose(refs_out);
  (void)fclose(select_out);
  blocking_setup_params_destroy(params);
  config_destroy(config);
}

typedef enum {
  BSS_PLAYER_STATIC,
  BSS_PLAYER_PAT,
  BSS_PLAYER_ADJUSTED,
  // A PlayChooser simulating its top candidates (no-PAT static rollouts)
  // for sim_ms per move.
  BSS_PLAYER_SIM,
} bss_player_t;

static bss_player_t bss_parse_player(const char *name) {
  if (strings_equal(name, "static")) {
    return BSS_PLAYER_STATIC;
  }
  if (strings_equal(name, "pat")) {
    return BSS_PLAYER_PAT;
  }
  if (strings_equal(name, "adjusted")) {
    return BSS_PLAYER_ADJUSTED;
  }
  if (strings_equal(name, "sim")) {
    return BSS_PLAYER_SIM;
  }
  log_fatal("unknown player %s", name);
  return BSS_PLAYER_STATIC;
}

typedef struct BSSPlayerState {
  MoveList *list;
  BlockingSetupSamples *samples;
  BlockingSetupChecker *checker;
  const BlockingSetupParams *params;
  int universe;
  int exchanges;
  Equity exchange_margin;
  int num_racks;
  BlockingSetupRaceSettings race;
  PlayChooser *chooser;
  Move *chosen;
} BSSPlayerState;

static void bss_generate(const Game *game, MoveList *list, bool disable_pat) {
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
      .disable_pat = disable_pat,
  };
  generate_moves(&args);
  move_list_sort_moves(list);
}

typedef struct BSSDecision {
  int candidate_racks;
  double movegen_ms;
  double check_ms;
  double total_ms;
  int universe;
  bool changed;
  bool checked;
} BSSDecision;

static int bss_lead(const Game *game) {
  const int on_turn = game_get_player_on_turn_index(game);
  return equity_to_int(player_get_score(game_get_player(game, on_turn)) -
                       player_get_score(game_get_player(game, 1 - on_turn)));
}

// The player's move for the position (an index into state->list).
static int bss_decide(BSSPlayerState *state, bss_player_t kind,
                      const Game *game, uint64_t seed, BSSDecision *decision) {
  const double start = bss_now_ms();
  bss_generate(game, state->list, kind != BSS_PLAYER_PAT);
  decision->movegen_ms = bss_now_ms() - start;
  decision->check_ms = 0.0;
  decision->universe = 0;
  decision->changed = false;
  decision->checked = false;
  decision->candidate_racks = 0;
  int choice = 0;
  const int bag = bag_get_letters(game_get_bag(game));
  if (kind == BSS_PLAYER_ADJUSTED && bag > 0 &&
      move_list_get_count(state->list) > 1) {
    double blocking_weight = 0.0;
    double setup_weight = 0.0;
    blocking_setup_params_get_weights(state->params, bag, bss_lead(game),
                                      &blocking_weight, &setup_weight);
    if (blocking_weight != 0.0 || setup_weight != 0.0) {
      const double check_start = bss_now_ms();
      blocking_setup_samples_deal(
          state->samples, game, state->num_racks,
          blocking_setup_params_get_teacher_partition(state->params),
          blocking_setup_params_get_teacher_condition_draws(state->params),
          seed);
      blocking_setup_checker_set_value(
          state->checker,
          blocking_setup_params_get_teacher_value(state->params));
      blocking_setup_checker_load(
          state->checker, game, state->samples,
          blocking_setup_params_get_teacher_followup_draws(state->params));
      const Equity top = move_get_equity(move_list_get_move(state->list, 0));
      const Move *universe[BSS_MAX_POOL];
      Equity base[BSS_MAX_POOL] = {0};
      int indices[BSS_MAX_POOL];
      int count = 0;
      int placements = 0;
      int exchanges = 0;
      for (int move_idx = 0;
           move_idx < move_list_get_count(state->list) &&
           (placements < state->universe || exchanges < state->exchanges);
           move_idx++) {
        const Move *move = move_list_get_move(state->list, move_idx);
        const bool is_exchange = move_get_type(move) == GAME_EVENT_EXCHANGE;
        if (move_get_type(move) == GAME_EVENT_PASS ||
            (is_exchange &&
             (exchanges >= state->exchanges ||
              move_get_equity(move) < top - state->exchange_margin)) ||
            (!is_exchange && placements >= state->universe)) {
          continue;
        }
        if (is_exchange) {
          exchanges++;
        } else {
          placements++;
        }
        universe[count] = move;
        base[count] = move_get_equity(move);
        indices[count] = move_idx;
        count++;
      }
      if (count > 0) {
        BlockingSetupRaceStats stats;
        const int pick = blocking_setup_checker_choose(
            state->checker, universe, base, count, blocking_weight,
            setup_weight, &state->race, &stats);
        choice = indices[pick];
        decision->candidate_racks = stats.candidate_racks;
      }
      decision->universe = count;
      decision->check_ms = bss_now_ms() - check_start;
      decision->checked = true;
      decision->changed = choice != 0;
    }
  }
  decision->total_ms = bss_now_ms() - start;
  return choice;
}

// The sim player's move, as an index into state->list (the static list).
static int bss_sim_decide(BSSPlayerState *state, Game *game,
                          BSSDecision *decision) {
  const double start = bss_now_ms();
  bss_generate(game, state->list, true);
  decision->movegen_ms = bss_now_ms() - start;
  decision->check_ms = 0.0;
  decision->universe = 0;
  decision->changed = false;
  decision->checked = false;
  decision->candidate_racks = 0;
  ErrorStack *error_stack = error_stack_create();
  play_chooser_choose_move(state->chooser, game, state->chosen, error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  int choice = 0;
  for (int move_idx = 0; move_idx < move_list_get_count(state->list);
       move_idx++) {
    if (compare_moves_without_equity(move_list_get_move(state->list, move_idx),
                                     state->chosen, true) == -1) {
      choice = move_idx;
      break;
    }
  }
  decision->checked = bag_get_letters(game_get_bag(game)) > 0;
  decision->changed = choice != 0;
  decision->total_ms = bss_now_ms() - start;
  decision->check_ms = decision->total_ms - decision->movegen_ms;
  return choice;
}

// A game's result for player a: win 1, tie 0.5, loss 0.
static double bss_game_score(int spread) {
  if (spread > 0) {
    return 1.0;
  }
  if (spread == 0) {
    return 0.5;
  }
  return 0.0;
}

// Player b's own option (key_b=) when given, else the shared one.
static const char *bss_player_key(const BSOptions *options, int player_idx,
                                  const char *key, const char *key_b) {
  return player_idx == 1 && bs_options_get(options, key_b, NULL) != NULL ? key_b
                                                                         : key;
}

static void bss_games(const BSOptions *options) {
  Config *config = bss_config_create(options);
  BlockingSetupParams *params = bss_params_create(options);
  // Player b may use other parameters (params_b=), e.g. another teacher_value.
  BlockingSetupParams *params_b =
      bss_params_create_from(bs_options_get(options, "params_b", NULL));
  const bss_player_t kinds[2] = {
      bss_parse_player(bs_options_require(options, "a")),
      bss_parse_player(bs_options_require(options, "b"))};
  const long pairs = bs_options_get_long(options, "pairs", BSS_DEFAULT_PAIRS);
  const long first_pair = bs_options_get_long(options, "first", 0);
  const long worker = bs_options_get_long(options, "worker", 0);
  const long workers = bs_options_get_long(options, "workers", 1);
  const uint64_t seed = bs_options_get_u64(options, "seed", 0);
  BSSPlayerState states[2];
  for (int player_idx = 0; player_idx < 2; player_idx++) {
    BSSPlayerState *state = &states[player_idx];
    state->list = move_list_create(BSS_MOVE_LIST_CAPACITY);
    state->params = player_idx == 1 && params_b != NULL ? params_b : params;
    state->universe = (int)bs_options_get_long(
        options, bss_player_key(options, player_idx, "universe", "universe_b"),
        BSS_DEFAULT_UNIVERSE);
    state->exchanges =
        (int)bs_options_get_long(options, "exch", BSS_DEFAULT_EXCHANGES);
    state->exchange_margin = int_to_equity((int)bs_options_get_long(
        options, "exchmargin", BSS_DEFAULT_EXCHANGE_MARGIN));
    state->num_racks = 0;
    state->chooser = NULL;
    state->chosen = NULL;
    if (kinds[player_idx] == BSS_PLAYER_SIM) {
      ErrorStack *error_stack = error_stack_create();
      config_load_win_pcts(config, error_stack);
      assert(error_stack_is_empty(error_stack));
      error_stack_destroy(error_stack);
      const PlayChooserStrategy strategy = {
          .pre_endgame_eval = PLAY_CHOOSER_EVAL_SIM,
          .endgame_eval = PLAY_CHOOSER_EVAL_STATIC,
          .fixed_seconds_per_move =
              (double)bs_options_get_long(options, "sim_ms", 100) / 1000.0,
          .sim_plies = (int)bs_options_get_long(options, "sim_plies", 2),
          .sim_max_candidates =
              (int)bs_options_get_long(options, "sim_cands", 15),
          .win_pcts = config_get_win_pcts(config),
          .num_threads = 1,
          .pat_rollout_disabled = true,
          .seed = bs_options_get_u64(options, "seed", 0) + (uint64_t)player_idx,
      };
      state->chooser = play_chooser_create(&strategy);
      state->chosen = move_create();
    }
    // z=0 (the default) measures every candidate in full; z > 0 races.
    state->race = (BlockingSetupRaceSettings){
        .batch_racks = (int)bs_options_get_long(options, "batch",
                                                BLOCKING_SETUP_RACE_BATCH),
        .min_racks = (int)bs_options_get_long(options, "batch",
                                              BLOCKING_SETUP_RACE_BATCH),
        .z = bs_options_get_double(
            options, bss_player_key(options, player_idx, "z", "z_b"), 0.0)};
    state->samples = NULL;
    state->checker = NULL;
    if (kinds[player_idx] == BSS_PLAYER_ADJUSTED) {
      if (state->params == NULL) {
        log_fatal("adjusted players need params=");
      }
      state->num_racks = (int)bs_options_get_long(
          options, "racks",
          blocking_setup_params_get_teacher_racks(state->params));
      state->samples =
          blocking_setup_samples_create(state->num_racks, BSS_POOL_CAPACITY);
      state->checker = blocking_setup_checker_create();
    }
  }
  const char *out_path = bs_options_require(options, "out");
  char *path = get_formatted_string("%s.games.csv", out_path);
  FILE *games_out = fopen_or_die(path, "w");
  free(path);
  path = get_formatted_string("%s.moves.csv", out_path);
  FILE *moves_out = fopen_or_die(path, "w");
  free(path);
  (void)fprintf(games_out, "pair,game,a_seat,a_score,b_score,a_spread,a_win,"
                           "turns\n");
  (void)fprintf(moves_out,
                "pair,game,turn,player,bag,lead,universe,checked,"
                "changed,movegen_ms,check_ms,total_ms,candidate_racks\n");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *game = config_get_game(config);
  for (long pair_idx = first_pair; pair_idx < first_pair + pairs; pair_idx++) {
    if (pair_idx % workers != worker) {
      continue;
    }
    const uint64_t pair_seed = bs_mix(seed ^ bs_mix((uint64_t)pair_idx));
    for (int game_in_pair = 0; game_in_pair < 2; game_in_pair++) {
      // Player a sits in seat game_in_pair; both games share the seed, so
      // each seat draws the same tiles in both.
      const int a_seat = game_in_pair;
      game_reset(game);
      game_seed(game, pair_seed);
      game_set_starting_player_index(game, (int)(pair_idx % 2));
      draw_starting_racks(game);
      int turn = 0;
      while (!game_over(game) && turn < BSS_MAX_TURNS) {
        const int seat = game_get_player_on_turn_index(game);
        const int player_idx = seat == a_seat ? 0 : 1;
        BSSDecision decision;
        const int bag = bag_get_letters(game_get_bag(game));
        const int lead = bss_lead(game);
        const int choice =
            kinds[player_idx] == BSS_PLAYER_SIM
                ? bss_sim_decide(&states[player_idx], game, &decision)
                : bss_decide(
                      &states[player_idx], kinds[player_idx], game,
                      bs_mix(pair_seed ^ (uint64_t)((turn * 2) + game_in_pair)),
                      &decision);
        (void)fprintf(moves_out,
                      "%ld,%d,%d,%c,%d,%d,%d,%d,%d,%.3f,%.3f,%.3f,%d\n",
                      pair_idx, game_in_pair, turn, player_idx == 0 ? 'a' : 'b',
                      bag, lead, decision.universe, decision.checked,
                      decision.changed, decision.movegen_ms, decision.check_ms,
                      decision.total_ms, decision.candidate_racks);
        play_move(move_list_get_move(states[player_idx].list, choice), game,
                  NULL);
        turn++;
      }
      const int a_score =
          equity_to_int(player_get_score(game_get_player(game, a_seat)));
      const int b_score =
          equity_to_int(player_get_score(game_get_player(game, 1 - a_seat)));
      const int spread = a_score - b_score;
      (void)fprintf(games_out, "%ld,%d,%d,%d,%d,%d,%.1f,%d\n", pair_idx,
                    game_in_pair, a_seat, a_score, b_score, spread,
                    bss_game_score(spread), turn);
      (void)fflush(games_out);
    }
    (void)fflush(moves_out);
  }
  for (int player_idx = 0; player_idx < 2; player_idx++) {
    move_list_destroy(states[player_idx].list);
    blocking_setup_samples_destroy(states[player_idx].samples);
    blocking_setup_checker_destroy(states[player_idx].checker);
    if (states[player_idx].chooser != NULL) {
      play_chooser_destroy(states[player_idx].chooser);
      move_destroy(states[player_idx].chosen);
    }
  }
  (void)fclose(games_out);
  (void)fclose(moves_out);
  blocking_setup_params_destroy(params);
  blocking_setup_params_destroy(params_b);
  config_destroy(config);
}

void blocking_setup_study_run_spec(const char *spec) {
  StringSplitter *fields = split_string(spec, ':', true);
  assert(string_splitter_get_number_of_items(fields) >= 1);
  BSOptions options;
  bs_options_parse(&options, fields);
  const char *stage = string_splitter_get_item(fields, 0);
  if (strings_equal(stage, "pools")) {
    bss_pools(&options);
  } else if (strings_equal(stage, "games")) {
    bss_games(&options);
  } else {
    log_fatal("bsstudy: unknown stage '%s'", stage);
  }
  bs_options_destroy(&options);
  string_splitter_destroy(fields);
}
