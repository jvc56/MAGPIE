#include "benchmark_sim_test.h"

#include "../src/compat/ctime.h"
#include "../src/def/bai_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/bai_result.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/stats.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/win_pct.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/simmer.h"
#include "../src/str/move_string.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// Player config for sim benchmark: ply count and time limit per turn.
typedef struct {
  int plies;
  double time_limit_secs;
} SimPlayerConfig;

// Accumulated per-player statistics across a benchmark run.
typedef struct {
  double total_time;
  double max_overrun;
  int num_turns;
  int num_overruns;
  uint64_t total_nodes;
} SimPlayerStats;

static void sim_player_label(const SimPlayerConfig *cfg, char *buf, size_t len) {
  if (cfg->time_limit_secs < 1.0) {
    (void)snprintf(buf, len, "%d-ply %.1fs", cfg->plies,
                   cfg->time_limit_secs);
  } else {
    (void)snprintf(buf, len, "%d-ply %.0fs", cfg->plies,
                   cfg->time_limit_secs);
  }
}

// Play a single turn using sim: generate moves, simulate, play best.
// Returns the wall-clock time for the turn.
// When the bag is empty, uses static eval (top equity move) instead of sim.
static double play_sim_turn(Config *config, const SimPlayerConfig *cfg,
                            uint64_t *nodes_out) {
  Game *game = config_get_game(config);
  *nodes_out = 0;

  Timer t;
  ctimer_start(&t);

  // When bag is empty, use static eval
  if (bag_get_letters(game_get_bag(game)) == 0) {
    Move *best = get_top_equity_move(game, 0, config_get_move_list(config));
    play_move(best, game, NULL);
    return ctimer_elapsed_seconds(&t);
  }

  // Generate candidate moves
  char *set_cmd =
      get_formatted_string("set -plies %d -tlim %g -numplays 15 "
                           "-scond none -it 1000000000 -threads 16",
                           cfg->plies, cfg->time_limit_secs);
  load_and_exec_config_or_die(config, set_cmd);
  free(set_cmd);
  load_and_exec_config_or_die(config, "gen");

  MoveList *move_list = config_get_move_list(config);
  int num_moves = move_list_get_count(move_list);

  if (num_moves <= 1) {
    // Only one move (or pass): just play it, no sim needed
    if (num_moves == 1) {
      play_move(move_list_get_move(move_list, 0), game, NULL);
    }
    return ctimer_elapsed_seconds(&t);
  }

  // Run simulation with time limit
  SimResults *sim_results = config_get_sim_results(config);
  error_code_t status =
      config_simulate_and_return_status(config, NULL, NULL, sim_results);
  assert(status == ERROR_STATUS_SUCCESS);

  *nodes_out = sim_results_get_node_count(sim_results);

  // Get best simmed play and play it
  const int num_simmed = sim_results_get_number_of_plays(sim_results);
  const SimmedPlay *best_play = NULL;
  for (int i = 0; i < num_simmed; i++) {
    const SimmedPlay *sp = sim_results_get_simmed_play(sim_results, i);
    if (!best_play ||
        stat_get_mean(simmed_play_get_win_pct_stat(sp)) >
            stat_get_mean(simmed_play_get_win_pct_stat(best_play))) {
      best_play = sp;
    }
  }
  assert(best_play);

  play_move(simmed_play_get_move(best_play), game, NULL);
  return ctimer_elapsed_seconds(&t);
}

// Play a full game from a fresh start. Player A uses cfg_a, player B uses
// cfg_b. player_a_idx determines which seat player A occupies.
// Returns spread from player A's perspective.
static int play_sim_game(Config *config, const SimPlayerConfig *cfg_a,
                         const SimPlayerConfig *cfg_b,
                         SimPlayerStats *stats_a, SimPlayerStats *stats_b,
                         int player_a_idx) {
  Game *game = config_get_game(config);

  while (game_get_game_end_reason(game) == GAME_END_REASON_NONE) {
    int on_turn = game_get_player_on_turn_index(game);
    bool is_a = (on_turn == player_a_idx);
    const SimPlayerConfig *cfg = is_a ? cfg_a : cfg_b;
    SimPlayerStats *stats = is_a ? stats_a : stats_b;

    uint64_t nodes = 0;
    double elapsed = play_sim_turn(config, cfg, &nodes);

    stats->total_time += elapsed;
    stats->num_turns++;
    stats->total_nodes += nodes;

    // Check overrun (only for turns that used sim, i.e. nodes > 0)
    if (nodes > 0) {
      double overrun = elapsed - cfg->time_limit_secs;
      if (overrun > 0) {
        stats->num_overruns++;
        if (overrun > stats->max_overrun) {
          stats->max_overrun = overrun;
        }
      }
    }
  }

  int score_a =
      equity_to_int(player_get_score(game_get_player(game, player_a_idx)));
  int score_b =
      equity_to_int(player_get_score(game_get_player(game, 1 - player_a_idx)));
  return score_a - score_b;
}

typedef struct {
  SimPlayerConfig a;
  SimPlayerConfig b;
  int num_games;
  uint64_t seed;
} SimBenchmarkConfig;

