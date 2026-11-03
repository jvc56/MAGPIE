#include "inference.h"

#include "../compat/cpthread.h"
#include "../compat/cpthread_defs.h"
#include "../def/board_defs.h"
#include "../def/equity_defs.h"
#include "../def/game_defs.h"
#include "../def/game_history_defs.h"
#include "../def/inference_defs.h"
#include "../def/klv_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/players_data_defs.h"
#include "../def/rack_defs.h"
#include "../def/thread_control_defs.h"
#include "../ent/alias_method.h"
#include "../ent/bag.h"
#include "../ent/board.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../ent/inference_args.h"
#include "../ent/inference_results.h"
#include "../ent/klv.h"
#include "../ent/kwg.h"
#include "../ent/leave_rack.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/static_eval.h"
#include "../ent/stats.h"
#include "../ent/thread_control.h"
#include "../ent/validated_move.h"
#include "../str/inference_string.h"
#include "../str/rack_string.h"
#include "../util/io_util.h"
#include "../util/math_util.h"
#include "../util/string_util.h"
#include "census.h"
#include "census_play_scores.h"
#include "gameplay.h"
#include "move_gen.h"
#include "move_gen_base.h"
#include "rack_draw.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct Inference {
  // KLV used to evaluate leaves to determine
  // which moves are top equity. This should be
  // the KLV of the target.
  const KLV *klv;
  ThreadControl *thread_control;
  // The move's budget, from the thread that started the inference: the
  // worker charges it and stops when it is spent (gen_set_movegen_budget).
  MovegenBudget *movegen_budget;

  // The following fields are owned by this struct.

  int ld_size;
  int leave_list_capacity;
  // Target player index in the game
  int target_index;
  // Target player score
  Equity target_score;
  // Opening placement adjustment for the target move, or 0 when not applicable
  Equity target_placement_adjustment;
  // Number of tiles played by the target
  int target_tiles_played;
  // Number of tiles exchanged by the target
  int target_number_of_tiles_exchanged;
  // Maximum equity loss the target can
  // lose while still being considered
  // the top move.
  Equity equity_margin;
  bool use_infer_cutoff_optimization;
  // Every worker walks every possible leave in the same order;
  // current_rack_index is the position of the walk. A worker evaluates only
  // the leaves at the claim_size positions from claimed_rack_index, which it
  // has claimed from shared_next_rack_index, which hands each position to
  // exactly one worker. A claim of more than one position saves the workers'
  // contention for the counter when each leave is quick to evaluate.
  uint64_t current_rack_index;
  uint64_t claimed_rack_index;
  uint64_t claim_size;
  int num_threads;
  int print_interval;
  _Atomic uint64_t *shared_next_rack_index;
  cpthread_t cpthread_id;
  // Rack containing just the unknown leave, which is
  // the tiles on the target's rack unseen to
  // the observer making the inference.
  Rack *current_target_leave;
  // Rack containing just the exchange, which is
  // the tiles the target put back into the bag which
  // are unseen to the observer making the inference.
  Rack *current_target_exchanged;
  // For an exchange of N tiles: the tiles kept by the best exchange of N
  // tiles from the rack being evaluated (see get_best_exchange_equity).
  Rack *best_exchange_leave;
  // Rack containing the leave and the other tiles
  // which the observer may know about (for example, due
  // to a lost challenge, coffee housing, or accidental flash).
  Rack *current_target_rack;
  // The bag represented by a rack for convenience
  Rack *bag_as_rack;
  // MoveList used by the inference to generate moves
  MoveList *move_list;
  // Game used by the inference to generate moves
  Game *game;
  InferenceResults *results;
  // The board's census play scores (see census_play_scores_build), owned by
  // the context and only read by the workers, or NULL when each rack's best
  // move comes from a move generation.
  const CensusLattice *census_lattice;
  const Equity *census_play_scores;
  // The KLV's leave values by lattice rank (census_leaves_from_klv), owned by
  // the context, when census_play_scores is set.
  const Equity *census_leave_values;
} Inference;

struct InferenceCtx {
  Game *game;
  int num_workers;
  Inference **worker_inferences;
  _Atomic uint64_t shared_next_rack_index;
  Stat **leave_stats;
  Stat **exchanged_stats;
  Stat **rack_stats;
  // The census play scores' lattice and buffers, kept between inferences so
  // that they are allocated once; the workers point at them while an
  // inference uses them.
  CensusLattice *census_lattice;
  // The play scores, which every worker's thread builds into.
  Equity *census_play_scores;
  // The leave values by lattice rank of census_leave_klv, as it was when they
  // were read, which the next inference of that KLV reuses.
  Equity *census_leave_values;
  const KLV *census_leave_klv;
  uint64_t census_leave_klv_fingerprint;
  uint64_t census_leave_klv_mutation_counter;
};

void inference_destroy(Inference *inference) {
  if (!inference) {
    return;
  }
  rack_destroy(inference->current_target_leave);
  rack_destroy(inference->current_target_exchanged);
  rack_destroy(inference->best_exchange_leave);
  rack_destroy(inference->bag_as_rack);
  move_list_destroy(inference->move_list);
  inference_results_destroy(inference->results);
  game_destroy(inference->game);
  free(inference);
}

uint64_t get_number_of_draws_for_rack(const Rack *bag_as_rack,
                                      const Rack *rack) {
  uint64_t number_of_ways = 1;
  for (int i = 0; i < rack_get_dist_size(rack); i++) {
    if (rack_get_letter(rack, i) > 0) {
      number_of_ways *=
          choose(rack_get_letter(bag_as_rack, i) + rack_get_letter(rack, i),
                 rack_get_letter(rack, i));
    }
  }
  return number_of_ways;
}

void increment_subtotals_for_results(const Rack *rack,
                                     InferenceResults *results,
                                     inference_stat_t inference_stat_type,
                                     uint64_t number_of_draws_for_leave) {
  for (int i = 0; i < rack_get_dist_size(rack); i++) {
    if (rack_get_letter(rack, i) > 0) {
      inference_results_add_to_letter_subtotal(
          results, inference_stat_type, i, rack_get_letter(rack, i),
          INFERENCE_SUBTOTAL_DRAW, number_of_draws_for_leave);
      inference_results_add_to_letter_subtotal(results, inference_stat_type, i,
                                               rack_get_letter(rack, i),
                                               INFERENCE_SUBTOTAL_LEAVE, 1);
    }
  }
}

void record_valid_leave(const Rack *rack, InferenceResults *results,
                        inference_stat_t inference_stat_type,
                        double current_leave_value,
                        uint64_t number_of_draws_for_leave) {
  stat_push(inference_results_get_equity_values(results, inference_stat_type),
            current_leave_value, number_of_draws_for_leave);
  increment_subtotals_for_results(rack, results, inference_stat_type,
                                  number_of_draws_for_leave);
}

