#include "peg_oracle_test.h"

#include "../src/def/board_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/peg_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/validated_move.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/peg.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PEG_GREEDY_MAX_ONLY_MOVES = 16 };

void test_time_manager_match_replay(void) {
  const char *seed_text = getenv("TM_REPLAY_GAME_SEED");
  const char *start_text = getenv("TM_REPLAY_START");
  const char *moves_text = getenv("TM_REPLAY_MOVES");
  if (seed_text == NULL || start_text == NULL || moves_text == NULL) {
    log_fatal("tm replay requires TM_REPLAY_GAME_SEED, TM_REPLAY_START, and "
              "TM_REPLAY_MOVES");
    return;
  }
  char *seed_end = NULL;
  const uint64_t seed = strtoull(seed_text, &seed_end, 10);
  if (seed_end == seed_text || *seed_end != '\0') {
    log_fatal("invalid TM_REPLAY_GAME_SEED: %s", seed_text);
  }
  char *start_end = NULL;
  const long starting_player = strtol(start_text, &start_end, 10);
  if (start_end == start_text || *start_end != '\0' || starting_player < 0 ||
      starting_player > 1) {
    log_fatal("invalid TM_REPLAY_START: %s", start_text);
  }

  Config *config = config_create_or_die(
      "set -lex CSW24 -s1 equity -s2 equity -r1 all -r2 all -threads 1");
  Game *game = config_game_create(config);
  game_seed(game, seed);
  game_set_starting_player_index(game, (int)starting_player);
  draw_starting_racks(game);

  const size_t moves_len = strlen(moves_text);
  char *moves = malloc_or_die(moves_len + 1);
  memcpy(moves, moves_text, moves_len + 1);
  ErrorStack *error_stack = error_stack_create();
  char *saveptr = NULL;
  for (char *move_text = strtok_r(moves, "|", &saveptr); move_text != NULL;
       move_text = strtok_r(NULL, "|", &saveptr)) {
    ValidatedMoves *validated = validated_moves_create(
        game, game_get_player_on_turn_index(game), move_text,
        /*allow_phonies=*/false, /*allow_playthrough=*/true, error_stack);
    if (!error_stack_is_empty(error_stack) ||
        validated_moves_get_number_of_moves(validated) != 1) {
      log_fatal("tm replay failed to validate move: %s", move_text);
    }
    play_move(validated_moves_get_move(validated, 0), game, NULL);
    validated_moves_destroy(validated);
  }

  char *cgp = game_get_cgp(game, true);
  printf("TMREPLAY cgp=\"%s\"\n", cgp);
  free(cgp);
  error_stack_destroy(error_stack);
  free(moves);
  game_destroy(game);
  config_destroy(config);
}

