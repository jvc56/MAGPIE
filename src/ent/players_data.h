#ifndef PLAYERS_DATA_H
#define PLAYERS_DATA_H

#include "../def/move_defs.h"
#include "../def/players_data_defs.h"
#include "equity.h"
#include "inference_temperatures.h"
#include "klv.h"
#include "kwg.h"
#include "letter_distribution.h"
#include "outplay_check_weights.h"
#include "pat.h"
#include "peg_adjust.h"
#include "pool_shift.h"
#include "rack_info_table.h"
#include "rack_value_table.h"
#include "wmp.h"
#include "word_info_table.h"
#include <stdbool.h>

typedef struct PlayersData PlayersData;

PlayersData *players_data_create(bool use_wmp);
void players_data_destroy(PlayersData *players_data);

move_sort_t players_data_get_move_sort_type(const PlayersData *players_data,
                                            int player_index);
move_record_t players_data_get_move_record_type(const PlayersData *players_data,
                                                int player_index);
const char *players_data_get_data_name(const PlayersData *players_data,
                                       players_data_t players_data_type,
                                       int player_index);
KWG *players_data_get_kwg(const PlayersData *players_data, int player_index);
KLV *players_data_get_klv(const PlayersData *players_data, int player_index);
WMP *players_data_get_wmp(const PlayersData *players_data, int player_index);
RackInfoTable *players_data_get_rack_info_table(const PlayersData *players_data,
                                                int player_index);
WordInfoTable *players_data_get_word_info_table(const PlayersData *players_data,
                                                int player_index);
PATWeights *players_data_get_pat(const PlayersData *players_data,
                                 int player_index);
InferenceTemperatures *
players_data_get_inference_temperatures(const PlayersData *players_data,
                                        int player_index);
OutplayCheckWeights *
players_data_get_outplay_check(const PlayersData *players_data,
                               int player_index);
PegAdjust *players_data_get_peg_adjust(const PlayersData *players_data,
                                       int player_index);
uint32_t
players_data_get_pat_disabled_classes_mask(const PlayersData *players_data,
                                           int player_index);

void players_data_set_move_sort_type(PlayersData *players_data,
                                     int player_index,
                                     move_sort_t move_sort_type);
void players_data_set_move_record_type(PlayersData *players_data,
                                       int player_index,
                                       move_record_t move_record_type);
void players_data_set_pat_disabled_classes_mask(PlayersData *players_data,
                                                int player_index,
                                                uint32_t mask);
// Whether the player's own move generation (static play and the candidate
// list a sim starts from) leaves PAT out. Defaults to false.
void players_data_set_pat_candidates_disabled(PlayersData *players_data,
                                              int player_index, bool disabled);
bool players_data_get_pat_candidates_disabled(const PlayersData *players_data,
                                              int player_index);
// Whether the rollouts of the player's sims leave PAT out, and which classes
// they suppress when they don't. Default to false and 0 (every class).
void players_data_set_pat_rollout_disabled(PlayersData *players_data,
                                           int player_index, bool disabled);
bool players_data_get_pat_rollout_disabled(const PlayersData *players_data,
                                           int player_index);
// Whether the player's sims roll out with the PAT term static evaluation
// uses, in place of board-only PAT (see the patrolloutfast option).
void players_data_set_pat_rollout_exact(PlayersData *players_data,
                                        int player_index, bool exact);
bool players_data_get_pat_rollout_exact(const PlayersData *players_data,
                                        int player_index);
void players_data_set_pat_rollout_disabled_classes_mask(
    PlayersData *players_data, int player_index, uint32_t mask);
uint32_t players_data_get_pat_rollout_disabled_classes_mask(
    const PlayersData *players_data, int player_index);
// The player's pool shift weight (see pool_shift.h), in thousandths, from 0
// (no pool shift, the default) to POOL_SHIFT_MAX_WEIGHT_MILLI. Setting it
// does not build or free a table; players_data_update_pool_shift does.
void players_data_set_pool_shift_weight(PlayersData *players_data,
                                        int player_index, int weight_milli);