// Finds the best leave of exactly keep_size tiles drawn from the target's
// full rack (current_target_leave): the highest KLV value, the first found in
// this enumeration order on ties. Enumerates each multiset of tiles once.
// node_index and word_index locate the leave so far in the KLV, which each
// letter added walks one step further, as generate_exchange_moves does.
static void find_best_exchange_leave(const Inference *inference, Rack *leave,
                                     int start_ml, int remaining,
                                     uint32_t node_index, uint32_t word_index,
                                     Equity *best_value) {
  if (remaining == 0) {
    Equity value = 0;
    if (word_index != KLV_UNFOUND_INDEX) {
      value = klv_get_indexed_leave_value(inference->klv, word_index - 1);
    }
    if (value > *best_value) {
      *best_value = value;
      rack_copy(inference->best_exchange_leave, leave);
    }
    return;
  }
  for (int ml = start_ml; ml < inference->ld_size; ml++) {
    if (rack_get_letter(leave, ml) <
        rack_get_letter(inference->current_target_leave, ml)) {
      uint32_t sibling_word_index;
      const uint32_t sibling_node_index =
          increment_node_to_ml(inference->klv, node_index, word_index,
                               &sibling_word_index, (MachineLetter)ml);
      uint32_t child_word_index;
      const uint32_t child_node_index =
          follow_arc(inference->klv, sibling_node_index, sibling_word_index,
                     &child_word_index);
      rack_add_letter(leave, ml);
      find_best_exchange_leave(inference, leave, ml, remaining - 1,
                               child_node_index, child_word_index, best_value);
      rack_take_letter(leave, ml);
    }
  }
}

// Writes the distinct letters of a rack of at most RACK_SIZE tiles, in
// increasing order, and their counts, and returns how many there are. Every
// letter is written and only those on the rack are kept, without a branch on
// each, so letters and counts hold RACK_SIZE + 1.
static int census_distinct_letters(const Rack *rack, int ld_size,
                                   MachineLetter *letters, uint8_t *counts) {
  int number_of_letters = 0;
  for (int ml = 0; ml < ld_size; ml++) {
    const int count = rack_get_letter(rack, ml);
    letters[number_of_letters] = (MachineLetter)ml;
    counts[number_of_letters] = (uint8_t)count;
    number_of_letters += count > 0;
  }
  return number_of_letters;
}

// klv_get_leave_value of the rack, from the census leave values when the
// inference has them.
static Equity inference_get_leave_value(const Inference *inference,
                                        const Rack *rack) {
  if (inference->census_leave_values == NULL) {
    return klv_get_leave_value(inference->klv, rack);
  }
  const CensusLattice *lattice = inference->census_lattice;
  int remaining = rack_get_total_letters(rack);
  uint32_t terms = 0;
  for (int ml = 0; ml < inference->ld_size; ml++) {
    const int count = rack_get_letter(rack, ml);
    terms +=
        census_lattice_rank_term(lattice, (MachineLetter)ml, remaining, count);
    remaining -= count;
  }
  return inference
      ->census_leave_values[census_lattice_get_size_start(
                                lattice, rack_get_total_letters(rack)) +
                            terms];
}

// find_best_exchange_leave from the census leave values, in the same order:
// letter letter_idx of the rack's distinct letters keeps from as many copies
// as it can down to none, the letters before it chosen in chosen, and
// remaining tiles still to keep. terms sums the rank terms of the letters
// chosen, which the rank of a leave of keep_size tiles takes in increasing
// order of its letters.
static void census_find_best_exchange_leave(
    const Inference *inference, const MachineLetter *letters,
    const uint8_t *counts, int number_of_letters, int keep_size, int letter_idx,
    int remaining, uint32_t terms, uint8_t *chosen, uint8_t *best_chosen,
    Equity *best_value) {
  const CensusLattice *lattice = inference->census_lattice;
  if (remaining == 0) {
    const Equity value =
        inference->census_leave_values[census_lattice_get_size_start(
                                           lattice, keep_size) +
                                       terms];
    if (value > *best_value) {
      *best_value = value;
      for (int chosen_idx = 0; chosen_idx < number_of_letters; chosen_idx++) {
        best_chosen[chosen_idx] =
            chosen_idx < letter_idx ? chosen[chosen_idx] : 0;
      }
    }
    return;
  }
  if (letter_idx == number_of_letters) {
    return;
  }
  const int most =
      counts[letter_idx] < remaining ? counts[letter_idx] : remaining;
  for (int kept_count = most; kept_count >= 0; kept_count--) {
    chosen[letter_idx] = (uint8_t)kept_count;
    census_find_best_exchange_leave(
        inference, letters, counts, number_of_letters, keep_size,
        letter_idx + 1, remaining - kept_count,
        terms + census_lattice_rank_term(lattice, letters[letter_idx],
                                         remaining, kept_count),
        chosen, best_chosen, best_value);
  }
}

// The equity of the target's exchange of target_number_of_tiles_exchanged
// tiles that keeps best_exchange_leave, worth leave_value, computed the way
// move generation computes it.
static Equity get_exchange_equity(const Inference *inference,
                                  Equity leave_value) {
  const Game *game = inference->game;
  return static_eval_get_nonopening_move_equity(
      game_get_ld(game), inference->best_exchange_leave,
      player_get_rack(game_get_player(game, 1 - inference->target_index)),
      inference->target_number_of_tiles_exchanged,
      bag_get_letters(game_get_bag(game)), 0, leave_value);
}

// Sets best_exchange_leave to the tiles the target keeps in its best exchange
// of target_number_of_tiles_exchanged tiles and returns that exchange's
// equity.
static Equity get_best_exchange_equity(const Inference *inference) {
  const int keep_size = RACK_SIZE - inference->target_number_of_tiles_exchanged;
  Rack leave;
  rack_set_dist_size_and_reset(&leave, inference->ld_size);
  Equity best_value = EQUITY_MIN_VALUE;
  if (inference->census_leave_values != NULL) {
    MachineLetter letters[RACK_SIZE + 1];
    uint8_t counts[RACK_SIZE + 1];
    const int number_of_letters = census_distinct_letters(
        inference->current_target_leave, inference->ld_size, letters, counts);
    uint8_t chosen[RACK_SIZE];
    uint8_t best_chosen[RACK_SIZE] = {0};
    census_find_best_exchange_leave(inference, letters, counts,
                                    number_of_letters, keep_size, 0, keep_size,
                                    0, chosen, best_chosen, &best_value);
    rack_reset(inference->best_exchange_leave);
    for (int letter_idx = 0; letter_idx < number_of_letters; letter_idx++) {
      rack_add_letters(inference->best_exchange_leave, letters[letter_idx],
                       best_chosen[letter_idx]);
    }
  } else {
    find_best_exchange_leave(
        inference, &leave, 0, keep_size,
        kwg_get_dawg_root_node_index(klv_get_kwg(inference->klv)), 0,
        &best_value);
  }
  return get_exchange_equity(inference, best_value);
}

