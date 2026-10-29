#include "simmer.h"

#include "../def/equity_defs.h"
#include "../def/game_defs.h"
#include "../def/game_history_defs.h"
#include "../def/inference_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../def/sim_defs.h"
#include "../def/thread_control_defs.h"
#include "../ent/bag.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/inference_results.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/sim_args.h"
#include "../ent/sim_results.h"
#include "../ent/stats.h"
#include "../ent/thread_control.h"
#include "../str/rack_string.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "bai.h"
#include "inference.h"
#include "move_gen.h"
#include "rack_draw.h"
#include "random_variable.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

enum {
  // The candidates beyond the num_plays best plays that
  // SimArgs.tempo_candidates adds: a pass, the best exchange, the exchange
  // that keeps only the low tiles, and the best one- and two-tile plays.
  SIM_NUM_TEMPO_CANDIDATES = 5,
  // The exchange that keeps only the low tiles throws back every tile that
  // scores more than this, keeping the one-point tiles and blanks.
  SIM_TEMPO_KEPT_TILE_MAX_SCORE = 1,
  // The starting capacity of the list that holds every legal play for
  // SimArgs.tempo_candidates. It doubles whenever a position has more.
  SIM_TEMPO_ALL_PLAYS_INITIAL_CAPACITY = 1024,
};

enum {
  // The fewest cycles a bag-cycled sim gives each play (see
  // sim_args_set_bag_cycle_bai_options).
  SIM_BAG_CYCLE_MIN_CYCLES = 2,
};

// The sim context allows zero-alloc autoplay sims by maintaining the
// dynamically allocated sim structs in between sim calls. The following
// options are allowed to change in between sim calls:
// - move list
// - player on turn
// - spread
// - known opp rack
// - use inference
// - num plies
struct SimCtx {
  RandomVariables *rvs;
  InferenceCtx *inference_ctx;
  // Every legal play of the last position, for SimArgs.tempo_candidates.
  // NULL until a simulation asks for them.
  MoveList *all_plays;
};

void sim_ctx_destroy(SimCtx *sim_ctx) {
  if (!sim_ctx) {
    return;
  }
  rvs_destroy(sim_ctx->rvs);
  inference_ctx_destroy(sim_ctx->inference_ctx);
  move_list_destroy(sim_ctx->all_plays);
  free(sim_ctx);
}

static SimCtx *sim_ctx_get_or_create(SimCtx **sim_ctx) {
  if (*sim_ctx == NULL) {
    *sim_ctx = malloc_or_die(sizeof(SimCtx));
    (*sim_ctx)->rvs = NULL;
    (*sim_ctx)->inference_ctx = NULL;
    (*sim_ctx)->all_plays = NULL;
  }
  return *sim_ctx;
}

// A bag-cycled sample is a whole cycle of rollouts, but the sample limit
// (-iterations) and minimum (-minplayiterations) count rollouts. Converts them
// to this position's cycles, rounding up, and gives every play at least
// SIM_BAG_CYCLE_MIN_CYCLES, so that BAI never estimates a play's variance from
// one sample. Also tells BAI how many rollouts a cycle averages, so that its
// rounds hold about as many rollouts as a plain sim's.
static void sim_args_set_bag_cycle_bai_options(SimArgs *sim_args) {
  const uint64_t rotations =
      (uint64_t)rv_sim_bag_cycle_rotations(sim_args->game);
  const uint64_t limit = sim_args->bai_options.sample_limit;
  const uint64_t minimum = sim_args->bai_options.sample_minimum;
  uint64_t minimum_cycles =
      minimum / rotations + (minimum % rotations != 0 ? 1 : 0);
  if (minimum_cycles < SIM_BAG_CYCLE_MIN_CYCLES) {
    minimum_cycles = SIM_BAG_CYCLE_MIN_CYCLES;
  }
  sim_args->bai_options.sample_limit =
      limit / rotations + (limit % rotations != 0 ? 1 : 0);
  sim_args->bai_options.sample_minimum = minimum_cycles;
  sim_args->bai_options.draws_per_sample = rotations;
}

