// Research (magpie_test infhuman): how well inference predicts the tiles
// human players kept, over annotated games. Reads "id path" lines on stdin,
// one GCG each, all in the lexicon named by INFHUMAN_LEX. For every tile
// placement and exchange where both racks are known in full it runs one
// inference and, through the inference's rack hook, scores many variants at
// once: the probability each gives to the leave the mover really kept.
//
// The variants are temperatures without carry-over (fixed, and multiples of
// the bag-size schedule the decision harness uses), and temperatures with
// carry-over under several floors. Games with a phony are skipped.

#include "infer_human_test.h"

#include "../src/def/game_history_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/alias_method.h"
#include "../src/ent/bag.h"
#include "../src/ent/encoded_rack.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/game_history.h"
#include "../src/ent/inference_args.h"
#include "../src/ent/inference_carryover.h"
#include "../src/ent/inference_results.h"
#include "../src/ent/klv.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/validated_move.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/inference.h"
#include "../src/str/rack_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  HUMAN_FLOOR_NONE,
  HUMAN_FLOOR_MAX,
  HUMAN_FLOOR_PRIOR,
  HUMAN_MAX_PLAIN = 32,
  HUMAN_MAX_CARRY = 40,
  HUMAN_MAX_SUBSETS = 35,
  HUMAN_LINE = 2048,
};

typedef struct HumanTemperature {
  // A multiple of the schedule, or a fixed temperature.
  bool scheduled;
  double value;
} HumanTemperature;

typedef struct HumanFloor {
  int form;
  double value;
} HumanFloor;

typedef struct HumanState {
  const KLV *klv;
  int ld_size;
  const Rack *truth;
  double truth_loss;
  int num_plain;
  // Temperatures in equity units, for this move.
  double plain_temperature[HUMAN_MAX_PLAIN];
  double plain_total[HUMAN_MAX_PLAIN];
  double plain_truth[HUMAN_MAX_PLAIN];
  int num_carry;
  double carry_temperature[HUMAN_MAX_CARRY];
  double carry_floor[HUMAN_MAX_CARRY];
  bool carry_has_in[HUMAN_MAX_CARRY];
  InferenceCarryoverSlot *carry_in[HUMAN_MAX_CARRY];
  InferenceCarryoverSlot *carry_out[HUMAN_MAX_CARRY];
  double carry_total[HUMAN_MAX_CARRY];
  double carry_truth[HUMAN_MAX_CARRY];
  // Tiles the mover kept after their previous move, 0 with no table.
  int kept_in;
} HumanState;

