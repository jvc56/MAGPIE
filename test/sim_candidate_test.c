#include "sim_candidate_test.h"

#include "../src/def/board_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/players_data_defs.h"
#include "../src/def/tws_defense_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/bonus_square.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/klv.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/stats.h"
#include "../src/ent/tws_defense.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// Candidate recall harness (on-demand test "candrecall").
//
// Samples mid-game positions from seeded static self-play and, for each,
// simulates a pool of the top P static-equity plays with round-robin sampling
// at a fixed per-arm sample count, then writes one CSV row per play with its
// static-equity components and its sim results. Selection rules (top-k by
// equity, coverage variants) are then evaluated offline on the same data, so
// candidate selection is measured in isolation from the sampling rule and
// the rollout policy. Configured by environment variables:
//   CANDRECALL_OUT        output CSV path (default candrecall.csv)
//   CANDRECALL_POSITIONS  positions to sample (default 300)
//   CANDRECALL_POOL       pool size P (default 60)
//   CANDRECALL_ITERS      samples per arm (default 400)
//   CANDRECALL_PLIES      sim plies (default 2)
//   CANDRECALL_THREADS    sim threads (default 8)
//   CANDRECALL_MAX_TURNS  positions are taken after 0..MAX_TURNS-1 static
//                         plays (default 20)
//   CANDRECALL_SEED       base seed (default 1)
//   CANDRECALL_TWD        TWS defense weights the players use (default none)
//   CANDRECALL_TWD_DIAG   TWS defense weights scored into the twd column but
//                         NOT given to the players, so the term is a
//                         diagnostic that never enters static equity or the
//                         candidate pool (default: whatever CANDRECALL_TWD is)
//   CANDRECALL_PATH       -path value (default ./data)
//   CANDRECALL_LEX        lexicon (default CSW21)
//   CANDRECALL_WMP        "true"/"false" for -wmp (default true; the super
//                         board's lexica ship a 15-board wmp, so use false)

enum {
  CANDRECALL_DEFAULT_POSITIONS = 300,
  CANDRECALL_DEFAULT_POOL = 60,
  CANDRECALL_DEFAULT_ITERS = 400,
  CANDRECALL_DEFAULT_PLIES = 2,
  CANDRECALL_DEFAULT_THREADS = 8,
  CANDRECALL_DEFAULT_MAX_TURNS = 20,
  CANDRECALL_DEFAULT_SEED = 1,
  // Skip positions too close to the endgame for a 2-ply sim to be the
  // right tool; the play chooser hands those to PEG.
  CANDRECALL_MIN_BAG = 8,
  CANDRECALL_CMD_SIZE = 1024,
};

static long candrecall_env_long(const char *name, long default_value) {
  const char *value = getenv(name);
  if (value == NULL || value[0] == '\0') {
    return default_value;
  }
  return strtol(value, NULL, 10);
}

static const char *candrecall_env_string(const char *name,
                                         const char *default_value) {
  const char *value = getenv(name);
  if (value == NULL || value[0] == '\0') {
    return default_value;
  }
  return value;
}

// xorshift64*: a self-contained generator so the position sample is
// reproducible from CANDRECALL_SEED alone.
static uint64_t candrecall_next_random(uint64_t *state) {
  uint64_t x = *state;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  *state = x;
  return x * 2685821657736338717ULL;
}

