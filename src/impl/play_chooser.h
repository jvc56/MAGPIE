#ifndef PLAY_CHOOSER_H
#define PLAY_CHOOSER_H

#include "../ent/game.h"
#include "../ent/game_timer.h"
#include "../ent/move.h"
#include "../ent/win_pct.h"
#include "../util/io_util.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// How the play chooser evaluates a position when deciding on a play (or
// when valuing the keep/challenge branches of a challenge decision).
typedef enum {
  // Best static-equity move from move generation. Effectively instant.
  PLAY_CHOOSER_EVAL_STATIC,
  // Monte Carlo simulation over the top static candidates. Requires
  // win_pcts. Respects the per-move time budget.
  PLAY_CHOOSER_EVAL_SIM,
  // Endgame solver. Only valid once the bag is empty. Respects the
  // per-move time budget.
  PLAY_CHOOSER_EVAL_ENDGAME,
  // Pre-endgame solver: enumerates the bag's draw scenarios and solves the
  // resulting (near-)endgames, ranking candidates by win probability. Valid
  // only while the bag holds [PEG_MIN_BAG, PEG_MAX_BAG] tiles; for larger bags
  // the chooser falls back to SIM (when win_pcts are set) or STATIC. Values a
  // position by the score+win utility in [0, 1] (see utility_w_spread below),
  // so keep/challenge branches are directly comparable. Respects the
  // per-move / per-decision time budget.
  PLAY_CHOOSER_EVAL_PEG,
} play_chooser_eval_t;

// Describes how a computer player delegates its decisions. Examples:
//   static always:          pre_endgame_eval=STATIC, endgame_eval=STATIC
//   static until endgame:   pre_endgame_eval=STATIC, endgame_eval=ENDGAME
//   sim with endgame solve: pre_endgame_eval=SIM,    endgame_eval=ENDGAME
//   peg into endgame:       pre_endgame_eval=PEG,    endgame_eval=ENDGAME
typedef struct PlayChooserStrategy {
  // Evaluation used while the bag still has tiles: STATIC, SIM, or PEG. PEG
  // only runs in the low-bag pre-endgame ([PEG_MIN_BAG, PEG_MAX_BAG]); above
  // that it falls back to SIM (if win_pcts set) or STATIC.
  play_chooser_eval_t pre_endgame_eval;
  // Evaluation used once the bag is empty: STATIC or ENDGAME.
  play_chooser_eval_t endgame_eval;
  int sim_plies;          // 0 = default
  int sim_max_candidates; // 0 = default
  // Maximum endgame solve depth in plies; 0 = solve as deep as the time
  // budget allows.
  int endgame_plies;
  // Per-move time budget in seconds. If > 0, a flat budget is used.
  // Otherwise, if game_timer is set and the game is timed, the budget is
  // the player's remaining clock split across an estimate of their
  // remaining plays. Otherwise a default flat budget is used. STATIC
  // evaluation ignores the budget.
  double fixed_seconds_per_move;
  GameTimer *game_timer; // not owned; may be NULL
  // Length of an overtime penalty period. Once the main clock expires,
  // PlayChooser may spend time already covered by the current started period,
  // while reserving enough time to avoid starting the next one. Zero disables
  // overtime search and falls back to static play after flag fall.
  double overtime_period_seconds;
  // Whether play_chooser_decide_challenge considers challenging phonies
  // at all. When false it always advises against challenging.
  bool enable_challenges;
  // Time limit in seconds for the entire challenge/no-challenge decision.
  // This lets the chooser commit to a challenge decision well before it
  // knows what its own play will be post-challenge. With the endgame
  // solver the keep and challenge branches are solved concurrently
  // against a shared transposition table, each using the whole window;
  // other evaluations split the window sequentially. 0 = default.
  //
  // The shared transposition table persists into the next
  // play_chooser_choose_move call, so whichever branch the verdict
  // selects, the preliminary search seeds the move-choosing solve (the
  // post-verdict position is exactly that branch's root).
  double challenge_decision_seconds;
  WinPct *win_pcts; // required for SIM; not owned
  int num_threads;  // 0 = 1
  // PEG scenario stride: 1 = full enumeration, k > 1 = weight-stratified
  // sampling (faster, approximate), 0 = the solver's per-bag default. Only
  // used by PLAY_CHOOSER_EVAL_PEG.
  int peg_scenario_stride;
  // Score+win utility for valuing a branch, identical to the simmer's
  // sim_utility_blend (see sim_args.h): the branch value is
  //   (w_winpct * win% + w_spread * sigmoid(spread / spread_scale))
  //   / (w_winpct + w_spread),
  // bounded in [0, 1]. win% is the branch's win probability (1 / 0 / 0.5 for a
  // decided game) and spread is its mean final spread, so equal-win% branches
  // are separated by margin, with diminishing returns. Used by
  // PLAY_CHOOSER_EVAL_PEG (its pre-endgame, endgame, and game-over branches).
  // Zero/unset defaults match the simmer: w_winpct 1.0, w_spread 0.0 (pure
  // win%), spread_scale 100.0.
  double utility_w_winpct;
  double utility_w_spread;
  double utility_spread_scale;
  uint64_t seed;
} PlayChooserStrategy;

