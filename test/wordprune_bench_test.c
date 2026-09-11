#include "wordprune_bench_test.h"

#include "../src/compat/ctime.h"
#include "../src/def/kwg_defs.h"
#include "../src/ent/dictionary_word.h"
#include "../src/ent/move.h"
#include "../src/ent/board.h"
#include "../src/impl/move_gen.h"
#include "../src/util/io_util.h"
#include "../src/ent/game.h"
#include "../src/ent/kwg.h"
#include "../src/ent/player.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/words.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/kwg_maker.h"
#include "../src/impl/word_prune.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Per-position cost of endgame word pruning: time generate_possible_words and
// make_kwg_from_words_small separately, report word and node counts.
//   WPB_CGP   CGP file (one position per line)
//   WPB_LEX   lexicon (default CSW24)
//   WPB_REPS  repetitions per position (default 5, min over reps reported)
//   WPB_SOLVE_PLIES  if > 0, also solve each position single-threaded with
//             word pruning on and off (fresh solver each), reporting times/nodes
void test_wordprune_bench(void) {
  const char *cgp_file = getenv("WPB_CGP");
  const char *lex = getenv("WPB_LEX") ? getenv("WPB_LEX") : "CSW24";
  const int reps = getenv("WPB_REPS") ? atoi(getenv("WPB_REPS")) : 5;
  const int solve_plies =
      getenv("WPB_SOLVE_PLIES") ? atoi(getenv("WPB_SOLVE_PLIES")) : 0;
  const bool heuristics =
      !getenv("WPB_HEURISTICS") || atoi(getenv("WPB_HEURISTICS"));
  const char *actual_spec = getenv("WPB_ACTUAL"); // "tiny score length tiles"
  // WPB_MODE1: "full" (mode 1 skips pruning) or "refined" (mode 1 uses the
  // cross-check-aware prototype prune); mode 0 is always the current prune.
  const char *mode1 = getenv("WPB_MODE1") ? getenv("WPB_MODE1") : "full";
  const bool mode1_refined = strcmp(mode1, "refined") == 0;
  long sum_refined_words = 0;
  double sum_refined_gen = 0;
  double sum_solve_pruned = 0;
  double sum_solve_full = 0;
  int node_mismatches = 0;
  assert(cgp_file);
  char settings[256];
  (void)snprintf(settings, sizeof(settings), "set -lex %s -threads 1", lex);
  Config *config = config_create_or_die(settings);
  load_and_exec_config_or_die(config, "new");
  Game *game = config_get_game(config);
  FILE *file = fopen(cgp_file, "r");
  assert(file);
  char line[2048];
  int index = 0;
  double sum_gen = 0;
  double sum_kwg = 0;
  long sum_words = 0;
  long sum_nodes = 0;
  long full_nodes = 0;
  while (fgets(line, sizeof(line), file)) {
    ErrorStack *error = error_stack_create();
    game_load_cgp(game, line, error);
    assert(error_stack_is_empty(error));
    error_stack_destroy(error);
    const KWG *full_kwg = player_get_kwg(game_get_player(game, 0));
    full_nodes = (long)kwg_get_number_of_nodes(full_kwg);
    double best_gen = 1e9;
    double best_kwg = 1e9;
    int words = 0;
    long nodes = 0;
    for (int rep = 0; rep < reps; rep++) {
      DictionaryWordList *word_list = dictionary_word_list_create();
      Timer timer;
      ctimer_start(&timer);
      generate_possible_words(game, full_kwg, word_list);
      const double gen = ctimer_elapsed_seconds(&timer);
      ctimer_start(&timer);
      KWG *pruned = make_kwg_from_words_small(word_list, KWG_MAKER_OUTPUT_GADDAG,
                                              KWG_MAKER_MERGE_EXACT);
      const double build = ctimer_elapsed_seconds(&timer);
      words = dictionary_word_list_get_count(word_list);
      nodes = (long)kwg_get_number_of_nodes(pruned);
      kwg_destroy(pruned);
      dictionary_word_list_destroy(word_list);
      if (gen < best_gen) {
        best_gen = gen;
      }
      if (build < best_kwg) {
        best_kwg = build;
      }
    }
    double solve_pruned = 0;
    double solve_full = 0;
    if (solve_plies > 0) {
      uint64_t nodes_by_mode[2] = {0, 0};
      int value_by_mode[2] = {0, 0};
      for (int mode = 0; mode < 2; mode++) {
        EndgameCtx *solver = NULL;
        EndgameResults *results = endgame_results_create();
        ErrorStack *solve_error = error_stack_create();
        EndgameArgs args = {.game = game,
                            .thread_control = config_get_thread_control(config),
                            .plies = solve_plies,
                            .tt_fraction_of_mem = 0.01,
                            .initial_small_move_arena_size =
                                DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                            .num_threads = 1,
                            .num_top_moves = 1,
                            .use_heuristics = heuristics,
                            .skip_word_pruning = mode == 1 && !mode1_refined,
                            .seed = 42};
        Move actual_move;
        if (actual_spec) {
          unsigned long long tiny;
          unsigned score, length, played;
          const int fields = sscanf(actual_spec, "%llu %u %u %u", &tiny,
                                    &score, &length, &played);
          assert(fields == 4);
          SmallMove small = {0};
          small.tiny_move = tiny;
          small.metadata.score = score;
          small.metadata.play_length = length;
          small.metadata.tiles_played = played;
          small_move_to_move(&actual_move, &small, game_get_board(game));
          args.actual_move = &actual_move;
        }
        if (mode == 1 && mode1_refined) {
          setenv("WORDPRUNE_REFINE", "8", 1);
        }
        Timer timer;
        ctimer_start(&timer);
        endgame_solve(&solver, &args, results, solve_error);
        const double elapsed = ctimer_elapsed_seconds(&timer);
        unsetenv("WORDPRUNE_REFINE");
        assert(error_stack_is_empty(solve_error));
        nodes_by_mode[mode] = endgame_ctx_get_nodes_searched(solver);
        value_by_mode[mode] =
            endgame_results_get_value(results, ENDGAME_RESULT_BEST);
        {
          const PVLine *pv =
              endgame_results_get_pvline(results, ENDGAME_RESULT_BEST);
          printf("WPBPV,%d,%d,%llu,%u,%u,%u\n", index, mode,
                 (unsigned long long)(pv->num_moves ? pv->moves[0].tiny_move
                                                    : 0),
                 pv->num_moves ? (unsigned)pv->moves[0].metadata.score : 0U,
                 pv->num_moves ? (unsigned)pv->moves[0].metadata.play_length
                               : 0U,
                 pv->num_moves ? (unsigned)pv->moves[0].metadata.tiles_played
                               : 0U);
        }
        if (actual_spec) {
          printf("WPBACT,%d,%d,%d,%d,%d\n", index, mode,
                 endgame_results_get_actual_move_found(results),
                 endgame_results_get_value(results, ENDGAME_RESULT_ACTUAL),
                 endgame_results_get_depth(results, ENDGAME_RESULT_ACTUAL));
        }
        if (mode == 0) {
          solve_pruned = elapsed;
        } else {
          solve_full = elapsed;
        }
        endgame_ctx_destroy(solver);
        endgame_results_destroy(results);
        error_stack_destroy(solve_error);
      }
      if (nodes_by_mode[0] != nodes_by_mode[1] ||
          value_by_mode[0] != value_by_mode[1]) {
        node_mismatches++;
      }
      printf("WPBSOLVE,%d,%.6f,%.6f,%llu,%llu,%d,%d\n", index, solve_pruned,
             solve_full, (unsigned long long)nodes_by_mode[0],
             (unsigned long long)nodes_by_mode[1], value_by_mode[0],
             value_by_mode[1]);
      sum_solve_pruned += solve_pruned;
      sum_solve_full += solve_full;
    }
    {
      DictionaryWordList *refined_list = dictionary_word_list_create();
      WordPruneRefineStats rstats = {0};
      Timer rtimer;
      ctimer_start(&rtimer);
      generate_possible_words_refined(game, full_kwg, refined_list, 8, &rstats);
      const double rgen = ctimer_elapsed_seconds(&rtimer);
      printf("WPBREF,%d,%.6f,%d,%d,%d,%d,%d,%d\n", index, rgen,
             dictionary_word_list_get_count(refined_list), rstats.passes,
             rstats.nonplaythrough_words, rstats.words_after_pass[0],
             rstats.words_after_pass[1], rstats.words_after_pass[2]);
      sum_refined_words += dictionary_word_list_get_count(refined_list);
      sum_refined_gen += rgen;
      dictionary_word_list_destroy(refined_list);
    }
    printf("WPB,%d,%.6f,%.6f,%d,%ld\n", index++, best_gen, best_kwg, words,
           nodes);
    sum_gen += best_gen;
    sum_kwg += best_kwg;
    sum_words += words;
    sum_nodes += nodes;
  }
  fclose(file);
  printf("WPBSUM positions=%d gen_total=%.6f kwg_total=%.6f words_total=%ld "
         "nodes_total=%ld full_kwg_nodes=%ld solve_pruned_total=%.6f "
         "solve_full_total=%.6f node_or_value_mismatches=%d "
         "refined_words_total=%ld refined_gen_total=%.6f mode1=%s\n",
         index, sum_gen, sum_kwg, sum_words, sum_nodes, full_nodes,
         sum_solve_pruned, sum_solve_full, node_mismatches, sum_refined_words,
         sum_refined_gen, mode1);
  config_destroy(config);
}