void simulate(SimArgs *sim_args, SimCtx **sim_ctx, SimResults *sim_results,
              ErrorStack *error_stack) {
  if (!sim_args->move_list || move_list_get_count(sim_args->move_list) == 0) {
    error_stack_push(error_stack, ERROR_STATUS_SIM_NO_MOVES,
                     string_duplicate("cannot simulate without moves, use the "
                                      "'generate' command to generate moves"));
    return;
  }
  if (game_get_game_end_reason(sim_args->game) != GAME_END_REASON_NONE) {
    error_stack_push(
        error_stack, ERROR_STATUS_SIM_GAME_OVER,
        string_duplicate("cannot simulate when the game is already over"));
    return;
  }

  const Rack *known_opp_rack = sim_args->known_opp_rack;
  if (known_opp_rack && !rack_is_empty(known_opp_rack)) {
    const int player_off_turn_index =
        1 - game_get_player_on_turn_index(sim_args->game);
    if (!rack_is_drawable(sim_args->game, player_off_turn_index,
                          known_opp_rack)) {
      const LetterDistribution *ld = game_get_ld(sim_args->game);
      StringBuilder *sb = string_builder_create();
      string_builder_add_string(sb, "opponent rack '");
      string_builder_add_rack(sb, known_opp_rack, ld, false);
      string_builder_add_string(sb, "' is not available in the bag");
      char *err_msg = string_builder_dump_and_destroy(sb, NULL);
      error_stack_push(error_stack, ERROR_STATUS_SIM_OPP_RACK_NOT_IN_BAG,
                       err_msg);
      return;
    }
  }

  sim_ctx_get_or_create(sim_ctx);

  uint64_t num_infer_leaves = 0;
  if (sim_args->use_inference) {
    infer(&sim_args->inference_args, &((*sim_ctx)->inference_ctx),
          sim_args->inference_results, error_stack);
    if (!error_stack_is_empty(error_stack) ||
        thread_control_get_status(sim_args->thread_control) !=
            THREAD_CONTROL_STATUS_STARTED) {
      return;
    }
    num_infer_leaves =
        stat_get_num_unique_samples(inference_results_get_equity_values(
            sim_args->inference_results, INFERENCE_TYPE_LEAVE));
  }

  // If the bag is empty, set sample_limit to the number of moves and
  // sample_minimum to 1 for endgame simulations. These are set after inference,
  // which can return early, so that the reset below always undoes them.
  const uint64_t original_sample_limit = sim_args->bai_options.sample_limit;
  const uint64_t original_sample_minimum = sim_args->bai_options.sample_minimum;
  const uint64_t original_draws_per_sample =
      sim_args->bai_options.draws_per_sample;
  const int original_num_plies = sim_args->num_plies;
  if (bag_is_empty(game_get_bag(sim_args->game))) {
    sim_args->bai_options.sample_limit =
        move_list_get_count(sim_args->move_list);
    sim_args->bai_options.sample_minimum = 1;
    sim_args->num_plies = MAX_PLIES;
  } else if (sim_args->bag_cycle) {
    sim_args_set_bag_cycle_bai_options(sim_args);
  }

  RandomVariablesArgs rv_sim_args = {
      .type = RANDOM_VARIABLES_SIMMED_PLAYS,
      .sim_args = sim_args,
      .sim_results = sim_results,
  };

  if ((*sim_ctx)->rvs) {
    rvs_reset((*sim_ctx)->rvs, &rv_sim_args);
  } else {
    (*sim_ctx)->rvs = rvs_create(&rv_sim_args);
  }

  // On resume the existing results already carry the rack / known
  // opponent rack they were built with — don't stomp them with the
  // (possibly reconstructed) move list's view.
  if (!sim_args->resume_results) {
    sim_results_set_rack(sim_results, move_list_get_rack(sim_args->move_list));
    sim_results_set_known_opp_rack(sim_results, sim_args->known_opp_rack);
    sim_results_set_num_infer_leaves(sim_results, num_infer_leaves);
  }
  sim_results_set_cutoff(sim_results, sim_args->bai_options.cutoff);
  sim_results_set_utility_w_spread(sim_results, sim_args->utility_w_spread);

  bai(&sim_args->bai_options, (*sim_ctx)->rvs, sim_args->thread_control, NULL,
      sim_results_get_bai_result(sim_results));

  // Reset the sim args to their original values in case they were modified for
  // endgame or bag-cycled sims
  sim_args->bai_options.sample_limit = original_sample_limit;
  sim_args->bai_options.sample_minimum = original_sample_minimum;
  sim_args->bai_options.draws_per_sample = original_draws_per_sample;
  sim_args->num_plies = original_num_plies;
}

