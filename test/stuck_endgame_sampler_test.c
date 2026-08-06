#include "stuck_endgame_sampler_test.h"

#include "../src/compat/cpthread.h"
#include "../src/compat/ctime.h"
#include "../src/def/cpthread_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/stats.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/gameplay.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Stuck-tile endgame Monte-Carlo sampler.
//
// From each mid-game CGP (bag not empty), sample many continuations forward to
// the point the bag empties (the endgame boundary) and apply the stuck-tile
// detector. This estimates P(stuck-tile endgame | position) -- the quantity
// that governs how valuable banked clock time is once the bag is empty, since
// stuck endgames are the rare ones where extra endgame depth changes the exact
// answer.
//
// Each sample: copy the position, reseed the bag PRNG, redraw the (unknown)
// opponent rack, then play greedy top-equity moves to the boundary. At the
// boundary both players' stuck fractions are recorded. The greedy policy
// matches the existing terminal-rollout infra (klv3_sim_oracle_test.c); a
// simmed policy could later replace the single get_top_equity_move call.
//
// Input (env): MAGPIE_STUCK_CGP (corpus file, one CGP/line),
// MAGPIE_STUCK_SAMPLES, MAGPIE_STUCK_THREADS, MAGPIE_STUCK_SEED,
// MAGPIE_STUCK_START, MAGPIE_STUCK_MAX, MAGPIE_STUCK_LEX.
// Output: one STUCKRESULT line per position (machine-parseable key=value).

enum {
  STUCK_DEFAULT_SAMPLES = 1000,
  STUCK_DEFAULT_THREADS = 8,
  STUCK_MAX_PLAYOUT_TURNS = 400,
  STUCK_MAX_POSITIONS = 100000,
  STUCK_CGP_LINE_MAX = 4096,
  // Matches endgame.c's internal DEFAULT_ENDGAME_MOVELIST_CAPACITY.
  // TILES_PLAYED generation needs far less, but this is proven safe for any
  // endgame position.
  STUCK_MOVELIST_CAPACITY = 250000,
  // Joint (tiles, blanks, stuck) histogram at the endgame boundary. Total rack
  // tiles range 0..2*RACK_SIZE; blank counts above 2 are clamped into the
  // final blank bucket (standard distributions have two blanks).
  STUCK_BUCKET_TILES = 2 * RACK_SIZE + 1,
  STUCK_BUCKET_BLANKS = 3,
  STUCK_BUCKET_STUCK = 2,
  STUCK_BUCKET_COUNT =
      STUCK_BUCKET_TILES * STUCK_BUCKET_BLANKS * STUCK_BUCKET_STUCK,
};

static int stuck_bucket_index(int tiles, int blanks, bool stuck) {
  if (blanks > STUCK_BUCKET_BLANKS - 1) {
    blanks = STUCK_BUCKET_BLANKS - 1;
  }
  return (tiles * STUCK_BUCKET_BLANKS + blanks) * STUCK_BUCKET_STUCK +
         (stuck ? 1 : 0);
}

// Per-worker accumulators. FRAC/HIT slots are pushed only for samples that
// reach a bag-empty endgame, so their means are conditional on REACHED.
// EG_EVENT: stuckness at the boundary snapshot is rare (full racks are almost
// always playable); the decisive event is a tile becoming unplayable at ANY
// point DURING the endgame (e.g. a deferred Q as the board closes). EG_EVENT
// is that any-turn indicator over the greedy continuation played to game end;
// EG_EVENT_TILES records total rack tiles at the first stuck moment (bigger =
// costlier: solve cost scales ~x2.6-2.9 per tile, and the stuck premium
// compounds with rack size).
enum {
  STUCK_S_REACHED = 0,
  STUCK_S_HIT_P0,
  STUCK_S_HIT_P1,
  STUCK_S_HIT_EITHER,
  STUCK_S_FRAC_P0,
  STUCK_S_FRAC_P1,
  STUCK_S_EG_EVENT,
  STUCK_S_EG_EVENT_TILES,
  STUCK_S_COUNT,
};

static const uint64_t STUCK_BASE_SEED = UINT64_C(0x535455434b454e47);
static const uint64_t STUCK_SEED_STRIDE = UINT64_C(0x9e3779b97f4a7c15);

