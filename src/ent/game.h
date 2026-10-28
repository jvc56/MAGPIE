#ifndef GAME_H
#define GAME_H

#include "../def/game_defs.h"
#include "../def/players_data_defs.h"
#include "bag.h"
#include "board.h"
#include "cross_set_cache.h"
#include "kwg.h"
#include "letter_distribution.h"
#include "move_undo.h"
#include "player.h"
#include <stdbool.h>

typedef struct Game Game;

typedef struct GameArgs {
  PlayersData *players_data;
  const BoardLayout *board_layout;
  const LetterDistribution *ld;
  game_variant_t game_variant;
  int bingo_bonus;
  uint64_t seed;
} GameArgs;

Game *game_create(const GameArgs *game_args);
void game_destroy(Game *game);
void game_update(Game *game, const GameArgs *game_args);
Game *game_duplicate(const Game *game);
void game_copy(Game *dst, const Game *src);
void game_reset(Game *game);
void game_seed(Game *game, uint64_t seed);

game_variant_t game_get_variant(const Game *game);
int game_get_bingo_bonus(const Game *game);
game_variant_t get_game_variant_type_from_name(const char *variant_name);
Board *game_get_board(const Game *game);
Bag *game_get_bag(const Game *game);
const LetterDistribution *game_get_ld(const Game *game);
Player *game_get_player(const Game *game, int player_index);
int game_get_player_on_turn_index(const Game *game);
game_end_reason_t game_get_game_end_reason(const Game *game);
bool game_over(const Game *game);
backup_mode_t game_get_backup_mode(const Game *game);
int game_get_consecutive_scoreless_turns(const Game *game);
int game_get_starting_player_index(const Game *game);
int game_get_player_draw_index(const Game *game, int player_index);
int game_get_player_on_turn_draw_index(const Game *game);
bool game_get_data_is_shared(const Game *game,
                             players_data_t players_data_type);

void game_set_consecutive_scoreless_turns(Game *game, int value);
void game_set_player_on_turn_index(Game *game, int player_index);
int game_get_max_scoreless_turns(const Game *game);
bool game_reached_max_scoreless_turns(const Game *game);
void game_increment_consecutive_scoreless_turns(Game *game);
void game_set_endgame_solving_mode(Game *game);
void game_set_game_end_reason(Game *game, game_end_reason_t game_end_reason);
void game_start_next_player_turn(Game *game);

void game_set_backup_mode(Game *game, backup_mode_t backup_mode);
// Lets the game's first simulation backup record the board squares its moves
// change instead of copying the whole board, so that unplaying it restores
// only those (see game_backup). For a sim rollout's game, which plays a few
// moves after each such backup and then unplays them all at once.
void game_enable_rollout_board_undo(Game *game);
// Lets the game's classic cross sets come from, and go into, cache (see
// cross_set_cache.h), or not with NULL. The cache stays its caller's, who
// salts it whenever the game's lexica may change and frees it after the game
// stops using it. game_copy keeps the destination's cache; game_duplicate
// gives the copy none.
void game_set_cross_set_cache(Game *game, CrossSetCache *cache);
// The record the board's changes go into while a backup that records them is
// open, or NULL.
MoveUndo *game_get_open_rollout_board_undo(const Game *game);
void game_backup(Game *game);
void game_unplay_last_move(Game *game);
void game_set_starting_player_index(Game *game, int starting_player_index);
void game_gen_all_cross_sets(const Game *game);
#ifdef IF_ONLY
void game_if_only_swap(const Game *game, int player_index, int row, int col);
#endif
void game_gen_cross_set(const Game *game, int row, int col, int dir,
                        int cross_set_index);

// Override KWGs for cross-set generation (e.g., word-pruned KWGs in endgame).
// kwg0/kwg1 are not owned by Game. In IGNORANT mode, kwg0 is used for both
// cross-set indices. In INFORMED mode, kwg0/kwg1 are used for indices 0/1.
void game_set_override_kwgs(Game *game, const KWG *kwg0, const KWG *kwg1,
                            dual_lexicon_mode_t mode);
void game_clear_override_kwgs(Game *game);

// Returns the effective KWG for the given player/cross-set index, respecting
// any override KWGs. Falls back to the player's own KWG if no override is set.
const KWG *game_get_effective_kwg(const Game *game, int player_index);

#endif