typedef struct PlayChooser PlayChooser;

// Aggregate work completed by PlayChooser while benchmark collection is
// enabled. Timed searches should be compared by this work, not by wall time:
// their wall time is intentionally bounded by the same clock.
typedef struct PlayChooserBenchmarkStats {
  uint64_t static_moves;
  uint64_t fallback_moves;
  uint64_t sim_calls;
  uint64_t sim_iterations;
  uint64_t sim_nodes;
  uint64_t peg_calls;
  uint64_t peg_candidate_completions;
  uint64_t peg_candidate_events_dropped;
  uint64_t peg_completed_stages;
  uint64_t peg_final_candidates;
  uint64_t peg_final_scenarios;
  uint64_t peg_partial_calls;
  uint64_t endgame_calls;
  uint64_t endgame_nodes;
  uint64_t endgame_depth;
} PlayChooserBenchmarkStats;

// One completed PEG candidate. A call index identifies the PEG solve and the
// elapsed time is measured from the start of that solve. Stage indices and
// candidate ranks are zero-based. Events can arrive concurrently and are kept
// in callback order; elapsed_ns provides the actual within-call completion
// timeline, including useful work completed before a time limit interrupts a
// stage.
typedef struct PlayChooserPegCandidateEvent {
  uint64_t call_index;
  uint64_t elapsed_ns;
  int stage_index;
  int candidate_rank;
  int scenarios_completed;
} PlayChooserPegCandidateEvent;

typedef struct ChallengeDecision {
  // True if the move forms at least one word that is invalid in the
  // chooser's lexicon. The chooser never advises challenging valid plays.
  bool move_is_phony;
  bool should_challenge;
  // Diagnostic branch values from the chooser's perspective, always comparable
  // to each other within a single decision. When the two branches are in mixed
  // stages they are valued by each stage's method and projected onto the
  // score+win utility in [0, 1]. Two exceptions report final-spread points
  // instead: a STATIC decision (the fallback with no win model), and a
  // both-endgame decision (whose exact-spread verdict equals the utility
  // verdict, since the utility is monotonic in spread).
  double keep_value;
  double challenge_value;
} ChallengeDecision;

PlayChooser *play_chooser_create(const PlayChooserStrategy *strategy);
void play_chooser_destroy(PlayChooser *play_chooser);

// Enable/reset, snapshot, and disable the process-wide benchmark counters.
// Counters are atomic because autoplay may run chooser games concurrently.
// Collection is disabled by default and has only one predictable branch per
// chooser operation when unused.
void play_chooser_benchmark_reset(void);
void play_chooser_benchmark_get(PlayChooserBenchmarkStats *stats);
// Copies up to capacity PEG candidate events into events and returns the
// number copied. With a NULL events pointer or zero capacity, returns the
// number currently retained without copying. The stats snapshot reports any
// events dropped because the fixed process-wide event buffer filled.
size_t play_chooser_benchmark_get_peg_candidate_events(
    PlayChooserPegCandidateEvent *events, size_t capacity);
void play_chooser_benchmark_stop(void);

// Choose a move for the player on turn in game, delegating to static
// eval, sim, or the endgame solver per the strategy. Any tiles known to
// be on the opponent's rack (via player_get_known_rack_from_phonies) are
// passed along to the simulation.
void play_chooser_choose_move(PlayChooser *play_chooser, Game *game,
                              Move *out_move, ErrorStack *error_stack);

// Returns the current search budget for the player on turn. Zero means there
// is not enough safely spendable clock for a non-static search.
double play_chooser_get_seconds_for_move(const PlayChooserStrategy *strategy,
                                         const Game *game);

// Decide whether opp_move, announced by the player on turn in
// game_before_move, should be challenged off. game_before_move must be
// the position before opp_move is played. The decision is made within
// strategy->challenge_decision_seconds by comparing the value of the
// position with the play kept on the board against the value with the
// play challenged off — without choosing the chooser's own follow-up
// play. A low-scoring phony that opens up a large play (for example a
// triple-triple) or a favorable endgame sequence for the chooser will be
// left on the board.
void play_chooser_decide_challenge(PlayChooser *play_chooser,
                                   const Game *game_before_move,
                                   const Move *opp_move,
                                   ChallengeDecision *decision,
                                   ErrorStack *error_stack);

// Remove a successfully challenged move from the board and record the
// returned letters in the offending player's known rack from phonies (the
// full rack becomes known when the bag is empty). The challenged move
// must be the last move played on game, played with
// play_move_without_drawing_tiles under BACKUP_MODE_GCG.
void play_chooser_challenge_off(Game *game, const Move *challenged_move);

#endif
