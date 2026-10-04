#ifndef VALUE_NET_INFERENCE_H
#define VALUE_NET_INFERENCE_H

#include "../ent/alias_method.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/rack.h"
#include "../ent/sim_args.h"
#include "../ent/value_net_history.h"
#include <stdbool.h>

// Infers the leave an opponent kept from their last move with a value net as
// their policy. For every multiset of leave_size tiles that could have been
// on their rack besides the tiles the move played (from the tiles unseen to
// the inferring player), their rack is rebuilt on the position before the
// move, its top candidates by static equity are generated (with the
// player's PAT term unless disabled), the move they made is added when it
// is not among them, and the net scores each candidate by MAGPIE's utility.
// The leave's weight is the number of ways to draw it times the probability
// that a player choosing by softmax(utility / temperature) would have made
// that move. Meant for small leaves: 1 or 2 tiles is a few hundred racks.
typedef struct ValueNetLeaveInference ValueNetLeaveInference;

typedef struct ValueNetLeaveInferenceArgs {
  // The position before the move, with the target (who made it) on turn
  // holding a rack of the right size (its contents are not read beyond
  // their count).
  const Game *before_move;
  int target_index;
  const Move *move;
  // The target's value net history at that decision.
  const ValueNetHistory *target_history;
  // The inferring player's rack (known to them, not in the pool).
  const Rack *nontarget_rack;
  int leave_size;
  // Candidates per rack, by static equity; at most VALUE_NET_INFERENCE_MAX_
  // CANDIDATES.
  int candidates;
  double temperature;
  double utility_w_winpct;
  double utility_w_spread;
  double utility_spread_scale;
  bool use_pat;
  value_net_rows_fn evaluate;
  void *evaluate_context;
} ValueNetLeaveInferenceArgs;

enum { VALUE_NET_INFERENCE_MAX_CANDIDATES = 64 };

ValueNetLeaveInference *value_net_leave_inference_create(void);
void value_net_leave_inference_destroy(ValueNetLeaveInference *inference);

// Runs the inference. Returns false when there is nothing to infer from (no
// leave of that size, or the move is not a tile placement); otherwise the
// leaves and their probabilities are available until the next run.
bool value_net_leave_inference_run(ValueNetLeaveInference *inference,
                                   const ValueNetLeaveInferenceArgs *args);

// Recomputes the probabilities of the last run's leaves for another
// temperature, without evaluating again.
void value_net_leave_inference_set_temperature(
    ValueNetLeaveInference *inference, double temperature);

int value_net_leave_inference_get_count(
    const ValueNetLeaveInference *inference);
const Rack *
value_net_leave_inference_get_leave(const ValueNetLeaveInference *inference,
                                    int index);
double value_net_leave_inference_get_probability(
    const ValueNetLeaveInference *inference, int index);
// The probability of leave (0 if it was not a candidate).
double value_net_leave_inference_probability_of(
    const ValueNetLeaveInference *inference, const Rack *leave);
// Rows the net evaluated in the last run.
int value_net_leave_inference_get_rows(const ValueNetLeaveInference *inference);

// Fills alias_method (reset first) with the leaves weighted by their
// probabilities (as integer counts) and generates its tables. Returns false
// when there are none.
bool value_net_leave_inference_fill_alias(
    const ValueNetLeaveInference *inference, AliasMethod *alias_method);

#endif