void simulate_without_ctx(SimArgs *sim_args, SimResults *sim_results,
                          ErrorStack *error_stack) {
  SimCtx *ctx = NULL;
  simulate(sim_args, &ctx, sim_results, error_stack);
  sim_ctx_destroy(ctx);
}

// Generates every legal play into all_plays, best first. A list that comes
// back full may have dropped plays, so it grows and generates again.
static void generate_all_plays_sorted(const MoveGenArgs *gen_args,
                                      MoveList *all_plays) {
  MoveGenArgs all_args = *gen_args;
  all_args.move_list = all_plays;
  all_args.move_record_type = MOVE_RECORD_ALL;
  generate_moves(&all_args);
  while (move_list_get_count(all_plays) ==
         move_list_get_capacity(all_plays)) {
    move_list_resize(all_plays, 2 * move_list_get_capacity(all_plays));
    generate_moves(&all_args);
  }
  move_list_sort_moves(all_plays);
}

// Whether the exchange throws back exactly the tiles in high_tiles.
static bool exchange_throws_exactly(const Move *exchange,
                                    const Rack *high_tiles) {
  const int tiles_thrown = move_get_tiles_played(exchange);
  if (tiles_thrown != rack_get_total_letters(high_tiles)) {
    return false;
  }
  Rack thrown;
  rack_set_dist_size_and_reset(&thrown, rack_get_dist_size(high_tiles));
  for (int tile_idx = 0; tile_idx < tiles_thrown; tile_idx++) {
    rack_add_letter(&thrown, move_get_tile(exchange, tile_idx));
  }
  return racks_are_equal(&thrown, high_tiles);
}

