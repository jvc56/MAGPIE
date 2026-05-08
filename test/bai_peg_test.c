#include "bai_peg_test.h"

#include "../src/compat/cpthread.h"
#include "../src/compat/ctime.h"
#include "../src/def/board_defs.h"
#include "../src/def/cpthread_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/move_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/impl/bai_peg.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
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

// Macondo's Test1PEGPass (FRA20). The CGP reports opp_rack=empty and
// bag=8 — but those 8 unseen tiles will be 7 opp draw + 1 bag tile, so
// from the mover's perspective this is the canonical 1-in-bag state.
// bai_peg_solve should accept it (since it ignores the opp_rack
// partition and works directly off the unseen pool).
//
// Row 3 of the macondo literal (`8A1E1DO`) is 14 wide; padded to 15
// here with a trailing `1`. Mover rack: AEINRST. Pass is the winning
// move in macondo (5W-1D-2L), but our solver doesn't yet evaluate
// passes — for now we just assert that the CGP loads and the solver
// completes without an error.
#define BAI_PEG_TEST_FRENCH_PASS_CGP                                           \
  "cgp 11ONZE/10J2O1/8A1E1DO1/7QUETEE1H/10E1F1U/8ECUMERA/8C1R1TIR/"            \
  "7WOKS2ET/6DUR6/5G2N1M4/4HALLALiS3/1G1P1P1OM1XI3/VIVONS1BETEL3/"             \
  "IF1N3AS1RYAL1/ETUDIAIS7 AEINRST/ 301/300 0 -lex FRA20 -ld french"