static void run_sim_benchmark(const SimBenchmarkConfig *bench) {
  log_set_level(LOG_WARN);

  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all");

  load_and_exec_config_or_die(config, "new");

  char label_a[64];
  char label_b[64];
  sim_player_label(&bench->a, label_a, sizeof(label_a));
  sim_player_label(&bench->b, label_b, sizeof(label_b));

  SimPlayerStats stats_a = {0};
  SimPlayerStats stats_b = {0};
  int total_a_spread = 0;
  int total_b_spread = 0;
  int a_wins = 0;
  int b_wins = 0;
  int ties = 0;

  printf("\n");
  printf("==============================================================\n");
  printf("  Sim Benchmark: %d game pairs\n", bench->num_games);
  printf("  A: %s\n", label_a);
  printf("  B: %s\n", label_b);
  printf("==============================================================\n\n");

  Game *game = config_get_game(config);

  for (int i = 0; i < bench->num_games; i++) {
    // Game 1: A is player 0, B is player 1
    game_reset(game);
    game_seed(game, bench->seed + (uint64_t)i);
    draw_starting_racks(game);

    Game *saved_game = game_duplicate(game);

    SimPlayerStats turn_stats_a1 = {0};
    SimPlayerStats turn_stats_b1 = {0};
    int spread_1 =
        play_sim_game(config, &bench->a, &bench->b, &turn_stats_a1,
                       &turn_stats_b1, 0);

    // Game 2: B is player 0, A is player 1 (from same starting position)
    game_copy(game, saved_game);

    SimPlayerStats turn_stats_a2 = {0};
    SimPlayerStats turn_stats_b2 = {0};
    int spread_2 =
        play_sim_game(config, &bench->a, &bench->b, &turn_stats_a2,
                       &turn_stats_b2, 1);

    game_destroy(saved_game);

    // spread_1 is from A's perspective in game 1
    // spread_2 is from A's perspective in game 2
    int pair_advantage = spread_1 + spread_2;
    total_a_spread += spread_1 + spread_2;
    total_b_spread += -spread_1 + -spread_2;

    // Accumulate stats
    stats_a.total_time += turn_stats_a1.total_time + turn_stats_a2.total_time;
    stats_a.num_turns += turn_stats_a1.num_turns + turn_stats_a2.num_turns;
    stats_a.num_overruns +=
        turn_stats_a1.num_overruns + turn_stats_a2.num_overruns;
    stats_a.total_nodes +=
        turn_stats_a1.total_nodes + turn_stats_a2.total_nodes;
    if (turn_stats_a1.max_overrun > stats_a.max_overrun) {
      stats_a.max_overrun = turn_stats_a1.max_overrun;
    }
    if (turn_stats_a2.max_overrun > stats_a.max_overrun) {
      stats_a.max_overrun = turn_stats_a2.max_overrun;
    }

    stats_b.total_time += turn_stats_b1.total_time + turn_stats_b2.total_time;
    stats_b.num_turns += turn_stats_b1.num_turns + turn_stats_b2.num_turns;
    stats_b.num_overruns +=
        turn_stats_b1.num_overruns + turn_stats_b2.num_overruns;
    stats_b.total_nodes +=
        turn_stats_b1.total_nodes + turn_stats_b2.total_nodes;
    if (turn_stats_b1.max_overrun > stats_b.max_overrun) {
      stats_b.max_overrun = turn_stats_b1.max_overrun;
    }
    if (turn_stats_b2.max_overrun > stats_b.max_overrun) {
      stats_b.max_overrun = turn_stats_b2.max_overrun;
    }

    if (pair_advantage > 0) {
      a_wins++;
    } else if (pair_advantage < 0) {
      b_wins++;
    } else {
      ties++;
    }

    printf("  Pair %3d: G1=%+4d, G2=%+4d, adv=%+4d  "
           "(A: %.2fs %" PRIu64 "n, B: %.2fs %" PRIu64 "n)\n",
           i + 1, spread_1, spread_2, pair_advantage,
           turn_stats_a1.total_time + turn_stats_a2.total_time,
           turn_stats_a1.total_nodes + turn_stats_a2.total_nodes,
           turn_stats_b1.total_time + turn_stats_b2.total_time,
           turn_stats_b1.total_nodes + turn_stats_b2.total_nodes);
    (void)fflush(stdout);
  }

  printf("\n==============================================================\n");
  printf("  RESULTS: %d game pairs (%d games total)\n", bench->num_games,
         bench->num_games * 2);
  printf("  A (%s) total spread: %+d (avg %+.1f per pair)\n", label_a,
         total_a_spread, (double)total_a_spread / bench->num_games);
  printf("  B (%s) total spread: %+d (avg %+.1f per pair)\n", label_b,
         total_b_spread, (double)total_b_spread / bench->num_games);
  printf("  Pair wins: A=%d, B=%d, ties=%d\n", a_wins, b_wins, ties);
  printf("\n");
  printf("  Player A (%s):\n", label_a);
  printf("    Total time: %.2fs across %d turns\n", stats_a.total_time,
         stats_a.num_turns);
  printf("    Total nodes: %" PRIu64 "\n", stats_a.total_nodes);
  printf("    Overruns: %d (max %.3fms)\n", stats_a.num_overruns,
         stats_a.max_overrun * 1000.0);
  printf("\n");
  printf("  Player B (%s):\n", label_b);
  printf("    Total time: %.2fs across %d turns\n", stats_b.total_time,
         stats_b.num_turns);
  printf("    Total nodes: %" PRIu64 "\n", stats_b.total_nodes);
  printf("    Overruns: %d (max %.3fms)\n", stats_b.num_overruns,
         stats_b.max_overrun * 1000.0);
  printf("==============================================================\n");

  config_destroy(config);
}

static const SimBenchmarkConfig sim_configs[] = {
    // 2-ply vs 3-ply at 10s per turn
    {{2, 10.0}, {3, 10.0}, 100, 42},
};

void test_benchmark_sim(void) {
  int n = (int)(sizeof(sim_configs) / sizeof(sim_configs[0]));
  for (int i = 0; i < n; i++) {
    run_sim_benchmark(&sim_configs[i]);
  }
}