// ---- Safety check for the refined prune ----
// Every word formed by every legal move in every position reachable within
// max_plies (all move sequences, both players, full lexicon movegen) must be
// in the refined word list. WPB_SAFE_PLIES sets the depth (default 1);
// WPB_SAFE_BRANCH caps the moves expanded per ply after the first (default
// all). Passes are not expanded (they form no words); the bag is empty so
// there are no exchanges.
static int wp_words_checked;
static int wp_words_missing;
static int wp_moves_played;
static const DictionaryWordList *wp_refined;

static bool wp_list_contains(const DictionaryWordList *list,
                             const MachineLetter *word, int length) {
  int lo = 0;
  int hi = dictionary_word_list_get_count(list) - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    const DictionaryWord *w = dictionary_word_list_get_word(list, mid);
    const int wl = dictionary_word_get_length(w);
    const int n = length < wl ? length : wl;
    int cmp = memcmp(dictionary_word_get_word(w), word, (size_t)n);
    if (cmp == 0) {
      cmp = wl - length;
    }
    if (cmp == 0) {
      return true;
    }
    if (cmp < 0) {
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return false;
}

static void wp_check_reachable(Game *game, int plies_left, int branch_cap) {
  if (plies_left == 0 ||
      game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
    return;
  }
  MoveList *moves = move_list_create(100000);
  const MoveGenArgs args = {.game = game,
                            .move_list = moves,
                            .move_record_type = MOVE_RECORD_ALL,
                            .move_sort_type = MOVE_SORT_SCORE,
                            .override_kwg = NULL,
                            .eq_margin_movegen = 0,
                            .target_equity = EQUITY_MAX_VALUE,
                            .target_leave_size_for_exchange_cutoff =
                                UNSET_LEAVE_SIZE};
  generate_moves(&args);
  const int count = move_list_get_count(moves);
  int expanded = 0;
  for (int i = 0; i < count; i++) {
    const Move *move = move_list_get_move(moves, i);
    if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      continue;
    }
    if (branch_cap > 0 && expanded >= branch_cap) {
      break;
    }
    expanded++;
    FormedWords *fw = formed_words_create(game_get_board(game), move);
    for (int w = 0; w < formed_words_get_num_words(fw); w++) {
      wp_words_checked++;
      if (!wp_list_contains(wp_refined, formed_words_get_word(fw, w),
                            formed_words_get_word_length(fw, w))) {
        wp_words_missing++;
        if (wp_words_missing <= 20) {
          printf("WPBMISS word_len=%d\n", formed_words_get_word_length(fw, w));
        }
      }
    }
    formed_words_destroy(fw);
    play_move(move, game, NULL); // play_move backs up the game itself
    wp_moves_played++;
    wp_check_reachable(game, plies_left - 1, branch_cap);
    game_unplay_last_move(game);
  }
  move_list_destroy(moves);
}