static void test_bai_peg_accepts_empty_opp_rack(void) {
  Config *config = config_create_or_die("set -s1 score -s2 score");
  load_and_exec_config_or_die(config, BAI_PEG_TEST_FRENCH_PASS_CGP);
  Game *game = config_get_game(config);
  // Exact bag size depends on the CGP's tile partition; what matters is
  // the canonical-1-in-bag invariant from the mover's perspective.
  assert(rack_get_total_letters(player_get_rack(game_get_player(
             game, game_get_player_on_turn_index(game)))) == RACK_SIZE);

  BaiPegArgs args = {
      .game = game,
      .thread_control = config_get_thread_control(config),
      .num_threads = 1,
      .tt_fraction_of_mem = 0.0,
      .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
      .max_depth = 1,
      .endgame_time_per_solve = 0.05,
      .time_budget_seconds = 1.0,
      .max_evaluations = 8,
      .puct_c = 1.0,
  };
  BaiPegResult result;
  ErrorStack *error_stack = error_stack_create();
  bai_peg_solve(&args, &result, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(result.evaluations_done > 0);
  error_stack_destroy(error_stack);
  bai_cand_stats_free(result.cand_stats);
  config_destroy(config);
}

// On-demand: ground-truth-at-depth measurement on the French
// Test1PEGPass position. Skips PUCT — sweeps every cand (top 32 +
// pass) at depths 1..sweep_max_depth and logs each (cand, depth, q,
// time) tuple. Used for the depth->value regression dataset.
void test_bai_peg_french_pass_solve(void) {
  Config *config = config_create_or_die("set -s1 score -s2 score");
  load_and_exec_config_or_die(config, BAI_PEG_TEST_FRENCH_PASS_CGP);
  Game *game = config_get_game(config);
  assert(rack_get_total_letters(player_get_rack(game_get_player(
             game, game_get_player_on_turn_index(game)))) == RACK_SIZE);

  // Sweep depth comes from env so smoke (1) and full (5) share code.
  const char *env_d = getenv("PEG_SWEEP_DEPTH");
  int sweep_d = env_d && *env_d ? atoi(env_d) : 1;
  if (sweep_d < 1) {
    sweep_d = 1;
  }
  // Opp's inner PEG depth cap (iterative deepening to this depth).
  const char *opp_d_env = getenv("PEG_OPP_DEPTH");
  int opp_d = opp_d_env && *opp_d_env ? atoi(opp_d_env) : 4;
  if (opp_d < 1) {
    opp_d = 1;
  }
  // Trim cand pool size for quick threshold-finding runs.
  const char *topk_env = getenv("PEG_TOP_K");
  int top_k = topk_env && *topk_env ? atoi(topk_env) : 32;
  if (top_k < 1) {
    top_k = 1;
  }
  // Number of executor workers (0 = no executor, run sequentially).
  const char *exec_env = getenv("PEG_EXEC_WORKERS");
  int exec_workers = exec_env && *exec_env ? atoi(exec_env) : 0;
  if (exec_workers < 0) {
    exec_workers = 0;
  }
  BaiPegExecutor *executor =
      exec_workers > 0 ? bai_peg_executor_create(exec_workers, 100) : NULL;
  // Toggle to drop the pass cand entirely — useful for re-evaluating the
  // top tile-play(s) at deeper plies without paying for pass eval.
  const char *inc_pass_env = getenv("PEG_INCLUDE_PASS");
  bool include_pass =
      !(inc_pass_env && *inc_pass_env && atoi(inc_pass_env) == 0);

  BaiPegArgs args = {
      .game = game,
      .thread_control = config_get_thread_control(config),
      .num_threads = 1,
      .tt_fraction_of_mem = 0.0,
      .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
      .initial_top_k = top_k,
      .max_depth = sweep_d,
      .endgame_time_per_solve = 5.0, // generous per-eval cap
      .time_budget_seconds = 0.0,    // no global budget; let depth gate
      .puct_c = 1.0,
      .utility_alpha = 1e-4, // pure-win-pct ranking, tiny spread tiebreak
      .progressive_widening = false, // we want all 32 in the active set
      .min_active = 0,
      .sweep_max_depth = sweep_d,
      .include_pass = include_pass,
      .pass_opp_max_depth = opp_d,
      .executor = executor,
      .log_solve_details = true,
      .request_cand_stats = true,
  };
  BaiPegResult result;
  ErrorStack *error_stack = error_stack_create();
  bai_peg_solve(&args, &result, error_stack);
  assert(error_stack_is_empty(error_stack));

  printf("\n=== French Test1PEGPass result ===\n");
  printf("evals_done=%d  cands_considered=%d  time=%.2fs\n",
         result.evaluations_done, result.candidates_considered,
         result.seconds_elapsed);
  printf("best move: %s  (small_move_is_pass=%d)\n",
         small_move_is_pass(&result.best_move) ? "(Pass)" : "<non-pass>",
         (int)small_move_is_pass(&result.best_move));
  printf("best win%%=%.4f  spread=%+0.4f  depth=%d\n", result.best_win_pct,
         result.best_mean_spread, result.best_depth_evaluated);

  // Dump full per-cand ranking so we can compare pass against the top
  // tile-play options at the chosen depth.
  if (result.cand_stats) {
    printf("Per-cand stats (rank by final utility):\n");
    int show =
        result.candidates_considered < 20 ? result.candidates_considered : 20;
    for (int i = 0; i < show; i++) {
      const BaiCandStats *s = &result.cand_stats[i];
      printf("  [%2d] static_score=%-3d depth=%-2d visits=%-3d "
             "final_q_win%%=%.3f final_q_spread=%+0.2f%s\n",
             i, s->static_score, s->depth_evaluated, s->visits,
             s->final_q_win_pct, s->final_q_mean_spread,
             s->is_best ? "  <-- BEST" : "");
    }

    // Pass-vs-best-other comparison: macondo + old peg.c claim pass is the
    // best move on this position (5.5/8 = 0.6875). Surface whether our
    // solver agrees by comparing pass cand's final win% against the best
    // non-pass cand's.
    int pass_idx = -1;
    int best_other_idx = -1;
    for (int i = 0; i < result.candidates_considered; i++) {
      const BaiCandStats *s = &result.cand_stats[i];
      bool is_pass = small_move_is_pass(&s->move);
      if (is_pass) {
        pass_idx = i;
      } else {
        if (best_other_idx < 0 ||
            s->final_q_win_pct >
                result.cand_stats[best_other_idx].final_q_win_pct ||
            (s->final_q_win_pct ==
                 result.cand_stats[best_other_idx].final_q_win_pct &&
             s->final_q_mean_spread >
                 result.cand_stats[best_other_idx].final_q_mean_spread)) {
          best_other_idx = i;
        }
      }
    }
    if (pass_idx >= 0 && best_other_idx >= 0) {
      const BaiCandStats *p = &result.cand_stats[pass_idx];
      const BaiCandStats *o = &result.cand_stats[best_other_idx];
      double dwin = p->final_q_win_pct - o->final_q_win_pct;
      double dspread = p->final_q_mean_spread - o->final_q_mean_spread;
      const char *verdict;
      if (dwin > 1e-9) {
        verdict = "PASS BEST (strict win%)";
      } else if (dwin < -1e-9) {
        verdict = "TILE-PLAY BEST (strict win%)";
      } else if (dspread > 1e-9) {
        verdict = "PASS BEST (win% tied, higher spread)";
      } else if (dspread < -1e-9) {
        verdict = "TILE-PLAY BEST (win% tied, higher spread)";
      } else {
        verdict = "TIE (identical win% and spread)";
      }
      printf("Pass-vs-best-other: pass win%%=%.4f spread=%+0.4f depth=%d | "
             "best-other (rank %d) static_score=%d win%%=%.4f "
             "spread=%+0.4f depth=%d | dwin=%+.4f dspread=%+.4f -> %s\n",
             p->final_q_win_pct, p->final_q_mean_spread, p->depth_evaluated,
             best_other_idx, o->static_score, o->final_q_win_pct,
             o->final_q_mean_spread, o->depth_evaluated, dwin, dspread,
             verdict);
      // Macondo's Test1PEGPass on the same CGP at plies=4..8 asserts pass is
      // the unique best move with Points=5.5/8 = 0.6875. Once the solver and
      // the scenario sweep depth are deep enough, we should match. Only
      // assert when both env knobs leave defaults large enough to converge:
      // PEG_OPP_DEPTH unset (or >=2) AND PEG_TOP_K unset (or >=8) AND
      // PEG_INCLUDE_PASS unset (i.e. include_pass=true). We deliberately
      // skip the assertion when callers tweak those knobs for diagnostics.
      if (include_pass && top_k >= 8 && opp_d >= 2 && sweep_d >= 1) {
        if (dwin <= 1e-9) {
          fprintf(stderr,
                  "FAIL: pass should be best on French CGP (5.5/8 = "
                  "0.6875). Got pass=%.4f best-other=%.4f.\n",
                  p->final_q_win_pct, o->final_q_win_pct);
        }
        assert(dwin > 1e-9 &&
               "pass must beat the best non-pass cand on French CGP");
        if (p->final_q_win_pct < 0.685 || p->final_q_win_pct > 0.69) {
          fprintf(stderr,
                  "FAIL: pass win%% should be ~0.6875 (matches macondo). "
                  "Got %.4f.\n",
                  p->final_q_win_pct);
        }
        assert(p->final_q_win_pct > 0.685 && p->final_q_win_pct < 0.69 &&
               "pass win%% must be the macondo 5.5/8 = 0.6875");
      }
    } else {
      printf("Pass-vs-best-other: pass cand %s; best-other cand %s\n",
             pass_idx >= 0 ? "found" : "MISSING",
             best_other_idx >= 0 ? "found" : "MISSING");
    }
  }
  printf("==============================\n");

  error_stack_destroy(error_stack);
  bai_cand_stats_free(result.cand_stats);
  if (executor) {
    bai_peg_executor_destroy(executor);
  }
  config_destroy(config);
}

void test_bai_peg(void) {
  test_bai_peg_smoke();
  test_bai_peg_progressive_widening();
  test_bai_peg_concurrent_invocations();
  test_bai_peg_returns_pass_when_no_scoring_plays();
  test_bai_peg_rejects_non_one_in_bag();
  test_bai_peg_accepts_empty_opp_rack();
}

// English (TWL98) seed=9973 candidate from the pass-PEG search: mover
// has AEINRST, opp has AEINRST, bag has Q. Mover lead +19. Q unplayable
// at the root, single bingo lane, all bingos answerable. Engineered to
// be a position where pass should be the best move for verification
// against macondo.
#define BAI_PEG_TEST_ENGLISH_PASS_CGP                                          \
  "cgp 3P3B5AG/3R1ELOIN3XI/2DIGLOT6M/3C2PFUI2AWE/3E3L1FRUGAL/3Y3Y5V1/"         \
  "8JO2HE1/5E1IODIZED1/3UNBATED1OW2/5O5O3/5N5S3/5I5p3/5T2COVET2/"              \
  "5E1AAH1R3/1UNmASKER2M3 AEINRST/AEINRST 398/379 0 -lex TWL98"

void test_bai_peg_english_pass_solve(void) {
  Config *config = config_create_or_die("set -s1 score -s2 score");
  load_and_exec_config_or_die(config, BAI_PEG_TEST_ENGLISH_PASS_CGP);
  Game *game = config_get_game(config);
  assert(rack_get_total_letters(player_get_rack(game_get_player(
             game, game_get_player_on_turn_index(game)))) == RACK_SIZE);

  const char *env_d = getenv("PEG_SWEEP_DEPTH");
  int sweep_d = env_d && *env_d ? atoi(env_d) : 1;
  if (sweep_d < 1) {
    sweep_d = 1;
  }
  const char *opp_d_env = getenv("PEG_OPP_DEPTH");
  int opp_d = opp_d_env && *opp_d_env ? atoi(opp_d_env) : 4;
  if (opp_d < 1) {
    opp_d = 1;
  }
  const char *topk_env = getenv("PEG_TOP_K");
  int top_k = topk_env && *topk_env ? atoi(topk_env) : 32;
  if (top_k < 1) {
    top_k = 1;
  }
  const char *exec_env = getenv("PEG_EXEC_WORKERS");
  int exec_workers = exec_env && *exec_env ? atoi(exec_env) : 8;
  if (exec_workers < 0) {
    exec_workers = 0;
  }
  BaiPegExecutor *executor =
      exec_workers > 0 ? bai_peg_executor_create(exec_workers, 100) : NULL;
  const char *inc_pass_env = getenv("PEG_INCLUDE_PASS");
  bool include_pass =
      !(inc_pass_env && *inc_pass_env && atoi(inc_pass_env) == 0);

  BaiPegArgs args = {
      .game = game,
      .thread_control = config_get_thread_control(config),
      .num_threads = 1,
      .tt_fraction_of_mem = 0.0,
      .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
      .initial_top_k = top_k,
      .max_depth = sweep_d,
      .endgame_time_per_solve = 5.0,
      .time_budget_seconds = 0.0,
      .puct_c = 1.0,
      .utility_alpha = 1e-4,
      .progressive_widening = false,
      .min_active = 0,
      .sweep_max_depth = sweep_d,
      .include_pass = include_pass,
      .pass_opp_max_depth = opp_d,
      .executor = executor,
      .log_solve_details = true,
      .request_cand_stats = true,
  };
  BaiPegResult result;
  ErrorStack *error_stack = error_stack_create();
  bai_peg_solve(&args, &result, error_stack);
  assert(error_stack_is_empty(error_stack));

  printf("\n=== English seed=9973 pass-PEG result ===\n");
  printf("evals_done=%d  cands=%d  time=%.2fs\n", result.evaluations_done,
         result.candidates_considered, result.seconds_elapsed);
  printf("best move: %s  win%%=%.4f  spread=%+0.4f  depth=%d\n",
         small_move_is_pass(&result.best_move) ? "(Pass)" : "<non-pass>",
         result.best_win_pct, result.best_mean_spread,
         result.best_depth_evaluated);

  if (result.cand_stats) {
    printf("Top-15 cand stats:\n");
    int show =
        result.candidates_considered < 15 ? result.candidates_considered : 15;
    const LetterDistribution *ld_print = game_get_ld(game);
    for (int i = 0; i < show; i++) {
      const BaiCandStats *s = &result.cand_stats[i];
      StringBuilder *sb = string_builder_create();
      if (small_move_is_pass(&s->move)) {
        string_builder_add_string(sb, "(Pass)");
      } else {
        Move m;
        small_move_to_move(&m, &s->move, game_get_board(game));
        string_builder_add_move(sb, game_get_board(game), &m, ld_print,
                                /*add_score=*/true);
      }
      printf("  [%2d] %-30s static=%-3d depth=%d "
             "win%%=%.4f spread=%+0.2f%s\n",
             i, string_builder_peek(sb), s->static_score, s->depth_evaluated,
             s->final_q_win_pct, s->final_q_mean_spread,
             s->is_best ? "  <-- BEST" : "");
      string_builder_destroy(sb);
    }
  }
  printf("==========================================\n");

  error_stack_destroy(error_stack);
  bai_cand_stats_free(result.cand_stats);
  if (executor) {
    bai_peg_executor_destroy(executor);
  }
  config_destroy(config);
}

// ---------------------------------------------------------------------------
// Oracle eval: deep-plies endgame_solve evaluation of a single tile-placement
// move across all bag-tile scenarios. Ported (and trimmed) from peg-solver's
// benchmark_peg_test.c. Not safe for pass moves (which need a 1peg sub-solve
// to model opp's response — use bp_evaluate_pass for that).
// ---------------------------------------------------------------------------

static void oracle_compute_unseen(const Game *game, int mover_idx,
                                  uint8_t unseen[MAX_ALPHABET_SIZE]) {
  const LetterDistribution *ld = game_get_ld(game);
  int ld_size = ld_get_size(ld);
  for (int ml = 0; ml < ld_size; ml++) {
    unseen[ml] = (uint8_t)ld_get_dist(ld, ml);
  }
  const Rack *mover_rack = player_get_rack(game_get_player(game, mover_idx));
  for (int ml = 0; ml < ld_size; ml++) {
    unseen[ml] -= (uint8_t)rack_get_letter(mover_rack, ml);
  }
  const Board *board = game_get_board(game);
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      if (board_is_empty(board, row, col)) {
        continue;
      }
      MachineLetter ml = board_get_letter(board, row, col);
      if (get_is_blanked(ml)) {
        if (unseen[BLANK_MACHINE_LETTER] > 0) {
          unseen[BLANK_MACHINE_LETTER]--;
        }
      } else {
        if (unseen[ml] > 0) {
          unseen[ml]--;
        }
      }
    }
  }
}