// The equity of the best move of the target's full rack, as
// get_top_equity_move_for_inferences finds it, from the census play scores
// instead of a move generation. Once it finds a move above the cutoff, it
// returns that move's equity instead.
//
// It values every split of the rack into the tiles a move uses and the tiles
// it keeps the way generate_moves values the move it stands for: a play of
// the used tiles, at the best score the census found for them, and an
// exchange of them when exchanges are legal. The splits are counted like an
// odometer over the copies of each distinct letter kept, the lowest letter
// changing fastest. A multiset's rank adds a term for each letter, set by the
// tiles at that letter or after it, so each split only recomputes the sums
// of the letters at or below the one that changed.
static Equity get_top_equity_from_census(const Inference *inference,
                                         Equity cutoff) {
  const Game *game = inference->game;
  const LetterDistribution *ld = game_get_ld(game);
  const CensusLattice *lattice = inference->census_lattice;
  MachineLetter letters[RACK_SIZE + 1];
  uint8_t counts[RACK_SIZE + 1];
  const int number_of_letters = census_distinct_letters(
      inference->current_target_rack, inference->ld_size, letters, counts);
  const Rack *opp_rack =
      player_get_rack(game_get_player(game, 1 - inference->target_index));
  const int number_of_tiles_in_bag = bag_get_letters(game_get_bag(game));
  // The same test as gen_look_up_leaves_and_record_exchanges.
  const bool add_exchange =
      number_of_tiles_in_bag + rack_get_total_letters(opp_rack) >=
      RACK_SIZE * 2;
  // The tiles kept, which only the endgame adjustment reads.
  Rack kept;
  rack_set_dist_size_and_reset(&kept, inference->ld_size);

  // For each letter, the copies kept, and over it and the letters after it,
  // the tiles kept and used and the sums of their rank terms.
  uint8_t kept_counts[RACK_SIZE + 1] = {0};
  int tiles_kept[RACK_SIZE + 1];
  int tiles_played[RACK_SIZE + 1];
  uint32_t kept_terms[RACK_SIZE + 1];
  uint32_t played_terms[RACK_SIZE + 1];
  tiles_kept[number_of_letters] = 0;
  tiles_played[number_of_letters] = 0;
  kept_terms[number_of_letters] = 0;
  played_terms[number_of_letters] = 0;

  Equity best_equity = EQUITY_PASS_VALUE;
  int changed_idx = number_of_letters - 1;
  while (true) {
    // The sums carried in locals, so that each letter does not wait on the
    // store of the one after it.
    int kept_sum = tiles_kept[changed_idx + 1];
    int played_sum = tiles_played[changed_idx + 1];
    uint32_t kept_terms_sum = kept_terms[changed_idx + 1];
    uint32_t played_terms_sum = played_terms[changed_idx + 1];
    for (int letter_idx = changed_idx; letter_idx >= 0; letter_idx--) {
      const int kept_count = kept_counts[letter_idx];
      const int played_count = counts[letter_idx] - kept_count;
      kept_sum += kept_count;
      played_sum += played_count;
      kept_terms_sum += census_lattice_rank_term(lattice, letters[letter_idx],
                                                 kept_sum, kept_count);
      played_terms_sum += census_lattice_rank_term(lattice, letters[letter_idx],
                                                   played_sum, played_count);
      tiles_kept[letter_idx] = kept_sum;
      tiles_played[letter_idx] = played_sum;
      kept_terms[letter_idx] = kept_terms_sum;
      played_terms[letter_idx] = played_terms_sum;
    }
    // Keeping every tile is a pass, valued below every move.
    if (tiles_played[0] > 0) {
      if (number_of_tiles_in_bag == 0) {
        rack_reset(&kept);
        for (int letter_idx = 0; letter_idx < number_of_letters; letter_idx++) {
          rack_add_letters(&kept, letters[letter_idx], kept_counts[letter_idx]);
        }
      }
      const Equity leave_value =
          inference->census_leave_values[census_lattice_get_size_start(
                                             lattice, tiles_kept[0]) +
                                         kept_terms[0]];
      const Equity play_score =
          inference->census_play_scores[census_lattice_get_size_start(
                                            lattice, tiles_played[0]) +
                                        played_terms[0]];
      // Whether the tiles make a play is as good as random, so the play is
      // valued either way and dropped by a select, not a branch.
      const Equity play_equity = static_eval_get_nonopening_move_equity(
          ld, &kept, opp_rack, tiles_played[0], number_of_tiles_in_bag,
          play_score, leave_value);
      best_equity = play_score != CENSUS_UNPLAYABLE && play_equity > best_equity
                        ? play_equity
                        : best_equity;
      if (add_exchange) {
        const Equity exchange_equity = static_eval_get_nonopening_move_equity(
            ld, &kept, opp_rack, tiles_played[0], number_of_tiles_in_bag, 0,
            leave_value);
        best_equity =
            exchange_equity > best_equity ? exchange_equity : best_equity;
      }
      if (best_equity > cutoff) {
        return best_equity;
      }
    }
    int letter_idx = 0;
    while (letter_idx < number_of_letters &&
           kept_counts[letter_idx] == counts[letter_idx]) {
      kept_counts[letter_idx] = 0;
      letter_idx++;
    }
    if (letter_idx == number_of_letters) {
      return best_equity;
    }
    kept_counts[letter_idx]++;
    changed_idx = letter_idx;
  }
}

// A rack is consistent with the target's move when no move beats it by more
// than the equity margin. For a tile placement the move's equity is its
// score plus the leave value. For an exchange of N tiles it is the best
// exchange of N tiles that rack can make, and the leave and exchanged tiles
// recorded for the rack come from that exchange. Exchanges of other sizes
// are ordinary moves under this rule.
void evaluate_possible_leave(Inference *inference) {
  const Equity current_leave_value =
      inference_get_leave_value(inference, inference->current_target_leave);
  const bool is_exchange = inference->target_number_of_tiles_exchanged > 0;

  Equity target_equity_cutoff;
  if (is_exchange) {
    target_equity_cutoff =
        get_best_exchange_equity(inference) + inference->equity_margin;
  } else {
    const Game *game = inference->game;
    const Equity nonopening_equity = static_eval_get_nonopening_move_equity(
        game_get_ld(game), inference->current_target_leave,
        player_get_rack(game_get_player(game, 1 - inference->target_index)),
        inference->target_tiles_played, bag_get_letters(game_get_bag(game)),
        inference->target_score, current_leave_value);
    target_equity_cutoff = nonopening_equity +
                           inference->target_placement_adjustment +
                           inference->equity_margin;
  }

  // With the cutoff optimization, move generation stops at the first move
  // above the cutoff and skips anchors that cannot reach it, so the move it
  // returns is above the cutoff exactly when the best move is. The census
  // always stops there.
  Equity top_equity;
  if (inference->census_play_scores != NULL) {
    top_equity = get_top_equity_from_census(inference, target_equity_cutoff);
  } else {
    top_equity = move_get_equity(get_top_equity_move_for_inferences(
        inference->game, inference->move_list,
        inference->use_infer_cutoff_optimization ? target_equity_cutoff
                                                 : EQUITY_MAX_VALUE));
  }

  const bool recordable = target_equity_cutoff >= top_equity ||
                          rack_is_empty(inference->bag_as_rack);
  if (!recordable) {
    return;
  }

  const uint64_t number_of_draws_for_leave = get_number_of_draws_for_rack(
      inference->bag_as_rack, inference->current_target_leave);
  if (is_exchange) {
    record_valid_leave(inference->current_target_leave, inference->results,
                       INFERENCE_TYPE_RACK,
                       equity_to_double(current_leave_value),
                       number_of_draws_for_leave);
    const Rack *best_exchange_leave = inference->best_exchange_leave;
    rack_copy(inference->current_target_exchanged,
              inference->current_target_leave);
    rack_subtract_using_floor_zero(inference->current_target_exchanged,
                                   best_exchange_leave);
    record_valid_leave(best_exchange_leave, inference->results,
                       INFERENCE_TYPE_LEAVE,
                       equity_to_double(inference_get_leave_value(
                           inference, best_exchange_leave)),
                       number_of_draws_for_leave);
    record_valid_leave(inference->current_target_exchanged, inference->results,
                       INFERENCE_TYPE_EXCHANGED,
                       equity_to_double(inference_get_leave_value(
                           inference, inference->current_target_exchanged)),
                       number_of_draws_for_leave);
    LeaveRackList *lrl =
        inference_results_get_leave_rack_list(inference->results);
    if (lrl) {
      leave_rack_list_insert_rack(
          best_exchange_leave, inference->current_target_exchanged,
          (int)number_of_draws_for_leave, current_leave_value, lrl);
    }
    alias_method_add_rack(
        inference_results_get_alias_method(inference->results),
        best_exchange_leave, (int)number_of_draws_for_leave);
    rack_reset(inference->current_target_exchanged);
  } else {
    record_valid_leave(inference->current_target_leave, inference->results,
                       INFERENCE_TYPE_LEAVE,
                       equity_to_double(current_leave_value),
                       number_of_draws_for_leave);
    alias_method_add_rack(
        inference_results_get_alias_method(inference->results),
        inference->current_target_leave, (int)number_of_draws_for_leave);
    LeaveRackList *lrl =
        inference_results_get_leave_rack_list(inference->results);
    if (lrl) {
      leave_rack_list_insert_rack(inference->current_target_leave, NULL,
                                  (int)number_of_draws_for_leave,
                                  current_leave_value, lrl);
    }
  }
}

