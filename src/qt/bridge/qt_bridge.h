#ifndef QT_BRIDGE_H
#define QT_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handles
typedef struct _BridgeGameHistory BridgeGameHistory;
typedef struct _BridgeGame BridgeGame;

BridgeGameHistory* bridge_game_history_create(void);
void bridge_game_history_destroy(BridgeGameHistory* gh);

// Returns 0 on success, 1 on failure. Populates error_msg if failed.
int bridge_load_gcg(BridgeGameHistory* gh, const char* gcg_content, const char* data_path, char* error_msg, int error_msg_len);

BridgeGame* bridge_game_create_from_history(BridgeGameHistory* gh, const char* data_path);
void bridge_game_destroy(BridgeGame* game);

// Navigation
// Updates the game state to the given event index.
void bridge_game_play_to_index(BridgeGameHistory* gh, BridgeGame* game, int index);

// Accessors
const char* bridge_get_player_name(BridgeGameHistory* gh, int player_index);
int bridge_get_player_score(BridgeGame* game, int player_index);
int bridge_get_num_events(BridgeGameHistory* gh);

// Board
// Returns a newly allocated string for the square at row, col. Caller must free.
// Returns empty string if empty.
char* bridge_get_board_square_string(BridgeGame* game, int row, int col);

// Returns the raw bonus type for the square at row, col.
// See bonus_square.h for how to interpret this value.
uint8_t bridge_get_board_bonus(BridgeGame* game, int row, int col);

// Returns the machine letter on the square at row, col.
uint8_t bridge_get_machine_letter(BridgeGame* game, int row, int col);

// Returns the score for a given machine letter.
int bridge_get_letter_score(BridgeGame* game, uint8_t ml);

// Returns true if the machine letter is a blank.
bool bridge_is_blank(uint8_t ml);

#ifdef __cplusplus
}
#endif

#endif