// ---------------------------------------------------------------------------
// Oracle eval: evaluate a fixed candidate move on a 1-in-bag PEG by direct
// scenario-by-scenario endgame_solve. Bypasses the PEG search and gives
// the ground-truth win%/spread for the chosen move at the requested
// endgame depth.
// ---------------------------------------------------------------------------
void test_pass_peg_oracle_eval_move(void) {
  // Passpeg position 1 (the lone disagreement vs macondo) and C6 REEST
  // (macondo's "winner" tile play). Hardcoded; edit here to probe another move.
  const char *cgp =
      "ENTITy1YONIC2F/1A9H1AR/1P9U1TA/JELL7R1aY/1R1OVA3CON1V1/AI3GLAD2I1I1/"
      "BE5BOP1N1S1/OS4WOWING1TI/D4EH3U3N/E4XI3K1O1G/6Z3E1O1U/10DURAL/12I1F/"
      "12E1E/14D AEEMRST/AEEMRST 364/351 0";
  const char *move_str = "C6 REEST";
  const int plies = 12;
  const double per_solve_time = 30.0;

  Config *config = config_create_or_die("set -s1 score -s2 score");
  char load_cmd[10240];
  (void)snprintf(load_cmd, sizeof(load_cmd), "cgp %s -lex TWL98", cgp);
  load_and_exec_config_or_die(config, load_cmd);
  const Game *game = config_get_game(config);

  const int mover_idx = game_get_player_on_turn_index(game);
  const int opp_idx = 1 - mover_idx;
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);

  // Parse the move using validated_moves.
  ErrorStack *parse_err = error_stack_create();
  ValidatedMoves *vms = validated_moves_create(
      game, mover_idx, move_str,
      /*allow_phonies=*/false, /*allow_playthrough=*/true, parse_err);
  if (!error_stack_is_empty(parse_err)) {
    log_fatal("oracle eval: failed to parse move %s", move_str);
  }
  if (validated_moves_get_number_of_moves(vms) < 1) {
    log_fatal("oracle eval: no moves parsed from %s", move_str);
  }
  const Move *move = validated_moves_get_move(vms, 0);

  // Compute unseen pool from board (board-only).
  uint8_t unseen[MAX_ALPHABET_SIZE] = {0};
  for (int ml = 0; ml < ld_size; ml++) {
    unseen[ml] = (uint8_t)ld_get_dist(ld, ml);
  }
  const Rack *mr = player_get_rack(game_get_player(game, mover_idx));
  for (int ml = 0; ml < ld_size; ml++) {
    int n = (int)rack_get_letter(mr, (MachineLetter)ml);
    unseen[ml] -= (uint8_t)n;
  }
  const Board *board = game_get_board(game);
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      MachineLetter on_board = board_get_letter(board, row, col);
      if (on_board == ALPHABET_EMPTY_SQUARE_MARKER) {
        continue;
      }
      MachineLetter eff =
          get_is_blanked(on_board) ? BLANK_MACHINE_LETTER : on_board;
      if (unseen[eff] > 0) {
        unseen[eff]--;
      }
    }
  }
  int total_unseen = 0;
  for (int ml = 0; ml < ld_size; ml++) {
    total_unseen += unseen[ml];
  }
  if (total_unseen != RACK_SIZE + 1) {
    log_fatal("oracle eval: expected %d unseen, got %d", RACK_SIZE + 1,
              total_unseen);
  }

  // Build distinct tile list with multiplicities.
  MachineLetter tile_types[MAX_ALPHABET_SIZE] = {0};
  int tile_counts[MAX_ALPHABET_SIZE];
  int num_tile_types = 0;
  for (int ml = 0; ml < ld_size; ml++) {
    if (unseen[ml] > 0) {
      tile_types[num_tile_types] = (MachineLetter)ml;
      tile_counts[num_tile_types] = (int)unseen[ml];
      num_tile_types++;
    }
  }

  (void)fprintf(stderr,
                "[passpegoracle] move=%s plies=%d soft_time=%.1fs "
                "num_scenarios=%d\n",
                move_str, plies, per_solve_time, num_tile_types);

  // Per-scenario eval: build post-cand game, set scenario rack/bag, solve.
  EndgameCtx *ctx = NULL;
  EndgameResults *results = endgame_results_create();

  int64_t spread_sum = 0;
  int64_t wins_x2 = 0;
  int weight_sum = 0;

  for (int ti = 0; ti < num_tile_types; ti++) {
    const MachineLetter tile = tile_types[ti];
    const int tcnt = tile_counts[ti];

    Game *scenario = game_duplicate(game);
    game_set_endgame_solving_mode(scenario);
    game_set_backup_mode(scenario, BACKUP_MODE_OFF);
    play_move_without_drawing_tiles(move, scenario);
    game_set_game_end_reason(scenario, GAME_END_REASON_NONE);

    // Empty the bag (CGP load left the bag with the original 1 tile;
    // for the scenario we want the bag-tile assigned to mover instead).
    Bag *bag = game_get_bag(scenario);
    for (int ml = 0; ml < ld_size; ml++) {
      while (bag_get_letter(bag, (MachineLetter)ml) > 0) {
        (void)bag_draw_letter(bag, (MachineLetter)ml, 0);
      }
    }

    // Reset opp's rack to unseen \ {tile}.
    Rack *opp_rack = player_get_rack(game_get_player(scenario, opp_idx));
    rack_reset(opp_rack);
    for (int ml = 0; ml < ld_size; ml++) {
      int n = (int)unseen[ml] - (ml == tile ? 1 : 0);
      for (int i = 0; i < n; i++) {
        rack_add_letter(opp_rack, (MachineLetter)ml);
      }
    }

    // Mover already played the cand; rack now holds the leave. Add the
    // drawn bag tile to make the post-draw rack.
    Rack *mover_rack = player_get_rack(game_get_player(scenario, mover_idx));
    rack_add_letter(mover_rack, tile);

    int32_t mover_lead =
        equity_to_int(player_get_score(game_get_player(scenario, mover_idx))) -
        equity_to_int(player_get_score(game_get_player(scenario, opp_idx)));

    ThreadControl *tc = config_get_thread_control(config);
    EndgameArgs ea = {
        .thread_control = tc,
        .game = scenario,
        .plies = plies,
        .shared_tt = NULL,
        .initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
        .num_threads = 1,
        .use_heuristics = true,
        .num_top_moves = 1,
        .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
        .skip_word_pruning = false,
        .soft_time_limit = per_solve_time,
        .hard_time_limit = per_solve_time,
    };
    endgame_results_reset(results);
    endgame_solve_inline(&ctx, &ea, results);
    int eg_val = endgame_results_get_value(results, ENDGAME_RESULT_BEST);
    int32_t mover_total = mover_lead - eg_val;

    spread_sum += (int64_t)mover_total * tcnt;
    if (mover_total > 0) {
      wins_x2 += 2 * (int64_t)tcnt;
    } else if (mover_total == 0) {
      wins_x2 += tcnt;
    }
    weight_sum += tcnt;

    (void)fprintf(stderr,
                  "  scenario tile=%s w=%d  mover_lead=%+d  eg_val=%+d  "
                  "mover_total=%+d\n",
                  ld->ld_ml_to_hl[tile], tcnt, mover_lead, eg_val, mover_total);
    (void)fflush(stderr);

    game_destroy(scenario);
  }

  endgame_ctx_destroy(ctx);
  endgame_results_destroy(results);
  validated_moves_destroy(vms);
  error_stack_destroy(parse_err);

  double q_spread = weight_sum > 0 ? (double)spread_sum / weight_sum : 0.0;
  double q_win = weight_sum > 0 ? (double)wins_x2 / (2.0 * weight_sum) : 0.0;

  printf("\n=== Oracle eval ===\n");
  printf("CGP: %s\n", cgp);
  printf("Move: %s   plies=%d\n", move_str, plies);
  printf("Aggregated: win%%=%.4f  mean_spread=%+0.4f  weight=%d\n", q_win,
         q_spread, weight_sum);

  config_destroy(config);
}

