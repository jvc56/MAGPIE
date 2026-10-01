#ifndef BLOCKING_SETUP_H
#define BLOCKING_SETUP_H

#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/rack.h"
#include <stdbool.h>
#include <stdint.h>

// Pass-relative blocking and setup checks for root candidates.
//
// Both checks compare a candidate with passing on the same sampled opponent
// racks, so a board that is simply open (or closed) moves every candidate's
// value together and only the candidate's own effect remains:
//
//   blocking_delta = mean over racks of
//       (opponent's best placement score after our pass
//        - opponent's best placement score after the candidate)
//
//   setup_delta = mean over racks and follow-up draws of
//       (our best placement score after the candidate and the opponent's
//        reply - our best placement score after our pass and the opponent's
//        reply to it)
//
// The setup follow-up rack is the candidate's leave plus the same refill
// tiles in both branches, so the difference isolates board opportunity: the
// pass branch does not keep our original rack. Replies and follow-ups are
// the best tile placement by score (0 when there is none; exchanges are not
// considered). A follow-up after an opponent reply that ends the game counts
// 0, in whichever branch it happens.
//
// Everything is in points (doubles). Equity is millipoints; callers that add
// an adjustment to an equity convert with double_to_equity.
//
// The checks use only what the player on turn can see: the board, their own
// rack, the bag size and the unseen pool (the bag plus the opponent's rack,
// as one multiset). The opponent's actual rack and the bag order are never
// read: sampled racks replace the opponent's rack in scratch copies, and
// follow-up racks are dealt from the samples.

enum {
  // Bounds on BlockingSetupSamples.num_racks.
  BLOCKING_SETUP_MIN_RACKS = 1,
  BLOCKING_SETUP_MAX_RACKS = 1024,
  BLOCKING_SETUP_MAX_FOLLOWUP_DRAWS = 16,
};

// The sampled opponent racks and follow-up draw orders, shared by every
// candidate of one position so that candidate differences are paired.
typedef struct BlockingSetupSamples {
  int num_racks;
  // Unseen tiles from the on-turn player's view when the samples were dealt.
  int pool_size;
  // num_racks racks.
  Rack *opponent_racks;
  // Row rack_idx (at rack_idx * pool_capacity) holds draw_sizes[rack_idx]
  // tiles in draw order: the unseen pool less that rack when draws are
  // conditioned on the rack, else the whole pool.
  MachineLetter *draw_orders;
  int *draw_sizes;
  int rack_capacity;
  int pool_capacity;
} BlockingSetupSamples;

// Capacity for up to max_racks racks over a pool of up to pool_capacity
// tiles.
BlockingSetupSamples *blocking_setup_samples_create(int max_racks,
                                                    int pool_capacity);
void blocking_setup_samples_destroy(BlockingSetupSamples *samples);

// The unseen pool from the on-turn player's view in ascending machine letter
// order (blanks as BLANK_MACHINE_LETTER): the letter distribution less the
// board's tiles and the player's rack. Returns the pool size; pool must hold
// ld_get_total_tiles(ld) tiles.
int blocking_setup_unseen_pool(const Game *game, MachineLetter *pool);

// Deals num_racks opponent racks of min(RACK_SIZE, pool size) tiles from the
// unseen pool, and a follow-up draw order for each, from seed. With
// partition, each shuffle of the pool is dealt into as many disjoint racks
// as fit before reshuffling; otherwise every rack is drawn independently.
// With condition_draws, a rack's follow-up order excludes that rack's tiles
// (the opponent holds them); otherwise it is the whole pool.
void blocking_setup_samples_deal(BlockingSetupSamples *samples,
                                 const Game *game, int num_racks,
                                 bool partition, bool condition_draws,
                                 uint64_t seed);

typedef struct BlockingSetupResult {
  double pass_reply_mean;
  double candidate_reply_mean;
  double blocking_delta;
  double pass_followup_mean;
  double candidate_followup_mean;
  double setup_delta;
  // Racks whose reply to the candidate ended the game.
  int terminal_replies;
} BlockingSetupResult;

// Scratch state for measuring one position's candidates. Not thread safe:
// each thread uses its own checker (move generation already uses one
// MoveGen per thread).
typedef struct BlockingSetupChecker BlockingSetupChecker;

BlockingSetupChecker *blocking_setup_checker_create(void);
void blocking_setup_checker_destroy(BlockingSetupChecker *checker);

// Plays our pass and each sampled rack's best reply to it, keeping one board
// per rack for the setup baseline. The game must not be over, its bag must
// hold at least one tile, and samples must stay valid and unchanged until
// the next load. followup_draws (1 to BLOCKING_SETUP_MAX_FOLLOWUP_DRAWS)
// follow-up racks are taken from each rack's draw order: draw k uses the
// tiles at positions k * n + i (mod the order's length), where n is the
// number of tiles the candidate's leave needs.
void blocking_setup_checker_load(BlockingSetupChecker *checker,
                                 const Game *game,
                                 const BlockingSetupSamples *samples,
                                 int followup_draws);

// Measures one candidate (a tile placement or an exchange of the on-turn
// player in the loaded game).
void blocking_setup_checker_measure(BlockingSetupChecker *checker,
                                    const Move *candidate,
                                    BlockingSetupResult *result);

// Picking only the best candidate (a rollout or static-ish policy) needs less
// than measuring all of them (a list of every play's values). The race
// measures the candidates in batches of racks, all on the same racks, and
// after each batch drops any candidate whose adjusted value trails the
// leader's by more than z standard errors of their paired per-rack
// difference. Adjusted value = base equity + blocking_weight *
// blocking_delta + setup_weight * setup_delta. With z <= 0 nothing is
// dropped and the result is exactly the argmax of full measurements; with
// z > 0 it is a different, faster policy whose choices can differ.
enum {
  BLOCKING_SETUP_RACE_BATCH = 8,
};

typedef struct BlockingSetupRaceSettings {
  // Racks per batch; 0 for BLOCKING_SETUP_RACE_BATCH.
  int batch_racks;
  // Racks measured before the first elimination.
  int min_racks;
  // Elimination threshold in standard errors; <= 0 disables elimination.
  double z;
} BlockingSetupRaceSettings;

typedef struct BlockingSetupRaceStats {
  // Candidate-rack measurements made (num_candidates * racks without
  // elimination).
  int candidate_racks;
  // Racks the survivors were measured on.
  int racks;
  int survivors;
} BlockingSetupRaceStats;

// Returns the index of the candidate with the highest adjusted value among
// the race's survivors (ties to the earlier candidate). base_equities are
// the candidates' equities without the checks. stats may be NULL.
int blocking_setup_checker_choose(BlockingSetupChecker *checker,
                                  const Move *const *candidates,
                                  const Equity *base_equities,
                                  int num_candidates, double blocking_weight,
                                  double setup_weight,
                                  const BlockingSetupRaceSettings *settings,
                                  BlockingSetupRaceStats *stats);

// The rack the on-turn player keeps after the move: placed tiles (blanks as
// BLANK_MACHINE_LETTER) or exchanged tiles removed.
void blocking_setup_candidate_leave(const Game *game, const Move *move,
                                    Rack *leave);

#endif