static int env_positive_int(const char *name, int default_value) {
  const char *value = getenv(name);
  if (value == NULL) {
    return default_value;
  }
  const long parsed = strtol(value, NULL, 10);
  if (parsed <= 0 || parsed > INT32_MAX) {
    log_fatal("%s must be a positive integer", name);
  }
  return (int)parsed;
}

static int env_nonnegative_int(const char *name, int default_value) {
  const char *value = getenv(name);
  if (value == NULL) {
    return default_value;
  }
  const long parsed = strtol(value, NULL, 10);
  if (parsed < 0 || parsed > INT32_MAX) {
    log_fatal("%s must be a nonnegative integer", name);
  }
  return (int)parsed;
}

static uint64_t env_uint64(const char *name, uint64_t default_value) {
  const char *value = getenv(name);
  if (value == NULL || value[0] == '\0') {
    return default_value;
  }
  return strtoull(value, NULL, 0);
}

static const char *env_string(const char *name, const char *default_value) {
  const char *value = getenv(name);
  return value == NULL || value[0] == '\0' ? default_value : value;
}

// config_get_game is NULL until a game is created, but the newgame command
// dumps the fresh board to stdout. Redirect stdout to /dev/null around it so
// this tool's stdout carries only STUCK* lines (mirrors benchmark_endgame's
// exec_config_quiet).
static void create_game_quiet(Config *config) {
  (void)fflush(stdout);
  const int saved_stdout = dup(STDOUT_FILENO);
  const int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
  if (devnull >= 0) {
    (void)dup2(devnull, STDOUT_FILENO);
    (void)close(devnull);
  }
  load_and_exec_config_or_die(config, "newgame");
  (void)fflush(stdout);
  if (saved_stdout >= 0) {
    (void)dup2(saved_stdout, STDOUT_FILENO);
    (void)close(saved_stdout);
  }
}

typedef struct StuckSamplerWorker {
  const Game *base_game;
  int on_turn;
  int worker_index;
  int num_workers;
  int num_samples;
  uint64_t seed;
  Stat *stats[STUCK_S_COUNT];
  uint32_t bucket_counts[STUCK_BUCKET_COUNT];
} StuckSamplerWorker;

typedef struct StuckSampleResult {
  double reached_rate;
  uint64_t reached_count;
  double p0_stuck_rate;
  double p1_stuck_rate;
  double either_stuck_rate;
  double mean_frac_p0;
  double mean_frac_p1;
  double eg_event_rate;
  double eg_event_mean_tiles;
  double elapsed_seconds;
} StuckSampleResult;

