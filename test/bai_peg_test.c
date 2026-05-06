#include "bai_peg_test.h"

#include "../src/compat/cpthread.h"
#include "../src/def/cpthread_defs.h"
#include "../src/def/game_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/impl/bai_peg.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

// Smoke test: confirms bai_peg_solve runs end-to-end on a 1-in-bag position,
// returns a non-pass move, and produces non-zero evaluation/candidate counts.
// Picking quality at low budget is not asserted (the solver may not converge
// on the truly best move at this budget); the goal here is to catch obvious
// regressions in the search loop, threading, or progressive widening logic.
//
// Budget is intentionally tight (max_evaluations capped) so this test stays
// fast and predictable on CI under ASAN/UBSAN. The deeper-search variant
// lives in test_bai_peg_thorough on the on-demand table.
static void run_bai_peg_smoke(int max_evaluations, double time_budget,
                              double endgame_time_per_solve) {
  Config *config = config_create_or_die("set -s1 score -s2 score");
  load_and_exec_config_or_die(
      config, "cgp 15/3Q7U3/3U2TAURINE2/1CHANSONS2W3/2AI6JO3/DIRL1PO3IN3/"
              "E1D2EF3V4/F1I2p1TRAIK3/O1L2T4E4/ABy1PIT2BRIG2/ME1MOZELLE5/"
              "1GRADE1O1NOH3/WE3R1V7/AT5E7/G6D7 ENOSTXY/ACEISUY 356/378 0 -lex "
              "NWL20");

  Game *game = config_get_game(config);
  assert(bag_get_letters(game_get_bag(game)) == 1);

  BaiPegArgs args = {
      .game = game,
      .thread_control = config_get_thread_control(config),
      .num_threads = 1,
      .tt_fraction_of_mem = 0.0,
      .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
      .initial_top_k = 32,
      .max_depth = 2,
      .endgame_time_per_solve = endgame_time_per_solve,
      .time_budget_seconds = time_budget,
      .max_evaluations = max_evaluations,
      .puct_c = 1.0,
  };
  BaiPegResult result;
  ErrorStack *error_stack = error_stack_create();
  bai_peg_solve(&args, &result, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(!small_move_is_pass(&result.best_move));
  assert(result.evaluations_done > 0);
  assert(result.candidates_considered > 0);
  printf("  bai_peg smoke: evals=%d  best_win%%=%.1f%%  best_spread=%+.2f  "
         "depth=%d  time=%.2fs\n",
         result.evaluations_done, result.best_win_pct * 100.0,
         result.best_mean_spread, result.best_depth_evaluated,
         result.seconds_elapsed);
  error_stack_destroy(error_stack);
  bai_cand_stats_free(result.cand_stats);
  config_destroy(config);
}

static void test_bai_peg_smoke(void) {
  // CI-friendly: max_evaluations cap guarantees the test exits quickly even
  // under high CI load when wall-clock is unreliable.
  run_bai_peg_smoke(/*max_evaluations=*/16, /*time_budget=*/1.0,
                    /*endgame_time_per_solve=*/0.05);
}

void test_bai_peg_thorough(void) {
  // Deeper search for on-demand runs: more evaluations, longer per-scenario
  // endgame, longer wall-clock budget.
  run_bai_peg_smoke(/*max_evaluations=*/0, /*time_budget=*/30.0,
                    /*endgame_time_per_solve=*/0.5);
}

// Same one-in-bag CGP as run_bai_peg_smoke; pulled out so the widening and
// concurrent tests can reuse it without duplicating the long literal.
#define BAI_PEG_TEST_ONE_IN_BAG_CGP                                            \
  "cgp 15/3Q7U3/3U2TAURINE2/1CHANSONS2W3/2AI6JO3/DIRL1PO3IN3/E1D2EF3V4/"       \
  "F1I2p1TRAIK3/O1L2T4E4/ABy1PIT2BRIG2/ME1MOZELLE5/1GRADE1O1NOH3/"             \
  "WE3R1V7/AT5E7/G6D7 ENOSTXY/ACEISUY 356/378 0 -lex NWL20"

// Exercises the progressive_widening + min_active code path that the CLI's
// default config relies on (widening_c=5, min_active=32). The basic smoke
// runs without widening, so this catches regressions in the lazy
// candidate-admission and neighbor-Q-inheritance logic.
static void test_bai_peg_progressive_widening(void) {
  Config *config = config_create_or_die("set -s1 score -s2 score");
  load_and_exec_config_or_die(config, BAI_PEG_TEST_ONE_IN_BAG_CGP);
  Game *game = config_get_game(config);
  assert(bag_get_letters(game_get_bag(game)) == 1);

  BaiPegArgs args = {
      .game = game,
      .thread_control = config_get_thread_control(config),
      .num_threads = 1,
      .tt_fraction_of_mem = 0.0,
      .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
      .max_depth = 2,
      .endgame_time_per_solve = 0.05,
      .time_budget_seconds = 1.0,
      .max_evaluations = 24,
      .puct_c = 1.0,
      .progressive_widening = true,
      .widening_c = 5.0,
      .min_active = 8,
  };
  BaiPegResult result;
  ErrorStack *error_stack = error_stack_create();
  bai_peg_solve(&args, &result, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(!small_move_is_pass(&result.best_move));
  assert(result.evaluations_done > 0);
  // With widening, the candidate pool keeps every generated move; the
  // smoke pool is small enough that this should comfortably exceed the
  // min_active size once any visits accumulate.
  assert(result.candidates_considered >= args.min_active);
  printf("  bai_peg widening: evals=%d  cands=%d  best_win%%=%.1f%%  "
         "spread=%+.2f  depth=%d  time=%.2fs\n",
         result.evaluations_done, result.candidates_considered,
         result.best_win_pct * 100.0, result.best_mean_spread,
         result.best_depth_evaluated, result.seconds_elapsed);
  error_stack_destroy(error_stack);
  bai_cand_stats_free(result.cand_stats);
  config_destroy(config);
}

typedef struct ConcurrentInvokeArgs {
  Config *config;
  int thread_index_offset;
  BaiPegResult result;
  ErrorStack *error_stack;
} ConcurrentInvokeArgs;

static void *bai_peg_concurrent_worker(void *arg) {
  ConcurrentInvokeArgs *cargs = (ConcurrentInvokeArgs *)arg;
  Game *game = config_get_game(cargs->config);
  BaiPegArgs args = {
      .game = game,
      .thread_control = config_get_thread_control(cargs->config),
      .num_threads = 1,
      .thread_index_offset = cargs->thread_index_offset,
      .tt_fraction_of_mem = 0.0,
      .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
      .initial_top_k = 32,
      .max_depth = 2,
      .endgame_time_per_solve = 0.05,
      .time_budget_seconds = 1.0,
      .max_evaluations = 16,
      // Different alphas across the two callers so any leftover shared
      // state on the qsort path would corrupt results visibly.
      .utility_alpha = (cargs->thread_index_offset == 0) ? 0.0 : 0.05,
      .puct_c = 1.0,
  };
  bai_peg_solve(&args, &cargs->result, cargs->error_stack);
  return NULL;
}

// Two bai_peg_solve calls run in parallel with disjoint thread_index_offset
// ranges and different utility_alpha values. Verifies the contracts behind
// the sort_utility caching and thread_index_offset wiring: neither call
// should crash, both should return non-pass moves, and the cached_gens[]
// movegen slots should not be corrupted across calls.
static void test_bai_peg_concurrent_invocations(void) {
  Config *config_a = config_create_or_die("set -s1 score -s2 score");
  Config *config_b = config_create_or_die("set -s1 score -s2 score");
  load_and_exec_config_or_die(config_a, BAI_PEG_TEST_ONE_IN_BAG_CGP);
  load_and_exec_config_or_die(config_b, BAI_PEG_TEST_ONE_IN_BAG_CGP);

  ConcurrentInvokeArgs args_a = {.config = config_a,
                                 .thread_index_offset = 0,
                                 .error_stack = error_stack_create()};
  ConcurrentInvokeArgs args_b = {.config = config_b,
                                 .thread_index_offset = 1,
                                 .error_stack = error_stack_create()};

  pthread_t thread_a;
  pthread_t thread_b;
  cpthread_create(&thread_a, bai_peg_concurrent_worker, &args_a);
  cpthread_create(&thread_b, bai_peg_concurrent_worker, &args_b);
  cpthread_join(thread_a);
  cpthread_join(thread_b);

  assert(error_stack_is_empty(args_a.error_stack));
  assert(error_stack_is_empty(args_b.error_stack));
  assert(!small_move_is_pass(&args_a.result.best_move));
  assert(!small_move_is_pass(&args_b.result.best_move));
  assert(args_a.result.evaluations_done > 0);
  assert(args_b.result.evaluations_done > 0);
  printf("  bai_peg concurrent: a_evals=%d a_win%%=%.1f%% | "
         "b_evals=%d b_win%%=%.1f%%\n",
         args_a.result.evaluations_done, args_a.result.best_win_pct * 100.0,
         args_b.result.evaluations_done, args_b.result.best_win_pct * 100.0);

  bai_cand_stats_free(args_a.result.cand_stats);
  bai_cand_stats_free(args_b.result.cand_stats);
  error_stack_destroy(args_a.error_stack);
  error_stack_destroy(args_b.error_stack);
  config_destroy(config_a);
  config_destroy(config_b);
}

// Helper: returns true iff the current side-on-turn has at least one
// scoring (non-pass) move. Uses MOVE_RECORD_BEST: if the best move comes
// back as pass-with-score-0, the player has nothing scoring to play.
// Caller must have WMP enabled for movegen to consider every play.
static bool has_any_scoring_move(Game *game, MoveList *move_list) {
  const Move *best = get_top_equity_move(game, 0, move_list);
  if (best == NULL) {
    return false;
  }
  return move_get_type(best) != GAME_EVENT_PASS || move_get_score(best) != 0;
}

// Forced-rack tiles for the no-scoring-plays finder. These are
// individually hard-to-play letters; biasing the mover's rack to this
// 7-tile set makes it dramatically more likely to land in a stuck
// position than waiting for it to occur naturally during random play.
static const char FORCED_STUCK_RACK[] = "QCCVVZJ";

// Returns the number of physically-on-rack tiles (counting duplicates)
// that have NO single-tile play available on the current board. Counts
// across all rack tiles; e.g. if a Q is unplayable but C is playable
// then a QCCVVZJ rack returns 1 + 0 + ... summed by playability.
// Useful as a histogram input to gauge how often a given rack is fully
// (or near-fully) stuck.
static int count_unplayable_rack_tiles(Game *game, MoveList *move_list) {
  int player_idx = game_get_player_on_turn_index(game);
  uint64_t tiles_bv = 0;
  const MoveGenArgs args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_TILES_PLAYED,
      .move_sort_type = MOVE_SORT_SCORE,
      .override_kwg = NULL,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .tiles_played_bv = &tiles_bv,
  };
  generate_moves(&args);
  const Rack *rack = player_get_rack(game_get_player(game, player_idx));
  const LetterDistribution *ld = game_get_ld(game);
  int ld_size = ld_get_size(ld);
  int unplayable = 0;
  for (int ml = 0; ml < ld_size; ml++) {
    int count = rack_get_letter(rack, ml);
    if (count == 0) {
      continue;
    }
    // cppcheck-suppress knownConditionTrueFalse
    if (!(tiles_bv & ((uint64_t)1 << ml))) {
      unplayable += count;
    }
  }
  return unplayable;
}

// CGP discovered by test_bai_peg_find_no_scoring_position (run on demand).
// 1-in-bag position where the mover (CCJQVVZ) has no playable non-pass
// move on the board, so bai_peg_solve must fall back to pass-with-score-0
// rather than erroring. If this CGP ever stops triggering the no-scoring
// path (e.g. lexicon changes), regenerate via the on-demand finder.
// Uses TWL98: that lexicon lacks several letter-pair plays (notably QI)
// that newer lexica include, which is what makes a fully-stuck rack
// findable in the first place.
#define BAI_PEG_TEST_NO_SCORING_PLAYS_CGP                                      \
  "cgp 5G1K7/1RETRO1UNRAISED/5OWE2U4/5PI3T4/4S1L3U4/1FOOTIE3M4/"               \
  "AAH1R5N4/AX1GENTS7/4W2A7/3BEY1N7/2DIDO1I7/1RAG1U1t7/"                       \
  "FIN4I2ALA2/OBI4E2LIMY1/H1O4SEDATiON CCJQVVZ/EEELPRT 0/643 0 -lex TWL98"

// Confirms bai_peg_solve returns success with a pass-move best (score 0)
// when the rack has no playable non-pass move. The "no scoring plays" path
// is a fallback rather than an error because the caller has nothing
// actionable to do besides pass.
static void test_bai_peg_returns_pass_when_no_scoring_plays(void) {
  Config *config = config_create_or_die("set -wmp true -s1 score -s2 score");
  load_and_exec_config_or_die(config, BAI_PEG_TEST_NO_SCORING_PLAYS_CGP);
  Game *game = config_get_game(config);
  assert(bag_get_letters(game_get_bag(game)) == 1);
  // Sanity: the fixture must actually exhibit "no scoring moves", or the
  // assertion below tests nothing.
  MoveList *probe_ml = move_list_create(1);
  assert(!has_any_scoring_move(game, probe_ml));
  move_list_destroy(probe_ml);

  BaiPegArgs args = {
      .game = game,
      .thread_control = config_get_thread_control(config),
      .num_threads = 1,
      .tt_fraction_of_mem = 0.0,
      .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
      .max_depth = 1,
      .endgame_time_per_solve = 0.05,
      .time_budget_seconds = 1.0,
      .puct_c = 1.0,
  };
  BaiPegResult result;
  ErrorStack *error_stack = error_stack_create();
  bai_peg_solve(&args, &result, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(small_move_is_pass(&result.best_move));
  assert(small_move_get_score(&result.best_move) == 0);
  assert(result.candidates_considered == 0);
  error_stack_destroy(error_stack);
  bai_cand_stats_free(result.cand_stats);
  config_destroy(config);
}

enum {
  FINDER_NUM_THREADS = 1,
  FINDER_MAX_ATTEMPTS = 10000000,
  FINDER_BASE_SEED = 1,
};

// Score-bucket boundaries for the natural-best-move histogram. Bin k
// counts positions whose top-equity best move scores in
// [SCORE_BUCKETS[k], SCORE_BUCKETS[k+1]). The final bin catches everything
// at or above the last boundary. Bin 0 = "score == 0", which is exactly
// the stuck case (best move is a 0-score pass).
static const int SCORE_BUCKETS[] = {0, 1, 5, 10, 20, 30, 50};
enum { NUM_SCORE_BUCKETS = sizeof(SCORE_BUCKETS) / sizeof(SCORE_BUCKETS[0]) };

typedef struct FinderThreadArgs {
  int thread_index;
  atomic_int *next_attempt;
  atomic_int *found;
  cpthread_mutex_t *print_mutex;
  cpthread_mutex_t *init_mutex;
  // Histogram of natural top-equity-best-move scores at 1-in-bag, before
  // any rack forcing: tells us how rare a stuck rack actually is from
  // random self-play. Bin 0 = "best score == 0" = the case we're hunting.
  atomic_uint *score_hist;
  // Histogram of "stuck-tile count" across QCCVVZJ racks: hist[k] is the
  // number of positions where exactly k of the 7 forced rack tiles have no
  // single-tile play. hist[7] = fully stuck. Only filled when
  // force_stuck_rack succeeds at constructing the swap.
  atomic_uint *hist;
} FinderThreadArgs;

static int score_bucket(int score) {
  for (int k = NUM_SCORE_BUCKETS - 1; k >= 0; k--) {
    if (score >= SCORE_BUCKETS[k]) {
      return k;
    }
  }
  return 0;
}

static void *finder_worker(void *arg) {
  FinderThreadArgs *fa = (FinderThreadArgs *)arg;
  // Serialize Config creation: data filepath / KWG loading isn't safe
  // to run in parallel from multiple threads. After this block the Game
  // and MoveList are thread-local and parallel work begins.
  cpthread_mutex_lock(fa->init_mutex);
  Config *config = config_create_or_die(
      "set -lex TWL98 -wmp true -threads 1 -s1 score -s2 score");
  load_and_exec_config_or_die(config, "new");
  cpthread_mutex_unlock(fa->init_mutex);
  MoveList *move_list = move_list_create(1);
  Game *game = config_get_game(config);

  while (!atomic_load(fa->found)) {
    int i = atomic_fetch_add(fa->next_attempt, 1);
    if (i >= FINDER_MAX_ATTEMPTS) {
      break;
    }
    if (i > 0 && i % 10000 == 0) {
      cpthread_mutex_lock(fa->print_mutex);
      printf("  pegfindnoscore: %d attempts.\n", i);
      printf("    natural best-move score buckets ");
      for (int k = 0; k < NUM_SCORE_BUCKETS; k++) {
        printf("[≥%d]", SCORE_BUCKETS[k]);
        if (k < NUM_SCORE_BUCKETS - 1) {
          printf(" ");
        }
      }
      printf(":\n     ");
      for (int k = 0; k < NUM_SCORE_BUCKETS; k++) {
        printf("%u%s", atomic_load(&fa->score_hist[k]),
               k == NUM_SCORE_BUCKETS - 1 ? "\n" : " ");
      }
      printf("    QCCVVZJ unplayable-tile histogram: ");
      for (int k = 0; k <= 7; k++) {
        printf("%u%s", atomic_load(&fa->hist[k]), k == 7 ? "\n" : " ");
      }
      fflush(stdout);
      cpthread_mutex_unlock(fa->print_mutex);
    }

    game_reset(game);
    game_seed(game, (uint64_t)FINDER_BASE_SEED + (uint64_t)i);

    // Withhold the FORCED_STUCK_RACK tiles from the bag so they're never
    // drawn during random play. Then draw starting racks and immediately
    // return the mover's tiles to the bag — only the opponent will play.
    // After 85 tiles end up on the board the bag has 1 left and the
    // opponent has 7; injecting FORCED_STUCK_RACK into the mover's empty
    // rack yields a fully-balanced 100-tile 1-in-bag position.
    Bag *bag = game_get_bag(game);
    const LetterDistribution *ld = game_get_ld(game);
    const int ld_size = ld_get_size(ld);
    {
      uint8_t withhold[MAX_ALPHABET_SIZE] = {0};
      for (size_t k = 0; FORCED_STUCK_RACK[k] != '\0'; k++) {
        const char tile_str[2] = {FORCED_STUCK_RACK[k], '\0'};
        MachineLetter ml = ld_hl_to_ml(ld, tile_str);
        withhold[ml]++;
      }
      for (int ml = 0; ml < ld_size; ml++) {
        for (int n = 0; n < withhold[ml]; n++) {
          if (!bag_draw_letter(bag, (MachineLetter)ml, 0)) {
            // Couldn't withhold (bag didn't contain enough of this tile).
            // Distribution mismatch — skip this attempt.
            continue;
          }
        }
      }
    }
    draw_starting_racks(game);
    int starting_mover_idx = game_get_player_on_turn_index(game);
    {
      Rack *mover_rack =
          player_get_rack(game_get_player(game, starting_mover_idx));
      for (int ml = 0; ml < ld_size; ml++) {
        int n = rack_get_letter(mover_rack, ml);
        for (int j = 0; j < n; j++) {
          bag_add_letter(bag, (MachineLetter)ml, 0);
        }
      }
      rack_reset(mover_rack);
    }

    // Solo opp play. The mover passes implicitly (no rack to play) — we
    // step the side-on-turn manually, only running movegen when it's the
    // opp's turn.
    int opp_idx = 1 - starting_mover_idx;
    int play_count = 0;
    while (bag_get_letters(bag) > 1) {
      if (++play_count > 200) {
        break;
      }
      // Force the opp to be on turn.
      game_set_player_on_turn_index(game, opp_idx);
      const Move *move = get_top_equity_move(game, fa->thread_index, move_list);
      if (move == NULL || move_get_type(move) == GAME_EVENT_PASS) {
        break;
      }
      play_move(move, game, NULL);
      if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
        break;
      }
    }
    if (bag_get_letters(bag) != 1) {
      continue;
    }
    if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
      continue;
    }

    // Inject FORCED_STUCK_RACK into the mover's rack and put them on turn.
    {
      Rack *mover_rack =
          player_get_rack(game_get_player(game, starting_mover_idx));
      rack_reset(mover_rack);
      for (size_t k = 0; FORCED_STUCK_RACK[k] != '\0'; k++) {
        const char tile_str[2] = {FORCED_STUCK_RACK[k], '\0'};
        MachineLetter ml = ld_hl_to_ml(ld, tile_str);
        rack_add_letter(mover_rack, ml);
      }
      game_set_player_on_turn_index(game, starting_mover_idx);
    }

    // Natural-rack histogram (reused with a single-bin meaning): score
    // bucket of the QCCVVZJ rack's best move.
    const Move *natural_best =
        get_top_equity_move(game, fa->thread_index, move_list);
    int natural_score =
        natural_best != NULL ? (int)move_get_score(natural_best) : 0;
    atomic_fetch_add(&fa->score_hist[score_bucket(natural_score)], 1);
    int unplayable = count_unplayable_rack_tiles(game, move_list);
    atomic_fetch_add(&fa->hist[unplayable], 1);
    if (natural_score != 0) {
      continue;
    }
    char *cgp = game_get_cgp(game, true);
    cpthread_mutex_lock(fa->print_mutex);
    if (!atomic_load(fa->found)) {
      printf("Found no-scoring 1-in-bag position (QCCVVZJ rack) at seed "
             "%d:\n  %s\n",
             i, cgp);
      fflush(stdout);
      atomic_store(fa->found, 1);
    }
    cpthread_mutex_unlock(fa->print_mutex);
    free(cgp);
  }

  move_list_destroy(move_list);
  config_destroy(config);
  return NULL;
}