void increment_letter_for_inference(Inference *inference,
                                    MachineLetter letter) {
  rack_take_letter(inference->bag_as_rack, letter);
  rack_add_letter(inference->current_target_rack, letter);
  rack_add_letter(inference->current_target_leave, letter);
}

void decrement_letter_for_inference(Inference *inference,
                                    MachineLetter letter) {
  rack_add_letter(inference->bag_as_rack, letter);
  rack_take_letter(inference->current_target_rack, letter);
  rack_take_letter(inference->current_target_leave, letter);
}

void complete_inference_setup(Inference *inference, const InferenceArgs *args) {
  // This will return the inference->current_target_rack to the bag.
  return_rack_to_bag(inference->game, 0);
  return_rack_to_bag(inference->game, 1);

  Rack temp_target_rack;
  rack_set_dist_size_and_reset(&temp_target_rack, inference->ld_size);

  rack_union(&temp_target_rack, args->target_played_tiles);
  rack_union(&temp_target_rack, args->target_known_rack);

  bool success = draw_rack_from_bag(inference->game, args->target_index,
                                    &temp_target_rack);
  if (!success) {
    const LetterDistribution *ld = game_get_ld(inference->game);
    StringBuilder *sb = string_builder_create();
    string_builder_add_string(sb, "failed to draw combined (");
    string_builder_add_rack(sb, &temp_target_rack, ld, false);
    string_builder_add_string(sb, ") inferred player played letters (");
    string_builder_add_rack(sb, args->target_played_tiles, ld, false);
    string_builder_add_string(sb, ") and inferred player known rack (");
    string_builder_add_rack(sb, args->target_known_rack, ld, false);
    string_builder_add_string(sb, ") from the bag");
    char *err_msg = string_builder_dump_and_destroy(sb, NULL);
    log_fatal("%s", err_msg);
    free(err_msg);
  }

  success = draw_rack_from_bag(inference->game, 1 - args->target_index,
                               args->nontarget_known_rack);

  if (!success) {
    const LetterDistribution *ld = game_get_ld(inference->game);
    StringBuilder *sb = string_builder_create();
    string_builder_add_string(sb, "failed to draw nontarget player rack (");
    string_builder_add_rack(sb, args->nontarget_known_rack, ld, false);
    string_builder_add_string(sb, ") from the bag");
    char *err_msg = string_builder_dump_and_destroy(sb, NULL);
    log_fatal("%s", err_msg);
    free(err_msg);
  }

  // Add the letters that are known to have been kept on the rack
  // for their target inferred play.
  rack_copy(inference->current_target_leave, args->target_known_rack);
  rack_subtract_using_floor_zero(inference->current_target_leave,
                                 args->target_played_tiles);

  // Set the bag_as_rack to the bag
  Bag *bag = game_get_bag(inference->game);
  int bag_letters_array[MAX_ALPHABET_SIZE];
  memset(bag_letters_array, 0, sizeof(bag_letters_array));
  bag_increment_unseen_count(bag, bag_letters_array);
  for (int i = 0; i < MAX_ALPHABET_SIZE; i++) {
    rack_add_letters(inference->bag_as_rack, i, bag_letters_array[i]);
  }

  // MoveGen reads only the count of tiles in game->bag (not their identities)
  // and checks bag_get_letters(bag) + rack_get_total_letters(&opponent_rack) >=
  // 2 * RACK_SIZE for exchange legality. After drawing temp_target_rack and
  // nontarget_known_rack, total_unseen_count is the number of tiles unseen to
  // the observer (the target's rack plus the bag). Adjust game->bag so MoveGen
  // sees the true pre-move bag count (0 when all unseen tiles are on the
  // target's rack, and total_unseen_count - nontarget_known_count when the
  // observer's rack is known).
  const int target_known_and_played =
      rack_get_total_letters(inference->current_target_rack);
  const int current_bag_count = bag_get_letters(bag);
  const int total_unseen_count = target_known_and_played + current_bag_count;
  const int nontarget_known_count =
      rack_get_total_letters(args->nontarget_known_rack);
  const int target_bag_count = total_unseen_count > RACK_SIZE
                                   ? total_unseen_count - nontarget_known_count
                                   : 0;
  for (int draw_idx = current_bag_count; draw_idx > target_bag_count;
       draw_idx--) {
    bag_draw_random_letter(bag, args->target_index);
  }
  for (int add_idx = current_bag_count; add_idx < target_bag_count; add_idx++) {
    bag_add_letter(bag, BLANK_MACHINE_LETTER, args->target_index);
  }

  inference->target_tiles_played =
      rack_get_total_letters(args->target_played_tiles);
  inference->target_placement_adjustment = 0;
  const Board *board = game_get_board(inference->game);
  if (args->target_num_exch == 0 && args->target_move != NULL &&
      move_get_type(args->target_move) == GAME_EVENT_TILE_PLACEMENT_MOVE &&
      board_get_tiles_played(board) == 0) {
    Equity word_penalties[2 * BOARD_DIM];
    Equity letter_penalties[2 * BOARD_DIM];
    board_copy_opening_penalties(board, word_penalties, letter_penalties);
    inference->target_placement_adjustment =
        placement_adjustment(game_get_ld(inference->game), args->target_move,
                             word_penalties, letter_penalties, 0);
  }
}