static void *stuck_sampler_worker(void *uncasted_worker) {
  StuckSamplerWorker *worker = (StuckSamplerWorker *)uncasted_worker;
  Game *scratch = game_duplicate(worker->base_game);
  MoveList *playout_move_list = move_list_create(1);
  MoveList *stuck_move_list = move_list_create_small(STUCK_MOVELIST_CAPACITY);
  const int opponent = 1 - worker->on_turn;
  for (int sample_index = worker->worker_index;
       sample_index < worker->num_samples;
       sample_index += worker->num_workers) {
    const uint64_t sample_seed =
        worker->seed + (uint64_t)sample_index * STUCK_SEED_STRIDE;
    game_copy(scratch, worker->base_game);
    game_seed(scratch, sample_seed);
    // The side to move keeps its known CGP rack; the opponent's rack is unknown
    // from a mid-game decision point, so it is returned to the bag and redrawn.
    set_random_rack(scratch, opponent, NULL);
    const Bag *bag = game_get_bag(scratch);
    int turns = 0;
    while (!bag_is_empty(bag) && !game_over(scratch)) {
      if (turns++ >= STUCK_MAX_PLAYOUT_TURNS) {
        log_fatal("stuck sampler exceeded %d continuation turns",
                  STUCK_MAX_PLAYOUT_TURNS);
      }
      const Move *move = get_top_equity_move(scratch, playout_move_list);
      play_move(move, scratch, NULL);
    }
    const bool reached_endgame = bag_is_empty(bag) && !game_over(scratch);
    stat_push(worker->stats[STUCK_S_REACHED], reached_endgame ? 1.0 : 0.0, 1);
    if (reached_endgame) {
      const double frac_p0 =
          (double)endgame_player_stuck_fraction(scratch, stuck_move_list, 0);
      const double frac_p1 =
          (double)endgame_player_stuck_fraction(scratch, stuck_move_list, 1);
      stat_push(worker->stats[STUCK_S_FRAC_P0], frac_p0, 1);
      stat_push(worker->stats[STUCK_S_FRAC_P1], frac_p1, 1);
      stat_push(worker->stats[STUCK_S_HIT_P0], frac_p0 > 0.0 ? 1.0 : 0.0, 1);
      stat_push(worker->stats[STUCK_S_HIT_P1], frac_p1 > 0.0 ? 1.0 : 0.0, 1);
      stat_push(worker->stats[STUCK_S_HIT_EITHER],
                (frac_p0 > 0.0 || frac_p1 > 0.0) ? 1.0 : 0.0, 1);
      // Joint (tiles, blanks, stuck) difficulty bucket at the boundary: total
      // rack tiles and blank count drive endgame cost alongside stuckness.
      const Rack *rack_p0 = player_get_rack(game_get_player(scratch, 0));
      const Rack *rack_p1 = player_get_rack(game_get_player(scratch, 1));
      const int boundary_tiles = (int)rack_get_total_letters(rack_p0) +
                                 (int)rack_get_total_letters(rack_p1);
      const int boundary_blanks =
          (int)rack_get_letter(rack_p0, BLANK_MACHINE_LETTER) +
          (int)rack_get_letter(rack_p1, BLANK_MACHINE_LETTER);
      worker->bucket_counts[stuck_bucket_index(
          boundary_tiles, boundary_blanks, frac_p0 > 0.0 || frac_p1 > 0.0)]++;
      // Continue the greedy playout through the endgame, checking each turn
      // for a stuck event: some player's rack containing a tile no legal move
      // can play. This is the rare-but-decisive case (17x realized endgame
      // cost in the pilot games where it occurred).
      bool eg_event = frac_p0 > 0.0 || frac_p1 > 0.0;
      int eg_event_tiles = eg_event ? boundary_tiles : 0;
      while (!game_over(scratch)) {
        if (turns++ >= STUCK_MAX_PLAYOUT_TURNS) {
          log_fatal("stuck sampler exceeded %d endgame turns",
                    STUCK_MAX_PLAYOUT_TURNS);
        }
        if (!eg_event) {
          const double turn_frac_p0 = (double)endgame_player_stuck_fraction(
              scratch, stuck_move_list, 0);
          const double turn_frac_p1 = (double)endgame_player_stuck_fraction(
              scratch, stuck_move_list, 1);
          if (turn_frac_p0 > 0.0 || turn_frac_p1 > 0.0) {
            eg_event = true;
            eg_event_tiles = (int)rack_get_total_letters(
                                 player_get_rack(game_get_player(scratch, 0))) +
                             (int)rack_get_total_letters(
                                 player_get_rack(game_get_player(scratch, 1)));
          }
        }
        const Move *eg_move = get_top_equity_move(scratch, playout_move_list);
        play_move(eg_move, scratch, NULL);
      }
      stat_push(worker->stats[STUCK_S_EG_EVENT], eg_event ? 1.0 : 0.0, 1);
      if (eg_event) {
        stat_push(worker->stats[STUCK_S_EG_EVENT_TILES], (double)eg_event_tiles,
                  1);
      }
    }
  }
  move_list_destroy(playout_move_list);
  small_move_list_destroy(stuck_move_list);
  game_destroy(scratch);
  return NULL;
}

