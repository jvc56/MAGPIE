#ifndef GAMEPLAY_H
#define GAMEPLAY_H

#include "../def/move_defs.h"
#include "../ent/bag.h"
#include "../ent/best_move_carry.h"
#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../ent/move.h"
#include "../ent/move_undo.h"
#include "../ent/rack.h"
#include "../ent/sim_args.h"
#include "move_gen.h"
#include <stdbool.h>

void draw_starting_racks(const Game *game);
Equity calculate_end_rack_points(const Rack *rack,
                                 const LetterDistribution *ld);
Equity calculate_end_rack_penalty(const Rack *rack,
                                  const LetterDistribution *ld);
void play_move(const Move *move, Game *game, Rack *leave);
void play_move_no_cross_set_update(const Move *move, Game *game, Rack *leave);
void play_move_without_drawing_tiles(const Move *move, Game *game);
void set_random_rack(const Game *game, int player_index,
                     const Rack *known_rack);
const Move *get_top_equity_move(Game *game, MoveList *move_list);
// get_top_equity_move, starting from and adding to the best moves remembered
// in best_move_carry (see best_move_carry.h). The move is the same.
const Move *get_top_equity_move_with_carry(Game *game, MoveList *move_list,
                                           BestMoveCarry *best_move_carry);
const Move *get_top_move_for_player_on_turn(Game *game, MoveList *move_list,
                                            bool record_all);
Move *get_top_equity_move_for_inferences(Game *game, MoveList *move_list,
                                         Equity target_equity);
void generate_moves_for_game_override_record_type(
    const MoveGenArgs *args, move_record_t move_record_type);
void generate_moves_for_game(const MoveGenArgs *args);
void draw_to_full_rack(const Game *game, int player_index);
void draw_leave_from_bag(Bag *bag, int player_draw_index, Rack *rack_to_update,
                         const Rack *rack_to_draw);
void get_leave_for_move(const Move *move, const Game *game, Rack *leave);
void return_rack_to_bag(const Game *game, int player_index);
Equity get_leave_value_for_move(const KLV *klv, const Move *move, Rack *rack);
void return_phony_letters(Game *game);

// Cross-set update for move affected squares
void update_cross_set_for_move(const Move *move, const Game *game);

// Incremental play/unplay functions for endgame solver optimization
void play_move_incremental(const Move *move, Game *game, MoveUndo *undo);
// Like play_move_incremental, but a tile placement leaves the anchors and
// cross-sets around the move stale and marks the board's cross-sets invalid.
// Before anything reads them, update_deferred_move_into_undo must run for
// this move and for every other deferred move still on the board.
void play_move_incremental_deferred(const Move *move, Game *game,
                                    MoveUndo *undo);
void unplay_move_incremental(Game *game, const MoveUndo *undo);
void play_move_endgame_outplay(const Move *move, Game *game, MoveUndo *undo);
void update_cross_sets_after_unplay(const Move *move, const Game *game);

// MoveUndo-based cross-set update for the move region (forward, lazy).
// Saves the previous contents of every square it modifies into the same
// undo, so unplay_move_incremental's square restore reverts the cross-set
// updates exactly — no recompute is needed after unplay. This excludes the
// parallel WIT block caches; unplay invalidates those derived entries.
void update_cross_set_for_move_from_undo(MoveUndo *undo, const Game *game);

// Updates the anchors and cross-sets around the move recorded in move_undo,
// which was played with play_move_incremental_deferred and is still on the
// board. Every square written is saved into save_undo, the undo of the most
// recent move, so unplaying that move restores them. Later moves may be on
// the board too: the update reads the board as it is now.
void update_deferred_move_into_undo(const MoveUndo *move_undo,
                                    MoveUndo *save_undo, const Game *game);

// Called at the initial position and after each event. Borrowed game/history
// are valid only during the callback. Return false to stop normally; errors
// from replay itself are reported through error_stack.
// Callback snapshots include tile ownership for recorded placements.
typedef bool (*GameHistoryCallback)(const GameHistory *game_history,
                                    const Game *game, int event_index,
                                    void *user_data);
void game_replay_history(GameHistory *game_history, Game *game,
                         GameHistoryCallback callback, void *user_data,
                         ErrorStack *error_stack);
// Event index 0 selects the start; N selects the position after event N.
void game_play_n_events(GameHistory *game_history, Game *game,
                        int num_events_to_play, bool validate,
                        ErrorStack *error_stack);
bool game_history_contains_end_rack_penalty_event(
    const GameHistory *game_history);

#endif