Inference *inference_create(const Game *game, const InferenceArgs *args,
                            const InferenceResults *results) {
  Inference *inference = malloc_or_die(sizeof(Inference));
  inference->game = game_duplicate(game);
  inference->move_list = move_list_create(1);
  inference->klv =
      player_get_klv(game_get_player(inference->game, args->target_index));

  inference->ld_size = ld_get_size(game_get_ld(inference->game));
  inference->leave_list_capacity = args->leave_list_capacity;
  inference->target_index = args->target_index;
  inference->target_score = args->target_score;
  inference->target_number_of_tiles_exchanged = args->target_num_exch;
  inference->equity_margin = args->equity_margin;
  // Disable cutoff optimization for exchanges with large leaves
  // as benchmarks show it hurts performance for small exchanges
  const int leave_size = RACK_SIZE - args->target_num_exch;
  const bool is_small_exchange =
      args->target_num_exch > 0 &&
      leave_size >= INFERENCE_CUTOFF_MIN_EXCHANGE_LEAVE_SIZE;
  inference->use_infer_cutoff_optimization =
      args->use_inference_cutoff_optimization && !is_small_exchange;
  inference->current_rack_index = 0;

  inference->current_target_leave = rack_create(inference->ld_size);
  inference->current_target_exchanged = rack_create(inference->ld_size);
  inference->best_exchange_leave = rack_create(inference->ld_size);
  inference->current_target_rack =
      player_get_rack(game_get_player(inference->game, args->target_index));
  inference->bag_as_rack = rack_create(inference->ld_size);

  inference->census_lattice = NULL;
  inference->census_play_scores = NULL;
  inference->census_leave_values = NULL;
  inference->claim_size = 1;
  inference->results =
      inference_results_create(inference_results_get_alias_method(results));
  inference_results_reset(inference->results, inference->leave_list_capacity,
                          inference->ld_size);

  // Multithreading
  inference->num_threads = args->num_threads;
  inference->print_interval = args->print_interval;
  inference->thread_control = args->thread_control;
  complete_inference_setup(inference, args);

  return inference;
}

void inference_reset(Inference *inference, const Game *game,
                     const InferenceArgs *args) {
  // Only update fields that will change between
  // inferences that happen in autoplay simming.
  game_copy(inference->game, game);

  inference->target_index = args->target_index;
  inference->target_score = args->target_score;
  inference->target_number_of_tiles_exchanged = args->target_num_exch;
  inference->equity_margin = args->equity_margin;
  // Disable cutoff optimization for exchanges with large leaves
  const int leave_size = RACK_SIZE - args->target_num_exch;
  const bool is_small_exchange =
      args->target_num_exch > 0 &&
      leave_size >= INFERENCE_CUTOFF_MIN_EXCHANGE_LEAVE_SIZE;
  inference->use_infer_cutoff_optimization =
      args->use_inference_cutoff_optimization && !is_small_exchange;
  inference->current_rack_index = 0;

  rack_reset(inference->current_target_leave);
  rack_reset(inference->current_target_exchanged);
  inference->current_target_rack = player_get_rack(
      game_get_player(inference->game, inference->target_index));
  rack_reset(inference->bag_as_rack);

  inference_results_reset(inference->results, inference->leave_list_capacity,
                          inference->ld_size);

  complete_inference_setup(inference, args);
}

void add_inference_results(InferenceResults *inference_results_to_add,
                           InferenceResults *inference_results_to_update) {
  inference_results_add_subtotals(inference_results_to_add,
                                  inference_results_to_update);
  LeaveRackList *lrl_to_add =
      inference_results_get_leave_rack_list(inference_results_to_add);
  if (!lrl_to_add) {
    // This means that lrl_to_update is also necessarily NULL, so no leave racks
    // to add.
    return;
  }
  LeaveRackList *lrl_to_update =
      inference_results_get_leave_rack_list(inference_results_to_update);

  while (leave_rack_list_get_count(lrl_to_add) > 0) {
    const LeaveRack *leave_rack_to_add = leave_rack_list_pop_rack(lrl_to_add);
    leave_rack_list_insert_leave_rack(leave_rack_to_add, lrl_to_update);
  }
}

bool should_print_info(const Inference *inference) {
  return inference->print_interval > 0 && inference->current_rack_index > 0 &&
         inference->current_rack_index % inference->print_interval == 0;
}

// Claims the next claim_size unclaimed positions of the walk. The positions
// are handed out in increasing order, so the claim is never behind the
// claiming worker.
static void claim_next_racks(Inference *inference) {
  inference->claimed_rack_index =
      atomic_fetch_add_explicit(inference->shared_next_rack_index,
                                inference->claim_size, memory_order_relaxed);
}

void iterate_through_all_possible_leaves(Inference *inference,
                                         int tiles_to_infer, int start_letter) {
  if (thread_control_get_status(inference->thread_control) ==
          THREAD_CONTROL_STATUS_USER_INTERRUPT ||
      gen_movegen_budget_spent()) {
    return;
  }
  if (tiles_to_infer == 0) {
    if (inference->current_rack_index >= inference->claimed_rack_index) {
      evaluate_possible_leave(inference);
      if (should_print_info(inference)) {
        print_ucgi_inference_current_rack(inference->current_rack_index,
                                          inference->thread_control);
      }
      if (inference->current_rack_index + 1 ==
          inference->claimed_rack_index + inference->claim_size) {
        claim_next_racks(inference);
      }
    }
    inference->current_rack_index++;
    return;
  }
  for (int letter = start_letter; letter < inference->ld_size; letter++) {
    if (rack_get_letter(inference->bag_as_rack, letter) > 0) {
      increment_letter_for_inference(inference, letter);
      iterate_through_all_possible_leaves(inference, tiles_to_infer - 1,
                                          letter);
      decrement_letter_for_inference(inference, letter);
    }
  }
}

void *infer_worker(void *uncasted_inference) {
  Inference *inference = (Inference *)uncasted_inference;
  (void)gen_set_movegen_budget(inference->movegen_budget);
  claim_next_racks(inference);
  iterate_through_all_possible_leaves(
      inference,
      (RACK_SIZE)-rack_get_total_letters(inference->current_target_rack),
      BLANK_MACHINE_LETTER);
  (void)gen_set_movegen_budget(NULL);
  return NULL;
}

void set_shared_variables_for_inference(
    Inference *inference, _Atomic uint64_t *shared_next_rack_index) {
  inference->shared_next_rack_index = shared_next_rack_index;
}

