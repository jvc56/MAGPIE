#ifndef PLAY_CHOOSER_H
#define PLAY_CHOOSER_H

#include "../ent/game.h"
#include "../ent/game_timer.h"
#include "../ent/move.h"
#include "../ent/win_pct.h"
#include "../util/io_util.h"
#include <stdbool.h>
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
} play_chooser_eval_t;

// Describes how a computer player delegates its decisions. Examples:
//   static always:          pre_endgame_eval=STATIC, endgame_eval=STATIC
//   static until endgame:   pre_endgame_eval=STATIC, endgame_eval=ENDGAME
//   sim with endgame solve: pre_endgame_eval=SIM,    endgame_eval=ENDGAME
typedef struct PlayChooserStrategy {
  // Evaluation used while the bag still has tiles: STATIC or SIM.
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
  uint64_t seed;
} PlayChooserStrategy;

typedef struct PlayChooser PlayChooser;

typedef struct ChallengeDecision {
  // True if the move forms at least one word that is invalid in the
  // chooser's lexicon. The chooser never advises challenging valid plays.
  bool move_is_phony;
  bool should_challenge;
  // Diagnostic branch values from the chooser's perspective. Units depend
  // on the evaluation mode (final spread points for STATIC/ENDGAME, win
  // fraction for SIM) but are always comparable to each other within a
  // single decision.
  double keep_value;
  double challenge_value;
} ChallengeDecision;

PlayChooser *play_chooser_create(const PlayChooserStrategy *strategy);
void play_chooser_destroy(PlayChooser *play_chooser);

// Choose a move for the player on turn in game, delegating to static
// eval, sim, or the endgame solver per the strategy. Any tiles known to
// be on the opponent's rack (via player_get_known_rack_from_phonies) are
// passed along to the simulation.
void play_chooser_choose_move(PlayChooser *play_chooser, Game *game,
                              Move *out_move, ErrorStack *error_stack);

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
