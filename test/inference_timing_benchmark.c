// Inference timing benchmark: measures wall-clock time for exhaustive inference
// generation across different move types and tile counts.
//
// Usage: ./bin/magpie_test inftiming
//
// Plays games from an empty board using top-equity static play. For each turn
// where the best move is a scoring play or exchange AND the bag has enough
// tiles for inference to be valid, runs inference and records the elapsed time.
//
// Collects 10 samples for each of:
//   - Scoring plays with num_played = 1..7
//   - Exchanges with num_exchanged = 1..7
// Total: 140 inference runs (14 situations × 10 samples each).

#include "../src/compat/ctime.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/inference_results.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/rack.h"
#include "../src/ent/thread_control.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/util/io_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <stdio.h>
#include <stdlib.h>

#define NUM_THREADS 10
#define SAMPLES_PER_BUCKET 10
#define MAX_GAMES 500

typedef enum {
  MOVE_KIND_SCORING,
  MOVE_KIND_EXCHANGE,
  NUM_MOVE_KINDS,
} move_kind_t;

static const char *move_kind_label(move_kind_t kind) {
  switch (kind) {
  case MOVE_KIND_SCORING:
    return "scoring";
  case MOVE_KIND_EXCHANGE:
    return "exchange";
  default:
    return "unknown";
  }
}

// Timing data for one bucket: move_kind × num_tiles (1..7)
typedef struct {
  int count;
  double elapsed_s[SAMPLES_PER_BUCKET];
} TimingBucket;

static bool all_buckets_full(TimingBucket buckets[NUM_MOVE_KINDS][RACK_SIZE]) {
  for (int kind = 0; kind < NUM_MOVE_KINDS; kind++) {
    for (int n = 0; n < RACK_SIZE; n++) {
      if (buckets[kind][n].count < SAMPLES_PER_BUCKET) {
        return false;
      }
    }
  }
  return true;
}

static int total_samples(TimingBucket buckets[NUM_MOVE_KINDS][RACK_SIZE]) {
  int total = 0;
  for (int kind = 0; kind < NUM_MOVE_KINDS; kind++) {
    for (int n = 0; n < RACK_SIZE; n++) {
      total += buckets[kind][n].count;
    }
  }
  return total;
}

static void print_bucket_status(TimingBucket buckets[NUM_MOVE_KINDS][RACK_SIZE],
                                int games_played) {
  printf("  --- Status after %d games ---\n", games_played);
  printf("  scoring:  ");
  for (int n = 0; n < RACK_SIZE; n++) {
    printf("%d:%d ", n + 1, buckets[MOVE_KIND_SCORING][n].count);
  }
  printf("\n  exchange: ");
  for (int n = 0; n < RACK_SIZE; n++) {
    printf("%d:%d ", n + 1, buckets[MOVE_KIND_EXCHANGE][n].count);
  }
  printf("\n  total: %d/%d\n\n", total_samples(buckets),
         SAMPLES_PER_BUCKET * RACK_SIZE * NUM_MOVE_KINDS);
}