static void oracle_set_opp_rack(Rack *opp_rack,
                                const uint8_t unseen[MAX_ALPHABET_SIZE],
                                int ld_size, MachineLetter bag_tile) {
  rack_reset(opp_rack);
  for (int ml = 0; ml < ld_size; ml++) {
    int cnt = (int)unseen[ml] - (ml == bag_tile ? 1 : 0);
    for (int i = 0; i < cnt; i++) {
      rack_add_letter(opp_rack, (MachineLetter)ml);
    }
  }
}

static void oracle_eval_tile_play(const Game *base_game_empty, const Move *move,
                                  int mover_idx, int opp_idx,
                                  const uint8_t unseen[MAX_ALPHABET_SIZE],
                                  int ld_size, int plies, double time_budget,
                                  ThreadControl *tc, FILE *log,
                                  double *win_pct_out, double *spread_out) {
  MachineLetter tile_types[MAX_ALPHABET_SIZE];
  int tile_counts[MAX_ALPHABET_SIZE];
  int num_tile_types = 0;
  for (int t = 0; t < ld_size; t++) {
    if (unseen[t] > 0) {
      tile_types[num_tile_types] = (MachineLetter)t;
      tile_counts[num_tile_types] = (int)unseen[t];
      num_tile_types++;
    }
  }

  double total_spread = 0.0;
  double total_wins = 0.0;
  int weight = 0;
  const LetterDistribution *ld = game_get_ld(base_game_empty);

  EndgameCtx *eg_ctx = NULL;
  EndgameResults *eg_results = endgame_results_create();

  for (int ti = 0; ti < num_tile_types; ti++) {
    MachineLetter tile = tile_types[ti];
    int cnt = tile_counts[ti];

    Game *g = game_duplicate(base_game_empty);
    game_set_endgame_solving_mode(g);
    game_set_backup_mode(g, BACKUP_MODE_OFF);

    Move scenario_move = *move;
    play_move_without_drawing_tiles(&scenario_move, g);
    game_set_game_end_reason(g, GAME_END_REASON_NONE);
    game_set_consecutive_scoreless_turns(g, 0);

    Rack *opp_rack = player_get_rack(game_get_player(g, opp_idx));
    oracle_set_opp_rack(opp_rack, unseen, ld_size, tile);
    Rack *mover_rack = player_get_rack(game_get_player(g, mover_idx));
    rack_add_letter(mover_rack, tile);

    int32_t mover_lead =
        equity_to_int(player_get_score(game_get_player(g, mover_idx))) -
        equity_to_int(player_get_score(game_get_player(g, opp_idx)));

    int64_t deadline_ns =
        ctimer_monotonic_ns() + (int64_t)(time_budget * 1.0e9);
    EndgameArgs ea = {
        .thread_control = tc,
        .game = g,
        .plies = plies,
        .initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
        .num_threads = 1,
        .use_heuristics = true,
        .num_top_moves = 1,
        .skip_word_pruning = true,
        .soft_time_limit = time_budget,
        .hard_time_limit = time_budget,
        .external_deadline_ns = deadline_ns,
        .thread_index_offset = 200, // far from PEG/BAI cache slots
    };

    Timer eg_timer;
    ctimer_start(&eg_timer);
    endgame_results_reset(eg_results);
    endgame_solve_inline(&eg_ctx, &ea, eg_results);
    double eg_secs = ctimer_elapsed_seconds(&eg_timer);

    int eg_val = endgame_results_get_value(eg_results, ENDGAME_RESULT_BEST);
    int eg_depth = endgame_results_get_depth(eg_results, ENDGAME_RESULT_BEST);
    int32_t mover_total = (int32_t)mover_lead - eg_val;

    total_spread += (double)mover_total * cnt;
    double scenario_win;
    if (mover_total > 0) {
      scenario_win = 1.0;
    } else if (mover_total == 0) {
      scenario_win = 0.5;
    } else {
      scenario_win = 0.0;
    }
    total_wins += scenario_win * cnt;
    weight += cnt;

    if (log) {
      const char *outcome = mover_total > 0    ? "WIN"
                            : mover_total == 0 ? "TIE"
                                               : "LOSS";
      fprintf(log,
              "      bag=%s (cnt=%d): final_spread=%+d  %s  depth=%d  "
              "time=%.2fs\n",
              ld->ld_ml_to_hl[tile], cnt, mover_total, outcome, eg_depth,
              eg_secs);
      fflush(log);
    }

    game_destroy(g);
  }

  endgame_results_destroy(eg_results);
  endgame_ctx_destroy(eg_ctx);

  *win_pct_out = (weight > 0) ? total_wins / weight : 0.0;
  *spread_out = (weight > 0) ? total_spread / weight : 0.0;
}