static float human_slot_load(InferenceCarryoverSlot *slot) {
  const uint32_t bits = atomic_load(slot);
  float value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

static void human_slot_add(InferenceCarryoverSlot *slot, double amount) {
  const float value = human_slot_load(slot) + (float)amount;
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  atomic_store(slot, bits);
}

// The decision harness's schedule: temperature by tiles in the bag.
static double human_schedule_temperature(int bag_tiles) {
  static const int bags[] = {4, 10, 17, 25, 38, 53, 75};
  static const double temps[] = {12, 8, 5, 3, 3, 2, 1.5};
  enum { KNOTS = sizeof(bags) / sizeof(bags[0]) };
  if (bag_tiles <= bags[0]) {
    return temps[0];
  }
  for (int idx = 1; idx < KNOTS; idx++) {
    if (bag_tiles <= bags[idx]) {
      const double weight =
          (double)(bag_tiles - bags[idx - 1]) / (bags[idx] - bags[idx - 1]);
      return exp((1 - weight) * log(temps[idx - 1]) + weight * log(temps[idx]));
    }
  }
  return temps[KNOTS - 1];
}

// The klv indices of every way to choose kept_in of the rack's tiles as the
// leave the mover kept last turn. Returns the number of ways, with
// KLV_UNFOUND_INDEX for a leave the klv does not hold.
static int human_subset_indices(const HumanState *state, const Rack *rack,
                                uint32_t *indices) {
  const int total = rack_get_total_letters(rack);
  if (state->kept_in <= 0 || total > (RACK_SIZE) || total < state->kept_in) {
    return 0;
  }
  MachineLetter tiles[RACK_SIZE];
  int num_tiles = 0;
  for (int ml = 0; ml < state->ld_size; ml++) {
    for (int copy = 0; copy < rack_get_letter(rack, ml); copy++) {
      tiles[num_tiles++] = (MachineLetter)ml;
    }
  }
  Rack kept;
  int subsets = 0;
  for (int mask = 0; mask < (1 << num_tiles); mask++) {
    if (__builtin_popcount((unsigned int)mask) != state->kept_in) {
      continue;
    }
    rack_set_dist_size_and_reset(&kept, state->ld_size);
    for (int bit = 0; bit < num_tiles; bit++) {
      if ((mask >> bit) & 1) {
        rack_add_letter(&kept, tiles[bit]);
      }
    }
    indices[subsets++] = klv_get_word_index(state->klv, &kept);
  }
  return subsets;
}

static void human_rack_hook(void *data, const Rack *rack, const Rack *kept,
                            uint64_t draws, uint64_t kept_draws, double loss) {
  HumanState *state = (HumanState *)data;
  const bool is_truth = racks_are_equal(kept, state->truth);
  if (is_truth && loss < state->truth_loss) {
    state->truth_loss = loss;
  }
  for (int idx = 0; idx < state->num_plain; idx++) {
    const double weight =
        (double)draws * exp(-loss / state->plain_temperature[idx]);
    state->plain_total[idx] += weight;
    if (is_truth) {
      state->plain_truth[idx] += weight;
    }
  }
  uint32_t subset_indices[HUMAN_MAX_SUBSETS];
  const int subsets = human_subset_indices(state, rack, subset_indices);
  const int kept_tiles = rack_get_total_letters(kept);
  const uint32_t kept_index =
      kept_tiles > 0 && kept_tiles < (RACK_SIZE) && kept_draws > 0
          ? klv_get_word_index(state->klv, kept)
          : KLV_UNFOUND_INDEX;
  for (int idx = 0; idx < state->num_carry; idx++) {
    double factor = 1.0;
    if (state->carry_has_in[idx] && subsets > 0) {
      double sum = 0;
      for (int subset_idx = 0; subset_idx < subsets; subset_idx++) {
        if (subset_indices[subset_idx] != KLV_UNFOUND_INDEX) {
          sum += (double)human_slot_load(
              &state->carry_in[idx][subset_indices[subset_idx]]);
        }
      }
      factor = sum / subsets + state->carry_floor[idx];
    }
    const double weight = (double)draws *
                          exp(-loss / state->carry_temperature[idx]) * factor;
    state->carry_total[idx] += weight;
    if (is_truth) {
      state->carry_truth[idx] += weight;
    }
    if (kept_index != KLV_UNFOUND_INDEX) {
      human_slot_add(&state->carry_out[idx][kept_index],
                     weight / (double)kept_draws);
    }
  }
}

// The probability the engine's own results give the true leave, to check the
// hook's arithmetic against (INFHUMAN_DEBUG).
static double human_engine_probability(const InferenceResults *results,
                                       const Rack *truth) {
  const AliasMethod *am = inference_results_get_alias_method(results);
  Rack rack;
  rack_set_dist_size_and_reset(&rack, rack_get_dist_size(truth));
  double total = 0;
  for (uint32_t idx = 0; idx < am->num_items; idx++) {
    const AliasMethodItem *item = &am->items[idx];
    rack_decode(&item->rack, &rack);
    if (racks_are_equal(&rack, truth)) {
      total += item->probability / am->num_items;
    }
    rack_decode(&am->items[item->alias].rack, &rack);
    if (racks_are_equal(&rack, truth)) {
      total += (1 - item->probability) / am->num_items;
    }
  }
  return total;
}

static void human_clear_tables(InferenceCarryoverTable **tables,
                               int num_carry) {
  for (int idx = 0; idx < num_carry; idx++) {
    inference_carryover_table_destroy(tables[idx]);
    tables[idx] = NULL;
  }
}

static bool human_game_has_phony(const GameHistory *history) {
  const int num_events = game_history_get_num_events(history);
  for (int event_idx = 0; event_idx < num_events; event_idx++) {
    const GameEvent *event = game_history_get_event(history, event_idx);
    const game_event_t type = game_event_get_type(event);
    if (type == GAME_EVENT_PHONY_TILES_RETURNED) {
      return true;
    }
    if (type == GAME_EVENT_TILE_PLACEMENT_MOVE &&
        validated_moves_is_phony(game_event_get_vms(event), 0)) {
      return true;
    }
  }
  return false;
}

void infer_human_stdin(void) {
  static const double fixed[] = {100000, 0.5, 1, 1.5, 2,  3,  4, 5,
                                 6,      8,   10, 12, 16, 24, 32};
  static const double scales[] = {0.5, 0.75, 1, 1.5, 2, 3, 4};
  static const HumanTemperature carry_temps[] = {
      {true, 0.5}, {true, 1}, {true, 1.5}, {true, 2},
      {true, 3},   {false, 2}, {false, 4},  {false, 8}};
  static const HumanFloor floors[] = {{HUMAN_FLOOR_MAX, 1e-3},
                                      {HUMAN_FLOOR_PRIOR, 1e-3},
                                      {HUMAN_FLOOR_PRIOR, 1e-2},
                                      {HUMAN_FLOOR_NONE, 0}};
  enum {
    NUM_FIXED = sizeof(fixed) / sizeof(fixed[0]),
    NUM_SCALES = sizeof(scales) / sizeof(scales[0]),
    NUM_CARRY_TEMPS = sizeof(carry_temps) / sizeof(carry_temps[0]),
    NUM_FLOORS = sizeof(floors) / sizeof(floors[0]),
    NUM_PLAIN = NUM_FIXED + NUM_SCALES,
    NUM_CARRY = NUM_CARRY_TEMPS * NUM_FLOORS,
  };
  assert(NUM_PLAIN <= HUMAN_MAX_PLAIN);
  assert(NUM_CARRY <= HUMAN_MAX_CARRY);
  HumanTemperature plain_temps[HUMAN_MAX_PLAIN];
  for (int idx = 0; idx < NUM_FIXED; idx++) {
    plain_temps[idx] = (HumanTemperature){false, fixed[idx]};
  }
  for (int idx = 0; idx < NUM_SCALES; idx++) {
    plain_temps[NUM_FIXED + idx] = (HumanTemperature){true, scales[idx]};
  }

  const char *lexicon = getenv("INFHUMAN_LEX");
  assert(lexicon != NULL);
  char *settings = get_formatted_string(
      "set -lex %s -wmp true -rit false -wit false -s1 equity -s2 equity -r1 "
      "all -r2 all -pat auto -threads 1 -numplays 100 -savesettings false "
      "-autosavegcg false",
      lexicon);
  Config *config = config_create_or_die(settings);
  free(settings);
  ErrorStack *errors = error_stack_create();
  ThreadControl *control = config_get_thread_control(config);

  printf("HUMAN_SETTINGS lex=%s plain=", lexicon);
  for (int idx = 0; idx < NUM_PLAIN; idx++) {
    printf("%s%s%g", idx ? "," : "", plain_temps[idx].scheduled ? "s" : "f",
           plain_temps[idx].value);
  }
  printf(" carry=");
  for (int idx = 0; idx < NUM_CARRY; idx++) {
    const HumanTemperature temp = carry_temps[idx / NUM_FLOORS];
    const HumanFloor floor = floors[idx % NUM_FLOORS];
    printf("%s%s%g:%s%g", idx ? "," : "", temp.scheduled ? "s" : "f",
           temp.value,
           floor.form == HUMAN_FLOOR_MAX
               ? "max"
               : (floor.form == HUMAN_FLOOR_PRIOR ? "prior" : "none"),
           floor.value);
  }
  printf("\n");
  fflush(stdout);

  HumanState state;
  memset(&state, 0, sizeof(state));
  state.num_plain = NUM_PLAIN;
  state.num_carry = NUM_CARRY;
  uint32_t num_leaves = 0;
  // The table each variant recorded for each player's last move.
  InferenceCarryoverTable *tables[2][HUMAN_MAX_CARRY];
  memset(tables, 0, sizeof(tables));
  int kept_last[2] = {0, 0};
  int chain_length[2] = {0, 0};
  InferenceResults *inferences = inference_results_create(NULL);

  char line[HUMAN_LINE];
  while (fgets(line, sizeof(line), stdin)) {
    int game_id;
    char path[HUMAN_LINE];
    if (sscanf(line, "%d %2047s", &game_id, path) != 2) {
      log_fatal("infhuman: bad line: %s", line);
    }
    char *command = get_formatted_string("load %s", path);
    thread_control_set_status(control, THREAD_CONTROL_STATUS_STARTED);
    config_load_command(config, command, errors);
    if (error_stack_is_empty(errors)) {
      config_execute_command(config, errors);
    }
    free(command);
    if (!error_stack_is_empty(errors)) {
      error_stack_reset(errors);
      printf("HUMAN_SKIP id=%d reason=load\n", game_id);
      continue;
    }
    GameHistory *history = config_get_game_history(config);
    if (human_game_has_phony(history)) {
      printf("HUMAN_SKIP id=%d reason=phony\n", game_id);
      continue;
    }
    Game *game = game_duplicate(config_get_game(config));
    const LetterDistribution *ld = game_get_ld(game);
    state.ld_size = ld_get_size(ld);
    state.klv = player_get_klv(game_get_player(game, 0));
    if (num_leaves == 0) {
      num_leaves = klv_get_number_of_leaves(state.klv);
      for (int idx = 0; idx < NUM_CARRY; idx++) {
        state.carry_in[idx] =
            calloc_or_die(num_leaves, sizeof(InferenceCarryoverSlot));
        state.carry_out[idx] =
            calloc_or_die(num_leaves, sizeof(InferenceCarryoverSlot));
      }
    }
    for (int player_idx = 0; player_idx < 2; player_idx++) {
      human_clear_tables(tables[player_idx], NUM_CARRY);
      kept_last[player_idx] = 0;
      chain_length[player_idx] = 0;
    }
    int scored = 0;
    int unscored = 0;
    bool replay_failed = false;
    const int num_events = game_history_get_num_events(history);
    for (int event_idx = 0; event_idx < num_events && !replay_failed;
         event_idx++) {
      const GameEvent *event = game_history_get_event(history, event_idx);
      const game_event_t type = game_event_get_type(event);
      if (type != GAME_EVENT_TILE_PLACEMENT_MOVE &&
          type != GAME_EVENT_EXCHANGE) {
        continue;
      }
      const int seat = game_event_get_player_index(event);
      game_play_n_events(history, game, event_idx, false, errors);
      if (!error_stack_is_empty(errors)) {
        error_stack_reset(errors);
        replay_failed = true;
        continue;
      }
      const Move *move = validated_moves_get_move(game_event_get_vms(event), 0);
      const bool exchange = type == GAME_EVENT_EXCHANGE;
      const Rack *mover = player_get_rack(game_get_player(game, seat));
      // The replay leaves the other player's tiles in the bag. Their rack is
      // the one their next move records; it is drawn from the bag here so
      // that the bag holds only what neither player has.
      Rack *other = player_get_rack(game_get_player(game, 1 - seat));
      if (rack_is_empty(other)) {
        const Rack *next_rack = NULL;
        for (int next_idx = event_idx + 1;
             next_idx < num_events && next_rack == NULL; next_idx++) {
          const GameEvent *next = game_history_get_event(history, next_idx);
          if (game_event_get_player_index(next) == 1 - seat &&
              (game_event_get_type(next) == GAME_EVENT_TILE_PLACEMENT_MOVE ||
               game_event_get_type(next) == GAME_EVENT_EXCHANGE ||
               game_event_get_type(next) == GAME_EVENT_PASS)) {
            next_rack = game_event_get_const_rack(next);
          }
        }
        bool drawable = next_rack != NULL &&
                        rack_get_dist_size(next_rack) == state.ld_size &&
                        rack_get_total_letters(next_rack) == (RACK_SIZE);
        for (int ml = 0; drawable && ml < state.ld_size; ml++) {
          drawable = rack_get_letter(next_rack, ml) <=
                     bag_get_letter(game_get_bag(game), (MachineLetter)ml);
        }
        if (drawable) {
          draw_leave_from_bag(game_get_bag(game), 1 - seat, other, next_rack);
        }
      }
      const Rack *event_rack = game_event_get_const_rack(event);
      const int bag_tiles = bag_get_letters(game_get_bag(game));
      Rack used;
      rack_set_dist_size_and_reset(&used, state.ld_size);
      for (int idx = 0; idx < move_get_tiles_length(move); idx++) {
        const MachineLetter ml = move_get_tile(move, idx);
        // In an exchange every tile counts: a blank has the same value as
        // the marker for a square played through.
        if (exchange || ml != PLAYED_THROUGH_MARKER) {
          rack_add_letter(&used,
                          get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml);
        }
      }
      Rack truth;
      rack_copy(&truth, mover);
      rack_subtract_using_floor_zero(&truth, &used);
      const int tiles_used = rack_get_total_letters(&used);
      const bool racks_known =
          rack_get_total_letters(mover) == (RACK_SIZE) &&
          rack_get_dist_size(event_rack) == state.ld_size &&
          racks_are_equal(mover, event_rack) &&
          rack_get_total_letters(other) == (RACK_SIZE) &&
          rack_get_total_letters(&truth) == (RACK_SIZE)-tiles_used;
      if (getenv("INFHUMAN_DEBUG") != NULL) {
        printf("HUMAN_DEBUG id=%d ev=%d mover=%d event_rack=%d/%d other=%d "
               "truth=%d used=%d bag=%d\n",
               game_id, event_idx, rack_get_total_letters(mover),
               rack_get_dist_size(event_rack),
               rack_get_dist_size(event_rack) == state.ld_size
                   ? rack_get_total_letters(event_rack)
                   : -1,
               rack_get_total_letters(other), rack_get_total_letters(&truth),
               tiles_used, bag_tiles);
      }
      if (!racks_known || bag_tiles <= 0 || rack_is_empty(&truth)) {
        // A move that is not inferred breaks the mover's chain, as in play.
        human_clear_tables(tables[seat], NUM_CARRY);
        kept_last[seat] = 0;
        chain_length[seat] = 0;
        unscored++;
        continue;
      }

      const double schedule =
          exchange ? 3.0 : human_schedule_temperature(bag_tiles);
      for (int idx = 0; idx < NUM_PLAIN; idx++) {
        const double temperature = plain_temps[idx].scheduled
                                       ? plain_temps[idx].value * schedule
                                       : plain_temps[idx].value;
        state.plain_temperature[idx] = (double)double_to_equity(temperature);
        state.plain_total[idx] = 0;
        state.plain_truth[idx] = 0;
      }
      // What the mover can hold, as the other player sees it.
      Rack pool;
      rack_copy(&pool, mover);
      for (int ml = 0; ml < state.ld_size; ml++) {
        rack_add_letters(&pool, ml, bag_get_letter(game_get_bag(game), ml));
      }
      state.kept_in = kept_last[seat];
      for (int idx = 0; idx < NUM_CARRY; idx++) {
        const HumanTemperature temp = carry_temps[idx / NUM_FLOORS];
        const HumanFloor floor = floors[idx % NUM_FLOORS];
        state.carry_temperature[idx] = (double)double_to_equity(
            temp.scheduled ? temp.value * schedule : temp.value);
        state.carry_total[idx] = 0;
        state.carry_truth[idx] = 0;
        const InferenceCarryoverTable *table_in = tables[seat][idx];
        state.carry_has_in[idx] = table_in != NULL;
        state.carry_floor[idx] = 0;
        if (table_in != NULL) {
          inference_carryover_table_fill_slots(table_in, state.carry_in[idx]);
          if (floor.form == HUMAN_FLOOR_MAX) {
            state.carry_floor[idx] =
                floor.value * (double)inference_carryover_table_get_max_within(
                                  table_in, &pool);
          } else if (floor.form == HUMAN_FLOOR_PRIOR) {
            state.carry_floor[idx] =
                floor.value / (1.0 - floor.value) *
                inference_carryover_table_get_mean_within(table_in, &pool);
          }
        }
        memset(state.carry_out[idx], 0,
               (size_t)num_leaves * sizeof(InferenceCarryoverSlot));
      }
      state.truth = &truth;
      state.truth_loss = INFINITY;

      Rack played;
      rack_set_dist_size_and_reset(&played, state.ld_size);
      if (!exchange) {
        rack_copy(&played, &used);
      }
      Rack known;
      rack_set_dist_size_and_reset(&known, state.ld_size);
      Rack observer;
      rack_copy(&observer, other);
      InferenceArgs args;
      infer_args_fill(&args, 0, int_to_equity(10000), NULL, game, 1, 0, 0,
                      control, false, true, seat, move_get_score(move),
                      exchange ? move_get_tiles_played(move) : 0, &played,
                      &known, &observer);
      args.temperature = double_to_equity(1.0);
      // The check: the engine itself runs the carry-over variant at a fixed
      // temperature of 2 with the max floor, reading that variant's table.
      enum { CHECK_VARIANT = 5 * NUM_FLOORS };
      const bool check = getenv("INFHUMAN_DEBUG") != NULL;
      if (check) {
        args.temperature = double_to_equity(2.0);
        if (state.carry_has_in[CHECK_VARIANT]) {
          args.carry_in = state.carry_in[CHECK_VARIANT];
          args.carry_in_kept = state.kept_in;
          args.carry_in_floor = state.carry_floor[CHECK_VARIANT];
        }
      }
      args.target_move = exchange ? NULL : move;
      args.rack_hook = human_rack_hook;
      args.rack_hook_data = &state;
      thread_control_set_status(control, THREAD_CONTROL_STATUS_STARTED);
      infer_without_ctx(&args, inferences, errors);
      if (!error_stack_is_empty(errors)) {
        error_stack_reset(errors);
        human_clear_tables(tables[seat], NUM_CARRY);
        kept_last[seat] = 0;
        chain_length[seat] = 0;
        unscored++;
        printf("HUMAN_FAIL id=%d ev=%d\n", game_id, event_idx);
        continue;
      }

      if (check) {
        printf("HUMAN_CHECK id=%d ev=%d hist=%d engine=%.9g hook=%.9g\n",
               game_id, event_idx, chain_length[seat],
               human_engine_probability(inferences, &truth),
               state.carry_total[CHECK_VARIANT] > 0
                   ? state.carry_truth[CHECK_VARIANT] /
                         state.carry_total[CHECK_VARIANT]
                   : 0.0);
      }
      printf("HUMAN id=%d ev=%d seat=%d bag=%d tiles=%d exch=%d leave=%d "
             "hist=%d kept=%d loss=%.3f plain=",
             game_id, event_idx, seat, bag_tiles, tiles_used, exchange,
             rack_get_total_letters(&truth), chain_length[seat],
             kept_last[seat],
             isinf(state.truth_loss)
                 ? -1.0
                 : state.truth_loss / (double)double_to_equity(1.0));
      for (int idx = 0; idx < NUM_PLAIN; idx++) {
        printf("%s%.6g", idx ? "," : "",
               state.plain_total[idx] > 0
                   ? state.plain_truth[idx] / state.plain_total[idx]
                   : 0.0);
      }
      printf(" carry=");
      const int kept_now = (RACK_SIZE)-tiles_used;
      bool any_table = false;
      for (int idx = 0; idx < NUM_CARRY; idx++) {
        printf("%s%.6g", idx ? "," : "",
               state.carry_total[idx] > 0
                   ? state.carry_truth[idx] / state.carry_total[idx]
                   : 0.0);
        inference_carryover_table_destroy(tables[seat][idx]);
        tables[seat][idx] = inference_carryover_table_create_from_slots(
            state.carry_out[idx], state.klv, kept_now);
        any_table = any_table || tables[seat][idx] != NULL;
      }
      printf("\n");
      fflush(stdout);
      kept_last[seat] = any_table ? kept_now : 0;
      chain_length[seat] = any_table ? chain_length[seat] + 1 : 0;
      scored++;
    }
    printf("HUMAN_GAME id=%d scored=%d unscored=%d replay_failed=%d\n",
           game_id, scored, unscored, replay_failed);
    fflush(stdout);
    game_destroy(game);
  }
  for (int player_idx = 0; player_idx < 2; player_idx++) {
    human_clear_tables(tables[player_idx], NUM_CARRY);
  }
  for (int idx = 0; idx < NUM_CARRY; idx++) {
    free(state.carry_in[idx]);
    free(state.carry_out[idx]);
  }
  inference_results_destroy(inferences);
  error_stack_destroy(errors);
  config_destroy(config);
}

// Research (magpie_test infdump): the posterior over the tiles the opponent
// kept, at one position of a game. INFDUMP_LEX, INFDUMP_GCG, INFDUMP_PLAYED
// (events played) and INFDUMP_TEMP name it; INFDUMP_CARRY=false turns
// carry-over off. Prints one line per sampler
// entry: the leave, its share of the probability and its leave value. A
// leave can appear on several lines; the reader sums them.
void infer_dump_leaves(void) {
  const char *lexicon = getenv("INFDUMP_LEX");
  const char *gcg = getenv("INFDUMP_GCG");
  const char *played = getenv("INFDUMP_PLAYED");
  const char *temperature = getenv("INFDUMP_TEMP");
  assert(lexicon != NULL && gcg != NULL && played != NULL &&
         temperature != NULL);
  char *settings = get_formatted_string(
      "set -lex %s -wmp true -rit false -wit false -s1 equity -s2 equity -r1 "
      "all -r2 all -pat auto -threads 4 -numplays 15 -inftemp %s -ima 10000 "
      "-infcarryover %s -savesettings false -autosavegcg false",
      lexicon, temperature,
      getenv("INFDUMP_CARRY") != NULL ? getenv("INFDUMP_CARRY") : "true");
  Config *config = config_create_or_die(settings);
  free(settings);
  char *command = get_formatted_string("load %s", gcg);
  load_and_exec_config_or_die(config, command);
  free(command);
  command = get_formatted_string("goto %s", played);
  load_and_exec_config_or_die(config, command);
  free(command);
  load_and_exec_config_or_die(config, "infer");
  const Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  const KLV *klv = player_get_klv(game_get_player(game, 0));
  const AliasMethod *am = inference_results_get_alias_method(
      config_get_inference_results(config));
  Rack rack;
  rack_set_dist_size_and_reset(&rack, ld_get_size(ld));
  for (uint32_t idx = 0; idx < am->num_items; idx++) {
    const AliasMethodItem *item = &am->items[idx];
    for (int part = 0; part < 2; part++) {
      rack_decode(part == 0 ? &item->rack : &am->items[item->alias].rack,
                  &rack);
      const double share =
          (part == 0 ? item->probability : 1 - item->probability) /
          am->num_items;
      if (share <= 0) {
        continue;
      }
      StringBuilder *sb = string_builder_create();
      string_builder_add_rack(sb, &rack, ld, false);
      printf("LEAVE %s %.10g %.3f\n", string_builder_peek(sb), share,
             equity_to_double(klv_get_leave_value(klv, &rack)));
      string_builder_destroy(sb);
    }
  }
  config_destroy(config);
}

