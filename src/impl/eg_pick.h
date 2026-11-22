#ifndef EG_PICK_H
#define EG_PICK_H

// LAB ONLY (egregret), never for a PR. An empty-bag move pick that replaces the
// greedy pick of the endgame solver's leaf playouts and PEG's greedy playouts:
// static's top N candidates re-scored by "check + opponent's best score + split
// leaves" (scripts/egregret/HANDOFF_M2.md). Off unless MAGPIE_EGPICK_N > 0.
//
// Environment:
//   MAGPIE_EGPICK_N        candidates re-scored (0 or unset: off, stock code)
//   MAGPIE_EGPICK_MODE     full (default) | noobs (no obs term) | rule
//                          (v = eq - obo, no weights) | static (v = eq) |
//                          staticbest (v = eq, one best-move generation;
//                          N is ignored)
//   MAGPIE_EGPICK_WEIGHTS  weights file (default
//                          scripts/egregret/weights/check_best_split_l3.tsv)

#include "../ent/game.h"
#include "../ent/kwg.h"
#include "../ent/move.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct EgPick EgPick;

typedef struct EgPickStats {
  int64_t picks;
  int64_t candidates;
  int64_t reply_movegens;
  int64_t nanoseconds;
} EgPickStats;

bool eg_pick_enabled(void);

// While suppressed, eg_pick_create returns NULL, so solver workers created
// meanwhile run stock (for reference solves in the same process).
void eg_pick_set_suppressed(bool suppressed);

// Per-thread scratch. Returns NULL when the switch is off.
EgPick *eg_pick_create(const Game *game);
EgPick *eg_pick_create_own(const Game *game, int num_candidates);
void eg_pick_destroy(EgPick *pick);

// The move for the side on turn in `game`, whose bag is empty. `game` is used
// for move generation only and is left as it was. Returns false when the side
// has no move at all (not even a pass), leaving `out` untouched.
bool eg_pick_choose(EgPick *pick, Game *game, const KWG *override_kwg,
                    Move *out);

// Whether the pick is by static equity alone (mode staticbest), which the
// endgame's own best-play generation can rank by (see
// MoveGenArgs.best_small_face_weight).
bool eg_pick_is_static_best(const EgPick *pick);

// Totals over every pick since the last reset, across threads.
void eg_pick_stats_get(EgPickStats *stats);
void eg_pick_stats_reset(void);

#endif
