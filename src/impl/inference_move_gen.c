#include "inference_move_gen.h"

#include "../def/move_defs.h"
#include "../def/rack_defs.h"
#include "../ent/bit_rack.h"
#include "../ent/klv.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../str/move_string.h"
#include "../str/rack_string.h"
#include "../util/math_util.h"
#include "../util/string_util.h"
#include "move_gen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Temporary debug flag
#define DEBUG_INFERENCE_MOVEGEN 1

// Internal state for move generation
typedef struct InferenceMoveGenState {
  const InferenceMoveGenArgs *args;
  const LetterDistribution *ld;
  int ld_size;
  const KLV *klv;

  // Current rack being built from bag
  Rack current_rack;
  // Tiles available to draw from (initially the bag, modified during recursion)
  Rack available_tiles;
  // Original bag contents (unmodified, for draw counting)
  Rack original_bag;
  // Scratch space for exchange tiles
  MachineLetter exchange_tiles[RACK_SIZE];
  // Scratch space for move leave (used when generating moves)
  Rack leave_rack;
  // Target leave = current_rack - target_played_tiles (for draw counting)
  Rack target_leave;

  // Move list for scoring play generation
  MoveList *move_list;

  // Debug counters
  int rack_count;
  int exchange_count;
  int scoring_move_count;
} InferenceMoveGenState;

// Forward declarations
static void enumerate_racks_recursive(InferenceMoveGenState *state,
                                      int tiles_to_draw, MachineLetter start_ml);
static void generate_scoring_moves_for_rack(InferenceMoveGenState *state);
static void enumerate_exchanges_for_rack(InferenceMoveGenState *state);
static void enumerate_exchange_subsets(InferenceMoveGenState *state,
                                       MachineLetter start_ml,
                                       int num_exchanged);
static void record_exchange_for_leave(InferenceMoveGenState *state,
                                      int num_exchanged);
static uint64_t compute_draw_count(const Rack *bag, const Rack *leave);

// Main entry point
void generate_rack_moves_from_bag(const InferenceMoveGenArgs *args) {
  InferenceMoveGenState state;
  memset(&state, 0, sizeof(state));

  state.args = args;
  state.ld = game_get_ld(args->game);
  state.ld_size = ld_get_size(state.ld);

  // Get KLV from player or use override
  if (args->override_kwg) {
    // For now, if override_kwg is provided, we still need a KLV for leave values
    // Use player 0's KLV as fallback
    state.klv = player_get_klv(game_get_player(args->game, 0));
  } else {
    state.klv = player_get_klv(game_get_player(args->game, 0));
  }

  // Initialize racks
  rack_set_dist_size(&state.current_rack, state.ld_size);
  rack_set_dist_size(&state.available_tiles, state.ld_size);
  rack_set_dist_size(&state.original_bag, state.ld_size);
  rack_set_dist_size(&state.leave_rack, state.ld_size);
  rack_set_dist_size(&state.target_leave, state.ld_size);

  // Copy bag contents to available tiles and preserve original for draw counting
  rack_copy(&state.available_tiles, args->bag_as_rack);
  rack_copy(&state.original_bag, args->bag_as_rack);

  // Create move list for scoring play generation
  state.move_list = move_list_create(args->move_list_capacity);

  // Determine how many tiles to draw
  int tiles_to_draw = RACK_SIZE;

  // If target_played_tiles is provided, those tiles MUST be in the rack.
  // Start the rack with those tiles and only enumerate the remainder.
  rack_reset(&state.current_rack);
  if (args->target_played_tiles != NULL) {
    int num_played = rack_get_total_letters(args->target_played_tiles);
    tiles_to_draw = RACK_SIZE - num_played;

#if DEBUG_INFERENCE_MOVEGEN
    StringBuilder *sb = string_builder_create();
    string_builder_add_rack(sb, args->target_played_tiles, state.ld, false);
    printf("Revealed tiles: %s (%d tiles)\n", string_builder_peek(sb), num_played);
    printf("Need to enumerate %d more tiles from bag\n\n", tiles_to_draw);
    string_builder_destroy(sb);
#endif

    // Copy revealed tiles to current_rack
    for (int ml = 0; ml < state.ld_size; ml++) {
      int count = rack_get_letter(args->target_played_tiles, ml);
      if (count > 0) {
        rack_add_letters(&state.current_rack, ml, count);
        // Note: revealed tiles should NOT be in the bag (they came from opponent's rack)
        // So we don't remove them from available_tiles - they're already not there
      }
    }
  }

  // Start recursive enumeration of remaining tiles
  enumerate_racks_recursive(&state, tiles_to_draw, 0);

#if DEBUG_INFERENCE_MOVEGEN
  printf("\n=== SUMMARY ===\n");
  printf("Total racks enumerated: %d\n", state.rack_count);
  printf("Total scoring moves recorded: %d\n", state.scoring_move_count);
  printf("Total exchanges recorded: %d\n", state.exchange_count);
  printf("===============\n\n");
#endif

  // Clean up
  move_list_destroy(state.move_list);
}

