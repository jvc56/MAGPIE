#ifndef PLAYER_H
#define PLAYER_H

#include "../def/move_defs.h"
#include "klv.h"
#include "kwg.h"
#include "pat.h"
#include "players_data.h"
#include "rack.h"
#include "rack_info_table.h"
#include "wmp.h"
#include "word_info_table.h"
#include <stdbool.h>

typedef struct Player Player;

Player *player_create(const PlayersData *players_data,
                      const LetterDistribution *ld, int player_index);
void player_destroy(Player *player);

int player_get_index(const Player *player);
Rack *player_get_rack(const Player *player);
Rack *player_get_known_rack_from_phonies(const Player *player);
Equity player_get_score(const Player *player);
move_sort_t player_get_move_sort_type(const Player *player);
move_record_t player_get_move_record_type(const Player *player);
const KWG *player_get_kwg(const Player *player);
const KLV *player_get_klv(const Player *player);
const WMP *player_get_wmp(const Player *player);
const RackInfoTable *player_get_rack_info_table(const Player *player);
const WordInfoTable *player_get_word_info_table(const Player *player);
const PATWeights *player_get_pat(const Player *player);
uint32_t player_get_pat_disabled_classes_mask(const Player *player);
bool player_get_rollout_disable_pat(const Player *player);
// See player_set_rollout_pat_disabled_classes_mask below.
uint32_t player_get_rollout_pat_disabled_classes_mask(const Player *player);
// See player_set_rollout_zero_leave below.
bool player_get_rollout_zero_leave(const Player *player);
// The all-zero KLV player_set_rollout_zero_leave's flag swaps in when set;
// NULL until player_set_rollout_zero_klv is called. Not meant to be read
// outside get_top_equity_move's own swap-and-restore.
const KLV *player_get_rollout_zero_klv(const Player *player);

void player_set_score(Player *player, Equity score);
void player_set_move_sort_type(Player *player, move_sort_t move_sort_type);
void player_set_move_record_type(Player *player,
                                 move_record_t move_record_type);
void player_add_to_score(Player *player, Equity score);
// Swaps which PATWeights this player's own movegen calls read (see
// player_duplicate: a duplicated player shares its source's pat pointer,
// not a copy of it), without touching anything else on the player. The
// caller owns pat's lifetime; this does not take ownership or free the
// player's previous one. Meant for tooling that needs one duplicated
// game's own move choices to follow a different policy than its source
// (e.g. comparing two candidate weights files' own self-consistent
// rollouts), not for real gameplay, which loads PAT once per position.
void player_set_pat(Player *player, const PATWeights *pat);

// Swaps which KLV this player's own movegen calls read, exactly like
// player_set_pat above but for leave value instead of PAT. The caller owns
// klv's lifetime; this does not take ownership or free the player's
// previous one.
void player_set_klv(Player *player, const KLV *klv);

// Forces PAT fully off in get_top_equity_move's own MoveGenArgs, independent
// of pat_disabled_classes_mask (which get_top_equity_move does not consult).
// get_top_equity_move is what a Monte Carlo rollout's forward-play plies
// call at every simulated ply; it is not what generates the top-level
// candidate list a round-robin simulation is handed before simming starts
// (that goes through generate_moves_for_game with its own MoveGenArgs, which
// never reads this field), so setting this on a game's players after that
// candidate list is frozen affects only the simulated continuations, not the
// candidates themselves. Meant for tooling that wants a duplicated game's
// simulated rollouts to ignore PAT while its own top-level candidate
// generation stays untouched (see player_set_pat for the same "duplicated
// game plays a different policy" pattern). Defaults to false; real gameplay
// never sets it.
void player_set_rollout_disable_pat(Player *player, bool disable);

// Analogous to player_set_rollout_disable_pat above, but a class mask
// instead of a full on/off toggle: threaded into get_top_equity_move's own
// MoveGenArgs.pat_disabled_classes_mask, independent of
// pat_disabled_classes_mask (the production, non-rollout mask
// player_get_pat_disabled_classes_mask reads, which get_top_equity_move
// does not consult -- see that function's own comment). This is what lets a
// duplicated game's simulated rollouts run PAT restricted to a class subset
// (e.g. PAT_CLASS_MASK_ALL & ~PAT_CLASS_MASK_TWS_ONLY, for a TWS-only
// rollout) while its own top-level candidate generation stays on full PAT.
// Defaults to 0 (nothing disabled, i.e. full PAT): real gameplay never sets
// it, and it has no effect when rollout_disable_pat is also true (PAT is
// fully off either way).
void player_set_rollout_pat_disabled_classes_mask(Player *player,
                                                  uint32_t mask);

// Forces leave value to zero in get_top_equity_move's own equity ranking,
// analogous to player_set_rollout_disable_pat above but for leave instead
// of PAT, and for the same reason: get_top_equity_move is what a Monte
// Carlo rollout's forward-play plies call at every simulated ply, so a
// toggle meant to change ONLY those continuation-ply move choices (not the
// frozen top-level candidate list, and not any other leave-value use, such
// as random_variable.c's own separate leftover/leave-residual bookkeeping)
// has to be read there. Unlike rollout_disable_pat, there is no existing
// "disable leave" flag plumbed through MoveGenArgs/move_gen.c to piggyback
// on -- leave value is read deep inside move_gen.c's hot path (gen->klv,
// set from player_get_klv at position load, with klv_get_indexed_leave_value
// calls throughout) with no equivalent of disable_pat's single boolean
// gate. So get_top_equity_move instead temporarily swaps the player's own
// klv pointer (via player_set_klv) to the all-zero KLV set by
// player_set_rollout_zero_klv for the duration of its own generate_moves
// call only, then restores the real klv immediately after -- move_gen.c
// already supports a KLV being replaced between calls on the same player
// (see klv_get_instance_fingerprint's comment: MoveGen caches detect a
// replaced KLV via a content fingerprint, not just a pointer compare), so
// this swap-and-restore is exactly the kind of use that machinery was
// built for. Defaults to false; real gameplay never sets it, and
// rollout_zero_klv must be set (see player_set_rollout_zero_klv) before
// this flag is turned on -- get_top_equity_move relies on the caller
// having done so.
void player_set_rollout_zero_leave(Player *player, bool zero_leave);
// The all-zero KLV to swap in when rollout_zero_leave is set (see above).
// The caller owns klv's lifetime, exactly like player_set_klv/player_set_pat.
void player_set_rollout_zero_klv(Player *player, const KLV *klv);

void player_update(const PlayersData *players_data, Player *player);
Player *player_duplicate(const Player *player);
void player_copy(Player *dst, const Player *src);
void player_reset(Player *player);

#endif