// bucket_counts_out must hold STUCK_BUCKET_COUNT entries; it receives the
// joint (tiles, blanks, stuck) histogram over samples that reached bag-empty.
static StuckSampleResult run_stuck_samples(const Game *base_game, int on_turn,
                                           int num_samples,
                                           int requested_threads, uint64_t seed,
                                           uint32_t *bucket_counts_out) {
  const int num_threads =
      requested_threads < num_samples ? requested_threads : num_samples;
  StuckSamplerWorker *workers =
      malloc_or_die(sizeof(*workers) * (size_t)num_threads);
  cpthread_t *worker_ids =
      malloc_or_die(sizeof(*worker_ids) * (size_t)num_threads);
  Timer timer;
  ctimer_start(&timer);
  for (int thread_index = 0; thread_index < num_threads; thread_index++) {
    for (int stat_index = 0; stat_index < STUCK_S_COUNT; stat_index++) {
      workers[thread_index].stats[stat_index] = stat_create(true);
    }
    workers[thread_index].base_game = base_game;
    workers[thread_index].on_turn = on_turn;
    workers[thread_index].worker_index = thread_index;
    workers[thread_index].num_workers = num_threads;
    workers[thread_index].num_samples = num_samples;
    workers[thread_index].seed = seed;
    memset(workers[thread_index].bucket_counts, 0,
           sizeof(workers[thread_index].bucket_counts));
    cpthread_create(&worker_ids[thread_index], stuck_sampler_worker,
                    &workers[thread_index]);
  }
  for (int thread_index = 0; thread_index < num_threads; thread_index++) {
    cpthread_join(worker_ids[thread_index]);
  }

  Stat *combined[STUCK_S_COUNT];
  Stat **per_metric = malloc_or_die(sizeof(Stat *) * (size_t)num_threads);
  for (int stat_index = 0; stat_index < STUCK_S_COUNT; stat_index++) {
    for (int thread_index = 0; thread_index < num_threads; thread_index++) {
      per_metric[thread_index] = workers[thread_index].stats[stat_index];
    }
    combined[stat_index] = stat_create(true);
    stats_combine(per_metric, num_threads, combined[stat_index]);
  }
  free(per_metric);

  const uint64_t reached_count =
      stat_get_num_samples(combined[STUCK_S_HIT_EITHER]);
  const bool any_reached = reached_count > 0;
  const StuckSampleResult result = {
      .reached_rate = stat_get_mean(combined[STUCK_S_REACHED]),
      .reached_count = reached_count,
      .p0_stuck_rate =
          any_reached ? stat_get_mean(combined[STUCK_S_HIT_P0]) : 0.0,
      .p1_stuck_rate =
          any_reached ? stat_get_mean(combined[STUCK_S_HIT_P1]) : 0.0,
      .either_stuck_rate =
          any_reached ? stat_get_mean(combined[STUCK_S_HIT_EITHER]) : 0.0,
      .mean_frac_p0 =
          any_reached ? stat_get_mean(combined[STUCK_S_FRAC_P0]) : 0.0,
      .mean_frac_p1 =
          any_reached ? stat_get_mean(combined[STUCK_S_FRAC_P1]) : 0.0,
      .eg_event_rate =
          any_reached ? stat_get_mean(combined[STUCK_S_EG_EVENT]) : 0.0,
      .eg_event_mean_tiles =
          stat_get_num_samples(combined[STUCK_S_EG_EVENT_TILES]) > 0
              ? stat_get_mean(combined[STUCK_S_EG_EVENT_TILES])
              : 0.0,
      .elapsed_seconds = ctimer_elapsed_seconds(&timer),
  };

  memset(bucket_counts_out, 0, sizeof(uint32_t) * STUCK_BUCKET_COUNT);
  for (int thread_index = 0; thread_index < num_threads; thread_index++) {
    for (int bucket = 0; bucket < STUCK_BUCKET_COUNT; bucket++) {
      bucket_counts_out[bucket] += workers[thread_index].bucket_counts[bucket];
    }
  }

  for (int stat_index = 0; stat_index < STUCK_S_COUNT; stat_index++) {
    stat_destroy(combined[stat_index]);
  }
  for (int thread_index = 0; thread_index < num_threads; thread_index++) {
    for (int stat_index = 0; stat_index < STUCK_S_COUNT; stat_index++) {
      stat_destroy(workers[thread_index].stats[stat_index]);
    }
  }
  free(worker_ids);
  free(workers);
  return result;
}

