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
// Whether this player's move generation leaves PAT out.
bool player_get_pat_disabled(const Player *player);

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

// Switches whether this player's move generation uses PAT and which classes
// it suppresses, until the next player_update. A sim sets this on its
// rollout copies of the game (see rv_sim_create), so its rollouts follow the
// simming player's rollout settings rather than the candidate-selection ones
// player_update loads.
void player_set_pat_usage(Player *player, bool disabled,
                          uint32_t disabled_classes_mask);

void player_update(const PlayersData *players_data, Player *player);
Player *player_duplicate(const Player *player);
void player_copy(Player *dst, const Player *src);
void player_reset(Player *player);

#endif