// Compute the number of ways to draw the given rack from the original bag.
// The bag_as_rack is the bag AFTER the player drew (i.e., current bag state).
// The rack is what the player drew.
// Original bag had: bag_count + rack_count of each letter.
// We compute C(bag_count + rack_count, rack_count) for each letter and multiply.
static uint64_t compute_draw_count(const Rack *bag_as_rack, const Rack *rack) {
  uint64_t count = 1;
  int dist_size = rack_get_dist_size(bag_as_rack);
  for (int ml = 0; ml < dist_size; ml++) {
    int bag_count = rack_get_letter(bag_as_rack, ml);
    int rack_count = rack_get_letter(rack, ml);
    if (rack_count > 0) {
      // C(bag_count + rack_count, rack_count) = number of ways to choose
      // rack_count items from original pool of (bag_count + rack_count) items
      count *= choose(bag_count + rack_count, rack_count);
    }
  }
  return count;
}

// Generate scoring moves for the current rack and record them
static void generate_scoring_moves_for_rack(InferenceMoveGenState *state) {
  Game *game = state->args->game;

  // Set the current rack on the player
  // Player 0 is assumed to be the player whose rack we're enumerating
  Rack *player_rack = player_get_rack(game_get_player(game, 0));
  rack_copy(player_rack, &state->current_rack);

  // Reset the move list
  move_list_reset(state->move_list);

  // Set up move generation args
  // Use MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST with eq_margin_movegen for pruning
  move_record_t record_type = MOVE_RECORD_ALL;
  if (state->args->eq_margin_movegen > 0) {
    record_type = MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST;
  }

  MoveGenArgs movegen_args = {
      .game = game,
      .move_list = state->move_list,
      .move_record_type = record_type,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = state->args->override_kwg,
      .thread_index = 0,
      .eq_margin_movegen = state->args->eq_margin_movegen,
  };

  // Generate moves
  generate_moves(&movegen_args);

  // Process each generated move
  int num_moves = move_list_get_count(state->move_list);
  for (int i = 0; i < num_moves; i++) {
    const Move *move = move_list_get_move(state->move_list, i);

    // Skip exchanges - we handle those separately
    if (move_get_type(move) == GAME_EVENT_EXCHANGE) {
      continue;
    }

    // Skip passes
    if (move_get_type(move) == GAME_EVENT_PASS) {
      continue;
    }

    // Compute the leave for this move
    rack_copy(&state->leave_rack, &state->current_rack);
    int tiles_length = move_get_tiles_length(move);
    for (int j = 0; j < tiles_length; j++) {
      MachineLetter ml = move_get_tile(move, j);
      if (ml == PLAYED_THROUGH_MARKER) {
        continue;
      }
      if (get_is_blanked(ml)) {
        rack_take_letter(&state->leave_rack, BLANK_MACHINE_LETTER);
      } else {
        rack_take_letter(&state->leave_rack, ml);
      }
    }

    // Compute draw count for the target leave (leave with respect to target play)
    uint64_t draws = compute_draw_count(&state->original_bag, &state->target_leave);

    // Create BitRack for the full rack (key by rack, not leave)
    BitRack bit_rack = bit_rack_create_from_rack(state->ld, &state->current_rack);

    // Get leave value
    Equity leave_value = klv_get_leave_value(state->klv, &state->leave_rack);

#if DEBUG_INFERENCE_MOVEGEN
    state->scoring_move_count++;
#endif

    // Add to rack hash table
    rack_hash_table_add_move(state->args->rack_hash_table, &bit_rack, leave_value,
                             (int)draws, 1.0f, move);
  }
}