void infer_manager(InferenceCtx *ctx, InferenceResults *results) {
  for (int thread_index = 0; thread_index < ctx->num_workers; thread_index++) {
    ctx->worker_inferences[thread_index]->movegen_budget =
        gen_get_movegen_budget();
    cpthread_create(&ctx->worker_inferences[thread_index]->cpthread_id,
                    infer_worker, ctx->worker_inferences[thread_index]);
  }

  const bool tiles_were_exchanged =
      ctx->worker_inferences[0]->target_number_of_tiles_exchanged > 0;

  for (int thread_index = 0; thread_index < ctx->num_workers; thread_index++) {
    cpthread_join(ctx->worker_inferences[thread_index]->cpthread_id);
    InferenceResults *worker_results =
        ctx->worker_inferences[thread_index]->results;
    add_inference_results(worker_results, results);
    ctx->leave_stats[thread_index] = inference_results_get_equity_values(
        worker_results, INFERENCE_TYPE_LEAVE);
    if (tiles_were_exchanged) {
      ctx->exchanged_stats[thread_index] = inference_results_get_equity_values(
          worker_results, INFERENCE_TYPE_EXCHANGED);
      ctx->rack_stats[thread_index] = inference_results_get_equity_values(
          worker_results, INFERENCE_TYPE_RACK);
    }
  }

  stats_combine(
      ctx->leave_stats, ctx->num_workers,
      inference_results_get_equity_values(results, INFERENCE_TYPE_LEAVE));
  if (tiles_were_exchanged) {
    stats_combine(
        ctx->exchanged_stats, ctx->num_workers,
        inference_results_get_equity_values(results, INFERENCE_TYPE_EXCHANGED));
    stats_combine(
        ctx->rack_stats, ctx->num_workers,
        inference_results_get_equity_values(results, INFERENCE_TYPE_RACK));
  }
}

void verify_inference_args(const InferenceArgs *args, const Game *game_dup,
                           ErrorStack *error_stack) {
  const Bag *bag = game_get_bag(game_dup);
  int bag_letter_counts[MAX_ALPHABET_SIZE];
  memset(bag_letter_counts, 0, sizeof(bag_letter_counts));
  bag_increment_unseen_count(bag, bag_letter_counts);

  const int ld_size = ld_get_size(game_get_ld(game_dup));

  // Add the current player racks to the bag letter counts
  const Rack *player0_rack = player_get_rack(game_get_player(game_dup, 0));
  const Rack *player1_rack = player_get_rack(game_get_player(game_dup, 1));
  for (int i = 0; i < ld_size; i++) {
    bag_letter_counts[i] +=
        rack_get_letter(player0_rack, i) + rack_get_letter(player1_rack, i);
  }

  Rack temp_target_rack;
  rack_set_dist_size_and_reset(&temp_target_rack, ld_size);

  rack_union(&temp_target_rack, args->target_played_tiles);
  rack_union(&temp_target_rack, args->target_known_rack);

  if (rack_get_total_letters(&temp_target_rack) > (RACK_SIZE)) {
    const LetterDistribution *ld = game_get_ld(game_dup);
    StringBuilder *sb = string_builder_create();
    string_builder_add_string(sb, "inferred player rack (");
    string_builder_add_rack(sb, &temp_target_rack, ld, false);
    string_builder_add_string(sb,
                              ") derived from the union of played letters (");
    string_builder_add_rack(sb, args->target_played_tiles, ld, false);
    string_builder_add_string(sb, ") and known letters (");
    string_builder_add_rack(sb, args->target_known_rack, ld, false);
    string_builder_add_string(sb,
                              ") is greater than the maximum rack size of ");
    string_builder_add_int(sb, RACK_SIZE);
    error_stack_push(error_stack, ERROR_STATUS_INFERENCE_RACK_OVERFLOW,
                     string_builder_dump_and_destroy(sb, NULL));
    return;
  }

  for (int i = 0; i < ld_size; i++) {
    bag_letter_counts[i] -= rack_get_letter(&temp_target_rack, i);
    if (bag_letter_counts[i] < 0) {
      StringBuilder *sb = string_builder_create();
      string_builder_add_string(sb, "inferred player played letters (");
      string_builder_add_rack(sb, &temp_target_rack, game_get_ld(game_dup),
                              false);
      string_builder_add_string(sb, ") not available in the bag");
      error_stack_push(error_stack,
                       ERROR_STATUS_INFERENCE_TARGET_LETTERS_NOT_IN_BAG,
                       string_builder_dump_and_destroy(sb, NULL));
      return;
    }
  }

  // This is the total number of letters that are unseen from the nontarget's
  // perspective. We start with the total tiles in the bag and on both players'
  // racks, then subtract the letters known to be on the nontarget's rack.
  // After the loop below, this value equals: bag tiles + all tiles on the
  // target's rack + any unknown tiles on the nontarget's rack.
  int total_unseen_count = bag_get_letters(bag) +
                           rack_get_total_letters(player0_rack) +
                           rack_get_total_letters(player1_rack);
  for (int i = 0; i < ld_size; i++) {
    const int num_letter_i_on_nontarget_rack =
        rack_get_letter(args->nontarget_known_rack, i);
    bag_letter_counts[i] -= num_letter_i_on_nontarget_rack;
    if (bag_letter_counts[i] < 0) {
      StringBuilder *sb = string_builder_create();
      string_builder_add_string(sb, "noninferred player rack letters (");
      string_builder_add_rack(sb, args->nontarget_known_rack,
                              game_get_ld(game_dup), false);
      string_builder_add_string(sb, ") not available in the bag");
      char *err_msg = string_builder_dump_and_destroy(sb, NULL);
      log_fatal("%s", err_msg);
      free(err_msg);
      return;
    }
    total_unseen_count -= num_letter_i_on_nontarget_rack;
  }

  // The zero-played-and-zero-exchanged (pass) case is handled by the caller
  // (infer_with_initialized_ctx) before verify_inference_args is ever
  // reached, so it can't occur here.
  const int num_played_letters =
      rack_get_total_letters(args->target_played_tiles);

  if (num_played_letters != 0 && args->target_num_exch != 0) {
    error_stack_push(error_stack, ERROR_STATUS_INFERENCE_BOTH_PLAY_AND_EXCHANGE,
                     string_duplicate("cannot infer when both a tile placement "
                                      "and exchange move are specified"));
    return;
  }

  if (args->target_num_exch != 0 && total_unseen_count < (RACK_SIZE) * 2) {
    error_stack_push(error_stack, ERROR_STATUS_INFERENCE_EXCHANGE_NOT_ALLOWED,
                     get_formatted_string(
                         "cannot infer an exchange where there are fewer "
                         "than %d unseen tiles from the player's perspective",
                         (RACK_SIZE) * 2));
    return;
  }

  if (args->target_num_exch != 0 && args->target_score != 0) {
    error_stack_push(
        error_stack, ERROR_STATUS_INFERENCE_EXCHANGE_SCORE_NOT_ZERO,
        string_duplicate("cannot infer an exchange with a nonzero score"));
    return;
  }

  if (num_played_letters > (RACK_SIZE)) {
    error_stack_push(
        error_stack, ERROR_STATUS_INFERENCE_RACK_OVERFLOW,
        get_formatted_string("inferred player played more tiles (%d) "
                             "than can fit in a rack (%d)",
                             num_played_letters, RACK_SIZE));
    return;
  }
}