void test_wordprune_safety(void) {
  const char *cgp_file = getenv("WPB_CGP");
  const char *lex = getenv("WPB_LEX") ? getenv("WPB_LEX") : "CSW24";
  const int plies = getenv("WPB_SAFE_PLIES") ? atoi(getenv("WPB_SAFE_PLIES")) : 1;
  const int branch = getenv("WPB_SAFE_BRANCH") ? atoi(getenv("WPB_SAFE_BRANCH")) : 0;
  assert(cgp_file);
  char settings[256];
  (void)snprintf(settings, sizeof(settings), "set -lex %s -threads 1", lex);
  Config *config = config_create_or_die(settings);
  load_and_exec_config_or_die(config, "new");
  Game *game = config_get_game(config);
  game_set_backup_mode(game, BACKUP_MODE_SIMULATION);
  FILE *file = fopen(cgp_file, "r");
  assert(file);
  char line[2048];
  int index = 0;
  int total_missing = 0;
  long total_checked = 0;
  long total_moves = 0;
  while (fgets(line, sizeof(line), file)) {
    ErrorStack *error = error_stack_create();
    game_load_cgp(game, line, error);
    assert(error_stack_is_empty(error));
    error_stack_destroy(error);
    const KWG *full_kwg = player_get_kwg(game_get_player(game, 0));
    DictionaryWordList *refined = dictionary_word_list_create();
    generate_possible_words_refined(game, full_kwg, refined, 8, NULL);
    wp_refined = refined;
    wp_words_checked = 0;
    wp_words_missing = 0;
    wp_moves_played = 0;
    wp_check_reachable(game, plies, branch);
    printf("WPBSAFE,%d,%d,%d,%d,%d\n", index++, wp_moves_played,
           wp_words_checked, wp_words_missing,
           dictionary_word_list_get_count(refined));
    total_missing += wp_words_missing;
    total_checked += wp_words_checked;
    total_moves += wp_moves_played;
    dictionary_word_list_destroy(refined);
  }
  fclose(file);
  printf("WPBSAFESUM positions=%d moves=%ld words=%ld missing=%d plies=%d branch=%d\n",
         index, total_moves, total_checked, total_missing, plies, branch);
  config_destroy(config);
}