// Recursively enumerate all possible racks of RACK_SIZE tiles from available
// tiles
static void enumerate_racks_recursive(InferenceMoveGenState *state,
                                      int tiles_to_draw,
                                      MachineLetter start_ml) {
  if (tiles_to_draw == 0) {
    // We have a complete rack, enumerate exchanges
    enumerate_exchanges_for_rack(state);
    return;
  }

  // Try adding each possible letter from start_ml onwards
  for (MachineLetter ml = start_ml; ml < state->ld_size; ml++) {
    int available = rack_get_letter(&state->available_tiles, ml);
    if (available == 0) {
      continue;
    }

    // Take up to min(available, tiles_to_draw) of this letter
    int max_to_take = available < tiles_to_draw ? available : tiles_to_draw;

    for (int count = 1; count <= max_to_take; count++) {
      // Add 'count' copies of ml to current rack
      rack_add_letters(&state->current_rack, ml, count);
      rack_take_letters(&state->available_tiles, ml, count);

      // Recurse with remaining tiles to draw
      enumerate_racks_recursive(state, tiles_to_draw - count, ml + 1);

      // Backtrack
      rack_take_letters(&state->current_rack, ml, count);
      rack_add_letters(&state->available_tiles, ml, count);
    }
  }
}

// For a complete rack, generate scoring moves and enumerate exchanges
static void enumerate_exchanges_for_rack(InferenceMoveGenState *state) {
#if DEBUG_INFERENCE_MOVEGEN
  state->rack_count++;
  // Print progress every 10000 racks
  if (state->rack_count % 10000 == 0 || state->rack_count <= 5) {
    StringBuilder *sb = string_builder_create();
    string_builder_add_rack(sb, &state->current_rack, state->ld, false);
    printf("RACK #%d: %s\n", state->rack_count, string_builder_peek(sb));
    string_builder_destroy(sb);
    fflush(stdout);
  }
#endif

  // Compute target_leave = current_rack - target_played_tiles (for draw counting)
  // This is the leave with respect to the TARGET PLAY, not any generated move
  rack_copy(&state->target_leave, &state->current_rack);
  if (state->args->target_played_tiles != NULL) {
    rack_subtract_using_floor_zero(&state->target_leave,
                                   state->args->target_played_tiles);
  }

  // Generate scoring moves for this rack
  generate_scoring_moves_for_rack(state);

  // An exchange must exchange at least 1 tile
  // The leave will have 0 to RACK_SIZE-1 tiles
  enumerate_exchange_subsets(state, 0, 0);
}

// Recursively enumerate exchange subsets
// num_exchanged: number of tiles selected for exchange so far
static void enumerate_exchange_subsets(InferenceMoveGenState *state,
                                       MachineLetter start_ml,
                                       int num_exchanged) {
  // If we have at least 1 tile exchanged, record this exchange
  if (num_exchanged > 0) {
    record_exchange_for_leave(state, num_exchanged);
  }

  // Try adding more tiles to exchange (if not already at RACK_SIZE)
  if (num_exchanged >= RACK_SIZE) {
    return;
  }

  for (MachineLetter ml = start_ml; ml < state->ld_size; ml++) {
    int in_rack = rack_get_letter(&state->current_rack, ml);
    if (in_rack == 0) {
      continue;
    }

    // Count how many of this letter we've already selected for exchange
    int already_exchanged = 0;
    for (int i = 0; i < num_exchanged; i++) {
      if (state->exchange_tiles[i] == ml) {
        already_exchanged++;
      }
    }

    int can_exchange = in_rack - already_exchanged;
    if (can_exchange <= 0) {
      continue;
    }

    // Add one more of this letter to exchange
    state->exchange_tiles[num_exchanged] = ml;
    enumerate_exchange_subsets(state, ml, num_exchanged + 1);
  }
}

