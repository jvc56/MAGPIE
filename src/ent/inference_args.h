#ifndef INFERENCE_ARGS_H
#define INFERENCE_ARGS_H

#include "../def/inference_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../ent/inference_results.h"
#include "../ent/move.h"
#include "../ent/rack.h"
#include "../ent/thread_control.h"
#include "../util/io_util.h"
#include <stdatomic.h>
#include <stdint.h>

// Research (infdecide, inftfit): one slot per KLV leave, holding a float's
// bits, so that inference workers can add to a slot without a lock.
typedef _Atomic uint32_t InferenceCarrySlot;

// Research (sim-steered weights): a fitted replacement for a rack's static
// loss, as a function of that loss and of the tiles kept. A rack whose static
// loss is above clip keeps it. Read-only during an inference.
typedef struct InferenceLossModel {
  double intercept;
  double slope;
  double clip;
  double letter[MAX_ALPHABET_SIZE];
  // One term for each distinct unordered pair of kept tiles; pair[a][b] with
  // a <= b, and a == b when the leave has two or more of that tile.
  double pair[MAX_ALPHABET_SIZE][MAX_ALPHABET_SIZE];
} InferenceLossModel;

typedef struct InferenceArgs {
  bool use_game_history;
  bool use_inference_cutoff_optimization;
  // Value the racks with the board's census play scores, built once, instead
  // of a move generation for each rack, where that gives the same values (see
  // inference_can_use_census in inference.c). The racks inferred are the same
  // either way.
  bool use_census_play_scores;
  GameHistory *game_history;
  int target_index;
  Equity target_score;
  int target_num_exch;
  int leave_list_capacity;
  Equity equity_margin;
  Equity temperature;
  // Test-only oracle (infdecide): when oracle_feature is nonzero the leave
  // weights ignore the observed move and are the draw counts of the leaves
  // that match oracle_leave on a feature. 1: same vowel count. 2: leave
  // value within oracle_tolerance. 3: both.
  int oracle_feature;
  Equity oracle_tolerance;
  const Rack *oracle_leave;
  // Test-only (infbench, infdecide): weight each leave by the ways to draw
  // the full rack, played tiles included, instead of the ways to draw the
  // leave from the tiles unseen after the play.
  bool full_rack_prior;
  // Research: carry-over between a player's turns. carry_in holds, for each
  // leave the target might have kept on their previous turn (carry_in_kept
  // tiles, indexed by klv_get_word_index), that leave's posterior weight
  // over its prior. Each rack inferred now is weighted by the mean of
  // carry_in over the ways to choose which of its tiles were kept then, plus
  // carry_in_floor. carry_out, when set, receives the same quantity for the
  // leaves kept on this turn (the caller zeroes it). Softmax only.
  InferenceCarrySlot *carry_in;
  int carry_in_kept;
  double carry_in_floor;
  InferenceCarrySlot *carry_out;
  // Research: softmax placements only. NULL for the static loss.
  const InferenceLossModel *loss_model;
  const Move *target_move;
  Rack *target_played_tiles;
  Rack *target_known_rack;
  Rack *nontarget_known_rack;
  const Game *game;
  int num_threads;
  int parent_worker_thread_index;
  int print_interval;
  ThreadControl *thread_control;
} InferenceArgs;

static inline void
infer_args_fill(InferenceArgs *args, int leave_list_capacity, Equity eq_margin,
                GameHistory *game_history, const Game *game, int num_threads,
                int parent_worker_thread_index, int print_interval,
                ThreadControl *thread_control, bool use_game_history,
                bool use_inference_cutoff_optimization, int target_index,
                Equity target_score, int target_num_exch,
                Rack *target_played_tiles, Rack *target_known_rack,
                Rack *nontarget_known_rack) {
  args->target_index = target_index;
  args->target_score = target_score;
  args->target_num_exch = target_num_exch;
  args->leave_list_capacity = leave_list_capacity;
  args->equity_margin = eq_margin;
  args->temperature = 0;
  args->oracle_feature = 0;
  args->oracle_tolerance = 0;
  args->oracle_leave = NULL;
  args->full_rack_prior = false;
  args->carry_in = NULL;
  args->carry_in_kept = 0;
  args->carry_in_floor = 0;
  args->carry_out = NULL;
  args->loss_model = NULL;
  args->target_move = NULL;
  args->target_played_tiles = target_played_tiles;
  args->target_known_rack = target_known_rack;
  args->nontarget_known_rack = nontarget_known_rack;
  args->use_game_history = use_game_history;
  args->use_inference_cutoff_optimization = use_inference_cutoff_optimization;
  args->use_census_play_scores = true;
  args->game_history = game_history;
  args->game = game;
  args->num_threads = num_threads;
  args->parent_worker_thread_index = parent_worker_thread_index;
  args->print_interval = print_interval;
  args->thread_control = thread_control;
}

#endif