// On-demand finder: parallel random self-play across FINDER_NUM_THREADS
// workers, each driving games to a 1-in-bag state and checking whether
// the side on turn has any scoring play. Prints the first CGP whose side
// on turn has only the pass option. Used to discover the
// BAI_PEG_TEST_NO_SCORING_PLAYS_CGP fixture above; not part of the CI
// suite (expected to take minutes). Run via `magpie_test pegfindnoscore`.
void test_bai_peg_find_no_scoring_position(void) {
  printf("  pegfindnoscore: starting parallel search across %d threads\n",
         FINDER_NUM_THREADS);
  fflush(stdout);

  cpthread_mutex_t print_mutex;
  cpthread_mutex_t init_mutex;
  cpthread_mutex_init(&print_mutex);
  cpthread_mutex_init(&init_mutex);
  atomic_int next_attempt = 0;
  atomic_int found = 0;
  atomic_uint hist[8] = {0};
  atomic_uint score_hist[NUM_SCORE_BUCKETS] = {0};

  pthread_t threads[FINDER_NUM_THREADS];
  FinderThreadArgs targs[FINDER_NUM_THREADS];
  for (int t = 0; t < FINDER_NUM_THREADS; t++) {
    targs[t] = (FinderThreadArgs){
        .thread_index = t,
        .next_attempt = &next_attempt,
        .found = &found,
        .print_mutex = &print_mutex,
        .init_mutex = &init_mutex,
        .hist = hist,
        .score_hist = score_hist,
    };
    cpthread_create(&threads[t], finder_worker, &targs[t]);
  }
  for (int t = 0; t < FINDER_NUM_THREADS; t++) {
    cpthread_join(threads[t]);
  }

  printf("Final natural best-move score histogram (buckets ");
  for (int k = 0; k < NUM_SCORE_BUCKETS; k++) {
    printf("[≥%d]%s", SCORE_BUCKETS[k],
           k == NUM_SCORE_BUCKETS - 1 ? "):\n" : " ");
  }
  printf("  ");
  for (int k = 0; k < NUM_SCORE_BUCKETS; k++) {
    printf("%u%s", atomic_load(&score_hist[k]),
           k == NUM_SCORE_BUCKETS - 1 ? "\n" : " ");
  }
  printf("Final QCCVVZJ unplayable-tile histogram: ");
  for (int k = 0; k <= 7; k++) {
    printf("%u%s", atomic_load(&hist[k]), k == 7 ? "\n" : " ");
  }
  if (!atomic_load(&found)) {
    printf("No no-scoring 1-in-bag position found in %d attempts\n",
           FINDER_MAX_ATTEMPTS);
  }
}