void test_inference_timing_benchmark(void) {
  setbuf(stdout, NULL);
  printf("\n");
  printf("========================================================\n");
  printf("  Inference Timing Benchmark\n");
  printf("  %d samples per bucket, %d threads\n", SAMPLES_PER_BUCKET,
         NUM_THREADS);
  printf("  Buckets: scoring×{1..7} + exchange×{1..7} = 14 buckets\n");
  printf("  Target: %d total inference runs\n",
         SAMPLES_PER_BUCKET * RACK_SIZE * NUM_MOVE_KINDS);
  printf("  All moves from natural top-equity static play\n");
  printf("========================================================\n\n");

  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -threads 10");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);

  const LetterDistribution *ld = config_get_ld(config);
  const int ld_size = ld_get_size(ld);
  Game *game = config_get_game(config);

  InferenceResults *inference_results = inference_results_create(NULL);
  ErrorStack *error_stack = error_stack_create();
  MoveList *move_list = move_list_create(1);

  TimingBucket buckets[NUM_MOVE_KINDS][RACK_SIZE];
  for (int kind = 0; kind < NUM_MOVE_KINDS; kind++) {
    for (int n = 0; n < RACK_SIZE; n++) {
      buckets[kind][n].count = 0;
    }
  }

  int games_played = 0;
  int total_exchanges_seen = 0;
  const uint64_t base_seed = 1000;

  while (!all_buckets_full(buckets) && games_played < MAX_GAMES) {
    game_reset(game);
    game_seed(game, base_seed + games_played);
    draw_starting_racks(game);

    while (!game_over(game)) {
      const int player_on_turn = game_get_player_on_turn_index(game);
      const Bag *bag = game_get_bag(game);
      const int bag_tiles = bag_get_letters(bag);

      const Move *top_move = get_top_equity_move(game, 0, move_list);
      const game_event_t move_type = move_get_type(top_move);

      move_kind_t kind;
      int num_tiles = 0;
      bool can_infer = false;

      if (move_type == GAME_EVENT_TILE_PLACEMENT_MOVE &&
          bag_tiles >= RACK_SIZE) {
        kind = MOVE_KIND_SCORING;
        num_tiles = move_get_tiles_played(top_move);
        can_infer = (num_tiles >= 1 && num_tiles <= RACK_SIZE);
      } else if (move_type == GAME_EVENT_EXCHANGE &&
                 bag_tiles >= RACK_SIZE * 2) {
        kind = MOVE_KIND_EXCHANGE;
        num_tiles = move_get_tiles_played(top_move);
        can_infer = (num_tiles >= 1 && num_tiles <= RACK_SIZE);
        total_exchanges_seen++;
      }

      if (can_infer) {
        TimingBucket *bucket = &buckets[kind][num_tiles - 1];

        if (bucket->count < SAMPLES_PER_BUCKET) {
          const Equity score = move_get_score(top_move);
          int num_exch = 0;
          Rack target_played_tiles;
          rack_set_dist_size_and_reset(&target_played_tiles, ld_size);
          Rack target_known_rack;
          rack_set_dist_size_and_reset(&target_known_rack, ld_size);
          Rack nontarget_known_rack;
          rack_set_dist_size_and_reset(&nontarget_known_rack, ld_size);

          if (kind == MOVE_KIND_EXCHANGE) {
            num_exch = num_tiles;
          } else {
            const int tiles_length = move_get_tiles_length(top_move);
            for (int i = 0; i < tiles_length; i++) {
              const MachineLetter ml = move_get_tile(top_move, i);
              if (ml != PLAYED_THROUGH_MARKER) {
                rack_add_letter(&target_played_tiles,
                                get_unblanked_machine_letter(ml));
              }
            }
          }

          Timer timer;
          ctimer_reset(&timer);

          thread_control_set_status(config_get_thread_control(config),
                                    THREAD_CONTROL_STATUS_STARTED);
          ctimer_start(&timer);
          config_infer(config, false, player_on_turn, score, num_exch,
                       &target_played_tiles, &target_known_rack,
                       &nontarget_known_rack, true, inference_results,
                       error_stack);
          ctimer_stop(&timer);

          if (!error_stack_is_empty(error_stack)) {
            error_stack_print_and_reset(error_stack);
          } else {
            double elapsed = ctimer_elapsed_seconds(&timer);
            bucket->elapsed_s[bucket->count] = elapsed;
            bucket->count++;

            printf("  [%s num_tiles=%d] sample %d/%d: %.4fs\n",
                   move_kind_label(kind), num_tiles, bucket->count,
                   SAMPLES_PER_BUCKET, elapsed);
          }
        }
      }

      play_move(top_move, game, NULL);
    }

    games_played++;

    if (games_played % 100 == 0) {
      print_bucket_status(buckets, games_played);
    }
  }

  // Print results
  printf("\n========================================================\n");
  printf("  RESULTS (%d games played, %d exchanges seen, %d total samples)\n",
         games_played, total_exchanges_seen, total_samples(buckets));
  printf("========================================================\n\n");

  // Header
  printf("%-10s  %3s  %5s  %10s  %10s  %10s  %10s\n", "move_type", "n",
         "count", "min_s", "max_s", "mean_s", "median_s");
  printf("%-10s  %3s  %5s  %10s  %10s  %10s  %10s\n", "----------", "---",
         "-----", "----------", "----------", "----------", "----------");

  double worst_case_scoring = 0.0;
  double worst_case_exchange = 0.0;

  for (int kind = 0; kind < NUM_MOVE_KINDS; kind++) {
    for (int n = 0; n < RACK_SIZE; n++) {
      TimingBucket *bucket = &buckets[kind][n];
      int num_tiles = n + 1;

      if (bucket->count == 0) {
        printf("%-10s  %3d  %5d  %10s  %10s  %10s  %10s\n",
               move_kind_label((move_kind_t)kind), num_tiles, 0, "N/A", "N/A",
               "N/A", "N/A");
        continue;
      }

      // Sort for median
      double sorted[SAMPLES_PER_BUCKET];
      for (int i = 0; i < bucket->count; i++) {
        sorted[i] = bucket->elapsed_s[i];
      }
      for (int i = 0; i < bucket->count - 1; i++) {
        for (int j = i + 1; j < bucket->count; j++) {
          if (sorted[j] < sorted[i]) {
            double tmp = sorted[i];
            sorted[i] = sorted[j];
            sorted[j] = tmp;
          }
        }
      }

      double min_s = sorted[0];
      double max_s = sorted[bucket->count - 1];
      double sum = 0.0;
      for (int i = 0; i < bucket->count; i++) {
        sum += sorted[i];
      }
      double mean_s = sum / bucket->count;
      double median_s;
      if (bucket->count % 2 == 0) {
        median_s =
            (sorted[bucket->count / 2 - 1] + sorted[bucket->count / 2]) / 2.0;
      } else {
        median_s = sorted[bucket->count / 2];
      }

      printf("%-10s  %3d  %5d  %10.4f  %10.4f  %10.4f  %10.4f\n",
             move_kind_label((move_kind_t)kind), num_tiles, bucket->count,
             min_s, max_s, mean_s, median_s);

      if (kind == MOVE_KIND_SCORING && max_s > worst_case_scoring) {
        worst_case_scoring = max_s;
      }
      if (kind == MOVE_KIND_EXCHANGE && max_s > worst_case_exchange) {
        worst_case_exchange = max_s;
      }
    }
    printf("\n");
  }

  printf("Worst-case observed times:\n");
  printf("  Scoring plays: %.4fs\n", worst_case_scoring);
  printf("  Exchanges:     %.4fs\n", worst_case_exchange);
  printf("\n");

  move_list_destroy(move_list);
  error_stack_destroy(error_stack);
  inference_results_destroy(inference_results);
  config_destroy(config);
}