void test_candidate_recall(void) {
  const char *out_path =
      candrecall_env_string("CANDRECALL_OUT", "candrecall.csv");
  const long num_positions =
      candrecall_env_long("CANDRECALL_POSITIONS", CANDRECALL_DEFAULT_POSITIONS);
  const long pool_size =
      candrecall_env_long("CANDRECALL_POOL", CANDRECALL_DEFAULT_POOL);
  const long iters_per_arm =
      candrecall_env_long("CANDRECALL_ITERS", CANDRECALL_DEFAULT_ITERS);
  const long plies =
      candrecall_env_long("CANDRECALL_PLIES", CANDRECALL_DEFAULT_PLIES);
  const long threads =
      candrecall_env_long("CANDRECALL_THREADS", CANDRECALL_DEFAULT_THREADS);
  const long max_turns =
      candrecall_env_long("CANDRECALL_MAX_TURNS", CANDRECALL_DEFAULT_MAX_TURNS);
  const long base_seed =
      candrecall_env_long("CANDRECALL_SEED", CANDRECALL_DEFAULT_SEED);
  const char *twd_name = candrecall_env_string("CANDRECALL_TWD", "none");
  const char *twd_diag_name =
      candrecall_env_string("CANDRECALL_TWD_DIAG", NULL);
  const char *data_path = candrecall_env_string("CANDRECALL_PATH", "./data");
  const char *lexicon = candrecall_env_string("CANDRECALL_LEX", "CSW21");
  const char *use_wmp = candrecall_env_string("CANDRECALL_WMP", "true");

  char cmd[CANDRECALL_CMD_SIZE];
  snprintf(cmd, sizeof(cmd),
           "set -lex %s -wmp %s -s1 equity -s2 equity -r1 all -r2 all "
           "-numplays %ld -plies %ld -threads %ld -iter %ld -minp %ld -sr rr "
           "-threshold none -scond none -seed 7 -savesettings false -path %s "
           "-twd %s",
           lexicon, use_wmp, pool_size, plies, threads,
           pool_size * iters_per_arm, iters_per_arm, data_path, twd_name);
  Config *config = config_create_or_die(cmd);
  // The config's game exists only once a position has been loaded. Build
  // an empty board of this binary's dimension rather than using the 15x15
  // literal, so the same harness serves the super board.
  char empty_cgp[CANDRECALL_CMD_SIZE];
  int cgp_len = snprintf(empty_cgp, sizeof(empty_cgp), "cgp ");
  for (int row = 0; row < BOARD_DIM; row++) {
    cgp_len +=
        snprintf(empty_cgp + cgp_len, sizeof(empty_cgp) - (size_t)cgp_len,
                 "%d%s", BOARD_DIM, row + 1 < BOARD_DIM ? "/" : "");
  }
  snprintf(empty_cgp + cgp_len, sizeof(empty_cgp) - (size_t)cgp_len,
           " / 0/0 0");
  load_and_exec_config_or_die(config, empty_cgp);
  Game *game = config_get_game(config);
  const LetterDistribution *ld = config_get_ld(config);
  SimResults *sim_results = config_get_sim_results(config);

  // Weights scored into the diagnostic column only. Loading them here
  // rather than through the player keeps the term out of static equity, so
  // the column measures what a board heuristic would add to a pool the
  // heuristic did not itself choose.
  TWDWeights *diag_twd = NULL;
  if (twd_diag_name != NULL) {
    ErrorStack *error_stack = error_stack_create();
    diag_twd =
        twd_create(config_get_data_paths(config), twd_diag_name, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_print_and_reset(error_stack);
      log_fatal("cannot load diagnostic TWS defense weights %s", twd_diag_name);
    }
    error_stack_destroy(error_stack);
    twd_prepare_hook_flex(diag_twd, player_get_kwg(game_get_player(game, 0)),
                          ld);
  }

  FILE *out = fopen_or_die(out_path, "we");
  (void)fprintf(
      out, "pos,seed,turn,bag,play,type,tiles_played,score,leave,twd,"
           "equity,equity_rank,win_pct,win_pct_sd,sim_equity,sim_equity_sd,"
           "samples,hook_contested,hook_uncontested,own_monopoly");
  char feature_name[64];
  for (int feature_index = 0; feature_index < TWD_NUM_FEATURES;
       feature_index++) {
    twd_feature_name(feature_index, feature_name, sizeof(feature_name));
    (void)fprintf(out, ",f_%s", feature_name);
  }
  (void)fprintf(out, ",move\n");

  uint64_t rng_state = (uint64_t)base_seed * 0x9E3779B97F4A7C15ULL + 1;
  long positions_written = 0;
  StringBuilder *move_sb = string_builder_create();
  for (long pos_idx = 0; pos_idx < num_positions; pos_idx++) {
    const uint64_t game_seed_value =
        (uint64_t)base_seed * 1000003ULL + (uint64_t)pos_idx;
    game_reset(game);
    game_seed(game, game_seed_value);
    draw_starting_racks(game);
    const int turns =
        (int)(candrecall_next_random(&rng_state) % (uint64_t)max_turns);
    for (int turn_idx = 0; turn_idx < turns && !game_over(game); turn_idx++) {
      play_top_n_equity_move(game, 0);
    }
    if (game_over(game) ||
        bag_get_letters(game_get_bag(game)) < CANDRECALL_MIN_BAG) {
      continue;
    }
    load_and_exec_config_or_die(config, "gen");
    const error_code_t status =
        config_simulate_and_return_status(config, NULL, NULL, sim_results);
    if (status != ERROR_STATUS_SUCCESS) {
      log_fatal("simulation failed with status %d", status);
    }
    const int num_plays = sim_results_get_number_of_plays(sim_results);
    if (num_plays < 2) {
      continue;
    }

    const int player_index = game_get_player_on_turn_index(game);
    const Player *player = game_get_player(game, player_index);
    const KLV *klv = player_get_klv(player);
    const Board *board = game_get_board(game);
    const int bag_count = bag_get_letters(game_get_bag(game));

    // The TWS defense term, combined exactly as static eval applies it (see
    // validated_move.c for the same stack-context idiom), but with every
    // unit walked: the feature row must carry the channels the current
    // weights leave at zero, or a fit on it could never give them weight.
    TWDEvalContext twd_eval_ctx;
    twd_eval_context_disable(&twd_eval_ctx);
    const TWDWeights *twd =
        (diag_twd != NULL) ? diag_twd : player_get_twd(player);
    if (twd != NULL) {
      twd_eval_context_load_all_units(
          &twd_eval_ctx, twd,
          board_get_readonly_lanes(
              board, board_get_cross_set_index(
                         game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG),
                         player_index)),
          ld, player_get_rack(player));
    }

    // Static-equity rank of each play within the pool (1 = best).
    int *ranks = malloc_or_die(sizeof(int) * num_plays);
    for (int play_idx = 0; play_idx < num_plays; play_idx++) {
      const Equity equity = move_get_equity(simmed_play_get_move(
          sim_results_get_simmed_play(sim_results, play_idx)));
      int rank = 1;
      for (int other_idx = 0; other_idx < num_plays; other_idx++) {
        const Equity other_equity = move_get_equity(simmed_play_get_move(
            sim_results_get_simmed_play(sim_results, other_idx)));
        if (other_equity > equity ||
            (other_equity == equity && other_idx < play_idx)) {
          rank++;
        }
      }
      ranks[play_idx] = rank;
    }

    for (int play_idx = 0; play_idx < num_plays; play_idx++) {
      const SimmedPlay *simmed_play =
          sim_results_get_simmed_play(sim_results, play_idx);
      const Move *move = simmed_play_get_move(simmed_play);
      Rack leave_rack;
      rack_copy(&leave_rack, player_get_rack(player));
      const Equity leave_value =
          get_leave_value_for_move(klv, move, &leave_rack);
      const Equity twd_penalty = twd_eval_move_penalty(&twd_eval_ctx, move);
      double move_features[TWD_NUM_FEATURES];
      TWDMoveDiagnostics diagnostics;
      twd_extract_move_features(&twd_eval_ctx, move, move_features,
                                &diagnostics);
      const Stat *win_pct_stat = simmed_play_get_win_pct_stat(simmed_play);
      const Stat *equity_stat = simmed_play_get_equity_stat(simmed_play);
      string_builder_clear(move_sb);
      string_builder_add_move(move_sb, board, move, ld, false);
      (void)fprintf(
          out,
          "%ld,%llu,%d,%d,%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%d,%.6f,%.6f,"
          "%.3f,%.3f,%llu,%d,%d,%d",
          positions_written, (unsigned long long)game_seed_value, turns,
          bag_count, play_idx, (int)move_get_type(move),
          move_get_tiles_played(move), equity_to_double(move_get_score(move)),
          equity_to_double(leave_value), equity_to_double(twd_penalty),
          equity_to_double(move_get_equity(move)), ranks[play_idx],
          stat_get_mean(win_pct_stat), stat_get_stdev(win_pct_stat),
          stat_get_mean(equity_stat), stat_get_stdev(equity_stat),
          (unsigned long long)stat_get_num_samples(win_pct_stat),
          diagnostics.hook_contested, diagnostics.hook_uncontested,
          diagnostics.own_monopoly);
      for (int feature_index = 0; feature_index < TWD_NUM_FEATURES;
           feature_index++) {
        (void)fprintf(out, ",%.4f", move_features[feature_index]);
      }
      (void)fprintf(out, ",%s\n", string_builder_peek(move_sb));
    }
    free(ranks);
    positions_written++;
  }
  string_builder_destroy(move_sb);
  (void)fclose(out);
  twd_destroy(diag_twd);
  printf("candrecall: wrote %ld positions to %s\n", positions_written,
         out_path);
  config_destroy(config);
}