void populate_inference_args_with_game_history(InferenceArgs *args,
                                               Game *game_dup,
                                               ErrorStack *error_stack) {
  GameHistory *game_history = args->game_history;
  const int most_recent_move_event_index =
      game_history_get_most_recent_move_event_index(game_history);
  if (most_recent_move_event_index < 0) {
    error_stack_push(
        error_stack, ERROR_STATUS_INFERENCE_EMPTY_GAME_HISTORY,
        get_formatted_string(
            "cannot infer the previous play for an empty game history"));
    return;
  }
  const GameEvent *target_move_event =
      game_history_get_event(game_history, most_recent_move_event_index);
  const ValidatedMoves *last_move = game_event_get_vms(target_move_event);
  const Move *move = validated_moves_get_move(last_move, 0);
  const int move_tiles_length = move_get_tiles_length(move);
  rack_reset(args->target_played_tiles);
  for (int i = 0; i < move_tiles_length; i++) {
    if (move_get_tile(move, i) != PLAYED_THROUGH_MARKER) {
      if (get_is_blanked(move_get_tile(move, i))) {
        rack_add_letter(args->target_played_tiles, BLANK_MACHINE_LETTER);
      } else {
        rack_add_letter(args->target_played_tiles, move_get_tile(move, i));
      }
    }
  }
  args->target_index = game_event_get_player_index(target_move_event);
  args->target_score = game_event_get_move_score(target_move_event);
  args->target_num_exch = 0;
  args->target_move =
      (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) ? move : NULL;
  if (move_get_type(move) == GAME_EVENT_EXCHANGE) {
    args->target_num_exch = move_get_tiles_played(move);
    rack_reset(args->target_played_tiles);
  }
  // Only overwrite nontarget_known_rack from the replayed game state if the
  // caller has not already provided one. Callers such as analyze pre-set this
  // to the current player's actual GCG rack so that inference only considers
  // leaves whose tiles are genuinely available in the simulation bag. If we
  // always overwrote it here, we would copy the nontarget player's rack from
  // the replayed game_dup, which is empty (their tiles are in the bag at the
  // replayed state), allowing inference to enumerate racks that include tiles
  // already held by the current player — causing fatal draw failures when
  // simulation later tries to draw those tiles from the bag.
  if (rack_is_empty(args->nontarget_known_rack)) {
    rack_copy(
        args->nontarget_known_rack,
        player_get_rack(game_get_player(game_dup, 1 - args->target_index)));
  }
  if (rack_is_empty(args->target_known_rack)) {
    for (int i = most_recent_move_event_index - 1; i >= 0; i--) {
      GameEvent *event = game_history_get_event(game_history, i);
      if (game_event_get_player_index(event) == args->target_index) {
        rack_copy(args->target_known_rack,
                  game_event_get_after_event_player_off_turn_rack(event));
        break;
      }
    }
  }

  // This will play all of the events right up to but not including the target
  // move event
  game_play_n_events(game_history, game_dup, most_recent_move_event_index,
                     false, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
}

// ***************************
// Inference Context functions
// ***************************

InferenceCtx *inference_ctx_create(int num_threads) {
  InferenceCtx *ctx = calloc_or_die(1, sizeof(InferenceCtx));
  ctx->num_workers = num_threads;
  ctx->worker_inferences = calloc_or_die(ctx->num_workers, sizeof(Inference *));
  return ctx;
}

void inference_ctx_set_game(InferenceCtx *ctx, const Game *game) {
  if (ctx->game) {
    game_copy(ctx->game, game);
  } else {
    ctx->game = game_duplicate(game);
  }
}

// Assumes that ctx->game is set
void inference_ctx_set_inferences(InferenceCtx *ctx, const InferenceArgs *args,
                                  InferenceResults *results) {
  atomic_store_explicit(&ctx->shared_next_rack_index, 0, memory_order_relaxed);
  inference_results_reset(results, args->leave_list_capacity,
                          ld_get_size(game_get_ld(args->game)));
  if (ctx->worker_inferences[0]) {
    for (int i = 0; i < ctx->num_workers; i++) {
      inference_reset(ctx->worker_inferences[i], ctx->game, args);
    }
  } else {
    for (int i = 0; i < ctx->num_workers; i++) {
      ctx->worker_inferences[i] = inference_create(ctx->game, args, results);
      set_shared_variables_for_inference(ctx->worker_inferences[i],
                                         &ctx->shared_next_rack_index);
    }
    ctx->leave_stats = malloc_or_die((sizeof(Stat *)) * (ctx->num_workers));
    ctx->exchanged_stats = malloc_or_die((sizeof(Stat *)) * (ctx->num_workers));
    ctx->rack_stats = malloc_or_die((sizeof(Stat *)) * (ctx->num_workers));
  }
}

static void inference_ctx_free_census(InferenceCtx *ctx) {
  free(ctx->census_play_scores);
  ctx->census_play_scores = NULL;
  free(ctx->census_leave_values);
  ctx->census_leave_values = NULL;
  ctx->census_leave_klv = NULL;
  census_lattice_destroy(ctx->census_lattice);
  ctx->census_lattice = NULL;
}

// Whether the census play scores value every rack of the inference exactly as
// get_top_equity_move_for_inferences does, and there are enough racks to pay
// for building them. They hold no opening placement adjustment (the board
// must have tiles) and no PAT term (which inferences leave out anyway). The
// census spells classic words with the first player's word graph, which must
// then be the target's, and generate_moves plays for the player on turn.
static bool inference_can_use_census(const Inference *inference,
                                     const InferenceArgs *args) {
  const Game *game = inference->game;
  const int tiles_to_infer =
      RACK_SIZE - rack_get_total_letters(inference->current_target_rack);
  return args->use_census_play_scores &&
         tiles_to_infer >= INFERENCE_CENSUS_MIN_TILES_TO_INFER &&
         !rack_is_empty(inference->bag_as_rack) && inference->klv != NULL &&
         board_get_tiles_played(game_get_board(game)) > 0 &&
         game_get_variant(game) == GAME_VARIANT_CLASSIC &&
         game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG) &&
         game_get_player_on_turn_index(game) == inference->target_index &&
         inference->ld_size <= CENSUS_MAX_LETTERS;
}

// One thread's share of building the census play scores: its slice of the
// lattice to reset, then the units it claims.
typedef struct CensusBuildWorker {
  const Game *game;
  const CensusLattice *lattice;
  const uint8_t *unseen_tally;
  Equity *play_scores;
  int number_of_workers;
  int thread_index;
  _Atomic int *next_unit;
  cpthread_t cpthread_id;
} CensusBuildWorker;

static void *census_reset_worker(void *uncasted_worker) {
  const CensusBuildWorker *worker = (const CensusBuildWorker *)uncasted_worker;
  const uint64_t lattice_size = census_lattice_get_size(worker->lattice);
  const uint32_t start =
      (uint32_t)(lattice_size * (uint64_t)worker->thread_index /
                 (uint64_t)worker->number_of_workers);
  const uint32_t end =
      (uint32_t)(lattice_size * (uint64_t)(worker->thread_index + 1) /
                 (uint64_t)worker->number_of_workers);
  for (uint32_t index = start; index < end; index++) {
    worker->play_scores[index] = CENSUS_UNPLAYABLE;
  }
  return NULL;
}

static void *census_build_worker(void *uncasted_worker) {
  const CensusBuildWorker *worker = (const CensusBuildWorker *)uncasted_worker;
  census_play_scores_build_claimed(
      worker->game, worker->lattice, worker->unseen_tally, worker->play_scores,
      worker->number_of_workers > 1
          ? census_lattice_get_num_letters(worker->lattice)
          : 1,
      worker->next_unit, CENSUS_ALL_LANES);
  return NULL;
}