void test_bai_peg_french_oracle(void) {
  Config *config = config_create_or_die("set -s1 score -s2 score");
  load_and_exec_config_or_die(config, BAI_PEG_TEST_FRENCH_PASS_CGP);
  Game *game = config_get_game(config);

  int mover_idx = game_get_player_on_turn_index(game);
  int opp_idx = 1 - mover_idx;
  const LetterDistribution *ld = game_get_ld(game);
  int ld_size = ld_get_size(ld);

  uint8_t unseen[MAX_ALPHABET_SIZE];
  oracle_compute_unseen(game, mover_idx, unseen);

  // Build base_game_empty: bag drained, opp rack still empty (CGP form),
  // mover rack preserved. Each scenario rebuilds opp's rack from `unseen -
  // {bag_tile}` and adds the bag_tile to mover's rack before solving.
  Game *base_game_empty = game_duplicate(game);
  game_set_endgame_solving_mode(base_game_empty);
  game_set_backup_mode(base_game_empty, BACKUP_MODE_OFF);
  {
    Rack saved_rack;
    rack_copy(&saved_rack,
              player_get_rack(game_get_player(base_game_empty, mover_idx)));
    Bag *bag = game_get_bag(base_game_empty);
    for (int ml = 0; ml < ld_size; ml++) {
      while (bag_get_letter(bag, ml) > 0) {
        bag_draw_letter(bag, (MachineLetter)ml, mover_idx);
      }
    }
    rack_copy(player_get_rack(game_get_player(base_game_empty, mover_idx)),
              &saved_rack);
  }

  // Get the top-equity move (should be the score=62 bingo we've been
  // investigating).
  MoveList *top_ml = move_list_create(1);
  const MoveGenArgs gen_args = {
      .game = game,
      .move_list = top_ml,
      .move_record_type = MOVE_RECORD_BEST,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&gen_args);
  assert(move_list_get_count(top_ml) > 0);
  const Move *bingo_move = move_list_get_move(top_ml, 0);
  Move bingo_copy = *bingo_move;

  StringBuilder *sb = string_builder_create();
  string_builder_add_move(sb, game_get_board(game), &bingo_copy, ld,
                          /*add_score=*/true);
  printf("\n=== French oracle eval — TILE PLAY only ===\n");
  printf("Move: %s  (score=%d, tiles_played=%d)\n", string_builder_peek(sb),
         (int)equity_to_int(move_get_score(&bingo_copy)),
         move_get_tiles_played(&bingo_copy));
  string_builder_destroy(sb);

  // Plies and per-scenario time cap from env.
  const char *plies_env = getenv("ORACLE_PLIES");
  int plies = plies_env && *plies_env ? atoi(plies_env) : 8;
  if (plies < 1) {
    plies = 1;
  }
  if (plies > MAX_SEARCH_DEPTH) {
    plies = MAX_SEARCH_DEPTH;
  }
  const char *time_env = getenv("ORACLE_TIME");
  double time_budget = time_env && *time_env ? atof(time_env) : 60.0;

  printf("plies=%d  time_budget=%.1fs per scenario\n\n", plies, time_budget);

  double win_pct = 0.0;
  double spread = 0.0;
  oracle_eval_tile_play(base_game_empty, &bingo_copy, mover_idx, opp_idx,
                        unseen, ld_size, plies, time_budget,
                        config_get_thread_control(config), stdout, &win_pct,
                        &spread);

  printf("\nResult: win%%=%.4f  spread=%+0.4f\n", win_pct, spread);

  move_list_destroy(top_ml);
  game_destroy(base_game_empty);
  config_destroy(config);
}
