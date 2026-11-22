#ifndef INFERENCE_ARGS_H
#define INFERENCE_ARGS_H

#include "../def/inference_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../ent/inference_carryover.h"
#include "../ent/inference_results.h"
#include "../ent/move.h"
#include "../ent/rack.h"
#include "../ent/thread_control.h"
#include "../util/io_util.h"
#include <stdatomic.h>
#include <stdint.h>

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
  InferenceCarryoverSlot *carry_in;
  int carry_in_kept;
  double carry_in_floor;
  InferenceCarryoverSlot *carry_out;
  // Research: softmax placements only. NULL for the static loss.
  const InferenceLossModel *loss_model;
  // Carry-over in play (softmax only; ignored while carry_in or carry_out is
  // set by hand). The inference weights each rack by the table recorded for
  // the target's previous move and records a table for this one. With
  // use_game_history the tables are kept on the game history's events
  // (game_history_get_inference_carryover_in); otherwise in carry_chain, which
  // the caller resets at the start of a game and whenever a move of the
  // target's other than a pass is not inferred. With carry_dump_dir set, the
  // table recorded is also written there for postmortems, as
  // carry_<tag>_p<target>.tsv: the tag is carry_dump_tag, or e<event index>
  // when that is empty and the tables are on the game history.
  // With the tables on the game history, the inference first fills in any
  // earlier move of the target's that has no table yet, back to the target's
  // last full draw, so that jumping into the middle of a game gives the same
  // tables as playing through it. history_event_index is the event to infer
  // for such a fill; negative means the most recent move played.
  bool use_carry_over;
  int history_event_index;
  InferenceCarryoverChain *carry_chain;
  const char *carry_dump_dir;
  char carry_dump_tag[48];
  // Research: with softmax, called for every rack the inference weighs, with
  // the tiles it would keep, the ways to draw the rack and the kept tiles
  // from the unseen pool, and the equity the observed move loses on it. For
  // scoring many temperatures and carry-over forms from one inference
  // (magpie_test infhuman). Called from the worker threads.
  void (*rack_hook)(void *data, const Rack *rack, const Rack *kept,
                    uint64_t draws, uint64_t kept_draws, double loss);
  void *rack_hook_data;
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
  args->use_carry_over = false;
  args->history_event_index = -1;
  args->rack_hook = NULL;
  args->rack_hook_data = NULL;
  args->carry_chain = NULL;
  args->carry_dump_dir = NULL;
  args->carry_dump_tag[0] = '\0';
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