// Confirms bai_peg_solve raises ERROR_STATUS_ENDGAME_BAG_NOT_EMPTY on a
// position with more than 1 tile in the bag, rather than silently solving
// a wrong subproblem.
static void test_bai_peg_rejects_non_one_in_bag(void) {
  Config *config = config_create_or_die("set -s1 score -s2 score");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/7CAT5/15/15/15/15/15/15/15 "
              "ABCDEFG/HIJKLMN 0/0 0 -lex NWL20");
  Game *game = config_get_game(config);
  assert(bag_get_letters(game_get_bag(game)) > 1);

  BaiPegArgs args = {
      .game = game,
      .thread_control = config_get_thread_control(config),
      .num_threads = 1,
      .tt_fraction_of_mem = 0.0,
      .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
      .max_depth = 1,
      .endgame_time_per_solve = 0.1,
      .time_budget_seconds = 1.0,
      .puct_c = 1.0,
  };
  BaiPegResult result;
  ErrorStack *error_stack = error_stack_create();
  bai_peg_solve(&args, &result, error_stack);
  assert(error_stack_top(error_stack) == ERROR_STATUS_ENDGAME_BAG_NOT_EMPTY);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

void test_bai_peg(void) {
  test_bai_peg_smoke();
  test_bai_peg_progressive_widening();
  test_bai_peg_concurrent_invocations();
  test_bai_peg_returns_pass_when_no_scoring_plays();
  test_bai_peg_rejects_non_one_in_bag();
}
