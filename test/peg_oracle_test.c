#include "peg_oracle_test.h"

#include "../src/def/board_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/rack_defs.h"
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
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/gameplay.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <stdint.h>
#include <stdio.h>

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