// Fills gen_args->move_list with the num_plays best plays and, where they are
// not among them, the tempo candidates (see SimArgs.tempo_candidates).
static void generate_candidates_with_tempo(const MoveGenArgs *gen_args,
                                           const int num_plays,
                                           SimCtx *sim_ctx) {
  if (sim_ctx->all_plays == NULL) {
    sim_ctx->all_plays =
        move_list_create(SIM_TEMPO_ALL_PLAYS_INITIAL_CAPACITY);
  }
  MoveList *all_plays = sim_ctx->all_plays;
  generate_all_plays_sorted(gen_args, all_plays);

  const Game *game = gen_args->game;
  const LetterDistribution *ld = game_get_ld(game);
  const Rack *rack =
      player_get_rack(game_get_player(game, game_get_player_on_turn_index(game)));
  Rack high_tiles;
  rack_set_dist_size_and_reset(&high_tiles, ld_get_size(ld));
  for (int ml = 0; ml < ld_get_size(ld); ml++) {
    if (ld_get_score(ld, (MachineLetter)ml) >
        int_to_equity(SIM_TEMPO_KEPT_TILE_MAX_SCORE)) {
      rack_add_letters(&high_tiles, (MachineLetter)ml,
                       rack_get_letter(rack, (MachineLetter)ml));
    }
  }

  MoveList *candidates = gen_args->move_list;
  if (move_list_get_capacity(candidates) <
      num_plays + SIM_NUM_TEMPO_CANDIDATES) {
    move_list_resize(candidates, num_plays + SIM_NUM_TEMPO_CANDIDATES);
  }
  move_list_reset(candidates);
  bool have_pass = false;
  bool have_exchange = false;
  bool have_low_keeper = false;
  bool have_one_tile = false;
  bool have_two_tile = false;
  const int number_of_plays = move_list_get_count(all_plays);
  for (int move_idx = 0; move_idx < number_of_plays; move_idx++) {
    const Move *move = move_list_get_move(all_plays, move_idx);
    const game_event_t move_type = move_get_type(move);
    const bool is_placement = move_type == GAME_EVENT_TILE_PLACEMENT_MOVE;
    const bool is_pass = move_type == GAME_EVENT_PASS;
    const bool is_exchange = move_type == GAME_EVENT_EXCHANGE;
    const bool is_low_keeper =
        is_exchange && exchange_throws_exactly(move, &high_tiles);
    const bool is_one_tile = is_placement && move_get_tiles_played(move) == 1;
    const bool is_two_tile = is_placement && move_get_tiles_played(move) == 2;
    if (move_idx < num_plays || (is_pass && !have_pass) ||
        (is_exchange && !have_exchange) ||
        (is_low_keeper && !have_low_keeper) ||
        (is_one_tile && !have_one_tile) || (is_two_tile && !have_two_tile)) {
      move_list_add_move(candidates, move);
      have_pass = have_pass || is_pass;
      have_exchange = have_exchange || is_exchange;
      have_low_keeper = have_low_keeper || is_low_keeper;
      have_one_tile = have_one_tile || is_one_tile;
      have_two_tile = have_two_tile || is_two_tile;
    }
  }
}

// The move_list and sim_args->move_list are expected to be the same pointer,
// but they are passed in separately because this function needs to generate
// moves on a nonconst MoveList pointer but the SimArgs MoveList pointer is
// const.
//
// The list's capacity is the number of best plays. With
// sim_args->tempo_candidates it grows to hold the tempo candidates too, and
// keeps that capacity.
//
// Sets *simulated to whether a simulation ran. A position with one legal play
// (a forced pass) is not simulated, and sim_results then still holds whatever
// the last simulation left in it, which describes another position.
const Move *get_top_simming_move(Game *game, MoveList *move_list,
                                 SimArgs *sim_args, SimCtx **sim_ctx,
                                 SimResults *sim_results, bool *simulated,
                                 ErrorStack *error_stack) {
  *simulated = false;
  // The candidates are the player's best plays by its own sort type, as a
  // static player's move is: a player sorted by score simulates the top plays
  // by score. Equity unless a player asks otherwise.
  const MoveGenArgs gen_args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = player_get_move_sort_type(
          game_get_player(game, game_get_player_on_turn_index(game))),
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
  };
  if (sim_args->tempo_candidates) {
    generate_candidates_with_tempo(&gen_args, sim_args->num_plays,
                                   sim_ctx_get_or_create(sim_ctx));
  } else {
    generate_moves(&gen_args);
  }

  if (move_list_get_count(move_list) == 1) {
    return move_list_get_move(move_list, 0);
  }

  simulate(sim_args, sim_ctx, sim_results, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return NULL;
  }
  // A stop request cuts the simulation short, and one that lands during the
  // inference that precedes it returns before the simulation starts: the
  // results are then still the last simulation's, of another position, and
  // their best play may not even be on this rack. Playing it corrupted the
  // game (rack counts below zero); the caller is abandoning the run anyway,
  // so the top static play stands in, as for a turn with nothing to simulate.
  if (thread_control_get_status(sim_args->thread_control) ==
      THREAD_CONTROL_STATUS_USER_INTERRUPT) {
    move_list_sort_moves(move_list);
    return move_list_get_move(move_list, 0);
  }
  *simulated = true;

  return sim_results_get_best_move(sim_results);
}