// Runs one phase of the build on every worker's thread, the first in the
// calling thread.
static void census_run_build_phase(CensusBuildWorker *workers,
                                   int number_of_workers,
                                   void *(*phase)(void *)) {
  for (int thread_index = 1; thread_index < number_of_workers; thread_index++) {
    cpthread_create(&workers[thread_index].cpthread_id, phase,
                    &workers[thread_index]);
  }
  phase(&workers[0]);
  for (int thread_index = 1; thread_index < number_of_workers; thread_index++) {
    cpthread_join(workers[thread_index].cpthread_id);
  }
}

// Builds the board's census play scores for the workers when
// inference_can_use_census allows it, and otherwise has them generate moves.
// Every worker's thread builds the units it claims into the same play
// scores.
static void inference_ctx_set_census_play_scores(InferenceCtx *ctx,
                                                 const InferenceArgs *args) {
  const Inference *first = ctx->worker_inferences[0];
  bool use_census = inference_can_use_census(first, args);
  if (use_census &&
      (ctx->census_lattice == NULL ||
       census_lattice_get_num_letters(ctx->census_lattice) != first->ld_size)) {
    inference_ctx_free_census(ctx);
    ErrorStack *error_stack = error_stack_create();
    ctx->census_lattice =
        census_lattice_create(first->ld_size, RACK_SIZE, error_stack);
    error_stack_destroy(error_stack);
    if (ctx->census_lattice != NULL &&
        census_lattice_get_size(ctx->census_lattice) <=
            INFERENCE_CENSUS_MAX_LATTICE_SIZE) {
      ctx->census_play_scores = malloc_or_die(
          sizeof(Equity) * census_lattice_get_size(ctx->census_lattice));
    }
  }
  use_census = use_census && ctx->census_play_scores != NULL;
  if (use_census && (ctx->census_leave_values == NULL ||
                     ctx->census_leave_klv != first->klv ||
                     ctx->census_leave_klv_fingerprint !=
                         klv_get_instance_fingerprint(first->klv) ||
                     ctx->census_leave_klv_mutation_counter !=
                         klv_get_mutation_counter(first->klv))) {
    if (ctx->census_leave_values == NULL) {
      ctx->census_leave_values = malloc_or_die(
          sizeof(Equity) * census_lattice_get_size(ctx->census_lattice));
    }
    census_leaves_from_klv(ctx->census_lattice, first->klv,
                           ctx->census_leave_values);
    ctx->census_leave_klv = first->klv;
    ctx->census_leave_klv_fingerprint =
        klv_get_instance_fingerprint(first->klv);
    ctx->census_leave_klv_mutation_counter =
        klv_get_mutation_counter(first->klv);
  }
  if (use_census) {
    // The pool every rack the inference values is drawn from: the target's
    // known tiles and what the observer has not seen. Not the board's unseen
    // tiles, which can differ from it when the bag has been set up by hand,
    // and are more tiles to spell words from.
    uint8_t unseen_tally[CENSUS_MAX_LETTERS];
    for (int ml = 0; ml < first->ld_size; ml++) {
      unseen_tally[ml] =
          (uint8_t)(rack_get_letter(first->bag_as_rack, ml) +
                    rack_get_letter(first->current_target_rack, ml));
    }
    _Atomic int next_unit;
    atomic_init(&next_unit, 0);
    CensusBuildWorker *workers =
        malloc_or_die(sizeof(CensusBuildWorker) * (size_t)ctx->num_workers);
    for (int thread_index = 0; thread_index < ctx->num_workers;
         thread_index++) {
      workers[thread_index] = (CensusBuildWorker){
          .game = first->game,
          .lattice = ctx->census_lattice,
          .unseen_tally = unseen_tally,
          .play_scores = ctx->census_play_scores,
          .number_of_workers = ctx->num_workers,
          .thread_index = thread_index,
          .next_unit = &next_unit,
      };
    }
    census_run_build_phase(workers, ctx->num_workers, census_reset_worker);
    census_run_build_phase(workers, ctx->num_workers, census_build_worker);
    free(workers);
  }
  for (int thread_index = 0; thread_index < ctx->num_workers; thread_index++) {
    Inference *inference = ctx->worker_inferences[thread_index];
    inference->census_lattice = use_census ? ctx->census_lattice : NULL;
    inference->census_play_scores = use_census ? ctx->census_play_scores : NULL;
    inference->census_leave_values =
        use_census ? ctx->census_leave_values : NULL;
    inference->claim_size = use_census ? INFERENCE_CENSUS_CLAIM_SIZE : 1;
  }
}

void inference_ctx_destroy(InferenceCtx *ctx) {
  if (!ctx) {
    return;
  }
  game_destroy(ctx->game);
  inference_ctx_free_census(ctx);
  for (int i = 0; i < ctx->num_workers; i++) {
    inference_destroy(ctx->worker_inferences[i]);
  }
  free(ctx->worker_inferences);
  free(ctx->leave_stats);
  free(ctx->exchanged_stats);
  free(ctx->rack_stats);
  free(ctx);
}

void infer_with_initialized_ctx(InferenceArgs *args, InferenceCtx *ctx,
                                InferenceResults *results,
                                ErrorStack *error_stack) {
  inference_ctx_set_game(ctx, args->game);

  if (args->use_game_history) {
    populate_inference_args_with_game_history(args, ctx->game, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }

  // A target move with zero played and zero exchanged tiles is a pass:
  // there is no evidence to infer a leave from. This isn't an error (a pass
  // is a perfectly ordinary move to hand to inference, e.g. from analyze or
  // annotation mode replaying a whole game) -- just return an empty, valid
  // result instead of running verify_inference_args, which would otherwise
  // reject it.
  if (rack_get_total_letters(args->target_played_tiles) == 0 &&
      args->target_num_exch == 0) {
    const int ld_size = ld_get_size(game_get_ld(ctx->game));
    inference_results_reset(results, 1, ld_size);
    inference_results_set_valid_for_current_game_state(results, true);
    return;
  }

  verify_inference_args(args, ctx->game, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  inference_ctx_set_inferences(ctx, args, results);
  inference_ctx_set_census_play_scores(ctx, args);

  infer_manager(ctx, results);

  const Inference *wi = ctx->worker_inferences[0];
  inference_results_finalize(
      args->target_played_tiles, wi->current_target_leave, wi->bag_as_rack,
      results, wi->target_score, wi->target_number_of_tiles_exchanged,
      wi->equity_margin,
      thread_control_get_status(args->thread_control) ==
          THREAD_CONTROL_STATUS_USER_INTERRUPT);
}

// Creates a new context if *ctx is NULL
// The caller is responsible for destroying the context
void infer(InferenceArgs *args, InferenceCtx **ctx, InferenceResults *results,
           ErrorStack *error_stack) {
  // The **ctx is a pointer to a pointer and must never be null;
  assert(ctx);
  if (*ctx == NULL) {
    *ctx = inference_ctx_create(args->num_threads);
  }
  infer_with_initialized_ctx(args, *ctx, results, error_stack);
}

void infer_without_ctx(InferenceArgs *args, InferenceResults *results,
                       ErrorStack *error_stack) {
  InferenceCtx *ctx = NULL;
  infer(args, &ctx, results, error_stack);
  inference_ctx_destroy(ctx);
}
