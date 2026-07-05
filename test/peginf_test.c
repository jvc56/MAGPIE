// Unit tests for peg_inference.c (pre-endgame inference).

#include "../src/def/letter_distribution_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/inference_args.h"
#include "../src/ent/inference_results.h"
#include "../src/ent/leave_rack.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/thread_control.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/peg_inference.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <assert.h>
#include <stddef.h>
#include <string.h>

// A real 4-in-bag CSW24 pre-endgame: player 0 (ACEINOP) is on turn, bag = 4.
#define PEG_INFER_4BAG_CGP                                                     \
  "cgp 3V3W6L/1BEATY1U5GI/2XU3S4FEN/3TA2H4LOY/2GEN1DUCAT1AD1/2O1I1I2WRITE1/"   \
  "2V1M1ZOAEA4/3JAGER2DRILL/2BOtONE5O1/1FERER7Q1/4S8U1/12NaM/12ATE/13ST/14H "  \
  "ACEINOP/DEIINOS 361/397 0 -lex CSW24"

static void fill_played_tiles(const Move *move, Rack *out, int ld_size) {
  rack_set_dist_size_and_reset(out, ld_size);
  const int n = move_get_tiles_length(move);
  for (int i = 0; i < n; i++) {
    MachineLetter tile = move_get_tile(move, i);
    if (tile == PLAYED_THROUGH_MARKER) {
      continue;
    }
    if (get_is_blanked(tile)) {
      tile = BLANK_MACHINE_LETTER;
    }
    rack_add_letter(out, tile);
  }
}

// peg_infer on a real tile-placement observed move must not crash and must
// leave the error stack empty. The observed move is player 0's top static play
// from the 4-in-bag position, so this drives move generation, leave
// enumeration/sampling, and at least one inner PEG solve on a packed board.
static void assert_peginf_tile_placement_no_crash(void) {
  Config *config = config_create_or_die(
      "set -lex CSW24 -s1 equity -s2 equity -r1 all -r2 all -numplays 15 "
      "-threads 4");
  load_and_exec_config_or_die(config, PEG_INFER_4BAG_CGP ";");

  ErrorStack *error_stack = error_stack_create();
  Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);

  // Observed move = player 0's top static play; its played tiles are the
  // target_played and the true leave is ACEINOP minus them.
  MoveList *move_list = move_list_create(64);
  const Move *top = get_top_equity_move(game, move_list);
  assert(move_get_type(top) == GAME_EVENT_TILE_PLACEMENT_MOVE);
  Move observed_move;
  move_copy(&observed_move, top);

  Rack target_played_tiles;
  Rack target_known_rack;
  Rack nontarget_known_rack;
  fill_played_tiles(&observed_move, &target_played_tiles, ld_size);
  rack_set_dist_size_and_reset(&target_known_rack, ld_size);
  rack_set_dist_size_and_reset(&nontarget_known_rack, ld_size);
  rack_copy(&nontarget_known_rack, player_get_rack(game_get_player(game, 1)));

  InferenceArgs base_args;
  infer_args_fill(&base_args, /*leave_list_capacity=*/5, int_to_equity(0), NULL,
                  game, /*num_threads=*/4, /*parent_worker_thread_index=*/0,
                  /*print_interval=*/0, config_get_thread_control(config),
                  /*use_game_history=*/false,
                  /*use_inference_cutoff_optimization=*/false,
                  /*target_index=*/0, /*target_score=*/int_to_equity(0),
                  /*target_num_exch=*/0, &target_played_tiles,
                  &target_known_rack, &nontarget_known_rack);

  InferenceResults *inference_results = inference_results_create(NULL);

  const PegInferenceArgs peg_args = {
      .base = &base_args,
      .observed_move = &observed_move,
      .num_candidate_plays = 5,
      .utility_w_winpct = 1.0,
      .utility_w_spread = 1.0,
      .utility_spread_scale = 100.0,
      .greedy_seed_only = true,
      .exhaustive_max_leave = 2,
      .time_budget_s = 4.0,
      .peg_utility_margin = 0.3,
  };

  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_STARTED);
  peg_infer(&peg_args, inference_results, error_stack);
  assert(error_stack_is_empty(error_stack));

  // The inference must record a non-empty distribution: the observed move is
  // the target's own top play, so at least one consistent leave is weighted.
  const LeaveRackList *lrl =
      inference_results_get_leave_rack_list(inference_results);
  assert(lrl != NULL);
  assert(leave_rack_list_get_count(lrl) > 0);

  move_list_destroy(move_list);
  inference_results_destroy(inference_results);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

void test_peginf(void) { assert_peginf_tile_placement_no_crash(); }
