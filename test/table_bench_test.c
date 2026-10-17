#include "table_bench_test.h"

#include "../src/compat/ctime.h"
#include "../src/impl/config.h"
#include "../src/impl/move_gen.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// What each of the three derived tables a birdtest player may use is worth
// to the games a contributor plays, so that a contributor short of disk
// space keeps the ones that matter most (contribute_choose_tables). Plays the
// same CSW24 games under each set of tables -- the moves, and so the move
// generations, are the same under every set; only the time differs -- with a
// static player and with a 2-ply simmer, timing the games alone (the tables
// load when the config is made, before the clock starts). Run by
// scripts/bench_tables.sh; TABLE_BENCH_STATIC_GAMES and TABLE_BENCH_SIM_GAMES
// set how many games each row plays.

typedef struct TableBenchSet {
  const char *label;
  const char *flags;
} TableBenchSet;

static const TableBenchSet table_bench_sets[] = {
    {"none", "-wmp false -rit false -wit false"},
    {"WIT", "-wmp false -rit false -wit true"},
    {"WMP", "-wmp true -rit false -wit false"},
    {"WMP+WIT", "-wmp true -rit false -wit true"},
    {"WMP+RIT", "-wmp true -rit true -wit false"},
    {"WMP+WIT+RIT", "-wmp true -rit true -wit true"},
};

static int table_bench_env_int(const char *name, int fallback) {
  const char *value = getenv(name);
  if (!value || !*value) {
    return fallback;
  }
  char *end = NULL;
  const long parsed = strtol(value, &end, 10);
  return (end && *end == '\0' && parsed > 0 && parsed <= INT_MAX) ? (int)parsed
                                                                  : fallback;
}

static void table_bench_row(const char *player, const char *player_flags,
                            const TableBenchSet *set, int games) {
  char *set_command = get_formatted_string(
      "set -lex CSW24 -threads 4 -ritmmap true -gp false %s %s", player_flags,
      set->flags);
  Config *config = config_create_or_die(set_command);
  free(set_command);
  char *autoplay_command =
      get_formatted_string("autoplay games %d -seed 7 -gp false", games);
  gen_start_counting_movegens();
  const uint64_t before = gen_get_movegen_count();
  Timer timer;
  ctimer_start(&timer);
  load_and_exec_config_or_die(config, autoplay_command);
  ctimer_stop(&timer);
  const uint64_t movegens = gen_get_movegen_count() - before;
  gen_stop_counting_movegens();
  free(autoplay_command);
  const double seconds = ctimer_elapsed_seconds(&timer);
  printf("%-8s %-12s %6d %12llu %10.2f %12.0f %10.4f\n", player, set->label,
         games, (unsigned long long)movegens, seconds,
         seconds > 0 ? (double)movegens / seconds : 0.0,
         games > 0 ? seconds / games : 0.0);
  (void)fflush(stdout);
  config_destroy(config);
}

void test_table_bench(void) {
  const int static_games =
      table_bench_env_int("TABLE_BENCH_STATIC_GAMES", 4000);
  const int sim_games = table_bench_env_int("TABLE_BENCH_SIM_GAMES", 20);
  const char *static_flags =
      "-s1 equity -s2 equity -r1 best -r2 best -numplays 1 -plies 0";
  const char *sim_flags =
      "-s1 equity -s2 equity -r1 all -r2 all -numplays 8 -plies 2 -iter 200 "
      "-minp 50 -sinfer false -mtmode igp";
  printf("%-8s %-12s %6s %12s %10s %12s %10s\n", "player", "tables", "games",
         "movegens", "seconds", "movegens/s", "s/game");
  const int num_sets =
      (int)(sizeof(table_bench_sets) / sizeof(table_bench_sets[0]));
  for (int set_idx = 0; set_idx < num_sets; set_idx++) {
    table_bench_row("static", static_flags, &table_bench_sets[set_idx],
                    static_games);
  }
  for (int set_idx = 0; set_idx < num_sets; set_idx++) {
    table_bench_row("sim2ply", sim_flags, &table_bench_sets[set_idx],
                    sim_games);
  }
}