int players_data_get_pool_shift_weight(const PlayersData *players_data,
                                       int player_index);
// Gives each player whose weight is not 0 a pool shift table of its KLV as it
// is now, building the ones that are missing, and frees every table built
// from leaves no player has any more. A table stays while a player has its
// leaves, even at weight 0, so that a game that read it can still do so.
// Players with the same KLV share one table. To be called once the players'
// leaves and weights are as a command leaves them, before any game is
// updated from them.
void players_data_update_pool_shift(PlayersData *players_data,
                                    const LetterDistribution *ld);
// The player's table, or NULL when its weight is 0 or it has no leaves.
// Gives each player's pool shift table the full racks' values for the exact
// leave values (see pool_shift.h): exact_values[player_index], or none for
// NULL. A table both players share has them if either player gives them.
// Returns false if a player's values are not for that player's table.
bool players_data_set_pool_shift_exact_values(
    PlayersData *players_data, const RackValueTable *const exact_values[2],
    const LetterDistribution *ld);

const PoolShiftTable *
players_data_get_pool_shift_table(const PlayersData *players_data,
                                  int player_index);
void *players_data_get_data(const PlayersData *players_data,
                            players_data_t players_data_type, int player_index);
bool players_data_get_is_shared(const PlayersData *players_data,
                                players_data_t players_data_type);
void players_data_set_is_shared(PlayersData *players_data,
                                players_data_t players_data_type,
                                bool is_shared);
bool players_data_get_use_when_available(const PlayersData *players_data,
                                         players_data_t players_data_type,
                                         int player_index);
void players_data_set_use_when_available(PlayersData *players_data,
                                         players_data_t players_data_type,
                                         int player_index,
                                         bool use_when_available);
void players_data_set(PlayersData *players_data,
                      players_data_t players_data_type, const char *data_paths,
                      const char *p1_data_name, const char *p2_data_name,
                      bool use_mmap_for_rit, ErrorStack *error_stack);
// Directly sets the data pointer for a slot, without taking it through the
// filename-based load path. Ownership of `data` is transferred to
// PlayersData: when the slot is replaced (via another set_data or
// set/reload) or when PlayersData is destroyed, `data` will be freed via
// the type-appropriate destroy function. Callers must not free or destroy
// `data` after passing it here. Use NULL to clear the slot; the previous
// value is NOT freed by set_data itself, so call players_data_destroy_data
// first if you need to release the old pointer before overwriting.
void players_data_set_data(PlayersData *players_data,
                           players_data_t players_data_type, int player_index,
                           void *data);
void players_data_validate_word_info_tables(const PlayersData *players_data,
                                            ErrorStack *error_stack);
// Whether player `player_index`'s word info table, if any, was built from that
// player's KWG. A table that does not record one cannot be checked, and
// matches.
bool players_data_wit_matches_kwg(const PlayersData *players_data,
                                  int player_index);
// Whether player `player_index`'s rack info table, if any, was built from
// that player's lexicon and leaves. A table that records neither (built
// before version 13) cannot be checked, and matches.
bool players_data_rit_matches_inputs(const PlayersData *players_data,
                                     int player_index);
// Whether player `player_index`'s rack info table, if any, holds the leave
// values that player's KLV gives. It stores leave values but not always which
// KLV they came from, so this samples entries spread across it.
bool players_data_rit_matches_klv(const PlayersData *players_data,
                                  int player_index);
// Destroys both players' data of this type and leaves the slots empty, so the
// next players_data_set loads from disk even for a name that is already
// loaded. The use-when-available flags are left as they are.
void players_data_evict(PlayersData *players_data,
                        players_data_t players_data_type);
void players_data_reload(PlayersData *players_data,
                         players_data_t players_data_type,
                         const char *data_paths, ErrorStack *error_stack);

#endif