// Final-board dump (on-demand test "boarddump").
//
// Writes one line per completed self-play game: BOARD_DIM*BOARD_DIM
// characters, '1' where a tile sits at the end of the game and '0' where the
// square is empty, in row-major order. This is the observable the pairwise
// maximum-entropy model of Witteveen and Bauer (arXiv:2605.00813) is fit to:
// the crossword pattern alone, with the letters discarded. Fitting fields and
// interactions to MAGPIE's own self-play says whether that model's structure
// (triple-word squares carrying the largest fields, orthogonal neighbours
// attracting and diagonal ones repelling) reproduces here, and yields the
// parameters a board-shape feature would be built from.
// Configured by environment variables:
//   BOARDDUMP_OUT     output path (default boards.txt)
//   BOARDDUMP_GAMES   games to play (default 2000)
//   BOARDDUMP_SEED    base seed (default 1)
//   BOARDDUMP_PATH    -path value (default ./data)
//   BOARDDUMP_TWD     TWS defense weights for both players (default none)

enum {
  BOARDDUMP_DEFAULT_GAMES = 2000,
  BOARDDUMP_DEFAULT_SEED = 1,
  BOARDDUMP_MAX_TURNS = 60,
};

void test_board_dump(void) {
  const char *out_path = candrecall_env_string("BOARDDUMP_OUT", "boards.txt");
  const long num_games =
      candrecall_env_long("BOARDDUMP_GAMES", BOARDDUMP_DEFAULT_GAMES);
  const long base_seed =
      candrecall_env_long("BOARDDUMP_SEED", BOARDDUMP_DEFAULT_SEED);
  const char *data_path = candrecall_env_string("BOARDDUMP_PATH", "./data");
  const char *twd_name = candrecall_env_string("BOARDDUMP_TWD", "none");

  char cmd[CANDRECALL_CMD_SIZE];
  snprintf(cmd, sizeof(cmd),
           "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
           "-numplays 1 -threads 1 -savesettings false -path %s -twd %s",
           data_path, twd_name);
  Config *config = config_create_or_die(cmd);
  char empty_cgp[CANDRECALL_CMD_SIZE];
  int cgp_len = snprintf(empty_cgp, sizeof(empty_cgp), "cgp ");
  for (int row = 0; row < BOARD_DIM; row++) {
    cgp_len +=
        snprintf(empty_cgp + cgp_len, sizeof(empty_cgp) - (size_t)cgp_len,
                 "%d%s", BOARD_DIM, row + 1 < BOARD_DIM ? "/" : "");
  }
  snprintf(empty_cgp + cgp_len, sizeof(empty_cgp) - (size_t)cgp_len,
           " / 0/0 0");
  load_and_exec_config_or_die(config, empty_cgp);
  Game *game = config_get_game(config);

  FILE *out = fopen_or_die(out_path, "we");
  // The bonus square layout is constant, so record it once as a legend for
  // the fit rather than per game: 3 = triple word, 2 = double word, t =
  // triple letter, d = double letter, . = plain.
  (void)fprintf(out, "#layout ");
  const Board *board = game_get_board(game);
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      const BonusSquare bonus = board_get_bonus_square(board, row, col);
      const int word_multiplier = bonus_square_get_word_multiplier(bonus);
      const int letter_multiplier = bonus_square_get_letter_multiplier(bonus);
      char symbol = '.';
      if (word_multiplier == 3) {
        symbol = '3';
      } else if (word_multiplier == 2) {
        symbol = '2';
      } else if (letter_multiplier == 3) {
        symbol = 't';
      } else if (letter_multiplier == 2) {
        symbol = 'd';
      }
      (void)fputc(symbol, out);
    }
  }
  (void)fputc('\n', out);

  long games_written = 0;
  for (long game_idx = 0; game_idx < num_games; game_idx++) {
    game_reset(game);
    game_seed(game, (uint64_t)base_seed * 7919ULL + (uint64_t)game_idx);
    draw_starting_racks(game);
    for (int turn = 0; turn < BOARDDUMP_MAX_TURNS && !game_over(game); turn++) {
      play_top_n_equity_move(game, 0);
    }
    if (!game_over(game)) {
      continue;
    }
    for (int row = 0; row < BOARD_DIM; row++) {
      for (int col = 0; col < BOARD_DIM; col++) {
        (void)fputc(board_is_empty(board, row, col) ? '0' : '1', out);
      }
    }
    (void)fputc('\n', out);
    games_written++;
  }
  (void)fclose(out);
  printf("boarddump: wrote %ld boards to %s\n", games_written, out_path);
  config_destroy(config);
}

// Prints the lexicon-derived floater through-table (on-demand test
// "throughtable"): for a floater letter and the span a word must cover to
// reach the triple from it, the mean tile value that word lays down besides
// the floater, and how many such words exist (log-scaled).
void test_through_table(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -savesettings false -path /tmp/twdcfg:./data -twd zeros");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  const Game *game = config_get_game(config);
  const LetterDistribution *ld = config_get_ld(config);
  const TWDWeights *twd = player_get_twd(game_get_player(game, 0));
  printf("letter  score  span2  span3  span5  span7   cnt2  cnt5\n");
  for (MachineLetter ml = 1; ml <= 26; ml++) {
    printf("%-6c %5d  %5d  %5d  %5d  %5d  %5d %5d\n", (char)('A' + ml - 1),
           equity_to_int(ld_get_score(ld, ml)),
           twd_get_through_score(twd, ml, 2), twd_get_through_score(twd, ml, 3),
           twd_get_through_score(twd, ml, 5), twd_get_through_score(twd, ml, 7),
           twd_get_through_count(twd, ml, 2),
           twd_get_through_count(twd, ml, 5));
  }
  config_destroy(config);
}