void test_stuck_endgame_sampler(void) {
  const char *cgp_file =
      env_string("MAGPIE_STUCK_CGP", "obj/stuck-sample-cgps.txt");
  const char *lex = env_string("MAGPIE_STUCK_LEX", "CSW24");
  const int num_samples =
      env_positive_int("MAGPIE_STUCK_SAMPLES", STUCK_DEFAULT_SAMPLES);
  const int threads =
      env_positive_int("MAGPIE_STUCK_THREADS", STUCK_DEFAULT_THREADS);
  const int start_position = env_nonnegative_int("MAGPIE_STUCK_START", 0);
  const int max_positions =
      env_positive_int("MAGPIE_STUCK_MAX", STUCK_MAX_POSITIONS);
  const uint64_t seed = env_uint64("MAGPIE_STUCK_SEED", STUCK_BASE_SEED);

  FILE *file = fopen(cgp_file, "re");
  if (file == NULL) {
    printf("STUCKERR no CGP file at %s\n", cgp_file);
    (void)fflush(stdout);
    return;
  }
  const int max_read =
      max_positions < STUCK_MAX_POSITIONS ? max_positions : STUCK_MAX_POSITIONS;
  size_t line_capacity = 256;
  char **cgp_lines = malloc_or_die(sizeof(char *) * line_capacity);
  int num_cgps = 0;
  char line_buffer[STUCK_CGP_LINE_MAX];
  while (num_cgps < max_read && fgets(line_buffer, sizeof(line_buffer), file)) {
    const size_t len = strlen(line_buffer);
    if (len > 0 && line_buffer[len - 1] == '\n') {
      line_buffer[len - 1] = '\0';
    }
    if (line_buffer[0] == '\0') {
      continue;
    }
    if ((size_t)num_cgps == line_capacity) {
      line_capacity *= 2;
      cgp_lines = realloc_or_die(cgp_lines, sizeof(char *) * line_capacity);
    }
    cgp_lines[num_cgps] = string_duplicate(line_buffer);
    num_cgps++;
  }
  (void)fclose(file);

  char settings[256];
  (void)snprintf(settings, sizeof(settings),
                 "set -lex %s -k1 %s -k2 %s -wmp true -s1 equity -s2 equity",
                 lex, lex, lex);
  Config *config = config_create_or_die(settings);
  create_game_quiet(config);
  Game *game = config_get_game(config);

  printf("STUCKCFG positions=%d start=%d samples=%d threads=%d seed=%" PRIu64
         " lex=%s file=%s policy=greedy_top_equity\n",
         num_cgps - start_position, start_position, num_samples, threads, seed,
         lex, cgp_file);
  (void)fflush(stdout);

  for (int pos = start_position; pos < num_cgps; pos++) {
    ErrorStack *error_stack = error_stack_create();
    game_load_cgp(game, cgp_lines[pos], error_stack);
    if (!error_stack_is_empty(error_stack)) {
      printf("STUCKERR pos=%d load\n", pos);
      error_stack_print_and_reset(error_stack);
      error_stack_destroy(error_stack);
      continue;
    }
    error_stack_destroy(error_stack);
    const int on_turn = game_get_player_on_turn_index(game);
    const int root_bag = bag_get_letters(game_get_bag(game));
    uint32_t bucket_counts[STUCK_BUCKET_COUNT];
    const StuckSampleResult result = run_stuck_samples(
        game, on_turn, num_samples, threads, seed, bucket_counts);
    printf(
        "STUCKRESULT cgp=%d bag=%d on_turn=%d samples=%d reached_endgame=%.5f "
        "no_endgame=%.5f p0_stuck_rate=%.5f p1_stuck_rate=%.5f "
        "either_stuck_rate=%.5f mean_frac_p0=%.5f mean_frac_p1=%.5f "
        "eg_stuck_event_rate=%.5f eg_event_mean_tiles=%.2f "
        "elapsed=%.3f\n",
        pos, root_bag, on_turn, num_samples, result.reached_rate,
        1.0 - result.reached_rate, result.p0_stuck_rate, result.p1_stuck_rate,
        result.either_stuck_rate, result.mean_frac_p0, result.mean_frac_p1,
        result.eg_event_rate, result.eg_event_mean_tiles,
        result.elapsed_seconds);
    // Joint difficulty histogram at the boundary: one t<T>b<B>s<S>=<count>
    // token per nonempty (tiles, blanks, stuck) bucket. Downstream multiplies
    // these probabilities by a per-bucket cost model to price expected endgame
    // time needs from this position.
    printf("STUCKBUCKETS cgp=%d", pos);
    for (int tiles = 0; tiles < STUCK_BUCKET_TILES; tiles++) {
      for (int blanks = 0; blanks < STUCK_BUCKET_BLANKS; blanks++) {
        for (int stuck = 0; stuck < STUCK_BUCKET_STUCK; stuck++) {
          const uint32_t count =
              bucket_counts[stuck_bucket_index(tiles, blanks, stuck == 1)];
          if (count > 0) {
            printf(" t%db%ds%d=%u", tiles, blanks, stuck, count);
          }
        }
      }
    }
    printf("\n");
    (void)fflush(stdout);
  }

  for (int pos = 0; pos < num_cgps; pos++) {
    free(cgp_lines[pos]);
  }
  free(cgp_lines);
  config_destroy(config);
}