// Record an exchange move for the current leave
static void record_exchange_for_leave(InferenceMoveGenState *state,
                                      int num_exchanged) {
  // Compute the leave: current_rack minus exchanged tiles
  rack_copy(&state->leave_rack, &state->current_rack);
  for (int i = 0; i < num_exchanged; i++) {
    rack_take_letter(&state->leave_rack, state->exchange_tiles[i]);
  }

  // Create BitRack for the full rack (key by rack, not leave)
  BitRack bit_rack = bit_rack_create_from_rack(state->ld, &state->current_rack);

  // Get leave value from KLV
  Equity leave_value = klv_get_leave_value(state->klv, &state->leave_rack);

  // Create the exchange move
  Move exchange_move;
  move_set_all(&exchange_move, state->exchange_tiles, 0, num_exchanged - 1,
               /*score=*/0, /*row_start=*/0, /*col_start=*/0,
               /*tiles_played=*/num_exchanged, BOARD_HORIZONTAL_DIRECTION,
               GAME_EVENT_EXCHANGE, leave_value);

  // Compute draw count for the target leave (leave with respect to target play)
  uint64_t draws = compute_draw_count(&state->original_bag, &state->target_leave);

#if DEBUG_INFERENCE_MOVEGEN
  state->exchange_count++;
  // Only print first few exchanges total to avoid flooding output
  // (127 exchanges per rack × millions of racks = too much)
#endif

  // Add to rack hash table
  rack_hash_table_add_move(state->args->rack_hash_table, &bit_rack, leave_value,
                           (int)draws, 1.0f, &exchange_move);
}

// Record a scoring play to the rack hash table.
// Computes leave from current_rack minus played tiles,
// then adds move with equity = score + leave_value.
void record_scoring_play_to_rack_hash_table(RackHashTable *rht,
                                            const LetterDistribution *ld,
                                            const KLV *klv,
                                            const Rack *current_rack,
                                            const Anchor *anchor,
                                            const Move *move) {
  // Anchor provides starting position (row, col) and direction
  // Its highest_possible_equity/score should be MAX values
  int start_row = anchor->row;
  int start_col = anchor->col;
  int dir = anchor->dir;
  (void)start_row;
  (void)start_col;
  (void)dir;

  // Compute the leave: current_rack minus tiles played
  Rack leave_rack;
  int ld_size = ld_get_size(ld);
  rack_set_dist_size(&leave_rack, ld_size);
  rack_copy(&leave_rack, current_rack);

  int tiles_played = move_get_tiles_played(move);
  for (int i = 0; i < tiles_played; i++) {
    MachineLetter ml = move_get_tile(move, i);
    // Skip playthrough markers (tiles already on the board)
    if (ml == PLAYED_THROUGH_MARKER) {
      continue;
    }
    // For blanks, we need to take the blank from the rack, not the letter it represents
    if (get_is_blanked(ml)) {
      rack_take_letter(&leave_rack, BLANK_MACHINE_LETTER);
    } else {
      rack_take_letter(&leave_rack, ml);
    }
  }

  // Create BitRack for the leave
  BitRack bit_rack = bit_rack_create_from_rack(ld, &leave_rack);

  // Get leave value from KLV
  Equity leave_value = klv_get_leave_value(klv, &leave_rack);

  // Compute equity = score + leave_value
  Equity score = move_get_score(move);
  Equity equity = score + leave_value;

  // Create a copy of the move with updated equity
  Move scored_move;
  memcpy(&scored_move, move, sizeof(Move));
  move_set_equity(&scored_move, equity);

#if DEBUG_INFERENCE_MOVEGEN
  StringBuilder *sb = string_builder_create();
  string_builder_add_rack(sb, &leave_rack, ld, false);
  char *leave_str = string_builder_dump(sb, NULL);

  string_builder_clear(sb);
  string_builder_add_rack(sb, current_rack, ld, false);
  char *rack_str = string_builder_dump(sb, NULL);

  const char *dir_str = (dir == BOARD_HORIZONTAL_DIRECTION) ? "H" : "V";
  printf("  SCORE: rack=%s, pos=(%d,%d,%s), score=%d, leave=%s (lv=%.2f), eq=%.2f\n",
         rack_str, start_row, start_col, dir_str, score, leave_str,
         leave_value / 100.0, equity / 100.0);

  free(leave_str);
  free(rack_str);
  string_builder_destroy(sb);
#endif

  // Simplified draw count for now
  int draws = 1;

  // Add to rack hash table
  rack_hash_table_add_move(rht, &bit_rack, leave_value, draws, 1.0f,
                           &scored_move);
}
