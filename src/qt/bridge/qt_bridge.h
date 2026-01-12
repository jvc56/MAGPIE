#ifndef QT_BRIDGE_H
#define QT_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handles
typedef struct _BridgeGameHistory BridgeGameHistory;
typedef struct _BridgeGame BridgeGame;

BridgeGameHistory *bridge_game_history_create(void);
void bridge_game_history_destroy(BridgeGameHistory *gh);

// Returns 0 on success, 1 on failure. Populates error_msg if failed.
int bridge_load_gcg(BridgeGameHistory *gh, const char *gcg_content,
                    const char *data_path, char *error_msg, int error_msg_len);

BridgeGame *bridge_game_create_from_history(BridgeGameHistory *gh);
void bridge_game_destroy(BridgeGame *game);
BridgeGame *bridge_game_clone(BridgeGame *game);

int bridge_game_get_history_num_events(BridgeGame *game);

// Navigation
// Updates the game state to the given event index.
void bridge_game_play_to_index(BridgeGameHistory *gh, BridgeGame *game,
                               int index);

// Accessors
const char *bridge_get_player_name(BridgeGameHistory *gh, int player_index);
int bridge_get_player_score(BridgeGame *game, int player_index);
int bridge_get_player_on_turn_index(BridgeGame *game);
int bridge_get_num_events(BridgeGameHistory *gh);
const char *bridge_get_lexicon(BridgeGameHistory *gh);

// Board
// Returns a newly allocated string for the square at row, col. Caller must
// free. Returns empty string if empty.
char *bridge_get_board_square_string(BridgeGame *game, int row, int col);

// Returns the raw bonus type for the square at row, col.
// See bonus_square.h for how to interpret this value.
uint8_t bridge_get_board_bonus(BridgeGame *game, int row, int col);

// Returns the machine letter on the square at row, col.
uint8_t bridge_get_machine_letter(BridgeGame *game, int row, int col);

// Returns the score for a given machine letter.
int bridge_get_letter_score(BridgeGame *game, uint8_t ml);

// Returns true if the machine letter is a blank.
bool bridge_is_blank(uint8_t ml);

// Helper to get current rack string
char *bridge_get_current_rack(BridgeGame *game);

// Get number of tiles in bag
int bridge_get_bag_count(BridgeGame *game);

// Get unseen tiles (bag + opponent rack)
// Returns string in *tiles (caller must free), and counts
void bridge_get_unseen_tiles(BridgeGame *game, char **tiles, int *vowel_count,
                             int *consonant_count, int *blank_count);

int bridge_get_board_tiles_played(BridgeGame *game);

// Returns the index of the current event (or next event to be played)
// 0 means start of game.
int bridge_get_current_event_index(BridgeGame *game);

// Get details for a specific event index
void bridge_get_event_details(BridgeGameHistory *gh, BridgeGame *game,
                              int index, int *player_index, int *type,
                              char **move_str, char **rack_str, int *score,
                              int *cumulative_score);

// Populates rows and cols arrays with the coordinates of tiles placed in the
// event at index. Returns the number of tiles found (up to max_count). Pass
// NULL for rows/cols to just query the count.
int bridge_get_last_move_tiles(BridgeGameHistory *gh, int index, int *rows,
                               int *cols, int max_count);

// Analysis
typedef struct _BridgeMoveList BridgeMoveList;
typedef struct _BridgeSimResults BridgeSimResults;
typedef struct _BridgeThreadControl BridgeThreadControl;

BridgeMoveList *bridge_generate_moves(BridgeGame *game);
void bridge_move_list_destroy(BridgeMoveList *ml);

BridgeSimResults *bridge_sim_results_create(void);
void bridge_sim_results_destroy(BridgeSimResults *sr);

BridgeThreadControl *bridge_thread_control_create(void);
void bridge_thread_control_destroy(BridgeThreadControl *tc);
void bridge_thread_control_stop(BridgeThreadControl *tc);

// This function blocks! Run in a separate thread.
void bridge_simulate(BridgeGame *game, BridgeMoveList *moves,
                     BridgeSimResults *results, BridgeThreadControl *tc,
                     int plies);

// Accessors for results
int bridge_sim_results_get_num_plays(BridgeSimResults *results);
// Returns 0 on success, 1 if index out of bounds.
// notation must be freed by caller.
int bridge_sim_results_get_play_info(BridgeGame *game,
                                     BridgeSimResults *results, int index,
                                     char **notation, double *win_pct,
                                     double *spread, int *iterations);
// Gameplay
// lexicon_name is e.g. "CSW24"
BridgeGameHistory *bridge_game_create_fresh(const char *data_path,
                                            const char *lexicon_name,
                                            char *error_msg, int error_msg_len);

// notation is UCGI format (e.g. "8h.WORD")
// Returns NULL if valid, or a newly allocated error message if invalid.
char *bridge_validate_move(BridgeGame *game, const char *notation);

// Returns NULL on success, or error message on failure.
char *bridge_play_move(BridgeGameHistory *gh, BridgeGame *game,
                       const char *notation);

// Returns UCGI notation for the best move. Caller must free.
char *bridge_get_computer_move(BridgeGame *game);

void bridge_game_draw_racks(BridgeGame *game);

uint64_t bridge_sim_results_get_iterations(BridgeSimResults *results);
double bridge_sim_results_get_confidence(BridgeSimResults *results);

#ifdef __cplusplus
}
#endif

#endif