void test_peg_greedy_candidate_dump(void) {
  const char *cgp = getenv("PEG_GREEDY_CGP");
  if (cgp == NULL) {
    log_fatal("peg greedy dump requires PEG_GREEDY_CGP");
    return;
  }
  const char *lexicon = getenv("PEG_GREEDY_LEX");
  if (lexicon == NULL) {
    lexicon = "CSW24";
  }
  const char *highlight = getenv("PEG_GREEDY_HIGHLIGHT");
  const char *threads_text = getenv("PEG_GREEDY_THREADS");
  const int num_threads = threads_text != NULL ? atoi(threads_text) : 4;

  Config *config = config_create_or_die("set -s1 equity -s2 equity");
  char load_cmd[10240];
  (void)snprintf(load_cmd, sizeof(load_cmd), "cgp %s -lex %s -wmp true", cgp,
                 lexicon);
  load_and_exec_config_or_die(config, load_cmd);
  const Game *game = config_get_game(config);
  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);

  // Stage 0 scores the entire candidate field, so a greedy-only solve reports
  // the exact ranking the halving stages cut against. That makes a play's
  // greedy win% and its rank directly comparable with the deep win% it would
  // have received had it survived the cut.
  PegArgs args;
  memset(&args, 0, sizeof(args));
  args.game = game;
  args.thread_control = config_get_thread_control(config);
  args.num_threads = num_threads > 0 ? num_threads : 1;
  args.greedy_seed_only = true;
  args.opp_model = PEG_OPP_RATIONAL;

  ErrorStack *error_stack = error_stack_create();
  // The published ranking retains only the leading candidates, so probing a
  // play the cut discarded means restricting the field to it: an only-moves
  // greedy solve reports that play's own stage-0 win% rather than its absence.
  const char *only_text = getenv("PEG_GREEDY_ONLY");
  ValidatedMoves *only_validated[PEG_GREEDY_MAX_ONLY_MOVES] = {0};
  const Move *only_moves[PEG_GREEDY_MAX_ONLY_MOVES] = {0};
  int num_only_moves = 0;
  char *only_copy = NULL;
  if (only_text != NULL) {
    only_copy = string_duplicate(only_text);
    char *only_saveptr = NULL;
    for (char *token = strtok_r(only_copy, ";", &only_saveptr); token != NULL;
         token = strtok_r(NULL, ";", &only_saveptr)) {
      if (num_only_moves >= PEG_GREEDY_MAX_ONLY_MOVES) {
        log_fatal("peg greedy dump: too many PEG_GREEDY_ONLY moves");
      }
      only_validated[num_only_moves] = validated_moves_create(
          game, game_get_player_on_turn_index(game), token,
          /*allow_phonies=*/false, /*allow_playthrough=*/true, error_stack);
      if (!error_stack_is_empty(error_stack) ||
          validated_moves_get_number_of_moves(only_validated[num_only_moves]) !=
              1) {
        error_stack_print_and_reset(error_stack);
        log_fatal("peg greedy dump: could not parse move '%s'", token);
      }
      only_moves[num_only_moves] =
          validated_moves_get_move(only_validated[num_only_moves], 0);
      num_only_moves++;
    }
    args.only_moves = only_moves;
    args.n_only_moves = num_only_moves;
  }
  PegResult result;
  memset(&result, 0, sizeof(result));
  thread_control_set_status(args.thread_control, THREAD_CONTROL_STATUS_STARTED);
  peg_solve(&args, &result, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("peg greedy dump: solve failed");
  }

  printf("PEG_GREEDY_DUMP candidates=%d\n", result.n_top_cands);
  StringBuilder *move_sb = string_builder_create();
  for (int cand_idx = 0; cand_idx < result.n_top_cands; cand_idx++) {
    const PegRankedCand *cand = &result.top_cands[cand_idx];
    string_builder_clear(move_sb);
    string_builder_add_move(move_sb, board, &cand->move, ld, false);
    const char *move_text = string_builder_peek(move_sb);
    bool marked = false;
    if (highlight != NULL) {
      char *targets = string_duplicate(highlight);
      char *saveptr = NULL;
      for (const char *token = strtok_r(targets, ",", &saveptr);
           token != NULL && !marked; token = strtok_r(NULL, ",", &saveptr)) {
        marked = strstr(move_text, token) != NULL;
      }
      free(targets);
    }
    printf("PEG_GREEDY_CAND rank=%d move=%s win=%.4f spread=%+.3f "
           "scenarios=%d marked=%d\n",
           cand_idx + 1, move_text, cand->win_pct, cand->mean_spread,
           cand->n_scenarios, marked ? 1 : 0);
  }
  string_builder_destroy(move_sb);
  for (int move_idx = 0; move_idx < num_only_moves; move_idx++) {
    validated_moves_destroy(only_validated[move_idx]);
  }
  free(only_copy);
  error_stack_destroy(error_stack);
  peg_result_destroy(&result);
  config_destroy(config);
}
